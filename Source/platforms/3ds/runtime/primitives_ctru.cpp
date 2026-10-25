#include "primitives.hpp"
#include "clock.hpp"
#include <type_traits>
extern "C" {
#include <3ds/synchronization.h>
#include <3ds/os.h>
}

static_assert(std::is_same_v<LightLock, std::int32_t>);
static_assert(std::is_same_v<CondVar, std::int32_t>);
static_assert(SYSCLOCK_ARM11 == dusk::native::nativeTickFrequency);

namespace dusk::native {
void lockInit(std::int32_t* value) { LightLock_Init(value); }
void lock(std::int32_t* value) { LightLock_Lock(value); }
bool tryLock(std::int32_t* value) { return LightLock_TryLock(value) == 0; }
void unlock(std::int32_t* value) { LightLock_Unlock(value); }
void conditionInit(std::int32_t* value) { CondVar_Init(value); }
void conditionWait(std::int32_t* condition, std::int32_t* lock) { CondVar_Wait(condition, lock); }
void conditionWaitTimeout(std::int32_t* condition, std::int32_t* lock, std::int64_t nanoseconds) {
    // The predicate owner distinguishes notifications from timeouts/spurious wakes.
    CondVar_WaitTimeout(condition, lock, nanoseconds);
}
void conditionBroadcast(std::int32_t* value) { CondVar_Broadcast(value); }
std::uint64_t counterTicks() { return svcGetSystemTick(); }
std::uint64_t wallMilliseconds() { return osGetTime(); }
}
