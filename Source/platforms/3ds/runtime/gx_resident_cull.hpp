#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace dusk::native::gx::resident_cull {

inline constexpr unsigned coarseIndices = 64 * 3;
inline constexpr unsigned fineIndices = 16 * 3;
inline constexpr unsigned minFineIndices = 4 * coarseIndices;
inline constexpr unsigned minMultiIndices = coarseIndices;
inline constexpr unsigned maxRuns = 64;

using Uniform = std::array<float, 4>;

struct Box {
    float center[3], extent[3];
};

struct MatrixBox {
    Box box;
    unsigned row;
};

// Bound both shader dot products well inside the PICA float24 exponent
// range. Fail open on positions and uniforms outside this domain.
inline constexpr float inputLimit = 0x1p18f;

struct Bounds {
    float low[3]{inputLimit, inputLimit, inputLimit};
    float high[3]{-inputLimit, -inputLimit, -inputLimit};

    bool add(const float (&position)[3]) {
        for (unsigned i = 0; i < 3; ++i) {
            if (!(std::fabs(position[i]) <= inputLimit)) return false;
            low[i] = std::min(low[i], position[i]);
            high[i] = std::max(high[i], position[i]);
        }
        return true;
    }

    void include(const Bounds& other) {
        for (unsigned i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], other.low[i]);
            high[i] = std::max(high[i], other.high[i]);
        }
    }

    Box box() const {
        Box result;
        for (unsigned i = 0; i < 3; ++i) {
            result.center[i] = 0.5f * (high[i] + low[i]);
            result.extent[i] = 0.5f * (high[i] - low[i]);
        }
        return result;
    }
};

struct Plane {
    Uniform coeff, magnitude;
};
using Planes = std::array<Plane, 5>;
inline constexpr unsigned sidePlanes = 0xf;
inline constexpr unsigned allPlanes = 0x1f;

inline bool makePlanes(std::span<const Uniform> uniforms, unsigned row, unsigned mask, Planes& planes) {
    if (uniforms.size() < 10 || row > uniforms.size() - 3) return false;
    const auto matrices = uniforms.subspan(row, 3);
    const auto projection = uniforms.subspan(6, 4);
    for (const auto inputs : {matrices, projection})
        for (const auto& vector : inputs)
            for (float value : vector)
                if (!(std::fabs(value) <= inputLimit)) return false;

    // The rigid shaders compute eye = M * position, then clip = P * eye.
    // Keep the absolute terms of both products before cancellation, including
    // the subtraction of w for a side plane. Clip z is used only when unclamped.
    Plane clip[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        if (i == 2 && !(mask & 0x10)) continue;
        clip[i].coeff[3] = projection[i][3];
        clip[i].magnitude[3] = std::fabs(projection[i][3]);
        for (unsigned j = 0; j < 4; ++j)
            for (unsigned k = 0; k < 3; ++k) {
                const float value = projection[i][k] * matrices[k][j];
                clip[i].coeff[j] += value;
                clip[i].magnitude[j] += std::fabs(value);
            }
    }
    for (unsigned i = 0; i < 4; ++i) {
        const float sign = i % 2 == 0 ? 1.f : -1.f;
        for (unsigned j = 0; j < 4; ++j) {
            planes[i].coeff[j] = sign * clip[i / 2].coeff[j] - clip[3].coeff[j];
            planes[i].magnitude[j] = clip[i / 2].magnitude[j] + clip[3].magnitude[j];
        }
    }
    if (mask & 0x10) planes[4] = clip[2];
    return true;
}

struct Classification {
    unsigned outside{}, crossing{};
};

