#pragma once
#include <cstdint>

// libctru implements these allocation-free operations. The host test provides
// a separate implementation of this narrow interface, never linked on 3DS.
namespace dusk::native {
void lockInit(std::int32_t* lock);
void lock(std::int32_t* lock);
bool tryLock(std::int32_t* lock);
void unlock(std::int32_t* lock);
void conditionInit(std::int32_t* condition);
void conditionWait(std::int32_t* condition, std::int32_t* lock);
void conditionWaitTimeout(std::int32_t* condition, std::int32_t* lock, std::int64_t nanoseconds);
void conditionBroadcast(std::int32_t* condition);
std::uint64_t counterTicks();
std::uint64_t wallMilliseconds();
}
