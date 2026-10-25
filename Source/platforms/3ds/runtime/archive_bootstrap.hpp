#pragma once
#include "resource_archive.hpp"
#include <cstddef>

class JKRMemArchive;
class JKRHeap;

namespace dusk::native {
class Disc;
enum class ArchiveStatus { ready, notReady, notFound, ambiguous, malformed, unsupported,
                           limitExceeded, noMemory, ioError, wrongThread, overlaysUnsupported };
const char* archiveStatusText(ArchiveStatus status);
enum class ArchiveOverlayPolicy { disabled, requested };
struct ArchiveLimits {
    RarcLimits rarc;
    std::size_t maxInputBytes;
};

// All methods and destruction belong to one loading worker. The caller owns the
// Disc exclusively during load(). Construction/destruction also serialize the
// original global JKR volume list with the system heap's recursive mutex.
// Immutable returned spans can be published to a renderer; close/destruction is
// allowed only after consumers have released them. Never unmount/delete or call
// open() on the borrowed original archive, or change the system heap while live.
class ArchiveBootstrap {
    void* owner = nullptr;
    void* allocation = nullptr;
    unsigned char* bytes = nullptr;
    std::size_t byteCount = 0;
    JKRMemArchive* mounted = nullptr;
    JKRHeap* systemHeap = nullptr;
    RarcArchive validated;
    ArchiveStatus mountOwned(const ArchiveLimits& limits);
    void releaseBytes();
public:
    ArchiveBootstrap() = default;
    ArchiveBootstrap(const ArchiveBootstrap&) = delete;
    ArchiveBootstrap& operator=(const ArchiveBootstrap&) = delete;
    ~ArchiveBootstrap();
    ArchiveStatus openCopy(ResourceSpan input, const ArchiveLimits& limits,
                           ArchiveOverlayPolicy overlays = ArchiveOverlayPolicy::disabled);
    ArchiveStatus load(Disc& disc, const char* path, const ArchiveLimits& limits,
                       ArchiveOverlayPolicy overlays = ArchiveOverlayPolicy::disabled);
    ArchiveStatus close();
    ArchiveStatus findBasename(const char* name, ResourceSpan& output);
    // For original J2D integration on the loading worker, with already validated
    // resource names (ASCII, components <=255; no '.'/'..'). Mutating resources,
    // overlay activation, ownership transfer and arbitrary paths are unsupported.
    JKRMemArchive* archiveForLoading() const;
    std::size_t archiveBytes() const { return byteCount; }
};
}
