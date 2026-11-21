#pragma once
#include <cstddef>
#include <cstdint>
// Retained 2D screens (Alpha 8). A menu or HUD screen is a J2DScreen: a tree
// of panes drawn one quad at a time through the whole GX path (material
// setters, register bank, state key, record lookup, vertex decode, Citro3D).
// The file select screen draws about 600 quads that way: 112-116 ms of a
// picture in Azahar, 5 pictures a second on the console (Alpha 7), with the
// time spread over every layer (PC profile, 940 samples: register writes
// 13 %, texture retention 10 %, memory copies and fills 14 %, GPU guard 5 %,
// vertex decode 5 %, Citro3D 9 %). Most pictures of a menu draw the same
// screens again unchanged.
//
// J2DScreen::draw therefore asks first whether the screen changed. It walks
// its pane tree and mixes into a fingerprint everything the draw would read:
// the bytes of each pane object, of each material with its blocks and texture
// matrices, of each texture and palette object, the text of each text box,
// the graf context and the GX mode registers no pane sets (J2DScreen.cpp).
// Then:
//  - a screen whose fingerprint equals its last recording's: the PICA command
//    words of that recording are sent again, the register bank receives the
//    words that recording changed, and the pane tree is not drawn;
//  - any other screen is drawn as before, and its command words are kept.
// A recording is self-contained in the same way as the HUD tape's
// (hud_tape.hpp): every Citro3D state group is marked changed before its
// first draw and after a replay, and its vertices live in a buffer frame
// begins do not reset.
//
// What a replay does not reproduce: bank words a pane wrote with the value
// they already had at the recording, when they hold another value now. Draws
// that follow a screen and rely on such a word are not known; the screen's
// own closing state (J2DScreen::draw) is always executed for real.
//
// A recording is not kept (the screen is simply drawn every picture) when a
// texture it binds can be rewritten by the game and its content is not part
// of the fingerprint, when a pane has a type other than the eight J2D classes, when the command
// span moved while it was made, when a copy or a target change happened in
// it, or when it does not fit the store. Kept recordings are dropped when
// texture storage was retired or re-housed, when the resident geometry pools
// were cleared, when the target changed or when their geometry buffer is
// recycled.
//
// A screen that keeps changing (a pulsing cursor, a counter) is split: the
// child subtrees of its root pane become recordings of their own, and a child
// that keeps changing is split again, so that only the pane that moves is
// drawn in every picture (J2DScreen.cpp). A pane with no children that keeps
// changing rests: it is drawn without recording for a while.
//
// SD switch screen-tape.txt (developer_script.cpp): 0 off (Alpha 7), 1 on
// (default), v verify: a screen that could be replayed is drawn as well, and
// the two command runs are compared word for word (counts in the report).
// In verify mode every recording starts from zeroed Citro3D uniforms, one
// fixed texture on every unit and default combiner stages: a recording sends
// all of them, and the ones no pane of the screen sets would otherwise carry
// whatever the 3D scene left there and differ from picture to picture.
namespace dusk::native::gx { struct Texture; }
namespace dusk::native {
enum class ScreenTapeMode : std::uint8_t {off, on, verify};
// For a screen's fingerprint: the content checksum of an image the game can
// rewrite (an item icon copied to the heap, the minimap). The image is then
// known to this picture's fingerprints, and a recording that binds it is
// kept: its fingerprint changes with the pixels. False when the image cannot
// be read or too many were asked for in one picture.
bool screenTapeTextureSum(const gx::Texture& texture,std::uint64_t& sum);
void setScreenTapeMode(ScreenTapeMode mode);
// Alpha 15 (gx-overlap.txt 1 and 2): a screen met while the painter's draws
// go to the second-core worker's queue is drawn as plain runs without a sync
// first (off: the Alpha 14 sync, then the same plain draw).
void setScreenTapeQueuedPlain(bool on);
// gxJobScreenTape (gx_worker_sync.hpp): a queued replay on the thread that draws.
void runScreenTapeJob(const void* arguments,std::size_t bytes);
ScreenTapeMode screenTapeMode();
struct ScreenTapeStatistics {
    // inPlace: recordings that took the command words of the owner's older
    // recording instead of new ones.
    std::uint64_t begins{},replayed{},recorded{},inPlace{},plain{},volatileScreens{},refusedSpan{},refusedState{},refusedGeometry{},
        refusedSize{},dropped{},recycled{},verified{},verifyMismatches{},bankWords{};
    std::uint32_t tapes{},words{},largestWords{},geometryBytes{};
    // Indexed by DuskScreenTapeNote.
    std::uint64_t notes[8]{};
};
ScreenTapeStatistics screenTapeStatistics();
// Profiled-frame line, when a count moved.
void reportScreenTape();
// Once per frame, before the first draw: recycles the geometry buffer when a
// recording asked for it (no command list of the previous frame refers to it
// any more: the frame begin waited for the GPU).
void beginScreenTapeFrame();
}
extern "C" {
// The fingerprint of the screen about to be drawn (two 32-bit halves) and
// whether it may be recorded at all. Returns 0: draw the screen (no tape);
// 1: draw it and call duskScreenTapeEnd afterwards; 2: the recording was
// replayed, skip the pane tree (the caller still restores its closing state).
int duskScreenTapeBegin(const void* screen,unsigned long fingerprintA,unsigned long fingerprintB,int recordable);
// Ends the recording. Returns 1 when this owner was now recorded several
// times in a row without one replay: something in it changes in every
// picture. The caller either records its parts separately from then on
// (J2DScreen.cpp splits the pane) or calls duskScreenTapeRest.
int duskScreenTapeEnd(void);
// The owner is drawn without recording for a while (longer each time in a row).
void duskScreenTapeRest(const void* screen);
// The screen object is going away.
void duskScreenTapeForget(const void* screen);
// The image bound by a draw can be rewritten by the game: a recording in
// progress is kept only when a fingerprint of this picture holds the image's
// content (screenTapeTextureSum); gx_texture_pica.cpp.
void duskScreenTapeVolatileTexture(unsigned long long handle);
// The picture being drawn (counts frame begins).
unsigned duskScreenTapePicture(void);
// Counts of the pane-tree side (J2DScreen.cpp) for the report: screens drawn
// split, their measured children, the children that joined a run, the ones
// that changed lately, the ones that cannot be recorded, pictures in which
// what every recording of a screen depends on changed (position, graf
// context, ambient GX state), and children left out for lack of room.
enum DuskScreenTapeNote {
    duskScreenNoteSplitScreens, duskScreenNoteChildren, duskScreenNoteInRun, duskScreenNoteUnsettled,
    duskScreenNoteUnrecordable, duskScreenNoteBaseChanged, duskScreenNoteOverflow, duskScreenNoteCount
};
void duskScreenTapeNote(unsigned which);
}
