#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace dusk::native {
inline constexpr char gameImagePath[] = "sdmc:/3ds/DuskLight/zelda.twilight";
enum class DiscStatus { ready, missing, ioError, unsupported, malformed, noMemory };
const char* discStatusText(DiscStatus status);
enum class DiscFileStatus { ready, notFound, notReady, invalidPath, ioError, malformed };
const char* discFileStatusText(DiscFileStatus status);
class Disc;
class DiscFile {
    friend class Disc;
    const Disc* owner = nullptr;
    std::uint64_t generation = 0;
    std::uint32_t offset = 0, bytes = 0, index = 0;
public:
    std::uint32_t size() const { return bytes; }
    std::uint32_t entryIndex() const { return index; }
    std::uint32_t discOffset() const { return offset; }
};
class DiscDirectory {
    friend class Disc;
    const Disc* owner = nullptr;
    std::uint64_t generation = 0;
    std::uint32_t index = 0, cursor = 0, end = 0;
public:
    std::uint32_t entryIndex() const { return index; }
    std::uint32_t location() const { return cursor; }
    std::uint32_t next() const { return end; }
};
struct DiscDirectoryEntry {
    std::uint32_t index = 0;
    bool directory = false;
    std::array<char, 256> name{};
};
// No resource payloads or arbitrary paths are logged. Supports GCN USA/PAL r0.
// Calls are for a loading/I/O worker, never a frame-critical render/audio callback.
class Disc {
    friend class DvdProvider;
    std::FILE* file = nullptr;
    std::uint64_t length = 0, physicalLength = 0;
    struct BundleRange { std::uint64_t logical, physical, bytes; };
    static_assert(sizeof(BundleRange) == 24);
    std::unique_ptr<BundleRange[]> bundleRanges;
    std::uint32_t bundleCount = 0;
    bool mapped(std::uint64_t offset, std::uint64_t size) const;
    bool readPhysical(std::uint64_t offset, void* output, std::size_t size);
    DiscStatus loadBundle();
    std::uint64_t generation = 0;
    std::uint32_t fstOffset = 0, fstBytes = 0, entryCount = 0;
    std::unique_ptr<unsigned char[]> fstCache;
    std::uint64_t sourceReadOperations = 0;
    bool validated = false;
    DiscStatus status = DiscStatus::missing;
    // One exclusively bound DVD owner publishes state changes; other threads
    // consume its atomic snapshot and never dereference this Disc or its FILE.
    void (*stateObserver)(DiscStatus) = nullptr;
    DiscStatus recordStatus(DiscStatus);
    void observeState(void (*observer)(DiscStatus));
    bool readBytes(std::uint64_t offset, void* output, std::size_t size);
    bool readFst(std::uint64_t offset, void* output, std::size_t size) const;
    DiscStatus validate();
    struct Entry {
        bool directory = false;
        std::uint32_t value = 0, extent = 0;
        std::array<unsigned char, 256> name{};
    };
    DiscFileStatus readEntry(std::uint32_t index, Entry& output);
    DiscFileStatus findEntry(const char* path, std::uint32_t& index);
public:
    Disc() = default;
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;
    ~Disc();
    DiscStatus open(const char* path);
    DiscStatus currentStatus() const { return status; } // Exclusive loading owner only.
    // Raw reads are admitted only after successful whole-FST validation.
    bool read(std::uint64_t offset, void* output, std::size_t size);
    // Loading/I/O-worker calls only, with exclusive ownership of this Disc.
    // ASCII case-insensitive, matching Aurora/DVD. An initial '/' is optional;
    // empty components, '.', '..', backslashes and paths >1023 bytes are refused.
    // Resolve once while loading, then retain the bounded handle for reads.
    DiscFileStatus findFile(const char* path, DiscFile& output);
    DiscFileStatus openFile(std::uint32_t entryIndex, DiscFile& output);
    // Directory iteration returns immediate children and skips their subtrees.
    // Each handle belongs to one Disc generation; notFound means end of directory.
    DiscFileStatus findDirectory(const char* path, DiscDirectory& output);
    DiscFileStatus openDirectory(std::uint32_t entryIndex, DiscDirectory& output);
    DiscFileStatus readDirectory(DiscDirectory& input, DiscDirectoryEntry& output);
    bool directoryValid(const DiscDirectory& input) const;
    bool rewindDirectory(DiscDirectory& input);
    // No FST scanning/allocation. Handles are invalid after any open() attempt
    // and may only be used with the same live Disc that created them.
    bool readFile(const DiscFile& input, std::uint64_t offset, void* output, std::size_t size);
    std::uint64_t size() const { return length; }
    // Loading-owner telemetry, not thread-safe counters or frame-time evidence.
    std::size_t fstCacheSize() const { return validated ? fstBytes : 0; }
    std::uint64_t sourceReads() const { return sourceReadOperations; }
    // Exact requested persistent index bytes, excludes allocator/FILE overhead.
    std::size_t bundleIndexBytes() const { return std::size_t(bundleCount) * sizeof(BundleRange); }
    bool isBundle() const { return bundleCount != 0; }
};
}
