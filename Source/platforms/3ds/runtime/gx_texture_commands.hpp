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
inline void (*watchedCaptureMaterialize)(){};
}
