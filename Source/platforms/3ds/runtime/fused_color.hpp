#pragma once
#include "gx_copy_pixels.hpp"
#include "texture_gx.hpp"

namespace dusk::native::private_fused {
// Pure color-only experiment. Caller owns all buffers and GPU synchronization.
// Complete BTI header/extra bytes are preserved; only its image is written.
// Rejects unsupported/stale plans and pairwise overlap before either write.
// No registration, cache publication, flush, allocation, retained pointer or
// runtime flag. Depth and indexed palette copies deliberately stay on old path.
bool encode(const ColorCopyPlan&, std::span<const std::byte> readback,
            std::span<std::uint8_t> bti, const NativeTexturePlan&,
            std::span<std::uint8_t> native);
}