template<bool FindCrossings = false>
inline Classification classify(const Planes& planes, const Box& box, unsigned mask) {
    Classification result;
    const float x = std::fabs(box.center[0]) + box.extent[0];
    const float y = std::fabs(box.center[1]) + box.extent[1];
    const float z = std::fabs(box.center[2]) + box.extent[2];
    for (unsigned q = 0; q < planes.size(); ++q) {
        const unsigned bit = 1u << q;
        if (!(mask & bit)) continue;
        const auto& [c, m] = planes[q];
        const float center = c[0] * box.center[0] + c[1] * box.center[1] + c[2] * box.center[2] + c[3];
        const float reach = std::fabs(c[0]) * box.extent[0] + std::fabs(c[1]) * box.extent[1] + std::fabs(c[2]) * box.extent[2];
        // Allow for float24 input and dot-product rounding, box rounding and
        // clip-plane subtraction. The absolute floor covers tiny flushes
        // magnified by the bounded transforms.
        const float margin = (m[0] * x + m[1] * y + m[2] * z + m[3]) * (1.f / 1024.f) + 1.e-4f;
        if (center - reach > margin) result.outside |= bit;
        else if constexpr (FindCrossings) {
            if (!(center + reach < -margin)) result.crossing |= bit;
        }
    }
    return result;
}

inline bool outsidePalette(std::span<const Uniform> uniforms, std::span<const MatrixBox> boxes, unsigned mask) {
    if (boxes.empty()) return false;
    // A triangle can join vertices from different matrices. All rows must
    // exclude their vertices on one common plane to leave the list out.
    for (const auto& [box, row] : boxes) {
        Planes planes;
        if (!makePlanes(uniforms, row, mask, planes)) return false;
        mask = classify(planes, box, mask).outside;
        if (!mask) return false;
    }
    return true;
}

struct Run {
    std::uint32_t first, count;
};

struct Runs {
    Run ranges[maxRuns];
    unsigned size{}, indices{};

    bool add(unsigned first, unsigned end, unsigned maxGap = 0) {
        if (size != 0) {
            auto& last = ranges[size - 1];
            const unsigned previousEnd = last.first + last.count;
            if (first - previousEnd <= maxGap) {
                indices += end - previousEnd;
                last.count = end - last.first;
                return true;
            }
        }
        if (size == maxRuns) return false;
        ranges[size++] = {first, end - first};
        indices += end - first;
        return true;
    }
};

// Fine boxes have the same triangle order and boundaries as the coarse
// boxes. Leave small gaps in a run and only pay for an extra draw if it
// saves at least one coarse block's worth of submitted indices.
inline bool refine(const Planes& planes, unsigned mask, const Box& list, std::span<const Box> coarse,
                   std::span<const MatrixBox> fine, unsigned count, Runs& output) {
    if (coarse.size() != (count + coarseIndices - 1) / coarseIndices ||
        fine.size() != (count + fineIndices - 1) / fineIndices) return false;
    const auto whole = classify<true>(planes, list, mask);
    if (whole.outside) return true;
    mask = whole.crossing;
    if (!mask) return output.add(0, count);
    Runs reduced;
    bool split = true;
    for (unsigned b = 0; b < coarse.size(); ++b) {
        const unsigned first = b * coarseIndices;
        const unsigned end = std::min(first + coarseIndices, count);
        const auto visible = classify<true>(planes, coarse[b], mask);
        if (visible.outside) continue;
        if (!output.add(first, end)) return false;
        if (!split) continue;
        if (!visible.crossing) {
            split = reduced.add(first, end, 2 * fineIndices);
            continue;
        }
        for (unsigned k = first; split && k < end; k += fineIndices) {
            if (!classify(planes, fine[k / fineIndices].box, visible.crossing).outside)
                split = reduced.add(k, std::min(k + fineIndices, end), 2 * fineIndices);
        }
    }
    const unsigned extra = reduced.size > output.size ? reduced.size - output.size : 0;
    if (split && reduced.indices <= output.indices && output.indices - reduced.indices >= extra * coarseIndices) {
        std::copy_n(reduced.ranges, reduced.size, output.ranges);
        output.size = reduced.size;
        output.indices = reduced.indices;
    }
    return true;
}

}
