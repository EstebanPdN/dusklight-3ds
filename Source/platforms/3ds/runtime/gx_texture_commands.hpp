#pragma once
#include <cstdint>
#include "gx_state.hpp"
namespace dusk::native::gx {
struct CommandRegisters;
// Resolves registered native tokens and exact image dimensions/format, with
// supported single-level sampler state. Returns a static diagnostic on failure.
bool resolveCommandTexture(const CommandRegisters&,unsigned map,Texture& out,const char*& failure);
// Draws of queued shapes (gx_worker_sync.hpp) that named a texture map the
// register bank holds nothing for. A draw issued right away takes the map's
// last GXLoadTexObj binding instead; a queued one cannot (that binding has
// moved on) and is refused. Expected to stay zero: GXInit loads every map.
inline std::uint32_t queuedTextureBindingRefusals{};
// Native PICA engine: per-frame screen captures marked optional by the game
// are skipped while no draw has sampled them recently (see GXNativeOptionalCopy).
inline std::uint32_t watchedCaptureToken{},watchedCaptureLastUse{},textureFrameCounter{};
// The captures of one frame are numbered in call order. Each has its own
// consumers: the draws between it and the next capture (the first one feeds
// the refracting water, the second the screen effects drawn after it).
inline std::uint32_t watchedCaptureSlotUse[4]{};
inline unsigned watchedCaptureSlot{};
// A draw of this frame sampled the current capture (the frame copy was made).
// Read by the thread that draws, or by the game thread after a sync.
inline bool watchedCaptureUsedThisFrame(){
    return watchedCaptureToken!=0&&textureFrameCounter!=0&&watchedCaptureSlotUse[watchedCaptureSlot&3u]==textureFrameCounter;
}
inline void (*watchedCaptureMaterialize)(){};
}
