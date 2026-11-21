#ifndef D_TICK_PROFILE_H
#define D_TICK_PROFILE_H

// Named timers for the logic of a tick (collision, model calc, camera). They
// time only on the profiled pictures (one in 30 in developer runs and the
// automatic test's profile phase) and print as "Native painter <name>" with
// the total of every tick of that picture; otherwise a scope is one load and
// a branch. On the sections stage of a measurement they are also summed by the
// process whose execute or draw method they run in (GxTickProfileScope).
#if defined(DUSK_TARGET_3DS)
#include "../../platforms/3ds/runtime/gx_profile.hpp"
// The index is kept +1 in a constant-initialised static (0 = not yet
// registered): no thread-safe static guard, so no memory barrier per call.
// Tick scopes run on the game thread only.
#define DUSK_TICK_PROFILE(var, name)                                                               \
    static unsigned var##_profile;                                                                 \
    if (var##_profile == 0) {                                                                      \
        var##_profile = dusk::native::registerGxNamedProfile(name) + 1;                            \
    }                                                                                              \
    dusk::native::GxTickProfileScope var(var##_profile - 1)

// Split profile (logic.txt split=1, measurement only): nested timers inflate
// the scope around them by two clock reads each, so in that mode the inner
// collision scopes (bg.kcl.*, bg.dzb.*) do not time, and the callers time
// their own sections one after another with DUSK_SPLIT_SPAN (start at the
// declaration, var.stop() at the end; inactive outside split mode).
namespace dNativeLogic {
extern bool g_split;
}
#define DUSK_TICK_PROFILE_INNER(var, name)                                                         \
    static unsigned var##_profile;                                                                 \
    if (var##_profile == 0) {                                                                      \
        var##_profile = dusk::native::registerGxNamedProfile(name) + 1;                            \
    }                                                                                              \
    dusk::native::GxTickProfileScope var(dNativeLogic::g_split ? ~0u : var##_profile - 1)
#define DUSK_SPLIT_SPAN(var, name)                                                                 \
    static unsigned var##_profile;                                                                 \
    if (var##_profile == 0) {                                                                      \
        var##_profile = dusk::native::registerGxNamedProfile(name) + 1;                            \
    }                                                                                              \
    dusk::native::GxNamedProfileSpan var(dNativeLogic::g_split ? var##_profile - 1 : ~0u)
#else
#define DUSK_TICK_PROFILE(var, name)
#define DUSK_TICK_PROFILE_INNER(var, name)
#define DUSK_SPLIT_SPAN(var, name)                                                                 \
    struct {                                                                                       \
        void stop() {}                                                                             \
    } var
#endif

#endif /* D_TICK_PROFILE_H */
