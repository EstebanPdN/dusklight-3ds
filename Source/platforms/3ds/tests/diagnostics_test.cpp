#include "../diagnostics.hpp"
#include <cassert>
#include <cstdio>

int main() {
    using dusk::native::DumpArm;
    DumpArm arm;
    assert(!arm.update(1, 0, false));
    assert(!arm.update(1, 2000, false)); // Startup-held chord must never fire.
    assert(!arm.update(0, 2001, false));
    assert(!arm.update(1, 2100, false) && arm.arming());
    assert(!arm.update(1, 3099, false));
    assert(arm.update(1, 3100, false) == 1 && !arm.arming());
    assert(!arm.update(1, 9999, false)); // Must release between captures.
    assert(!arm.update(0, 10000, false));
    assert(!arm.update(2, 10001, false));
    assert(!arm.update(1, 10002, false) && !arm.arming()); // Switching chords cancels.
    assert(!arm.update(1, 12000, false));
    assert(!arm.update(0, 12001, false));
    assert(!arm.update(2, 12002, false));
    assert(!arm.update(2, 14002, true) && !arm.arming()); // Re-entry refused.
    assert(!arm.update(2, 16002, false));
    assert(!arm.update(0, 16003, false));
    assert(!arm.update(2, 16004, false));
    assert(arm.update(2, 17004, false) == 2);
    std::puts("PASS: dump startup-held/release/one-second arm/cancel/reentry/debounce");
}
