#include "disc.hpp"
#include "tour_load_cost.hpp"
#if defined(__3DS__)
#include <3ds.h>
#include <cstdio>
extern "C" void OSReport(const char*,...);
#endif
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <sys/stat.h>

namespace dusk::native {
namespace {
std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint64_t be64(const unsigned char* p) {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}
std::uint32_t crc32(std::uint32_t crc, const unsigned char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
bool contains(std::uint64_t limit, std::uint64_t offset, std::uint64_t size) {
    return offset <= limit && size <= limit - offset;
}
unsigned char fold(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
}
Disc::~Disc() { if (file) std::fclose(file); }
DiscStatus Disc::recordStatus(DiscStatus value) {
    status=value;
    if(stateObserver)stateObserver(value);
    return value;
}
void Disc::observeState(void (*observer)(DiscStatus)) {
    stateObserver=observer;
    if(observer)observer(status);
}
bool Disc::read(std::uint64_t offset, void* output, std::size_t size) {
    return validated && readBytes(offset, output, size);
}
bool Disc::mapped(std::uint64_t offset, std::uint64_t size) const {
    if (!contains(length, offset, size)) return false;
    if (!size || !bundleCount) return true;
    // Find the last range whose logical start does not exceed offset.
    std::uint32_t first = 0, end = bundleCount;
    while (first < end) {
        const auto mid = first + (end - first) / 2;
        if (bundleRanges[mid].logical <= offset) first = mid + 1;
        else end = mid;
    }
    if (!first) return false;
    for (auto i = first - 1; i < bundleCount && size; ++i) {
        const auto& r = bundleRanges[i];
        if (offset < r.logical || offset >= r.logical + r.bytes) return false;
        const auto available = r.logical + r.bytes - offset;
        const auto n = size < available ? size : available;
        size -= n; offset += n;
    }
    return !size;
}
#if defined(__3DS__)
std::uint64_t tourGameLoadStart(){
    return !threadGetCurrent() && measureTourLoads ? svcGetSystemTick() : 0;
}
void finishTourDvdLock(std::uint64_t start,const char* operation){
    if(!start)return;
    const auto elapsed=svcGetSystemTick()-start;
    tourLoadCost.dvdLockTicks+=elapsed;
    if(elapsed>SYSCLOCK_ARM11/20)
        OSReport("Native DVD service wait %s %lu ms\n",operation,(unsigned long)(elapsed*1000/SYSCLOCK_ARM11));
}
namespace {
// Disc reads by thread kind: the game thread (libctru's main thread has no
// Thread object) against the loader threads. A read on the game thread is a
// picture held for its whole length (Alpha 11 measurement).
struct DiscReadCounter{std::atomic<std::uint64_t> reads{},bytes{},ticks{},longest{};};
DiscReadCounter gameReads,loaderReads;
void noteDiscRead(std::uint64_t ticks,std::size_t bytes){
    auto& counter=threadGetCurrent()?loaderReads:gameReads;
    if(&counter==&gameReads && measureTourLoads)tourLoadCost.gameReadTicks+=ticks;
    counter.reads.fetch_add(1,std::memory_order_relaxed);counter.bytes.fetch_add(bytes,std::memory_order_relaxed);
    counter.ticks.fetch_add(ticks,std::memory_order_relaxed);
    for(auto seen=counter.longest.load(std::memory_order_relaxed);ticks>seen&&!counter.longest.compare_exchange_weak(seen,ticks););
}
}
void reportDiscReads(unsigned pictures){
    const auto take=[](DiscReadCounter& c){
        return std::array<std::uint64_t,4>{c.reads.exchange(0),c.bytes.exchange(0),c.ticks.exchange(0),c.longest.exchange(0)};
    };
    const auto game=take(gameReads),loader=take(loaderReads);
    if(!game[0]&&!loader[0])return;
    const auto ms=[](std::uint64_t t){return t*1000.0/SYSCLOCK_ARM11;};
    OSReport("Native disc reads, last %u pictures: game thread %llu (%llu KiB, %.1f ms, longest %.1f) loaders %llu (%llu KiB, %.1f ms, longest %.1f)\n",pictures,
        static_cast<unsigned long long>(game[0]),static_cast<unsigned long long>(game[1]/1024),ms(game[2]),ms(game[3]),
        static_cast<unsigned long long>(loader[0]),static_cast<unsigned long long>(loader[1]/1024),ms(loader[2]),ms(loader[3]));
}
#endif
bool Disc::readPhysical(std::uint64_t offset, void* output, std::size_t size) {
    if (!file || (!output && size) || !contains(physicalLength, offset, size) ||
        offset > std::uint64_t(std::numeric_limits<off_t>::max())) return false;
    if (!size) return true;
    ++sourceReadOperations;
#if defined(__3DS__)
    // Hardware safety: never hand the FS service a destination that is not
    // mapped writable memory (the system FS module faults instead of failing).
    {
        const auto begin=reinterpret_cast<std::uintptr_t>(output),end=begin+size;
        for(std::uintptr_t at=begin;at<end;){
            MemInfo info{};PageInfo page{};
            if(R_FAILED(svcQueryMemory(&info,&page,at))||!(info.perm&MEMPERM_WRITE)||info.state==MEMSTATE_FREE){
                char text[160];std::snprintf(text,sizeof(text),"Native disc read refused: offset=%llx size=%lx buffer=%08lx bad page=%08lx state=%lx perm=%lx\n",
                    static_cast<unsigned long long>(offset),static_cast<unsigned long>(size),static_cast<unsigned long>(begin),
                    static_cast<unsigned long>(at),static_cast<unsigned long>(info.state),static_cast<unsigned long>(info.perm));
                svcOutputDebugString(text,std::strlen(text));
                recordStatus(DiscStatus::ioError);return false;
            }
            at=info.base_addr+info.size;
        }
    }
#endif
#if defined(__3DS__)
    const u64 readStart=svcGetSystemTick();
    struct ReadTimer{u64 start;std::size_t bytes;~ReadTimer(){noteDiscRead(svcGetSystemTick()-start,bytes);}} readTimer{readStart,size};
#endif
    if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {recordStatus(DiscStatus::ioError); return false;}
    // Bounded FS requests: 256 KiB per read.
    auto* dst=static_cast<unsigned char*>(output);
    for(std::size_t done=0;done<size;){
        const std::size_t n=size-done<262144?size-done:262144;
        if(std::fread(dst+done,1,n,file)!=n){recordStatus(DiscStatus::ioError);return false;}
        done+=n;
    }
    return true;
}
bool Disc::readBytes(std::uint64_t offset, void* output, std::size_t size) {
    // Preflight the whole request: an unmapped tail must not partially write output.
    if ((!output && size) || !mapped(offset, size)) return false;
    if (!size) return true;
    if (!bundleCount) return readPhysical(offset, output, size);
    std::uint32_t first = 0, end = bundleCount;
    while (first < end) {
        const auto mid = first + (end - first) / 2;
        if (bundleRanges[mid].logical <= offset) first = mid + 1;
        else end = mid;
    }
    auto* dst = static_cast<unsigned char*>(output);
    for (auto i = first - 1; size; ++i) {
        const auto& r = bundleRanges[i];
        const auto available = r.logical + r.bytes - offset;
        const auto n = size < available ? size : static_cast<std::size_t>(available);
        if (!readPhysical(r.physical + offset - r.logical, dst, n)) return false;
        offset += n; dst += n; size -= n;
    }
    return true;
}
DiscStatus Disc::loadBundle() {
    std::array<unsigned char, 128> header{};
    if (!contains(physicalLength, 0, header.size())) return DiscStatus::malformed;
    if (!readPhysical(0, header.data(), header.size())) return DiscStatus::ioError;
    if (std::memcmp(header.data(), "DLTWPK01", 8) || be32(header.data() + 8) != 1 ||
        be32(header.data() + 12) != 128 || be32(header.data() + 36) != 64)
        return DiscStatus::unsupported;
    for (unsigned i = 96; i < 128; ++i) if (header[i]) return DiscStatus::unsupported;
    const auto check = be32(header.data() + 60);
    std::memset(header.data() + 60, 0, 4);
    if ((crc32(~0u, header.data(), header.size()) ^ ~0u) != check) return DiscStatus::malformed;
    const auto logical = be64(header.data() + 16), physical = be64(header.data() + 24);
    const auto count = be32(header.data() + 32);
    const auto data = std::uint64_t(128) + std::uint64_t(count) * 64;
    if (logical < 0x440 || logical > 0x7fffffffu || physical != physicalLength ||
        physical > std::uint64_t(std::numeric_limits<off_t>::max()) || !count || count > 65556 ||
        be64(header.data() + 40) != 128 || be64(header.data() + 48) != data || data > physical)
        return DiscStatus::malformed;
    bundleRanges.reset(new(std::nothrow) BundleRange[count]);
    if (!bundleRanges) return DiscStatus::noMemory;
    std::array<unsigned char, 4096> block{};
    std::uint32_t crc = ~0u;
    std::uint64_t logicalEnd = 0, cursor = data;
    for (std::uint32_t i = 0; i < count;) {
        const auto records = count - i < 64 ? count - i : 64;
        const auto bytes = std::size_t(records) * 64;
        if (!readPhysical(128 + std::uint64_t(i) * 64, block.data(), bytes)) return DiscStatus::ioError;
        crc = crc32(crc, block.data(), bytes);
        for (std::uint32_t j = 0; j < records; ++j) {
            const auto* row = block.data() + j * 64;
            const auto lo = be64(row), po = be64(row + 8), n = be64(row + 16);
            if (!n || lo < logicalEnd || !contains(logical, lo, n) || po != cursor ||
                !contains(physical, po, n) || be64(row + 56)) return DiscStatus::malformed;
            bundleRanges[i + j] = {lo, po, n};
            logicalEnd = lo + n; cursor = po + n;
        }
        i += records;
    }
    if (cursor != physical || (crc ^ ~0u) != be32(header.data() + 56)) return DiscStatus::malformed;
    length = logical; bundleCount = count;
    return DiscStatus::ready;
}
bool Disc::readFst(std::uint64_t offset, void* output, std::size_t size) const {
    if (!fstCache || (!output && size) || !contains(fstBytes, offset, size)) return false;
    if (size) std::memcpy(output, fstCache.get() + offset, size);
    return true;
}
DiscStatus Disc::open(const char* path) {
    recordStatus(DiscStatus::missing);
    if (file) { std::fclose(file); file = nullptr; }
    validated = false;
    fstCache.reset(); bundleRanges.reset(); bundleCount = 0; physicalLength = 0; sourceReadOperations = 0;
    length = 0; fstOffset = fstBytes = entryCount = 0;
    // Refuse the theoretical wraparound rather than making old handles valid.
    if (generation == std::numeric_limits<std::uint64_t>::max()) return recordStatus(DiscStatus::ioError);
    ++generation;
    if (!path) return recordStatus(DiscStatus::missing);
    file = std::fopen(path, "rb");
    if (!file) return recordStatus(errno == ENOENT ? DiscStatus::missing : DiscStatus::ioError);
    const auto status = validate();
    validated = status == DiscStatus::ready;
    if (!validated) {
        std::fclose(file); file = nullptr;
        length = 0; fstOffset = fstBytes = entryCount = 0;
        fstCache.reset(); bundleRanges.reset(); bundleCount = 0; physicalLength = 0;
    }
    return recordStatus(status);
}
DiscStatus Disc::validate() {
    struct stat info{};
    if (fstat(fileno(file), &info) || !S_ISREG(info.st_mode) || info.st_size < 0)
        return DiscStatus::ioError;
    length = physicalLength = static_cast<std::uint64_t>(info.st_size);
    std::array<unsigned char, 8> magic{};
    if (!contains(physicalLength, 0, magic.size())) return DiscStatus::malformed;
    if (!readPhysical(0, magic.data(), magic.size())) return DiscStatus::ioError;
    if (!std::memcmp(magic.data(), "DLTWPK01", 8)) {
        const auto bundleStatus = loadBundle();
        if (bundleStatus != DiscStatus::ready) return bundleStatus;
    }
    std::array<unsigned char, 0x440> header{};
    if (!mapped(0, header.size())) return DiscStatus::malformed;
    if (!readBytes(0, header.data(), header.size())) return DiscStatus::ioError;
    if ((std::memcmp(header.data(), "GZ2E01", 6) && std::memcmp(header.data(), "GZ2P01", 6)) ||
        header[6] || header[7] || be32(header.data() + 0x1c) != 0xc2339f3d || be32(header.data() + 0x18))
        return DiscStatus::unsupported;
    const auto dol = be32(header.data() + 0x420);
    const auto fst = be32(header.data() + 0x424);
    const auto fstSize = be32(header.data() + 0x428);
    // A structural validator, not proof of content authenticity or full resource safety.
    if (dol < header.size() || fst < header.size() || fstSize < 12 || fstSize > 4 * 1024 * 1024 ||
        !mapped(dol, 0x100) || !mapped(fst, fstSize))
        return DiscStatus::malformed;
    std::array<unsigned char, 0x100> executable{};
    if (!readBytes(dol, executable.data(), executable.size())) return DiscStatus::ioError;
    for (unsigned i = 0; i < 18; ++i) {
        const auto offset = be32(executable.data() + i * 4);
        const auto bytes = be32(executable.data() + 0x90 + i * 4);
        if (bytes && (offset < executable.size() || !mapped(std::uint64_t(dol) + offset, bytes)))
            return DiscStatus::malformed;
    }
    std::array<unsigned char, 12> entry{};
    // One bounded loading-time read captures the complete immutable FST. Path
    // resolution and directory iteration then perform zero SD seeks/reads.
    fstCache.reset(new(std::nothrow) unsigned char[fstSize]);
    if (!fstCache) return DiscStatus::noMemory;
    if (!readBytes(fst, fstCache.get(), fstSize)) return DiscStatus::ioError;
    fstOffset = fst; fstBytes = fstSize;
    if (!readFst(0, entry.data(), entry.size())) return DiscStatus::malformed;
    const auto entries = be32(entry.data() + 8);
    if (be32(entry.data()) != 0x01000000 || be32(entry.data() + 4) || !entries ||
        entries > 65536 || entries > fstSize / 12) return DiscStatus::malformed;
    // Fixed-depth directory stack; reject pathological nesting without allocating.
    std::array<std::uint32_t, 64> parents{}, ends{};
    unsigned depth = 1;
    ends[0] = entries;
    const std::uint64_t strings = std::uint64_t(entries) * 12;
    for (std::uint32_t i = 1; i < entries; ++i) {
        while (depth > 1 && i == ends[depth - 1]) --depth;
        if (!readFst(std::uint64_t(i) * 12, entry.data(), entry.size()))
            return DiscStatus::ioError;
        const auto typeName = be32(entry.data());
        const auto offset = be32(entry.data() + 4), bytes = be32(entry.data() + 8);
        const auto name = typeName & 0xffffff;
        if ((typeName >> 24) > 1 || strings + name >= fstSize) return DiscStatus::malformed;
        // Bound name scanning and require a basename, not a path/traversal component.
        bool terminated = false;
        unsigned count = 0; bool onlyDots = true;
        std::array<unsigned char, 256> basename{};
        const auto available = fstSize - strings - name;
        const auto nameBytes = available < basename.size() ? available : basename.size();
        if (!readFst(strings + name, basename.data(), nameBytes)) return DiscStatus::malformed;
        for (; count < nameBytes; ++count) {
            const auto c = basename[count];
            if (!c) { terminated = true; break; }
            if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return DiscStatus::malformed;
            onlyDots &= c == '.';
        }
        if (!terminated || !count || (onlyDots && count <= 2)) return DiscStatus::malformed;
        if (typeName >> 24) {
            if (offset != parents[depth - 1] || bytes <= i || bytes > ends[depth - 1] || depth == ends.size())
                return DiscStatus::malformed;
            parents[depth] = i; ends[depth++] = bytes;
        } else if (!mapped(offset, bytes)) return DiscStatus::malformed;
    }
    fstOffset = fst; fstBytes = fstSize; entryCount = entries;
    return DiscStatus::ready;
}
DiscFileStatus Disc::openFile(std::uint32_t index, DiscFile& output) {
    output = {};
    if (!validated) return DiscFileStatus::notReady;
    if (!index || index >= entryCount) return DiscFileStatus::notFound;
    std::array<unsigned char, 12> entry{};
    if (!readFst(std::uint64_t(index) * 12, entry.data(), entry.size()))
        return DiscFileStatus::ioError;
    const auto type = be32(entry.data()) >> 24;
    if (type > 1) return DiscFileStatus::malformed;
    if (type) return DiscFileStatus::notFound;
    const auto offset = be32(entry.data() + 4), bytes = be32(entry.data() + 8);
    if (!contains(length, offset, bytes)) return DiscFileStatus::malformed;
    output.owner = this; output.generation = generation;
    output.offset = offset; output.bytes = bytes; output.index = index;
    return DiscFileStatus::ready;
}
DiscFileStatus Disc::readEntry(std::uint32_t index, Entry& output) {
    output = {};
    if (!validated) return DiscFileStatus::notReady;
    if (index >= entryCount) return DiscFileStatus::notFound;
    std::array<unsigned char, 12> entry{};
    if (!readFst(std::uint64_t(index) * 12, entry.data(), entry.size()))
        return DiscFileStatus::ioError;
    const auto typeName = be32(entry.data()), type = typeName >> 24;
    output.directory = type != 0;
    output.value = be32(entry.data() + 4); output.extent = be32(entry.data() + 8);
    if (type > 1 || (type && (output.extent <= index || output.extent > entryCount)))
        return DiscFileStatus::malformed;
    if (!index) return typeName == 0x01000000 && output.value == 0 && output.extent == entryCount
                           ? DiscFileStatus::ready : DiscFileStatus::malformed;
    const auto strings = std::uint64_t(entryCount) * 12;
    const auto name = typeName & 0xffffff;
    if (strings + name >= fstBytes || (!type && !contains(length, output.value, output.extent)))
        return DiscFileStatus::malformed;
    const auto available = fstBytes - strings - name;
    const auto bytes = available < output.name.size() ? available : output.name.size();
    if (!readFst(strings + name, output.name.data(), bytes))
        return DiscFileStatus::ioError;
    const auto* end = static_cast<const unsigned char*>(std::memchr(output.name.data(), 0, bytes));
    if (!end || end == output.name.data()) return DiscFileStatus::malformed;
    for (const auto* c = output.name.data(); c != end; ++c)
        if (*c == '/' || *c == '\\' || *c < 0x20 || *c == 0x7f) return DiscFileStatus::malformed;
    const auto size = std::size_t(end - output.name.data());
    if ((size == 1 && output.name[0] == '.') ||
        (size == 2 && output.name[0] == '.' && output.name[1] == '.')) return DiscFileStatus::malformed;
    return DiscFileStatus::ready;
}
DiscFileStatus Disc::findEntry(const char* path, std::uint32_t& output) const {
    output = 0;
    if (!validated) return DiscFileStatus::notReady;
    if (!path) return DiscFileStatus::invalidPath;
#if defined(__3DS__)
    struct LookupTime {
        std::uint64_t start = tourGameLoadStart();
        ~LookupTime(){if(start)tourLoadCost.pathLookupTicks+=svcGetSystemTick()-start;}
    } timer;
#endif
    // Validate the complete request before touching SD. Fixed bounds also match
    // the accepted FST directory depth and 255-byte basename limit.
    struct Component { std::uint16_t start, size; };
    std::array<Component, 64> components{};
    std::size_t length = 0;
    while (length <= 1023 && path[length]) ++length;
    if (!length || length > 1023) return DiscFileStatus::invalidPath;
    unsigned count = 0;
    for (std::size_t cursor = path[0] == '/' ? 1 : 0; cursor < length;) {
        const auto start = cursor;
        while (cursor < length && path[cursor] != '/') {
            const auto c = static_cast<unsigned char>(path[cursor]);
            if (c == '\\' || c < 0x20 || c == 0x7f) return DiscFileStatus::invalidPath;
            ++cursor;
        }
        const auto bytes = cursor - start;
        if (!bytes || bytes > 255 || count == components.size() ||
            (bytes == 1 && path[start] == '.') ||
            (bytes == 2 && path[start] == '.' && path[start + 1] == '.')) return DiscFileStatus::invalidPath;
        components[count++] = {static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(bytes)};
        if (cursor < length && ++cursor == length) return DiscFileStatus::invalidPath;
    }
    if (!count) return length == 1 && path[0] == '/' ? DiscFileStatus::ready : DiscFileStatus::invalidPath;
    // validate() checked every entry, parent, extent and NUL-terminated name
    // before publishing this immutable FST. Compare in place while traversing
    // siblings instead of copying and revalidating 256 bytes per candidate.
    const auto* entries = fstCache.get();
    const auto* strings = entries + entryCount * 12;
    std::uint32_t first = 1, end = entryCount;
    for (unsigned component = 0; component < count; ++component) {
        const auto& part = components[component];
        std::uint32_t found = 0, foundEnd = 0;
        bool foundDirectory = false;
        for (auto index = first; index < end;) {
            const auto* entry = entries + index * 12;
            const auto nameAndType = be32(entry);
            const bool type = (nameAndType >> 24) != 0;
            const auto next = type ? be32(entry + 8) : index + 1;
            const auto* basename = strings + (nameAndType & 0xffffff);
            unsigned i = 0;
            while (i < part.size && fold(basename[i]) == fold(static_cast<unsigned char>(path[part.start+i]))) ++i;
            const bool match = i == part.size && basename[i] == 0;
            if (match) {
                // A case-insensitive duplicate is ambiguous; never choose one
                // based on on-disc order and accidentally load another resource.
                if (found) return DiscFileStatus::malformed;
                found = index; foundEnd = next; foundDirectory = type != 0;
            }
            index = next;
        }
        if (!found) return DiscFileStatus::notFound;
        if (component + 1 == count) { output = found; return DiscFileStatus::ready; }
        if (!foundDirectory) return DiscFileStatus::notFound;
        first = found + 1; end = foundEnd;
    }
    return DiscFileStatus::notFound;
}
DiscFileStatus Disc::findFile(const char* path, DiscFile& output) {
    output = {};
    if (!validated) return DiscFileStatus::notReady;
    // Preserve the file-only API's rejection of a root directory as a file path.
    if (path && path[0] == '/' && !path[1]) return DiscFileStatus::invalidPath;
    std::uint32_t index = 0;
    const auto result = findEntry(path, index);
    return result == DiscFileStatus::ready ? openFile(index, output) : result;
}
DiscFileStatus Disc::findDirectory(const char* path, DiscDirectory& output) {
    output = {};
    std::uint32_t index = 0;
    const auto result = findEntry(path, index);
    return result == DiscFileStatus::ready ? openDirectory(index, output) : result;
}
DiscFileStatus Disc::openDirectory(std::uint32_t index, DiscDirectory& output) {
    output = {};
    Entry entry;
    const auto result = readEntry(index, entry);
    if (result != DiscFileStatus::ready) return result;
    if (!entry.directory) return DiscFileStatus::notFound;
    output.owner = this; output.generation = generation;
    output.index = index; output.cursor = index + 1; output.end = entry.extent;
    return DiscFileStatus::ready;
}
bool Disc::directoryValid(const DiscDirectory& input) const {
    return validated && input.owner == this && input.generation == generation &&
           input.index < input.cursor && input.cursor <= input.end && input.end <= entryCount;
}
bool Disc::rewindDirectory(DiscDirectory& input) {
    if (!directoryValid(input)) return false;
    input.cursor = input.index + 1; return true;
}
DiscFileStatus Disc::readDirectory(DiscDirectory& input, DiscDirectoryEntry& output) {
    output = {};
    if (!directoryValid(input)) return DiscFileStatus::notReady;
    if (input.cursor == input.end) return DiscFileStatus::notFound;
    Entry entry;
    const auto result = readEntry(input.cursor, entry);
    if (result != DiscFileStatus::ready) return result;
    if (entry.directory && (entry.value != input.index || entry.extent > input.end))
        return DiscFileStatus::malformed;
    output.index = input.cursor; output.directory = entry.directory;
    std::memcpy(output.name.data(), entry.name.data(), entry.name.size());
    input.cursor = entry.directory ? entry.extent : input.cursor + 1;
    return DiscFileStatus::ready;
}
bool Disc::readFile(const DiscFile& input, std::uint64_t offset, void* output, std::size_t size) {
    return validated && input.owner == this && input.generation == generation &&
           contains(input.bytes, offset, size) && readBytes(std::uint64_t(input.offset) + offset, output, size);
}
const char* discStatusText(DiscStatus status) {
    switch (status) {
    case DiscStatus::ready: return "GCN USA/PAL r0 structure validated";
    case DiscStatus::missing: return "Missing /3ds/DuskLight/zelda.twilight";
    case DiscStatus::ioError: return "Disc container read failed; check SD card";
    case DiscStatus::unsupported: return "Requires GCN USA/PAL revision 0 assets";
    case DiscStatus::malformed: return "Invalid/truncated disc structure";
    case DiscStatus::noMemory: return "Insufficient memory for bounded disc index";
    }
    return "Unknown disc error";
}
}
