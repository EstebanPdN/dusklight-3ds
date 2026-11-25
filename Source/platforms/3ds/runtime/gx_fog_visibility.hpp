#pragma once

#include "gx_gpu_geometry.hpp"
#include "gx_resident_cull.hpp"
#include <cstring>
#include <memory>
#include <new>

namespace dusk::native::gx {
// Both passes use the same ordered triangles. Only the frame arena's pending
// index list is compacted. Resident vertex records and their topology cache
// stay immutable, and a retained run never costs a separate GPU draw.
class FogVisibility {
    using Box = resident_cull::Box;
    using Uniform = resident_cull::Uniform;
    static constexpr unsigned blockSize = resident_cull::fineIndices;
    static constexpr unsigned byteBudget = 128 * 1024;

    struct Mesh {
        const std::byte* vertices{};
        unsigned count{}, row{};
        std::unique_ptr<std::uint16_t[]> topology;
        std::unique_ptr<Box[]> boxes;
    };
    std::array<Mesh, 16> meshes{};
    unsigned used{}, epoch{};
    bool allocationFailed{};

    const Mesh* find(const std::byte* vertices, std::span<const std::uint16_t> indices) {
        Mesh* vacant = nullptr;
        for (auto& m : meshes) {
            if (!m.vertices) { vacant = &m; break; }
            if (m.vertices == vertices && m.count == indices.size() &&
                !std::memcmp(m.topology.get(), indices.data(), indices.size_bytes())) return &m;
        }
        const unsigned blocks = (indices.size() + blockSize - 1) / blockSize;
        const unsigned bytes = indices.size_bytes() + blocks * sizeof(Box);
        if (!vacant || allocationFailed || bytes > byteBudget - used) return nullptr;
        Mesh candidate;
        candidate.topology.reset(new(std::nothrow) std::uint16_t[indices.size()]);
        candidate.boxes.reset(new(std::nothrow) Box[blocks]);
        if (!candidate.boxes || !candidate.topology) { allocationFailed = true; return nullptr; }
        std::memcpy(candidate.topology.get(), indices.data(), indices.size_bytes());
        candidate.vertices = vertices;
        candidate.count = indices.size();
        constexpr auto stride = sizeof(CompactRigidVertexMT);
        constexpr auto matrix = offsetof(CompactRigidVertexMT, matrix);
        candidate.row = std::to_integer<unsigned>(vertices[stride * indices.front() + matrix]);
        bool valid = candidate.row == 0;
        for (unsigned b = 0; valid && b < blocks; ++b) {
            resident_cull::Bounds bounds;
            const unsigned end = std::min(candidate.count, (b + 1) * blockSize);
            for (unsigned i = b * blockSize; i < end; ++i) {
                const auto* v = vertices + stride * indices[i];
                float pos[3];
                std::memcpy(pos, v, sizeof(pos));
                if (std::to_integer<unsigned>(v[matrix]) != candidate.row || !bounds.add(pos)) {
                    valid = false;
                    break;
                }
            }
            if (valid) candidate.boxes[b] = bounds.box();
        }
        // Remember unsupported layouts too. Never rescan their records each frame.
        if (!valid) candidate.boxes.reset();
        *vacant = std::move(candidate);
        used += bytes;
        return vacant;
    }

public:
    void reset(unsigned vertexEpoch) {
        if (epoch == vertexEpoch) return;
        for (auto& m : meshes) m = {};
        used = 0;
        allocationFailed = false;
        epoch = vertexEpoch;
    }

    unsigned select(const std::byte* vertices, std::span<std::uint16_t> indices, std::span<const Uniform> uniforms) {
        if (!vertices || indices.size() < resident_cull::minFineIndices || indices.size() > 16384 ||
            indices.size() % 3) return indices.size();
        const auto* mesh = find(vertices, indices);
        resident_cull::Planes planes;
        constexpr unsigned mask = resident_cull::sidePlanes;
        if (!mesh || !mesh->boxes || !resident_cull::makePlanes(uniforms, mesh->row, mask, planes)) return indices.size();

        unsigned kept = 0;
        unsigned run = 0;
        for (unsigned first = 0; first < mesh->count; first += blockSize) {
            if (!resident_cull::classify(planes, mesh->boxes[first / blockSize], mask).outside) continue;
            if (first > run) {
                if (kept != run) std::memmove(indices.data() + kept, indices.data() + run, (first - run) * sizeof(indices[0]));
                kept += first - run;
            }
            run = std::min(first + blockSize, mesh->count);
        }
        if (kept != run && run != mesh->count)
            std::memmove(indices.data() + kept, indices.data() + run, (mesh->count - run) * sizeof(indices[0]));
        return kept + mesh->count - run;
    }
};
}
