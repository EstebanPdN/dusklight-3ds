#include "dump_io.hpp"
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace dusk::native {
std::uint32_t dumpHash(const void* memory, std::size_t bytes, std::uint32_t value) {
    const auto* data = static_cast<const unsigned char*>(memory);
    while (bytes--) { value ^= *data++; value *= 16777619u; }
    return value;
}
bool flushDumpFile(FILE* file) {
    bool okay = !std::ferror(file) && !std::fflush(file);
    if (okay) okay = !fsync(fileno(file));
    if (std::fclose(file)) okay = false;
    return okay;
}
bool writeVerifiedDumpFile(const char* path, const void* data, std::size_t bytes,
                           void* scratch, std::size_t scratchBytes, std::uint32_t& checksum) {
    if (!path || (!data && bytes) || !scratch || !scratchBytes) return false;
    const auto expected = dumpHash(data, bytes);
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); return false; }
    bool okay = !bytes || std::fwrite(data, 1, bytes, file) == bytes;
    if (!flushDumpFile(file)) okay = false;
    if (!okay) return false;
    file = std::fopen(path, "rb");
    if (!file) return false;
    std::size_t remaining = bytes;
    std::uint32_t actual = 2166136261u;
    while (remaining) {
        const auto count = std::min(remaining, scratchBytes);
        if (std::fread(scratch, 1, count, file) != count) { okay = false; break; }
        actual = dumpHash(scratch, count, actual); remaining -= count;
    }
    if (std::fgetc(file) != EOF || std::ferror(file)) okay = false;
    if (std::fclose(file)) okay = false;
    if (!okay || actual != expected) return false;
    checksum = actual;
    return true;
}
bool writeStreamedDumpFile(const char* path,const void* data,std::size_t bytes,
                           void* scratch,std::size_t scratchBytes,std::uint32_t& checksum){
    checksum=0;
    if(!path||(!data&&bytes)||!scratch||!scratchBytes)return false;
    const auto a=reinterpret_cast<std::uintptr_t>(data),b=reinterpret_cast<std::uintptr_t>(scratch);
    if(bytes&&((a<=b&&b-a<bytes)||(b<a&&a-b<scratchBytes)))return false;
    const int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;
    FILE* file=fdopen(fd,"wb");if(!file){close(fd);return false;}
    const auto* source=static_cast<const unsigned char*>(data);
    std::uint32_t expected=2166136261u;bool okay=true;
    for(std::size_t offset=0;okay&&offset<bytes;){
        const auto size=std::min(scratchBytes,bytes-offset);
        std::memcpy(scratch,source+offset,size);expected=dumpHash(scratch,size,expected);
        okay=std::fwrite(scratch,1,size,file)==size;offset+=size;
    }
    if(!flushDumpFile(file))okay=false;
    file=okay?std::fopen(path,"rb"):nullptr;if(!file)return false;
    std::uint32_t actual=2166136261u;
    for(std::size_t remaining=bytes;okay&&remaining;){
        const auto size=std::min(scratchBytes,remaining);
        if(std::fread(scratch,1,size,file)!=size){okay=false;break;}
        actual=dumpHash(scratch,size,actual);remaining-=size;
    }
    if(std::fgetc(file)!=EOF||std::ferror(file))okay=false;
    if(std::fclose(file))okay=false;
    if(!okay||actual!=expected)return false;
    checksum=actual;return true;
}
}
