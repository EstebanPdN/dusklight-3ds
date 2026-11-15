#pragma once
#include <cstdint>
#include "gx_state.hpp"
namespace dusk::native::gx {
struct CommandRegisters;
// Resolves registered native tokens and exact image dimensions/format, with
// supported single-level sampler state. Returns a static diagnostic on failure.
bool resolveCommandTexture(const CommandRegisters&,unsigned map,Texture& out,const char*& failure);
// Native PICA engine: per-frame screen captures marked optional by the game
// are skipped while no draw has sampled them recently (see GXNativeOptionalCopy).
inline std::uint32_t watchedCaptureToken{},watchedCaptureLastUse{},textureFrameCounter{};
// The captures of one frame are numbered in call order. Each has its own
// consumers: the draws between it and the next capture (the first one feeds
// the refracting water, the second the screen effects drawn after it).
inline std::uint32_t watchedCaptureSlotUse[4]{};
inline unsigned watchedCaptureSlot{};
inline void (*watchedCaptureMaterialize)(){};
}
