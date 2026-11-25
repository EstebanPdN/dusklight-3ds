#pragma once
#include <cstdint>

namespace dusk::native {
// Only the game thread writes this. DVD/background reads must never enter it.
// Clearing per picture also keeps nested process creation from double-counting:
// retain the longest single create call and its original profile ID.
struct TourLoadCost {
    std::uint64_t gameReadTicks{}, longestCreateTicks{}, dvdLockTicks{}, pathLookupTicks{};
    std::int16_t process{-1};
};
inline bool measureTourLoads{};
inline TourLoadCost tourLoadCost;
#if defined(__3DS__)
std::uint64_t tourGameLoadStart();
void finishTourDvdLock(std::uint64_t start,const char* operation);
#endif
}
