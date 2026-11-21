// Mac-only: the draws the runtime makes of one J3D display list and the
// vertex records and indices of each, as runtime/gx_commands.cpp makes them
// (CommandProcessor::execute and drawBatch with the scratch game_startup.cpp
// gives both processors: 4096 vertices, 64 KiB packed vertices, 16 Ki
// indices, deduplication). Proven against that processor on every list of
// the disc by model_lod_batch_test.cpp; runtime/model_lod_store.hpp
// modelLodBatchRule names this rule.
//
// - Two or more consecutive triangle-kind primitives of one vertex format
//   that fit the caps: one draw; records are the distinct packed tuples in
//   first-use order (at least 3: the first is repeated), indices expanded by
//   aurora::gx::dl::expand_triangles and mapped to them.
// - Otherwise one primitive alone: its raw tuples, indices expanded.
// Lines and points are widened per picture and never recorded.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace modellod {
struct BatchCaps { std::uint32_t maxVertices = 4096, vertexBytes = 64 * 1024, maxIndices = 16 * 1024; };
struct RuntimeBatch {
    std::vector<std::uint32_t> tuple;   // per vertex record: offset of its packed tuple in the list
    std::vector<std::uint16_t> indices; // the full index list the runtime draws
    bool triangles = true;              // false: lines/points (never recorded)
};

template <class F>
inline bool expandTriangles(std::uint8_t primitive, std::uint32_t count, F&& f) {
    switch (primitive) {
    case 0x90:
        if (count < 3 || count % 3) return false;
        for (std::uint32_t v = 0; v < count; v += 3) f(v, v + 1, v + 2);
        return true;
    case 0x98:
        if (count < 3) return false;
        for (std::uint32_t v = 2; v < count; ++v) {
            if ((v & 1) == 0) f(v - 2, v - 1, v);
            else f(v - 1, v - 2, v);
        }
        return true;
    case 0xA0:
        if (count < 3) return false;
        for (std::uint32_t v = 2; v < count; ++v) f(0, v - 1, v);
        return true;
    case 0x80:
        if (count < 4 || count % 4) return false;
        for (std::uint32_t v = 0; v < count; v += 4) { f(v, v + 1, v + 2); f(v + 2, v + 3, v); }
        return true;
    }
    return false;
}
inline std::uint32_t batchIndices(std::uint8_t primitive, std::uint32_t count) {
    if (primitive == 0x90 && count >= 3 && count % 3 == 0) return count;
    if (primitive == 0x80 && count >= 4 && count % 4 == 0) return count / 4 * 6;
    if ((primitive == 0x98 || primitive == 0xA0) && count >= 3) return (count - 2) * 3;
    return 0;
}

// False: the list holds something the processor would not draw as a J3D
// shape list (a command other than primitives and NOP padding, truncation).
inline bool runtimeBatches(const std::uint8_t* list, std::uint32_t size, std::uint32_t stride, std::vector<RuntimeBatch>& out,
                           const BatchCaps& caps = {}) {
    out.clear();
    if (!stride) return false;
    std::uint32_t at = 0;
    const std::uint32_t vertexCap = std::min<std::uint32_t>(caps.maxVertices, 65535);
    while (at < size) {
        const std::uint8_t op = list[at];
        if (op == 0) { ++at; continue; }
        const std::uint8_t kind = op & 0xF8;
        if (!(op & 0x80) || kind > 0xB8) return false;
        if (at + 3 > size) return false;
        const std::uint32_t count = (std::uint32_t(list[at + 1]) << 8) | list[at + 2];
        const std::uint64_t bytes = 3 + std::uint64_t(count) * stride;
        if (bytes > size - at) return false;
        // drawBatch from this primitive.
        std::uint32_t cursor = at, vertices = 0, indices = 0, packets = 0;
        const std::uint8_t vat = op & 7;
        while (cursor + 3 <= size) {
            const std::uint8_t o = list[cursor];
            if ((o & 7) != vat) break;
            const std::uint8_t k = o & 0xF8;
            const std::uint32_t c = (std::uint32_t(list[cursor + 1]) << 8) | list[cursor + 2];
            const std::uint32_t idx = batchIndices(k, c);
            if (!idx) break;
            const std::uint64_t payload = std::uint64_t(c) * stride;
            if (payload > size - cursor - 3 || c > vertexCap - vertices || payload > caps.vertexBytes - std::uint64_t(vertices) * stride ||
                idx > caps.maxIndices - indices) break;
            vertices += c; indices += idx; cursor += 3 + std::uint32_t(payload); ++packets;
        }
        RuntimeBatch batch;
        if (packets >= 2) {
            std::map<std::string, std::uint16_t> seen;
            for (std::uint32_t p = at; p < cursor;) {
                const std::uint8_t k = list[p] & 0xF8;
                const std::uint32_t c = (std::uint32_t(list[p + 1]) << 8) | list[p + 2];
                std::vector<std::uint16_t> remap(c);
                for (std::uint32_t n = 0; n < c; ++n) {
                    const std::uint32_t offset = p + 3 + n * stride;
                    const std::string key(reinterpret_cast<const char*>(list + offset), stride);
                    auto [it, fresh] = seen.emplace(key, std::uint16_t(batch.tuple.size()));
                    if (fresh) batch.tuple.push_back(offset);
                    remap[n] = it->second;
                }
                expandTriangles(k, c, [&](std::uint32_t a, std::uint32_t b, std::uint32_t d) {
                    batch.indices.push_back(remap[a]); batch.indices.push_back(remap[b]); batch.indices.push_back(remap[d]);
                });
                p += 3 + c * stride;
            }
            while (batch.tuple.size() < 3) batch.tuple.push_back(batch.tuple.front());
            out.push_back(std::move(batch));
            at = cursor;
            continue;
        }
        for (std::uint32_t n = 0; n < count; ++n) batch.tuple.push_back(at + 3 + n * stride);
        batch.triangles = expandTriangles(kind, count, [&](std::uint32_t a, std::uint32_t b, std::uint32_t d) {
            batch.indices.push_back(std::uint16_t(a)); batch.indices.push_back(std::uint16_t(b)); batch.indices.push_back(std::uint16_t(d));
        });
        if (!batch.triangles) batch.indices.clear();
        out.push_back(std::move(batch));
        at += std::uint32_t(bytes);
    }
    return true;
}
}  // namespace modellod
