#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace dusk::native {
inline constexpr std::uint64_t dumpReserve = 8 * 1024 * 1024;
constexpr bool hasDumpSpace(std::uint64_t free, std::uint64_t payload, std::uint64_t overhead) {
    return free >= dumpReserve && overhead <= free - dumpReserve &&
           payload <= free - dumpReserve - overhead;
}
std::uint32_t dumpHash(const void* memory, std::size_t bytes, std::uint32_t value = 2166136261u);
bool flushDumpFile(std::FILE* file);
// Exclusive create, flush/close, then readback checksum. Caller supplies bounded
// scratch memory, which may alias data because hashing/writing finish first.
bool writeVerifiedDumpFile(const char* path, const void* data, std::size_t bytes,
                           void* scratch, std::size_t scratchBytes, std::uint32_t& checksum);
// For a live arena only: checksum the bytes actually copied into each bounded
// scratch chunk, then verify the stored file. The result is not an atomic
// process snapshot; workers may change other chunks while capture proceeds.
// Scratch must be disjoint from the source.
bool writeStreamedDumpFile(const char* path,const void* data,std::size_t bytes,
                           void* scratch,std::size_t scratchBytes,std::uint32_t& checksum);
}
