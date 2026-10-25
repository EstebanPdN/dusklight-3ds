#include "archive_bootstrap.hpp"
#include "bootstrap.hpp"
#include "disc.hpp"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace dusk::native {
namespace {
static_assert(sizeof(SArcHeader) == 32 && sizeof(SArcDataInfo) == 32);
static_assert(sizeof(JKRArchive::SDIDirEntry) == 16 && sizeof(JKRArchive::SDIFileEntry) == 20);
std::uint32_t be32(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
std::uint16_t be16(const unsigned char* p) { return std::uint16_t(p[0]) << 8 | p[1]; }
struct HeapLock {
    JKRHeap* heap;
    explicit HeapLock(JKRHeap* input) : heap(input) { heap->lock(); }
    ~HeapLock() { heap->unlock(); }
};
ArchiveStatus convert(ResourceStatus status) {
    switch (status) {
    case ResourceStatus::ready: return ArchiveStatus::ready;
    case ResourceStatus::notReady: return ArchiveStatus::notReady;
    case ResourceStatus::notFound: return ArchiveStatus::notFound;
    case ResourceStatus::ambiguous: return ArchiveStatus::ambiguous;
    case ResourceStatus::unsupported: return ArchiveStatus::unsupported;
    case ResourceStatus::limitExceeded: return ArchiveStatus::limitExceeded;
    default: return ArchiveStatus::malformed;
    }
}
bool canonicalName(const char* name, std::uint16_t expectedHash) {
    std::uint16_t hash = 0;
    std::size_t length = 0;
    for (; name[length]; ++length) {
        const auto c = static_cast<unsigned char>(name[length]);
        if (length == 255 || c >= 0x7f || (c >= 'A' && c <= 'Z')) return false;
        hash = static_cast<std::uint16_t>(hash * 3 + c);
    }
    return length && hash == expectedHash;
}
// RarcArchive already validated all byte extents and strings. These additional
// checks are specifically required by the original struct casts/name parser.
ArchiveStatus validateJkr(const unsigned char* memory, const RarcArchive& archive) {
    const auto header = be32(memory + 8);
    const auto* info = memory + header;
    const auto nodeOffset = header + be32(info + 4);
    const auto fileOffset = header + be32(info + 12);
    const auto dataOffset = header + be32(memory + 12);
    const auto* names = reinterpret_cast<const char*>(memory + header + be32(info + 20));
    if ((header | nodeOffset | fileOffset | dataOffset) & 3) return ArchiveStatus::unsupported;
    if (info[26] > 1 || !archive.entryCount()) return ArchiveStatus::malformed;
    const auto nodes = archive.nodeCount(), entries = archive.entryCount();
    if (entries > std::numeric_limits<u32>::max() / sizeof(void*) ||
        std::size_t(nodes) > std::numeric_limits<std::size_t>::max() / 4 - std::size_t(entries) * 2)
        return ArchiveStatus::limitExceeded;
    auto* scratch = static_cast<std::uint32_t*>(std::malloc((std::size_t(nodes) + entries * 2) * 4));
    if (!scratch) return ArchiveStatus::noMemory;
    struct Free { void* ptr; ~Free() { std::free(ptr); } } cleanup{scratch};
    auto* parents = scratch;
    auto* owners = parents + nodes;
    auto* order = owners + entries;
    std::fill(parents, parents + nodes, UINT32_MAX);
    std::fill(owners, owners + entries, UINT32_MAX);
    parents[0] = 0;
    for (std::uint32_t n = 0; n < nodes; ++n) {
        const auto* node = memory + nodeOffset + n * 16;
        if (!canonicalName(names + be32(node + 4), be16(node + 8))) return ArchiveStatus::unsupported;
        const auto count = be16(node + 10);
        const auto first = be32(node + 12);
        for (std::uint32_t j = first; j < first + count; ++j) {
            if (owners[j] != UINT32_MAX) return ArchiveStatus::malformed;
            owners[j] = n; order[j] = j;
            const auto* file = memory + fileOffset + j * 20;
            const auto flagsName = be32(file + 4);
            const auto* name = names + (flagsName & 0xffffff);
            if (!canonicalName(name, be16(file + 2))) return ArchiveStatus::unsupported;
            if (flagsName >> 24 & 0x84) return ArchiveStatus::unsupported; // inner compression
            if (flagsName >> 24 & 2) {
                const auto child = be32(file + 8);
                if (!std::strcmp(name, ".")) {
                    if (child != n) return ArchiveStatus::malformed;
                } else if (std::strcmp(name, "..")) {
                    if (!child || parents[child] != UINT32_MAX) return ArchiveStatus::malformed;
                    parents[child] = n;
                }
            }
        }
        std::sort(order + first, order + first + count, [&](auto a, auto b) {
            return std::strcmp(names + (be32(memory + fileOffset + a * 20 + 4) & 0xffffff),
                               names + (be32(memory + fileOffset + b * 20 + 4) & 0xffffff)) < 0;
        });
        for (std::uint32_t j = first + 1; j < first + count; ++j) {
            const auto a = be32(memory + fileOffset + order[j - 1] * 20 + 4) & 0xffffff;
            const auto b = be32(memory + fileOffset + order[j] * 20 + 4) & 0xffffff;
            if (!std::strcmp(names + a, names + b)) return ArchiveStatus::ambiguous;
        }
    }
    for (std::uint32_t n = 1; n < nodes; ++n) {
        auto parent = n;
        unsigned depth = 0;
        while (parent) {
            if (parent == UINT32_MAX || ++depth > 64) return ArchiveStatus::malformed;
            parent = parents[parent];
        }
    }
    for (std::uint32_t j = 0; j < entries; ++j) {
        if (owners[j] == UINT32_MAX) return ArchiveStatus::malformed;
        const auto* file = memory + fileOffset + j * 20;
        const auto flagsName = be32(file + 4);
        if ((flagsName >> 24 & 2) && !std::strcmp(names + (flagsName & 0xffffff), "..")) {
            const auto parent = be32(file + 8);
            if (owners[j] == 0 ? parent != UINT32_MAX && parent != 0 : parent != parents[owners[j]])
                return ArchiveStatus::malformed;
        }
    }
    return ArchiveStatus::ready;
}
bool validQuery(const char* name) {
    if (!name || !*name) return false;
    std::size_t length = 0;
    for (; name[length]; ++length) {
        const auto c = static_cast<unsigned char>(name[length]);
        if (length == 255 || c < 0x20 || c >= 0x7f || c == '/' || c == '\\') return false;
    }
    return std::strcmp(name, ".") && std::strcmp(name, "..");
}
}

const char* archiveStatusText(ArchiveStatus status) {
    switch (status) {
    case ArchiveStatus::ready: return "ready";
    case ArchiveStatus::notReady: return "not-ready";
    case ArchiveStatus::notFound: return "not-found";
    case ArchiveStatus::ambiguous: return "ambiguous";
    case ArchiveStatus::malformed: return "malformed";
    case ArchiveStatus::unsupported: return "unsupported";
    case ArchiveStatus::limitExceeded: return "limit-exceeded";
    case ArchiveStatus::noMemory: return "no-memory";
    case ArchiveStatus::ioError: return "io-error";
    case ArchiveStatus::wrongThread: return "wrong-thread";
    case ArchiveStatus::overlaysUnsupported: return "overlays-unsupported";
    }
    return "unknown";
}
void ArchiveBootstrap::releaseBytes() {
    validated = {}; std::free(allocation); allocation = nullptr; bytes = nullptr; byteCount = 0;
}
ArchiveBootstrap::~ArchiveBootstrap() {
    if (close() != ArchiveStatus::ready) fatal("Archive destroyed outside its loading-worker lifetime\n");
}
ArchiveStatus ArchiveBootstrap::close() {
    if (owner && owner != OSGetCurrentThread()) return ArchiveStatus::wrongThread;
    if (mounted) {
        if (systemHeap != JKRHeap::getSystemHeap()) return ArchiveStatus::unsupported;
        HeapLock held(systemHeap);
        mounted->~JKRMemArchive(); std::free(mounted); mounted = nullptr;
    }
    releaseBytes(); systemHeap = nullptr; owner = nullptr;
    return ArchiveStatus::ready;
}
ArchiveStatus ArchiveBootstrap::mountOwned(const ArchiveLimits& limits) {
    auto status = convert(validated.open(bytes, byteCount, limits.rarc));
    if (status != ArchiveStatus::ready) return status;
    status = validateJkr(bytes, validated);
    if (status != ArchiveStatus::ready) return status;
    systemHeap = JKRHeap::getSystemHeap();
    if (!systemHeap) return ArchiveStatus::notReady;
    auto* storage = std::malloc(sizeof(JKRMemArchive));
    if (!storage) return ArchiveStatus::noMemory;
    HeapLock held(systemHeap);
    mounted = new (storage) JKRMemArchive(bytes, static_cast<u32>(byteCount), JKRMEMBREAK_FLAG_UNKNOWN0);
    // The native memory-only adapter explicitly refuses overlay policy. A
    // buffer address must never be interpreted as a disc FST entry number.
    mounted->mEntryNum = -1;
    if (!mounted->isMounted()) {
        mounted->~JKRMemArchive(); std::free(mounted); mounted = nullptr;
        return ArchiveStatus::noMemory;
    }
    return ArchiveStatus::ready;
}
ArchiveStatus ArchiveBootstrap::openCopy(ResourceSpan input, const ArchiveLimits& limits,
                                       ArchiveOverlayPolicy overlays) {
    if (owner && owner != OSGetCurrentThread()) return ArchiveStatus::wrongThread;
    if (mounted) return ArchiveStatus::notReady; // explicit close prevents invalidating published spans
    if (overlays != ArchiveOverlayPolicy::disabled) return ArchiveStatus::overlaysUnsupported;
    owner = OSGetCurrentThread();
    if (!input.data || input.size < 4) return ArchiveStatus::malformed;
    if (input.size > limits.maxInputBytes) return ArchiveStatus::limitExceeded;
    const bool encoded = !std::memcmp(input.data, "Yaz0", 4);
    std::uint32_t expanded = 0;
    if (encoded) {
        const auto status = convert(yaz0ExpandedSize(input.data, input.size, limits.rarc.maxArchiveBytes, expanded));
        if (status != ArchiveStatus::ready) return status;
    }
    const auto size = encoded ? std::size_t(expanded) : input.size;
    if (size > limits.rarc.maxArchiveBytes || size > UINT32_MAX ||
        size > std::numeric_limits<std::size_t>::max() - 31) return ArchiveStatus::limitExceeded;
    allocation = std::malloc(size + 31);
    if (!allocation) return ArchiveStatus::noMemory;
    bytes = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(allocation) + 31) & ~std::uintptr_t(31));
    byteCount = size;
    ArchiveStatus status;
    if (encoded) {
        std::size_t written = 0;
        status = convert(decodeYaz0(input.data, input.size, bytes, size, limits.rarc.maxArchiveBytes, written));
        if (status == ArchiveStatus::ready && written != size) status = ArchiveStatus::malformed;
    } else {
        std::memcpy(bytes, input.data, size); status = ArchiveStatus::ready;
    }
    if (status == ArchiveStatus::ready) status = mountOwned(limits);
    if (status != ArchiveStatus::ready) releaseBytes();
    return status;
}
ArchiveStatus ArchiveBootstrap::load(Disc& disc, const char* path, const ArchiveLimits& limits,
                                   ArchiveOverlayPolicy overlays) {
    if (owner && owner != OSGetCurrentThread()) return ArchiveStatus::wrongThread;
    if (mounted) return ArchiveStatus::notReady;
    if (overlays != ArchiveOverlayPolicy::disabled) return ArchiveStatus::overlaysUnsupported;
    DiscFile file;
    const auto found = disc.findFile(path, file);
    if (found != DiscFileStatus::ready) return found == DiscFileStatus::notFound ? ArchiveStatus::notFound : ArchiveStatus::ioError;
    if (file.size() > limits.maxInputBytes) return ArchiveStatus::limitExceeded;
    auto* input = static_cast<unsigned char*>(std::malloc(file.size() ? file.size() : 1));
    if (!input) return ArchiveStatus::noMemory;
    const auto status = disc.readFile(file, 0, input, file.size())
        ? openCopy({input, file.size()}, limits, overlays) : ArchiveStatus::ioError;
    std::free(input);
    return status;
}
JKRMemArchive* ArchiveBootstrap::archiveForLoading() const {
    return owner == OSGetCurrentThread() && systemHeap == JKRHeap::getSystemHeap() ? mounted : nullptr;
}
ArchiveStatus ArchiveBootstrap::findBasename(const char* name, ResourceSpan& output) {
    output = {};
    if (owner && owner != OSGetCurrentThread()) return ArchiveStatus::wrongThread;
    if (!mounted) return ArchiveStatus::notReady;
    if (!validQuery(name)) return ArchiveStatus::unsupported;
    if (systemHeap != JKRHeap::getSystemHeap()) return ArchiveStatus::unsupported;
    RarcEntry entry;
    const auto status = convert(validated.findFile(name, entry));
    if (status != ArchiveStatus::ready) return status;
    HeapLock held(systemHeap);
    auto* data = mounted->getResource(u32(0), name);
    if (data != entry.contents.data || mounted->getResSize(data) != entry.contents.size)
        return ArchiveStatus::malformed;
    output = entry.contents;
    return ArchiveStatus::ready;
}
}
