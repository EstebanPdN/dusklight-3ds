#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace dusk::native {
// Loading workers retain validated archive bounds. Removal precedes freeing
// the archive and shares exclusion with the render owner's copy operation.
bool registerTextureArchive(const void* owner,std::span<const std::uint8_t> bytes);
void unregisterTextureArchive(const void* owner);
// Explicit static BTIs and live archive BTIs are copied into bounded storage.
// The original image pointer is only an identity, never an unbounded read.
bool retainTextureResource(const void* owner,std::span<const std::uint8_t> bti,const void* imageIdentity=nullptr);
// Bare original embedded GX images have explicit caller-proven array extents.
// Construct metadata from their real GX layout; retain the actual pixels.
bool retainRawTextureResource(const void* owner,std::span<const std::uint8_t> image,unsigned width,unsigned height,unsigned format);
bool retainArchiveTexture(const void* owner,const void* bti,std::uint64_t externalHandle=0);
// Original weather uses short-lived GX objects over archive-owned BTIs. Keep
// the retained image until that actual archive unregisters, including cache hits.
bool retainArchiveTextureForLifetime(const void* bti,std::uint64_t externalHandle=0);
// May be requested by a game worker. Actual GPU retirement stays on render.
void releaseTextureResources(const void* owner);
bool collectTextureResources();
std::size_t textureResourceBytes();
const char* textureResourceFailure();
}
