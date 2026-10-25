#pragma once
// Private observational diagnostic. Render owner only; no pixel views retained.
#include <array>
#include <cstddef>
#include <cstdint>
namespace dusk::native {
struct ColorCopyPlan;
void observeCopyLifecycleResidency(std::uint64_t,const ColorCopyPlan&,bool depth,bool indexed,std::size_t snapshotBytes);
namespace copyprobe {
inline bool enabled{}; // Every runtime hook checks this before scanning or calling.
inline constexpr char optionPath[]="sdmc:/3ds/dusklight/copy-lifecycle-probe.txt";
inline constexpr char reportPath[]="sdmc:/3ds/dusklight/copy-lifecycle-probe.log";
inline constexpr char recoveryPath[]="sdmc:/3ds/dusklight/copy-lifecycle-probe.log.previous";
inline constexpr char stagingPath[]="sdmc:/3ds/dusklight/copy-lifecycle-probe.log.tmp";
inline constexpr char optionValue[]="copy-lifecycle-v1\n";
inline constexpr unsigned imageCapacity=16,eventCapacity=192;
inline constexpr std::size_t reportLimit=128*1024;
enum class Kind:std::uint8_t { registration,copy,palette,imageInvalidate,globalInvalidate,bindAttempt,decode,flushUnknown,bindSuccess,retire,destroy };
enum class Reason:std::uint8_t { none,eligible,notResident,depth,palette,protectedDraw,layout,miss,dirty,hit,image,global,evict,release,shutdown,replaced };
enum Counter:unsigned { registrations,copies,residentCopies,eligibleCopies,excludedDepth,excludedPalette,
    excludedLayout,nonresidentCopies,protectedCopies,bindAttempts,misses,dirtyBinds,hits,decodes,flushesUnknown,
    binds,bindsBeforeGlobal,paletteChanges,imageInvalidations,globalInvalidations,evictions,releases,destructions,
    potentialUseful,potentialWasted,wastedGlobal,wastedImage,wastedRetire,wastedReplace,wastedDestroy,counterCount };
struct Image {
    std::uint64_t handle{},serial{},imageSequence{},copyGlobal{},copyCount{},bindCount{},decodeCount{},beforeGlobalCount{};
    std::uintptr_t original{},cloneHeader{},clonePayload{};
    std::uint64_t allocationBytes{},pixelBytes{},nativeBytes{},snapshotBytes{};
    std::uint64_t eligibleCount{},usefulCount{},wastedCount{};
    std::uint32_t format{},width{},height{};
    Reason lastInvalidation{};
    bool candidate{},boundBeforeGlobal{},resident{},dirty{},hitReported{};
};
struct Event {
    std::uint64_t sequence{},frame{},globalSequence{},handle{},serial{},imageSequence{};
    std::uintptr_t original{},cloneHeader{},clonePayload{};
    std::uint64_t allocationBytes{},pixelBytes{},nativeBytes{},snapshotBytes{};
    std::uint32_t format{},width{},height{};
    Kind kind{};Reason reason{},invalidationReason{};std::uint8_t flags{};
    std::uint64_t eligibleCount{},usefulCount{},wastedCount{};
};
struct State {
    std::array<Image,imageCapacity> images{};
    std::array<Event,eventCapacity> events{};
    std::array<std::uint64_t,Counter::counterCount> counters{};
    std::uint64_t frame{},sequence{},contentSerial{},globalSequence{},writeFailures{},droppedEvents{};
    unsigned head{},size{};
    bool initialized{},eventOverflow{},trackingOverflow{},counterOverflow{};
};
static_assert(sizeof(State)<=40*1024,"Private probe metadata bound");
void initialize(); // Exactly once; exact <=32-byte known file, absent/invalid => OFF.
void setFrame(std::uint64_t);
void registration(std::uint64_t,const void* original,const void* cloneHeader,const void* clonePayload,
                  std::size_t allocationBytes,std::size_t pixelBytes,unsigned format,unsigned width,unsigned height);
void copy(std::uint64_t,Reason,bool resident,bool dirty,std::size_t nativeBytes,std::size_t snapshotBytes);
void palette(std::uint64_t);
void imageInvalidate(std::uint64_t);
void globalInvalidate();
void bindAttempt(std::uint64_t,Reason);
void decode(std::uint64_t,Reason);
void flushUnknown(std::uint64_t); // Void Citro3D API cannot certify success/failure.
void bindSuccess(std::uint64_t,Reason);
void retire(std::uint64_t,Reason=Reason::evict);
void destroy(std::uint64_t);
bool writeReport(bool final); // Existing capture/normal-exit only; synchronous, fixed paths.
const State& state(); // Private host assertions; no reset/enable API.
}
}
