#pragma once
#include "disc.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <dolphin/dvd.h>

struct DVDFileInfo;
struct DVDCommandBlock;
struct DVDDiskID;
namespace dusk::native {
enum class DvdStatus { ready, notBound, invalidArgument, notFound, exhausted, ioError, unsupported };
// Exactly one loading worker may bind a provider at a time. No renderer/audio
// nested bindings or arbitrary FST-to-path reconstruction are implemented.
// DVDReadAsyncPrio completes synchronously on this loading worker, then invokes
// its callback; no background I/O or renderer/audio-thread admission is implied.
// A successful binding exclusively owns calls to its Disc.
// Keep each DVDFileInfo alive and unmoved until DVDClose or scope destruction.
class DvdProvider {
    struct Slot {
        DVDFileInfo* info = nullptr;
        DiscFile file;
        std::array<char, 1025> path{};
    };
    struct DirectorySlot {
        DVDDir* info = nullptr;
        DiscDirectory directory;
        DiscDirectoryEntry entry;
    };
    Disc& disc;
    void* owner = nullptr;
    // Logo startup retains more than 16 ARAM/DVD archives concurrently; each
    // original archive owns a live DVDFileInfo after its metadata loads.
    std::array<Slot, 64> slots{};
    // Original file-cache recursion may retain a handle at every FST depth.
    std::array<DirectorySlot, 64> directories{};
    DiscDirectory currentDirectory;
    std::array<char, 1024> currentPath{};
    DvdStatus result = DvdStatus::notBound;
    bool absolutePath(const char* path, std::array<char, 1024>& output, bool directory);
    bool directoryMatches(const DirectorySlot& slot, const DVDDir* info) const;
    bool found(DiscFileStatus status);
public:
    static constexpr unsigned fileCapacity=64;
    explicit DvdProvider(Disc& input);
    ~DvdProvider();
    DvdProvider(const DvdProvider&) = delete;
    DvdProvider& operator=(const DvdProvider&) = delete;
    bool bound() const;
    // Process-owned service mode serializes original DVD/JAS/ARAM worker calls.
    // Must be selected once by the startup owner before spawning game workers.
    bool shareWithGameWorkers();
    DvdStatus lastStatus() const { return result; }
    bool open(const char* path, DVDFileInfo* info);
    bool openEntry(std::int32_t entry,DVDFileInfo* info);
    bool close(DVDFileInfo* info);
    std::int32_t read(DVDFileInfo* info, void* output, std::int32_t length, std::int32_t offset);
    // Resolve an ISO FST index without allocating an open-file slot.
    std::int32_t pathEntry(const char* path);
    std::int32_t fileStatus(const DVDFileInfo* info) const;
    std::int32_t commandStatus(const DVDCommandBlock* block) const;
    bool diskID(DVDDiskID* output);
    bool changeDirectory(const char* path);
    bool currentDirectoryPath(char* output, std::uint32_t capacity);
    bool openDirectory(const char* path, DVDDir* info);
    bool openDirectoryEntry(std::int32_t entry, DVDDir* info);
    bool readDirectory(DVDDir* info, DVDDirEntry* output);
    bool closeDirectory(DVDDir* info);
    bool rewindDirectory(DVDDir* info);
    // Only paths registered by successful DVDOpen in this scope are known.
    // Unknown, expired or too-small requests return false without inventing data.
    bool entryPath(std::int32_t entry, char* output, std::uint32_t capacity);
};
bool initializeOriginalDisc();
}
