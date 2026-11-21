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
// Retains both original CI8 indices and the bounded GX palette, with aliases
// for the pointers passed to GXInitTexObjCI and GXInitTlutObj.
bool retainRawCi8TextureResource(const void* owner,std::span<const std::uint8_t> image,unsigned width,unsigned height,
                                std::span<const std::uint8_t> palette,unsigned paletteFormat);
bool retainArchiveTexture(const void* owner,const void* bti,std::uint64_t externalHandle=0);
// Original weather uses short-lived GX objects over archive-owned BTIs. Keep
// the retained image until that actual archive unregisters, including cache hits.
bool retainArchiveTextureForLifetime(const void* bti,std::uint64_t externalHandle=0);
// May be requested by a game worker. Actual GPU retirement stays on render.
void releaseTextureResources(const void* owner);
// A heap destroyed wholesale runs no destructors: a JUTTexture inside it never
// releases its records, and the next scene's texture loaded at the same image
// address then conflicts with the dead one. Before such a heap is freed,
// release every record (and every archive) whose owner lies in [begin, end).
// Same thread rules as releaseTextureResources. Returns the records released.
std::size_t releaseTextureResourcesInRange(const void* begin,const void* end);
bool collectTextureResources();
// Alpha 15 (gx-overlap.txt 1 and 2): an owner that already holds an image is
// answered without asking for the render owner (no sync with the worker).
void setTextureRetainWithoutOwner(bool on);
std::size_t textureResourceBytes();
const char* textureResourceFailure();
// Cumulative counters for the profiled report.
struct TextureResourceStatistics{
    unsigned long conflicts;       // retains refused: image address reused while retained
    unsigned long rangeReleases;   // releaseTextureResourcesInRange calls
    unsigned long rangeOwners;     // records those calls released
    unsigned images,owners;        // live image slots / owner records
    std::size_t bytes;
    unsigned long memoHits;        // repeated retains answered without a scan
};
TextureResourceStatistics textureResourceStatistics();
}
