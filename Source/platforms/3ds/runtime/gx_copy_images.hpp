#pragma once
#include <cstdint>
#include <span>
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
// CPU metadata only, for the render owner: verify the exact live color-copy
// owner, alias, registration and mutable single-mip RGBA8 image.
bool registeredFramebufferCapture(const void* identity,std::uint64_t handle);
// Render owner, at submission, outside a protected/reentrant GX draw. Resolve
// and refresh a live single-mip RGBA8 color copy. The C3D_Tex* is borrowed from
// the GX cache and expires on eviction, copy destruction or cache teardown.
// GX coordinates (s,t,q) sample as (s*width/texWidth, q-t*height/texHeight, q).
// Direct foreign bindings must invalidate the GX texture-binding cache.
bool acquireFramebufferCapture(const void* identity,void*& texture,unsigned& logicalWidth,unsigned& logicalHeight);
std::size_t copyImageBytes();
}
