#pragma once
#include "gx_state.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
namespace dusk::native::gx { struct CommandRegisters; }
namespace dusk::native {
// Retain the actual initialized original capture allocation before J3D builds
// its display lists. Subsequent real EFB copies refresh this same image/token.
// This does not perform a capture or imply that a frame has been rendered.
bool registerCopyImage(std::span<const std::uint8_t> bti, const void* identity);
// Explicit owner-proven CI8 destination and live palette extents. The owner
// must destroy the copy before freeing either original allocation.
bool registerIndexedCopyImage(std::span<const std::uint8_t> header,const void* identity,
                              std::size_t imageBytes,std::span<const std::uint8_t> palette);
bool refreshCopyImagePalette(const void* identity);
// The same with the GPU owner checked first (the sync); refreshCopyImagePalette
// takes it only for an indexed image. SD switch gx-palette-fast-off.txt: always.
[[gnu::noinline]] bool refreshCopyImagePaletteOwned(const void* identity);
void setCopyPaletteFastPath(bool on);
std::size_t copyImageBytes();
// Game-side access to a retained R8 map layer drains the draw queue first.
bool copyAreaMapBase(unsigned side, gx::TextureHandle previous, std::span<std::uint8_t> destination);
bool bindAreaMapBase(const gx::Texture& texture);
// The map destroys its copy target (and completes GPU work) before releasing these layers.
bool releaseAreaMapBase(gx::TextureHandle handle);
// GXCopyTex as a queued job (gx_worker_sync.hpp, gxJobCopyTex): run by the
// thread that draws, under the register bank the copy was issued with.
void runGxCopyTexJob(const void* arguments,std::size_t bytes,const gx::CommandRegisters& bank);
// Copies that waited in the queue instead of having it drawn first.
std::uint64_t queuedCopyTextures();
}
