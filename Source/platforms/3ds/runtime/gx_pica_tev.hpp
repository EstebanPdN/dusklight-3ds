#pragma once
#include "gx_state.hpp"
#include <array>

namespace dusk::native::gx {
// Portable PICA subset. Map these to GPU_PRIMARY_COLOR/TEXTURE0/PREVIOUS/CONSTANT
// and GPU_REPLACE/MODULATE; use SRC_COLOR RGB operands and SRC_ALPHA operands.
enum class PicaTevSource : std::uint8_t { primary, texture0, previous, constant };
enum class PicaTevFunction : std::uint8_t { replace, modulate };
struct PicaTevChannel {
    PicaTevFunction function{};
    std::array<PicaTevSource, 3> sources{};
};
struct PicaTevStage {
    PicaTevChannel rgb{}, alpha{};
    Rgba constant{}; // One shared RGBA constant for both channels of this stage.
};
struct PicaTevPlan {
    std::array<PicaTevStage, 6> stages{};
    std::uint8_t count = 0;
};
enum class PicaTevError : std::uint8_t {
    none, stageCount, operation, order, inputs, nonzeroBlack
};
const char* picaTevErrorText(PicaTevError error);

// Exact algebraic subset of original J2DPicture, not a general GX translator.
// Untextured PASSCLR, or texture RGB + texture/REG2 alpha, optionally followed
// by raster multiplication or black=RGBA(0) tint multiplication with REG1.
// No PREVIOUS_BUFFER dependency. Nonzero black is explicitly unsupported.
// Resets output on failure; no allocation, GPU calls, texture reads or retained
// packet pointers. Only TEV state is checked, not sampler/geometry/other state.
bool planPicaTev(const DrawPacket& packet, PicaTevPlan& output,
                 PicaTevError* error = nullptr);

// Backend contract: reset all six Citro3D environments and disable buffer updates
// before applying stages[0..count). Use scale1 and default source operands; leave
// later stages as PREVIOUS replace. These equations match in real arithmetic;
// host tests do not establish GX/PICA fixed-point rounding equivalence.
}
