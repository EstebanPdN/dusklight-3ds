#pragma once
#include <cstdint>
// One slice of a lock wait on a libctru LightLock word, as a counted waiter.
// The word (libctru synchronization.c, checked against the libctru.a
// disassembly): 0 or 1 free, n > 1 free with n-1 woken waiters still counted,
// -n held with n-1 waiters. LightLock_Unlock negates it and signals one waiter
// on the address arbiter while the count is not zero; a woken waiter takes the
// lock with 1-n (one waiter fewer) or waits again while it is held.
// Waiting as one of those counted waiters means the owner's unlock wakes this
// thread at once. On a timeout it leaves the count, or takes the lock if it
// was freed meanwhile, so the caller can run its cooperative checkpoint with
// nothing owed. Returns true when the lock is held by the caller.
// Ops (static members): load(word) and store(word, value) are the exclusive
// pair (store returns true on failure), clear(), barrier(), now() in
// nanoseconds, and wait(word, ns): sleep while *word < 0, until a signal or
// the timeout; the check and the sleep are atomic with respect to signals.
// Production instantiates it with ldrex/strex and the kernel arbiter
// (os_thread.cpp); the host test with an emulation of both.
namespace dusk::native {
template<class Ops>
bool lightLockWithin(std::int32_t* word, std::int64_t nanoseconds) {
    std::int32_t value; bool held;
    do {
        value = Ops::load(word);
        if (value == 0) value = 1;
        held = value < 0;
        value = held ? value - 1 : -value;
    } while (Ops::store(word, value));
    if (!held) { Ops::barrier(); return true; }
    const auto deadline = Ops::now() + std::uint64_t(nanoseconds);
    for (;;) {
        const auto now = Ops::now();
        if (now < deadline) Ops::wait(word, std::int64_t(deadline - now));
        const bool last = Ops::now() >= deadline;
        std::int32_t observed;
        for (;;) {
            observed = Ops::load(word);
            if (observed < 0 && !last) { Ops::clear(); break; }
            // Free: take it and leave the count. Held at the deadline: leave the count.
            if (!Ops::store(word, observed < 0 ? observed + 1 : 1 - observed)) break;
        }
        if (observed >= 0) { Ops::barrier(); return true; }
        if (last) return false;
    }
}
}
