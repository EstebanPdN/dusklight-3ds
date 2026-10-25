#pragma once
#include <cstddef>
#include <cstdint>

namespace dusk::native::appdump {
constexpr std::uint32_t chord = (1u<<0)|(1u<<8)|(1u<<9); // A + R + L
constexpr std::uint32_t addressLimit = 0x40000000;
constexpr std::size_t chunkBytes = 64*1024, maxRegions = 1024;
constexpr std::uint64_t maxPayload = 256ull*1024*1024;
struct Region { std::uint32_t base{}, size{}, perm{}, state{}, flags{}; };
const char* exclusion(const Region&);
bool validRegion(const Region&, std::uint32_t cursor);
class Trigger {
    bool ready_{}, timing_{}, suppress_{};
    std::uint64_t since_{};
public:
    // Boot-held chord is ignored. All three chord buttons must be released
    // before the first arm and after capture/interrupted arming.
    bool update(std::uint32_t keys, std::uint64_t milliseconds, bool busy=false);
    bool suppress() const { return suppress_; }
    bool arming() const { return timing_; }
};
struct Blob { const void* data{}; std::size_t bytes{}; };
struct Metadata {
    std::uint64_t frame{}, timestamp{};
    char build[128]{"unavailable"}, scene[16]{"unavailable"};
    int room{-1};
    std::uint32_t applicationRegionBytes{}, kernelVersion{};
    bool new3ds{};
};
struct Workspace;
struct Supplement { std::uint64_t inventoryBytes{}, payloadBytes{}; std::uint32_t inventoryHash{}; bool cancelled{}; };
struct Services {
    void* context{};
    bool (*query)(void*,std::uint32_t,Region&){};
    // Must requery the entire interval and refuse changed permissions/state.
    // Normal self-copy cannot catch an unmap racing after that query.
    bool (*read)(void*,const Region&,std::uint32_t,void*,std::size_t){};
    bool (*space)(void*,std::uint64_t&){};
    bool (*poll)(void*){}; // false = cancel; never called holding game locks
    void (*progress)(void*,const char*,std::uint64_t,std::uint64_t){};
    // Called before any progress/UI. Borrowed pixels retained until return.
    bool (*screens)(void*,Blob&,Blob&){};
    // Optional injected writer for fault tests. Null uses dump_io helpers.
    bool (*write)(void*,const char*,const void*,std::size_t,void*,std::size_t,std::uint32_t&,bool){};
    // Optional bounded adapter artifacts, persisted before progress overwrites LCD.
    bool (*supplement)(void*,const char*,Workspace&,Supplement&){};
    void (*removeSupplement)(void*,const char*){};
    std::uint32_t allocationUnit{4096}; // actual SD cluster size, preflight rounding
    std::uint32_t excludedBase{}, excludedSize{}; // diagnostic workspace, page aligned
};
struct Workspace {
    Region regions[maxRegions]{};
    alignas(4096) unsigned char data[chunkBytes]{};
    unsigned char verify[4096]{};
    char text[4096]{}, path[256]{};
    unsigned char lcdTop[400*240*4]{}; // converted presented LCD, excluded from RAM walk

};
enum class Result { success, cancelled, invalidMap, insufficientSpace, ioError, readError, screensError, pathError };
const char* resultText(Result);
// Main owner only, after C3D_FrameEnd. Fixed workspace; no worker suspension,
// game locks, kernel/foreign process access, or recursive directory deletion.
Result run(const char* root, const Metadata&, Services&, Workspace&, char (&output)[256]);
}
