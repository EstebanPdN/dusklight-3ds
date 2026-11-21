#pragma once
// worldMips (perf_features.hpp): how one texture unit of a draw is used, as
// the drawing code passes it to bindGxTexture. Which draws are world uses is
// decided from their registers in gx_world_mips.hpp.
#include <cstdint>

namespace dusk::native {
struct GxWorldMipUse {
    bool world{};             // plain world-model use: generated levels allowed
    bool thresholdKnown{};    // the alpha test passes exactly when alpha >= reference
    std::uint8_t reference{}; // 0: no test (passes always)
};
}
