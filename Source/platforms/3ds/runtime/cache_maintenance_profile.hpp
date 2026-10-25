#pragma once
#include <cstdint>
namespace dusk::native {
// Sparse profiling belongs to the calling thread. Audio/archive workers never
// update the render thread's counters or read its profiling switch.
struct CacheMaintenanceProfile {
    std::uint64_t flushTicks{},invalidateTicks{},flushBytes{},invalidateBytes{};
    unsigned flushCalls{},invalidateCalls{};
    bool active{};
};
inline thread_local CacheMaintenanceProfile cacheMaintenanceProfile;
}
