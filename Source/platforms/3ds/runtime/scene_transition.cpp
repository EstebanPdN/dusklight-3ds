#include "scene_transition.hpp"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_create_iter.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_graphic.h"
#include <3ds.h>
#include <cstdio>

extern "C" int duskNativeDvdPending;

namespace {
enum class State { idle, loading, warming };
State state = State::idle;
bool enabled = true;
// loading
std::uint64_t loadStart{}, lastProgress{};
unsigned minCreates{};
int lastDvd{};
// warming
unsigned warmPictures{};
float previousLoop{}, warmLoops[2]{};
std::uint64_t warmStart{};
unsigned transitions{};
float loadMs{};
const char* loadReason = "";

std::uint64_t now() { return svcGetSystemTick(); }
float ms(std::uint64_t ticks) { return float(ticks * 1000.0 / SYSCLOCK_ARM11); }

void* countCreate(void*, void* data) {
    ++*static_cast<unsigned*>(data);
    return nullptr; // keep iterating: count every pending create
}
unsigned pendingCreates() {
    unsigned count = 0;
    fpcCtIt_Judge(countCreate, &count);
    return count;
}
int pendingDvd() { return __atomic_load_n(&duskNativeDvdPending, __ATOMIC_ACQUIRE); }
}

namespace dusk::native {
void initializeSceneTransition(bool on) {
    enabled = on;
    OSReport("Native scene transitions: %s\n", on ? "curtain held until the zone is ready" :
             "original reveal (transition-hold-off.txt)");
}
bool transitionWarming() { return state == State::warming; }
bool transitionHeld() { return state != State::idle; }

TransitionEvent sceneTransitionPictureEnd(float loopMs) {
    if (state != State::warming) return TransitionEvent::none;
    ++warmPictures;
    if (warmPictures == 1) {
        previousLoop = loopMs;
        return TransitionEvent::warmingStarted;
    }
    warmLoops[0] = warmLoops[1];
    warmLoops[1] = loopMs;
    // Steady: this picture and the one before each cost at most 1.5 times the
    // picture before it, and nothing is still being created or loaded.
    const bool steady = warmPictures >= 3 && warmLoops[1] <= previousLoop * 1.5f && warmLoops[0] > 0 &&
                        warmLoops[1] <= warmLoops[0] * 1.5f;
    const bool idleWork = pendingCreates() == 0 && pendingDvd() == 0;
    const bool timeout = warmPictures >= 45 || ms(now() - warmStart) >= 4000.0f;
    previousLoop = loopMs;
    if (!((steady && idleWork) || timeout)) return TransitionEvent::none;
    OSReport("Native transition %u: load hold %.0f ms (%s), warm %u pictures / %.0f ms (%s), last picture %.1f ms\n",
             transitions, loadMs, loadReason, warmPictures, ms(now() - warmStart), timeout ? "timeout" : "ready", loopMs);
    state = State::idle;
    return TransitionEvent::released;
}
}

extern "C" bool duskNativeTransitionMayOpen() {
    using namespace dusk::native;
    if (!enabled) return true;
    const std::uint64_t t = now();
    if (state != State::loading) {
        state = State::loading;
        loadStart = lastProgress = t;
        minCreates = ~0u;
        lastDvd = -1;
        ++transitions;
    }
    const unsigned creates = pendingCreates();
    const int dvd = pendingDvd();
    // Progress: fewer creates pending than ever in this hold, or the DVD
    // queue moved (spawners that keep re-creating do not count as progress).
    if (creates < minCreates || dvd != lastDvd) {
        if (creates < minCreates) minCreates = creates;
        lastDvd = dvd;
        lastProgress = t;
    }
    const bool player = dComIfGp_getPlayer(0) != nullptr && dComIfGp_roomControl_getStayNo() >= 0;
    const bool ready = creates == 0 && dvd == 0 && player;
    // A create can wait for something only a running scene provides: no
    // progress for a second (and no load in flight) lets the scene start.
    const bool stalled = dvd == 0 && ms(t - lastProgress) >= 1000.0f;
    const bool timeout = ms(t - loadStart) >= 10000.0f;
    if (!(ready || stalled || timeout)) return false;
    loadMs = ms(t - loadStart);
    loadReason = ready ? "ready" : timeout ? "timeout" : "stalled";
    if (!ready) {
        OSReport("Native transition %u: released while %u creates and %d DVD commands were pending (%s)\n", transitions,
                 creates, dvd, loadReason);
    }
    // Warm only behind an opaque curtain: a cut without a fade shows at once.
    JUTFader* fader = mDoGph_gInf_c::getFader();
    if (fader && fader->getStatus() == JUTFader::None) {
        state = State::warming;
        warmPictures = 0;
        warmLoops[0] = warmLoops[1] = 0;
        warmStart = t;
        // The original loads the second sound bank when the fade-in ends,
        // while the zone is visible; it is idempotent, so start it now.
        if (mDoAud_zelAudio_c::isBgmSet()) mDoAud_load2ndDynamicWave();
    } else {
        state = State::idle;
        OSReport("Native transition %u: load hold %.0f ms (%s), no curtain to warm behind\n", transitions, loadMs,
                 loadReason);
    }
    return true;
}

extern "C" bool duskNativeCurtainHeld(const void* fader) {
    return dusk::native::transitionWarming() && fader == mDoGph_gInf_c::getFader();
}
