#pragma once
// Zone transitions with the curtain closed until the zone is ready (Alpha 12).
//
// The original reveals a new zone as soon as the play scene's own create is
// done: rooms, actors, archives and the second sound bank are still loading,
// and every first draw (texture conversion, material plans, stored geometry)
// happens while the zone is visible. On the console that was a freeze of about
// a second right after the fade opened (Alpha 11, F_SP103 -> F_SP104: 354,
// 161 and >= 655 ms pictures). Two stages keep it behind the curtain:
//   loading: the overlap does not send its "out" request (the new scene stays
//            paused exactly as the original pauses it) until nothing is being
//            created and the DVD thread has no command left; a create that
//            makes no progress for 1 s, or 10 s in all, releases it;
//   warming: the scene runs one logic tick per picture and draws, while the
//            screen fader stays opaque and frozen and the sound is paused; it
//            ends after at least 3 pictures once two pictures in a row cost at
//            most 1.5 times the one before (first-draw work is done), or after
//            45 pictures / 4 s.
// Then the original fade-in plays its full length, the game clock is reset
// (no catch-up ticks) and the sound resumes. SD switch transition-hold-off.txt
// restores the original behaviour.
namespace dusk::native {
// Main thread, before the game starts.
void initializeSceneTransition(bool enabled);
// Game thread, when the picture's ticks are chosen: one tick per picture.
bool transitionWarming();
// Loading or warming (the automatic test must not measure these pictures).
bool transitionHeld();
enum class TransitionEvent { none, warmingStarted, released };
// Game thread, once per picture after its telemetry sample.
TransitionEvent sceneTransitionPictureEnd(float loopMs);
}
// Original code hooks (C linkage, weak-free: always linked on 3DS).
extern "C" bool duskNativeTransitionMayOpen();
extern "C" bool duskNativeCurtainHeld(const void* fader);
