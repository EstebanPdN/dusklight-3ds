#pragma once
#include <cstdint>
// HUD tape: the 2D pass on top of the picture (hearts, buttons, minimap) cost
// more than a third of the painter (Alpha 7 wave 2, Azahar: 34.7 of 92.8 ms a
// profiled frame; console Alpha 6: 23 ms a picture), spread over every layer
// with no single hot spot. The pass is therefore not translated every picture.
// When it is drawn, the PICA command words it produced are kept; the pictures
// in between run those words again, so the PICA draws the same HUD over the
// new 3D picture with the same blending, and the game's 2D lists are not
// walked at all. The HUD then shows the state of the last recorded picture
// (one picture old with the default of two pictures per recording).
//
// A recording is self-contained: every Citro3D state group is marked changed
// before the first draw (the list Citro3D itself uses after the GPU was lost),
// so the words set the framebuffer, viewport, scissor, program, textures,
// combiner, raster state and uniforms they need. After a replay the same marks
// make the next draw send Citro3D's own record again. The geometry of a
// recorded pass lives in a buffer that frame begins do not reset
// (beginGxTapeGeometry).
//
// A recording is refused (the pass is simply drawn) when the command span
// moved while it was made (a submission, a reclaim, a draw the GPU guard ran
// alone), when a texture, copy or target changed, when the geometry buffer
// overflowed or when it is larger than the store: full-screen menus stay out
// by size. A kept tape is dropped when texture storage was retired or copied
// to, when the resident geometry pools were cleared or when the target changed.
namespace dusk::native {
// Pictures per recording: 1 turns the tape off (every picture translates the
// pass), 2 is the default. SD switch hud-tape.txt (developer_script.cpp).
void setHudTapePictures(unsigned pictures);
unsigned hudTapePictures();
// Retained screens (screen_tape.hpp) inside a pass being recorded are
// replayed or recorded as anywhere else: their command words are part of the
// pass, and the pass's tape then runs them again. Such a tape points at
// vertices in the retained screens' buffer, so they call this when that
// buffer is recycled: the kept tape is not run again.
void dropHudTape();
struct HudTapeStatistics {
    std::uint64_t recorded{},replayed{},refusedSpan{},refusedState{},refusedGeometry{},refusedSize{},dropped{};
    std::uint32_t words{},largestWords{};
    // Alpha 15 (gx-overlap.txt 2): passes whose replay or recording was a job
    // of the second-core worker's queue, and queued replays the drawing side
    // refused (that picture had no HUD).
    std::uint64_t queuedReplays{},queuedRecordings{},queuedReplayFailures{};
};
HudTapeStatistics hudTapeStatistics();
// gxJobHudTape (gx_worker_sync.hpp): the queued replay or an end of a queued
// recording, on the thread that draws.
void runHudTapeJob(const void* arguments,std::size_t bytes);
// Profiled-frame line, when a count moved.
void reportHudTape();
}
extern "C" {
// Before the pass: 0 = draw it (no tape), 1 = draw it and call
// duskHudTapeEnd afterwards, 2 = the tape was run, skip the pass.
int duskHudTapeBegin(void);
void duskHudTapeEnd(void);
}
