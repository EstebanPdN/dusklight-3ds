#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DMaterialFactory.h"
#include "JSystem/J2DGraph/J2DPictureEx.h"
#include "JSystem/J2DGraph/J2DTextBoxEx.h"
#include "JSystem/J2DGraph/J2DWindowEx.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JSupport/JSUMemoryStream.h"
#include <types.h>
#if defined(DUSK_TARGET_3DS)
#include <cstdlib>
#include <cstring>
#include <typeinfo>
#include "JSystem/J2DGraph/J2DMaterial.h"
#include "JSystem/JUtility/JUTPalette.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "../../../../platforms/3ds/runtime/gx_profile.hpp"
#include "../../../../platforms/3ds/runtime/gx_state.hpp"
#include "../../../../platforms/3ds/runtime/gx_stream.hpp"
#include "../../../../platforms/3ds/runtime/screen_tape.hpp"

// Retained 2D screens (platforms/3ds/runtime/screen_tape.hpp): the
// fingerprint of a screen. Everything the draw of the pane tree reads goes
// into it, so that an equal fingerprint means an equal picture:
//  - every pane object, whole, after draw() computed its bounds, matrix,
//    clip rectangle and alpha (J2DPane::sNativeMeasure). Only the eight J2D
//    classes are known whole; any other type makes the screen unrecordable.
//  - of a pane that is drawn: its materials (the object with its colour,
//    texgen and pixel blocks, the TEV block, the indirect block, the texture
//    matrix objects), its texture objects (object, image header, the handle
//    of the image the port holds for it) and palettes, and its text.
//  - of the graf context, the members the panes read (nativeMeasureGraf),
//    the position given to draw() and the GX mode words no pane sets
//    (scissor, viewport, projection, pixel and fog modes).
// A texture the game can rewrite goes in with a checksum of its pixels: its
// handle does not change with them.
namespace {
namespace nativegx = dusk::native::gx;
struct NativeScreenMeasure {
    u32 a = 2166136261u, b = 0x9e3779b9u;
    bool recordable = true;
    // Why not (the first cause): 1 a pane of another class, 4 an image the
    // game can rewrite that could not be read, 5 a TEV block of another kind,
    // 6 an indirect block of another kind, 7 a text with no end, 8 nested too
    // deep, 9 the graf context.
    u8 why = 0;
    u32 panes = 0, drawn = 0;
    void refuse(u8 cause) {
        if (recordable) {
            why = cause;
        }
        recordable = false;
    }
};
// Two running words, each one rotate and one add or exclusive-or per input
// word (a single ARM instruction each): a menu mixes about 100,000 words a
// picture, and the multiplying mix used first cost 13 ms of it. Both depend
// on the position of every word; nativeFinish spreads them at the end.
inline void nativeMix(NativeScreenMeasure& m, u32 word) {
    m.a = ((m.a << 5) | (m.a >> 27)) + word;
    m.b = ((m.b << 13) | (m.b >> 19)) ^ word;
}
void nativeMixBytes(NativeScreenMeasure& m, const void* data, size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    size_t i = 0;
    u32 a = m.a, b = m.b;
    if ((reinterpret_cast<uintptr_t>(bytes) & 3u) == 0) {
        // Objects are word aligned: whole words straight from memory.
        const u32* words = reinterpret_cast<const u32*>(bytes);
        const size_t count = size / 4;
        #pragma GCC unroll 4
        for (size_t n = 0; n < count; n++) {
            const u32 word = words[n];
            a = ((a << 5) | (a >> 27)) + word;
            b = ((b << 13) | (b >> 19)) ^ word;
        }
        i = count * 4;
    } else {
        for (; i + 4 <= size; i += 4) {
            u32 word;
            std::memcpy(&word, bytes + i, 4);
            a = ((a << 5) | (a >> 27)) + word;
            b = ((b << 13) | (b >> 19)) ^ word;
        }
    }
    m.a = a;
    m.b = b;
    u32 tail = u32(size) << 24;
    for (unsigned shift = 0; i < size; ++i, shift += 8) {
        tail ^= u32(bytes[i]) << shift;
    }
    nativeMix(m, tail);
}
// The fingerprint of a finished measure.
inline void nativeFinish(const NativeScreenMeasure& m, u32& a, u32& b) {
    u32 x = m.a ^ ((m.b << 16) | (m.b >> 16));
    x ^= x >> 16; x *= 0x85ebca6bu; x ^= x >> 13; x *= 0xc2b2ae35u; x ^= x >> 16;
    u32 y = m.b + m.a * 0x9e3779b1u;
    y ^= y >> 15; y *= 0x2c1b3c6du; y ^= y >> 12; y *= 0x297a2d39u; y ^= y >> 15;
    a = x;
    b = y;
}
void nativeMeasurePalette(NativeScreenMeasure& m, const JUTPalette* palette) {
    if (palette == NULL) {
        nativeMix(m, 0);
        return;
    }
    nativeMixBytes(m, palette, sizeof(JUTPalette));
    if (palette->getColorTable() != NULL && palette->getNumColors() <= 16384) {
        nativeMixBytes(m, palette->getColorTable(), size_t(palette->getNumColors()) * 2);
    }
}
void nativeMeasureTexture(NativeScreenMeasure& m, const JUTTexture* texture) {
    if (texture == NULL) {
        nativeMix(m, 0);
        return;
    }
    nativeMixBytes(m, texture, sizeof(JUTTexture));
    if (texture->getTexInfo() == NULL) {
        // No image: nothing of it can be bound (J2DTevBlock::loadTexture and
        // JUTTexture::load leave such a texture out).
        nativeMix(m, 0x6e6f696du);
        return;
    }
    const ResTIMG* const info = texture->getTexInfo();
    nativeMixBytes(m, info, sizeof(ResTIMG));
    // The image the draw binds for it (JUTTexture::storeTIMG) and what the
    // port holds for that image now. A GX object keeps the answer of the day
    // it was initialised, often before its image was first loaded, so it is
    // not asked. Pixels the game can rewrite (an item icon copied to the
    // heap, the minimap): their checksum goes in, the one the texture cache
    // compares at a bind.
    const s32 offset = info->imageOffset;
    const u8* const image = reinterpret_cast<const u8*>(info) + (offset != 0 ? offset : s32(sizeof(ResTIMG)));
    u32 token;
    nativegx::Texture registered;
    if (!nativegx::textureCommandToken(image, token) || !nativegx::textureFromCommandToken(token, registered)) {
        // The port holds nothing for this image: no draw has sampled it. A
        // material often loads a second texture no TEV stage reads (44 HUD
        // panes of a field zone were drawn in every picture for that until
        // Alpha 8 t13). The first draw that samples it registers it: its
        // handle then changes this fingerprint, and a recording made while
        // it was unknown is refused if the image can be rewritten
        // (duskScreenTapeVolatileTexture).
        nativeMix(m, 0x756e7265u);
        nativeMeasurePalette(m, texture->getPalette());
        return;
    }
    if (!registered.immutablePixels) {
        std::uint64_t sum = 0;
        if (!dusk::native::screenTapeTextureSum(registered, sum)) {
            m.refuse(4);
            return;
        }
        nativeMix(m, u32(sum));
        nativeMix(m, u32(sum >> 32));
    }
    nativeMix(m, u32(registered.handle));
    nativeMix(m, u32(u64(registered.handle) >> 32));
    nativeMeasurePalette(m, texture->getPalette());
}
void nativeMeasureMaterial(NativeScreenMeasure& m, J2DMaterial* material) {
    if (material == NULL) {
        nativeMix(m, 0);
        return;
    }
    nativeMixBytes(m, material, sizeof(J2DMaterial));
    for (u32 i = 0; i < 8; i++) {
        if (const J2DTexMtx* texMtx = material->getTexGenBlock()->nativeTexMtx(i)) {
            nativeMixBytes(m, texMtx, sizeof(J2DTexMtx));
        }
    }
    if (J2DTevBlock* tev = material->getTevBlock()) {
        size_t size;
        u32 textures;
        switch (tev->getType()) {
        case 'TVB1': size = sizeof(J2DTevBlock1); textures = 1; break;
        case 'TVB2': size = sizeof(J2DTevBlock2); textures = 2; break;
        case 'TVB4': size = sizeof(J2DTevBlock4); textures = 4; break;
        case 'TVB8': size = sizeof(J2DTevBlock8); textures = 8; break;
        case 'TV16': size = sizeof(J2DTevBlock16); textures = 8; break;
        default: m.refuse(5); return;
        }
        nativeMixBytes(m, tev, size);
        for (u32 i = 0; i < textures; i++) {
            nativeMeasureTexture(m, tev->getTexture(i));
            nativeMeasurePalette(m, tev->getPalette(i));
        }
    }
    if (J2DIndBlock* ind = material->getIndBlock()) {
        switch (ind->getType()) {
        case 'IBLN': break;
        case 'IBLF': nativeMixBytes(m, ind, sizeof(J2DIndBlockFull)); break;
        default: m.refuse(6); return;
        }
    }
}
void nativeMeasureText(NativeScreenMeasure& m, const J2DTextBox& box) {
    const TextSpan text = box.getStringPtr();
    if (text.buffer == NULL) {
        nativeMix(m, 0);
        return;
    }
    // A text box holds a C string in a buffer of text.size bytes (the box
    // itself gives 0xffff for a string it does not own). One with no end in
    // reach is not something to keep a recording of.
    const size_t reach = text.size != 0 && text.size < 4096 ? text.size : 4096;
    const void* end = std::memchr(text.buffer, 0, reach);
    if (end == NULL) {
        m.refuse(7);
        return;
    }
    nativeMixBytes(m, text.buffer, size_t(static_cast<const char*>(end) - text.buffer) + 1);
}
// The measuring pass and, for a screen with split panes, the drawing pass.
//
// A screen that is recorded whole is one recording. A screen that keeps
// changing (a pulsing cursor, a counter) or holds something that cannot be
// recorded (a texture the game rewrites) is split: the child subtrees of its
// root pane are then measured one by one, and consecutive children that have
// not changed lately form a run, which is one recording. A child that just
// changed is drawn as it is, between the runs before and after it; one that
// keeps changing and has children is split in turn. So a picture replays a
// few recordings and draws only the panes that move.
struct NativeScreenPass {
    // Fingerprint of what every recording of this screen depends on: the
    // position, the graf context and the ambient GX state.
    NativeScreenMeasure base;
    // The fingerprints being measured: [0] the screen itself, then one per
    // nested child of a split pane being visited.
    NativeScreenMeasure stack[16];
    unsigned depth = 0;
    // Children measured below a child that is not split: their marks are not
    // followed (the child is one piece: measured and drawn whole).
    unsigned sealed = 0;
    // Measured children of split panes, in the order their subtrees ended.
    struct Child {
        const J2DPane* pane;
        u32 a, b;           // its fingerprint (run start: of the whole run)
        u16 runFirst;       // index of the first child of its run
        u16 runLast;        // run start only: index of the last child
        u8 state;           // run start only, drawing pass: 0 draw, 1 recording, 2 replayed
        bool inRun;         // settled and recordable: part of a run
        bool sealedHere;    // this child sealed its subtree
    };
    Child children[384];
    unsigned childCount = 0;
    // The open run of each nesting level: the split pane it belongs to and
    // its first child (0xffff: none).
    const J2DPane* runParent[16] = {};
    u16 runOpen[16] = {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
                       0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
    // Where the drawing pass found its last child (they come in the same order).
    unsigned cursor = 0;
    bool overflow = false;
};
// Panes whose children are separate recordings. The marks outlive their
// panes; a new pane at a marked address is split from its first draw, which
// costs a few more recordings.
const J2DPane* nativeSplitPanes[256];
unsigned nativeSplitCount = 0;
inline unsigned nativeSplitHome(const J2DPane* pane) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(pane);
    return unsigned((at >> 4) ^ (at >> 12)) & 255u;
}
bool nativeIsSplit(const J2DPane* pane) {
    if (nativeSplitCount == 0) {
        return false;
    }
    for (unsigned n = 0, at = nativeSplitHome(pane); n < 256; n++, at = (at + 1) & 255u) {
        if (nativeSplitPanes[at] == pane) return true;
        if (nativeSplitPanes[at] == NULL) return false;
    }
    return false;
}
// False when the table is full (the pane then rests instead).
bool nativeMarkSplit(const J2DPane* pane) {
    if (nativeSplitCount >= 192) {
        return false;
    }
    for (unsigned n = 0, at = nativeSplitHome(pane); n < 256; n++, at = (at + 1) & 255u) {
        if (nativeSplitPanes[at] == pane) return true;
        if (nativeSplitPanes[at] == NULL) {
            nativeSplitPanes[at] = pane;
            ++nativeSplitCount;
            return true;
        }
    }
    return false;
}
// A whole-screen recording keeps changing: split the screen when it has
// children, otherwise let it rest (drawn plainly for a while).
void nativeKeepsChanging(J2DPane* pane) {
    if (pane->getFirstChildPane() == NULL || !nativeMarkSplit(pane)) {
        duskScreenTapeRest(pane);
    }
}
// What each child of a split pane looked like in the last picture, to tell
// the ones that stand still from the ones that move. `changes` rises by 8
// with every changed picture and falls by one with every unchanged one: a
// child joins a run when it is back at zero (8 quiet pictures after a
// change), and one at 32 or more (changed in four recent pictures) keeps
// changing. A pane new to the table starts as just changed.
//
// Two places per pane: with one, two panes sharing it displaced each other in
// every picture and neither ever stood still (a menu has several hundred
// children, and the file select screen drew twice the quads it needed). The
// table lives in the C heap, so the program's static size stays as it is.
struct NativeHistory {
    const J2DPane* pane;
    u32 a, b;
    u16 seen;  // the picture it was last asked in
    u8 changes;
};
constexpr unsigned nativeHistorySize = 1024;
NativeHistory* nativeHistory;
NativeHistory* nativeHistoryEntry(const J2DPane* pane, u16 picture, bool& found) {
    if (nativeHistory == NULL) {
        nativeHistory = static_cast<NativeHistory*>(std::calloc(nativeHistorySize, sizeof(NativeHistory)));
        if (nativeHistory == NULL) {
            return NULL;
        }
    }
    const u32 at = u32(reinterpret_cast<uintptr_t>(pane));
    NativeHistory& first = nativeHistory[((at >> 4) ^ (at >> 13)) & (nativeHistorySize - 1)];
    NativeHistory& second = nativeHistory[(((at >> 3) * 0x9e3779b1u) >> 22) & (nativeHistorySize - 1)];
    found = true;
    if (first.pane == pane) return &first;
    if (second.pane == pane) return &second;
    found = false;
    // The place of a pane no picture asked for lately (gone or hidden), then
    // of the one that changes more: a pane standing still keeps its place.
    if (first.pane == NULL || u16(picture - first.seen) > 2) return &first;
    if (second.pane == NULL || u16(picture - second.seen) > 2) return &second;
    return first.changes >= second.changes ? &first : &second;
}
bool nativeSettled(const J2DPane* pane, u32 a, u32 b, bool& keepsChanging) {
    keepsChanging = false;
    const u16 picture = u16(duskScreenTapePicture());
    bool found = false;
    NativeHistory* const h = nativeHistoryEntry(pane, picture, found);
    if (h == NULL) {
        return false;
    }
    h->seen = picture;
    if (!found) {
        h->pane = pane;
        h->a = a;
        h->b = b;
        h->changes = 2;
        return false;
    }
    if (h->a == a && h->b == b) {
        if (h->changes != 0) {
            --h->changes;
        }
        return h->changes == 0;
    }
    h->a = a;
    h->b = b;
    h->changes = h->changes > 247 ? 255 : u8(h->changes + 8);
    keepsChanging = h->changes >= 32;
    return false;
}
// The members of a graf context the panes read: the bounds and the scissor
// box (J2DPane::draw, J2DOrthoGraph::scissorBounds), the position matrix
// handed to every drawSelf, and the ortho box of an orthographic one. A
// context is often built on the stack, and its constructor leaves the pen
// position, the blend triples, the projection matrix (until setPort) and the
// padding as that memory was: mixed whole, the fingerprint of an unchanged
// screen changed with whatever ran before (in the field zones every picture,
// so nothing was ever replayed there). The projection reaches the panes
// through the XF words (nativeMeasureAmbient).
void nativeMeasureGraf(NativeScreenMeasure& m, const J2DGrafContext* graf, bool ortho) {
    nativeMixBytes(m, &graf->mBounds, sizeof(graf->mBounds));
    nativeMixBytes(m, &graf->mScissorBounds, sizeof(graf->mScissorBounds));
    nativeMixBytes(m, &graf->mPosMtx, sizeof(graf->mPosMtx));
    if (ortho) {
        nativeMixBytes(m, static_cast<const J2DOrthoGraph*>(graf)->getOrtho(), sizeof(JGeometry::TBox2<f32>));
    }
}
// Report count: pictures in which what every recording of a screen depends
// on is not what it was in the picture before.
struct NativeBase {
    const J2DScreen* screen;
    u32 a, b;
};
NativeBase nativeBases[16];
void nativeNoteBase(const J2DScreen* screen, const NativeScreenMeasure& base) {
    u32 a, b;
    nativeFinish(base, a, b);
    NativeBase* place = &nativeBases[(reinterpret_cast<uintptr_t>(screen) >> 4) & 15u];
    for (NativeBase& entry : nativeBases) {
        if (entry.screen == screen) {
            place = &entry;
            break;
        }
    }
    if (place->screen == screen && (place->a != a || place->b != b)) {
        duskScreenTapeNote(duskScreenNoteBaseChanged);
    }
    place->screen = screen;
    place->a = a;
    place->b = b;
}
void nativeMeasurePane(J2DPane* pane, bool drawn, void* context) {
    NativeScreenPass& pass = *static_cast<NativeScreenPass*>(context);
    NativeScreenMeasure& m = pass.stack[pass.depth];
    ++m.panes;
    nativeMix(m, drawn);
    // One type_info object per class in this program: the addresses compare.
    const std::type_info* const type = &typeid(*pane);
    if (type == &typeid(J2DPictureEx)) {
        nativeMixBytes(m, pane, sizeof(J2DPictureEx));
        if (drawn) {
            ++m.drawn;
            nativeMeasureMaterial(m, static_cast<J2DPictureEx*>(pane)->getMaterial());
        }
    } else if (type == &typeid(J2DPane)) {
        nativeMixBytes(m, pane, sizeof(J2DPane));
    } else if (type == &typeid(J2DTextBoxEx)) {
        J2DTextBoxEx* box = static_cast<J2DTextBoxEx*>(pane);
        nativeMixBytes(m, pane, sizeof(J2DTextBoxEx));
        if (drawn) {
            ++m.drawn;
            nativeMeasureMaterial(m, box->getMaterial());
            nativeMeasureText(m, *box);
        }
    } else if (type == &typeid(J2DWindowEx)) {
        J2DWindowEx* window = static_cast<J2DWindowEx*>(pane);
        nativeMixBytes(m, pane, sizeof(J2DWindowEx));
        if (drawn) {
            ++m.drawn;
            for (u8 i = 0; i < 4; i++) {
                nativeMeasureMaterial(m, window->getFrameMaterial(i));
            }
            nativeMeasureMaterial(m, window->getContentsMaterial());
        }
    } else if (type == &typeid(J2DPicture)) {
        J2DPicture* picture = static_cast<J2DPicture*>(pane);
        nativeMixBytes(m, pane, sizeof(J2DPicture));
        if (drawn) {
            ++m.drawn;
            // Its own palette is attached to these textures (measured there).
            for (u8 i = 0; i < 2; i++) {
                nativeMeasureTexture(m, picture->getTexture(i));
            }
        }
    } else if (type == &typeid(J2DTextBox)) {
        J2DTextBox* box = static_cast<J2DTextBox*>(pane);
        nativeMixBytes(m, pane, sizeof(J2DTextBox));
        if (drawn) {
            ++m.drawn;
            nativeMeasureText(m, *box);
        }
    } else if (type == &typeid(J2DWindow)) {
        J2DWindow* window = static_cast<J2DWindow*>(pane);
        nativeMixBytes(m, pane, sizeof(J2DWindow));
        if (drawn) {
            ++m.drawn;
            for (u8 i = 0; i < 4; i++) {
                nativeMeasureTexture(m, window->getFrameTexture(i, 0));
            }
            nativeMeasureTexture(m, window->getContentsTexture(0));
        }
    } else if (type == &typeid(J2DScreen)) {
        nativeMixBytes(m, pane, sizeof(J2DScreen));
        if (drawn) {
            ++m.drawn;
        }
    } else {
        m.refuse(1);
    }
}
// The children of split panes that are drawn in every picture because they
// cannot be recorded: the first 24 of them, with the cause.
void nativeReportRefused(const J2DPane* child, const NativeScreenMeasure& m) {
    static const J2DPane* seen[24];
    static unsigned count = 0;
    if (count >= ARRAY_SIZEU(seen)) {
        return;
    }
    for (unsigned n = 0; n < count; n++) {
        if (seen[n] == child) {
            return;
        }
    }
    seen[count++] = child;
    const u64 tag = child->getUserInfo();
    OSReport("Native retained screens: pane %p (tag %08lx%08lx, %lu panes, %lu drawn, %s children) not recordable, cause %u\n", child,
             static_cast<unsigned long>(tag >> 32), static_cast<unsigned long>(tag), static_cast<unsigned long>(m.panes),
             static_cast<unsigned long>(m.drawn), const_cast<J2DPane*>(child)->getFirstChildPane() != NULL ? "with" : "no", unsigned(m.why));
}
// A mark is followed only down a chain of split panes from the screen.
bool nativeSplitHook(J2DPane* pane, void* context) {
    const NativeScreenPass& pass = *static_cast<const NativeScreenPass*>(context);
    return pass.sealed == 0 && nativeIsSplit(pane);
}
// The nesting level a child of a split pane is measured or drawn from, and
// that level's open run when it belongs to another pane.
inline unsigned nativeRunLevel(NativeScreenPass& pass, const J2DPane* parent) {
    const unsigned level = pass.depth < ARRAY_SIZEU(pass.runOpen) ? pass.depth : ARRAY_SIZEU(pass.runOpen) - 1;
    if (pass.runParent[level] != parent) {
        pass.runParent[level] = parent;
        pass.runOpen[level] = 0xffff;
    }
    return level;
}
// Measuring pass: one child subtree of a split pane starts its own fingerprint.
int nativeMeasureChildBegin(J2DPane* child, void* context) {
    NativeScreenPass& pass = *static_cast<NativeScreenPass*>(context);
    if (pass.depth + 1 >= ARRAY_SIZEU(pass.stack)) {
        // Too deep to keep apart: measured into the enclosing fingerprint,
        // which is then not recordable (its panes are drawn every picture).
        pass.stack[pass.depth].refuse(8);
        pass.overflow = true;
        duskScreenTapeNote(duskScreenNoteOverflow);
        return 0;
    }
    pass.stack[++pass.depth] = pass.base;
    // A child that is not split is one piece.
    if (!nativeIsSplit(child)) {
        ++pass.sealed;
        return 3;
    }
    return 1;
}
void nativeMeasureChildEnd(J2DPane* child, int begun, void* context) {
    NativeScreenPass& pass = *static_cast<NativeScreenPass*>(context);
    if (begun != 1 && begun != 3) {
        return;
    }
    const bool sealedHere = begun == 3;
    if (sealedHere) {
        --pass.sealed;
    }
    const NativeScreenMeasure m = pass.stack[pass.depth--];
    const unsigned level = nativeRunLevel(pass, child->getParentPane());
    if (pass.childCount >= ARRAY_SIZEU(pass.children)) {
        pass.overflow = true;
        pass.runOpen[level] = 0xffff;
        duskScreenTapeNote(duskScreenNoteOverflow);
        return;
    }
    duskScreenTapeNote(duskScreenNoteChildren);
    const u16 index = u16(pass.childCount++);
    NativeScreenPass::Child& entry = pass.children[index];
    entry.pane = child;
    nativeFinish(m, entry.a, entry.b);
    entry.runFirst = index;
    entry.runLast = index;
    entry.state = 0;
    entry.sealedHere = sealedHere;
    bool keepsChanging = false;
    const bool settled = nativeSettled(child, entry.a, entry.b, keepsChanging);
    // Something in it keeps changing, or cannot be recorded (a texture the
    // game rewrites): its children are told apart from the next picture on,
    // so that only the one holding it is drawn every time.
    if ((keepsChanging || !m.recordable) && sealedHere && pass.base.recordable && child->getFirstChildPane() != NULL) {
        nativeMarkSplit(child);
    }
    entry.inRun = sealedHere && settled && m.recordable;
    if (!entry.inRun) {
        pass.runOpen[level] = 0xffff;
        if (sealedHere) {
            duskScreenTapeNote(m.recordable ? duskScreenNoteUnsettled : duskScreenNoteUnrecordable);
            if (!m.recordable) {
                nativeReportRefused(child, m);
            }
        }
        return;
    }
    duskScreenTapeNote(duskScreenNoteInRun);
    if (pass.runOpen[level] == 0xffff) {
        pass.runOpen[level] = index;
        return;
    }
    // Joins the open run: the run's fingerprint takes this child's.
    NativeScreenPass::Child& first = pass.children[pass.runOpen[level]];
    entry.runFirst = pass.runOpen[level];
    first.runLast = index;
    first.a = ((first.a << 7) | (first.a >> 25)) + entry.a * 0x9e3779b1u;
    first.b = ((first.b << 11) | (first.b >> 21)) ^ (entry.b + 0x85ebca6bu);
}
NativeScreenPass::Child* nativeFindChild(NativeScreenPass& pass, const J2DPane* child) {
    unsigned at = pass.cursor;
    for (unsigned n = 0; n < pass.childCount; ++n) {
        if (pass.children[at].pane == child) {
            pass.cursor = at;
            return &pass.children[at];
        }
        if (++at == pass.childCount) {
            at = 0;
        }
    }
    return NULL;
}
// Drawing pass: the first child of a run replays or starts the run's
// recording; the others follow it. 4: the child is one piece (sealed) and
// is drawn; 5: a split child, drawn through its own children.
int nativeDrawChildBegin(J2DPane* child, void* context) {
    NativeScreenPass& pass = *static_cast<NativeScreenPass*>(context);
    NativeScreenPass::Child* const entry = nativeFindChild(pass, child);
    if (entry == NULL) {
        // Not measured apart (table full, too deep): one piece, drawn.
        ++pass.sealed;
        return 4;
    }
    if (!entry->inRun) {
        if (entry->sealedHere) {
            ++pass.sealed;
            return 4;
        }
        return 5;
    }
    NativeScreenPass::Child& first = pass.children[entry->runFirst];
    if (entry == &first) {
        // The run's owner is its first child; its length is part of the key.
        first.state = u8(duskScreenTapeBegin(child, first.a + (first.runLast - entry->runFirst), first.b, 1));
    }
    if (first.state == 2) {
        return 2;
    }
    ++pass.sealed;
    return 4;
}
void nativeDrawChildEnd(J2DPane* child, int begun, void* context) {
    NativeScreenPass& pass = *static_cast<NativeScreenPass*>(context);
    if (begun == 4) {
        --pass.sealed;
    }
    if (begun == 5) {
        return;
    }
    NativeScreenPass::Child* const entry = nativeFindChild(pass, child);
    if (entry == NULL || !entry->inRun) {
        return;
    }
    NativeScreenPass::Child& first = pass.children[entry->runFirst];
    if (first.state == 1 && entry == &pass.children[first.runLast]) {
        // Runs are made of children that stand still: a run that changes
        // re-forms by itself, so its misses ask for nothing.
        duskScreenTapeEnd();
        first.state = 0;
    }
}
// GX state the panes draw under and do not set themselves. A word the bank
// does not know mixes as such.
void nativeMeasureAmbient(NativeScreenMeasure& m, const nativegx::CommandRegisters& r) {
    auto bp = [&](unsigned reg) {
        nativeMix(m, r.bpValid[reg] ? r.bp[reg] : 0xa5000000u | reg);
    };
    auto xf = [&](unsigned at) {
        nativeMix(m, r.xfValid[at] ? r.xf[at] : 0x5a000000u | at);
    };
    bp(0x00);
    for (unsigned reg = 0x20; reg <= 0x27; reg++) bp(reg);
    for (unsigned reg = 0x40; reg <= 0x45; reg++) bp(reg);
    bp(0x59);
    // Fog: off (type 0 in bits 21-23 of the last word but one) reads nothing else.
    const bool fog = r.bpValid[0xf1] && ((r.bp[0xf1] >> 21) & 7) != 0;
    if (fog || !r.bpValid[0xf1]) {
        for (unsigned reg = 0xee; reg <= 0xf2; reg++) bp(reg);
    } else {
        nativeMix(m, 0xf1f1f1f1u);
    }
    bp(0xf3);
    xf(0x1005);
    for (unsigned at = 0x101a; at <= 0x1026; at++) xf(at);
    nativeMix(m, r.bpMask);
}
// The screen about to be drawn. Returns 0: draw the tree; 1: draw it and end
// the whole-screen recording; 2: the whole screen was replayed; 3: the screen
// is split, draw the tree with `pass` as the hooks' context.
int nativeScreenTapeBegin(J2DScreen* screen, f32 x, f32 y, const J2DGrafContext* grafCtx, NativeScreenPass& pass,
                          J2DPane::NativeScreenHooks& hooks) {
    if (dusk::native::screenTapeMode() == dusk::native::ScreenTapeMode::off || J2DPane::sNativeHooks != NULL) {
        return 0;
    }
    nativegx::CommandStream* stream = nativegx::commandStream();
    if (stream == NULL || !stream->finish()) {
        return 0;
    }
    static const auto measureIndex = dusk::native::registerGxNamedProfile("j2d.screenMeasure");
    {
        dusk::native::GxNamedProfileScope scope(measureIndex);
        NativeScreenMeasure& base = pass.base;
        nativeMixBytes(base, &x, sizeof(x));
        nativeMixBytes(base, &y, sizeof(y));
        switch (grafCtx->getGrafType()) {
        case 0:
            if (&typeid(*grafCtx) != &typeid(J2DGrafContext)) base.refuse(9);
            else nativeMeasureGraf(base, grafCtx, false);
            break;
        case 1:
            if (&typeid(*grafCtx) != &typeid(J2DOrthoGraph)) base.refuse(9);
            else nativeMeasureGraf(base, grafCtx, true);
            break;
        default: base.refuse(9); break;
        }
        nativeMix(base, u32(grafCtx->getGrafType()));
        nativeMeasureAmbient(base, stream->registers());
        nativeNoteBase(screen, base);
        pass.stack[0] = base;
        hooks = {nativeMeasurePane, nativeSplitHook, nativeMeasureChildBegin, nativeMeasureChildEnd, &pass};
        J2DPane::sNativeHooks = &hooks;
        screen->J2DPane::draw(x, y, grafCtx, screen->mScissor, true);
        J2DPane::sNativeHooks = NULL;
    }
    if (nativeIsSplit(screen)) {
        // Each child subtree asks for itself while the tree is drawn.
        hooks = {NULL, nativeSplitHook, nativeDrawChildBegin, nativeDrawChildEnd, &pass};
        duskScreenTapeNote(duskScreenNoteSplitScreens);
        return 3;
    }
    static const auto tapeIndex = dusk::native::registerGxNamedProfile("j2d.screenTape");
    dusk::native::GxNamedProfileScope scope(tapeIndex);
    const NativeScreenMeasure& m = pass.stack[0];
    // As for a child (nativeMeasureChildEnd): a screen that cannot be
    // recorded whole is split from the next picture on.
    if (!m.recordable && pass.base.recordable && screen->getFirstChildPane() != NULL) {
        nativeMarkSplit(screen);
    }
    u32 a, b;
    nativeFinish(m, a, b);
    return duskScreenTapeBegin(screen, a, b, m.recordable && !pass.overflow);
}
}  // namespace
#endif

J2DScreen::J2DScreen()
    : J2DPane(NULL, true, 'root', JGeometry::TBox2<f32>(JGeometry::TVec2<f32>(0, 0), JGeometry::TVec2<f32>(640, 480))), mColor() {
    field_0x4 = -1;
    mScissor = false;
    mMaterialNum = 0;
    mMaterials = NULL;
    mTexRes = NULL;
    mFontRes = NULL;
    mNameTable = NULL;
}

J2DScreen::~J2DScreen() {
#if defined(DUSK_TARGET_3DS)
    if (dusk::native::gx::commandStream() != NULL) {
        duskScreenTapeForget(this);
    }
#endif
    clean();
}

void J2DScreen::clean() {
    JKR_DELETE_ARRAY(mMaterials);
    mMaterialNum = 0;
    mMaterials = NULL;

    JKR_DELETE_ARRAY(mTexRes);
    mTexRes = NULL;

    JKR_DELETE_ARRAY(mFontRes);
    mFontRes = NULL;

    if (mNameTable != NULL) {
        JKR_DELETE_ARRAY(const_cast<ResNTAB*>(mNameTable->getResNameTable()));
        JKR_DELETE(mNameTable);
        mNameTable = NULL;
    }
}

bool J2DScreen::setPriority(char const* resName, u32 param_1, JKRArchive* p_archive) {
    if (p_archive == NULL) {
        return false;
    }

    void* res = JKRGetNameResource(resName, p_archive);
    if (res != NULL) {
        JSUMemoryInputStream stream(res, p_archive->getExpandedResSize(res));
        return setPriority(&stream, param_1, p_archive);
    }

    return false;
}

bool J2DScreen::setPriority(JSURandomInputStream* p_stream, u32 param_1, JKRArchive* p_archive) {
    if (p_archive == NULL) {
        return false;
    } else {
        return private_set(p_stream, param_1, p_archive);
    }
}

bool J2DScreen::private_set(JSURandomInputStream* p_stream, u32 param_1, JKRArchive* p_archive) {
    if (!checkSignature(p_stream)) {
        return false;
    }

    if (!getScreenInformation(p_stream)) {
        return false;
    }

    bool make_end = makeHierarchyPanes(this, p_stream, param_1, p_archive) != 2;

    if (!(param_1 & 0x1F0000)) {
        clean();
    }

    return make_end ? p_stream->isGood() : false;
}

bool J2DScreen::checkSignature(JSURandomInputStream* p_stream) {
    J2DScrnHeader header;
    p_stream->read(&header, sizeof(J2DScrnHeader));

    if (header.mTag != 'SCRN' || (header.mType != 'blo1' && header.mType != 'blo2')) {
        JUT_WARN(257, "%s", "SCRN resource is broken.\n")
        return false;
    } else {
        return true;
    }
}

bool J2DScreen::getScreenInformation(JSURandomInputStream* p_stream) {
    J2DScrnInfoHeader info;
    p_stream->read(&info, sizeof(J2DScrnInfoHeader));

    if (info.mTag != 'INF1') {
        JUT_WARN(282, "%s", "SCRN resource is broken.\n")
        return false;
    }

    place(JGeometry::TBox2<f32>(0.0f, 0.0f, info.mWidth, info.mHeight));

    mColor = info.mColor;

    if (info.mSize > 0x10) {
        p_stream->skip(info.mSize - 0x10);
    }

    return true;
}

s32 J2DScreen::makeHierarchyPanes(J2DPane* p_basePane, JSURandomInputStream* p_stream, u32 param_2,
                                  JKRArchive* p_archive) {
    J2DPane* next_pane = p_basePane;

    while (true) {
        J2DScrnBlockHeader header;
        p_stream->peek(&header, sizeof(J2DScrnBlockHeader));

        switch (header.mTag) {
        case 'EXT1':
            p_stream->seek(header.mSize, JSUStreamSeekFrom_CUR);
            return 1;
        case 'BGN1': {
            p_stream->seek(header.mSize, JSUStreamSeekFrom_CUR);

            s32 ret = makeHierarchyPanes(next_pane, p_stream, param_2, p_archive);
            if (ret != 0) {
                return ret;
            }
            break;
        }
        case 'END1':
            p_stream->seek(header.mSize, JSUStreamSeekFrom_CUR);
            return 0;
        case 'TEX1':
            if ((mTexRes = getResReference(p_stream, param_2)) == NULL) {
                return 2;
            }
            break;
        case 'FNT1':
            if ((mFontRes = getResReference(p_stream, param_2)) == NULL) {
                return 2;
            }
            break;
        case 'MAT1':
            if (!createMaterial(p_stream, param_2, p_archive)) {
                return 2;
            }
            break;
        default:
            if (p_archive == NULL) {
                next_pane = createPane(header, p_stream, p_basePane, param_2);
            } else {
                next_pane = createPane(header, p_stream, p_basePane, param_2, p_archive);
            }

            if (next_pane == NULL) {
                return 2;
            }
            break;
        }
    }
}

J2DPane* J2DScreen::createPane(J2DScrnBlockHeader const& header, JSURandomInputStream* p_stream,
                               J2DPane* p_basePane, u32 param_3, JKRArchive* p_archive) {
    J2DPane* newPane = NULL;

    switch (header.mTag) {
    case 'PAN1':
        newPane = JKR_NEW J2DPane(p_basePane, p_stream, 0);
        break;
    case 'WIN1':
        newPane = JKR_NEW J2DWindow(p_basePane, p_stream, p_archive);
        break;
    case 'PIC1':
        newPane = JKR_NEW J2DPicture(p_basePane, p_stream, p_archive);
        break;
    case 'TBX1':
        newPane = JKR_NEW J2DTextBox(p_basePane, p_stream, p_archive);
        break;
    case 'PAN2':
        newPane = JKR_NEW J2DPane(p_basePane, p_stream, 1);
        break;
    case 'WIN2':
        if (param_3 & 0x1F0000) {
            newPane = JKR_NEW J2DWindowEx(p_basePane, p_stream, param_3, mMaterials);
            break;
        }
        newPane = JKR_NEW J2DWindow(p_basePane, p_stream, mMaterials);
        break;
    case 'PIC2':
        if (param_3 & 0x1F0000) {
            newPane = JKR_NEW J2DPictureEx(p_basePane, p_stream, param_3, mMaterials);
            break;
        }
        newPane = JKR_NEW J2DPicture(p_basePane, p_stream, mMaterials);
        break;
    case 'TBX2':
        if (param_3 & 0x1F0000) {
            newPane = JKR_NEW J2DTextBoxEx(p_basePane, p_stream, param_3, mMaterials);
            break;
        }
        newPane = JKR_NEW J2DTextBox(p_basePane, p_stream, param_3, mMaterials);
        break;
    default:
        JUT_WARN(446, "%s", "unknown pane");
        s32 position = p_stream->getPosition();
        s32 size = header.mSize;
        s32 start = size + position;

        newPane = JKR_NEW J2DPane(p_basePane, p_stream, 0);
        p_stream->seek(start, JSUStreamSeekFrom_SET);
        break;
    }

    return newPane;
}

void J2DScreen::draw(f32 x, f32 y, J2DGrafContext const* grafCtx) {
    u32 l_x, l_y, width, height;

    if (mScissor) {
        GXGetScissor(&l_x, &l_y, &width, &height);
    }

#if defined(DUSK_TARGET_3DS)
    // Retained screens: an unchanged screen replays the GPU commands of its
    // last recording instead of drawing its panes (see the measure above).
    // The closing state below is always set for real.
    NativeScreenPass nativePass;
    J2DPane::NativeScreenHooks nativeHooks;
    const int nativeTape = grafCtx != NULL ? nativeScreenTapeBegin(this, x, y, grafCtx, nativePass, nativeHooks) : 0;
    if (nativeTape == 3) {
        J2DPane::sNativeHooks = &nativeHooks;
    }
    if (nativeTape != 2) {
#endif
    if (grafCtx != NULL) {
        J2DPane::draw(x, y, grafCtx, mScissor, true);
    } else {
        J2DOrthoGraph ortho(0.0f, 0.0f, 640.0f, 480.0f, -1.0f, 1.0f);
        ortho.setPort();
        J2DPane::draw(x, y, &ortho, mScissor, true);
    }
#if defined(DUSK_TARGET_3DS)
    }
    if (nativeTape == 3) {
        J2DPane::sNativeHooks = NULL;
    }
    if (nativeTape == 1) {
        static const auto tapeEndIndex = dusk::native::registerGxNamedProfile("j2d.screenTape");
        dusk::native::GxNamedProfileScope scope(tapeEndIndex);
        if (duskScreenTapeEnd()) {
            nativeKeepsChanging(this);
        }
    }
#endif

    if (mScissor) {
        GXSetScissor(l_x, l_y, width, height);
    }

    GXSetNumIndStages(0);
    for (int i = 0; i < 0x10; i++) {
        GXSetTevDirect((GXTevStageID)i);
    }
    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetVtxDesc(GX_VA_TEX0, GX_NONE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetNumTexGens(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                  GX_AF_NONE);
    for (int i = 0; i < 4; i++) {
        GXSetTevSwapModeTable((GXTevSwapSel)i, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    }
}

J2DPane* J2DScreen::search(u64 tag) {
    if (tag == 0) {
        return NULL;
    }

    return J2DPane::search(tag);
}

J2DPane* J2DScreen::searchUserInfo(u64 tag) {
    if (tag == 0) {
        return NULL;
    }

    return J2DPane::searchUserInfo(tag);
}

void J2DScreen::drawSelf(f32 param_0, f32 param_1, Mtx* param_2) {
    GX_AND_TRACY_SCOPED("J2DScreen::drawSelf")

    JUtility::TColor color(mColor);
    u8 alpha = (color.a * mAlpha) / 255;

    if (alpha == 0) {
        return;
    }

    color = JUtility::TColor(alpha | ((u32)color & 0xFFFFFF00));

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);

    GXPosition3f32(0.0f, 0.0f, 0.0f);
    GXColor1u32(color);

    GXPosition3f32(getWidth(), 0.0f, 0.0f);
    GXColor1u32(color);

    GXPosition3f32(getWidth(), getHeight(), 0.0f);
    GXColor1u32(color);

    GXPosition3f32(0.0f, getHeight(), 0.0f);
    GXColor1u32(color);
    GXEnd();

    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
}

J2DResReference* J2DScreen::getResReference(JSURandomInputStream* p_stream, u32 param_1) {
    s32 position = p_stream->getPosition();
    p_stream->skip(4);

    s32 size1, size2;
    size1 = p_stream->readS32();
    p_stream->skip(4);
    size2 = p_stream->readS32();
    p_stream->seek(position + size2, JSUStreamSeekFrom_SET);

    size1 = size1 - size2;

    char* buffer;
    if (param_1 & 0x1F0000) {
        buffer = JKR_NEW_ARRAY(char, size1);
    } else {
        buffer = JKR_NEW_ARRAY_ARGS(char, size1, -4);
    }

    if (buffer != NULL) {
        p_stream->read(buffer, size1);
    }

    return (J2DResReference*)buffer;
}

bool J2DScreen::createMaterial(JSURandomInputStream* p_stream, u32 param_1, JKRArchive* p_archive) {
    s32 position = p_stream->getPosition();

    J2DScrnBlockHeader header;
    p_stream->read(&header, 8);
    mMaterialNum = p_stream->readU16();

    p_stream->skip(2);

    if (param_1 & 0x1F0000) {
        mMaterials = JKR_NEW_ARRAY(J2DMaterial, mMaterialNum);
    } else {
        mMaterials = JKR_NEW_ARRAY_ARGS(J2DMaterial, mMaterialNum, -4);
    }

    u8* buffer = JKR_NEW_ARRAY_ARGS(u8, header.mSize, -4);
    if (mMaterials != NULL && buffer != NULL) {
        J2DMaterialBlock* pBlock = (J2DMaterialBlock*)buffer;
        p_stream->seek(position, JSUStreamSeekFrom_SET);
        p_stream->read(buffer, header.mSize);
        J2DMaterialFactory factory(*pBlock);

        for (u16 i = 0; i < mMaterialNum; i++) {
            factory.create(&mMaterials[i], i, param_1, mTexRes, mFontRes, p_archive);
        }

        if (param_1 & 0x1F0000) {
            u32 offset =
                buffer[0x14] << 0x18 | buffer[0x15] << 0x10 | buffer[0x16] << 8 | buffer[0x17];
            ResNTAB* sec_s = (ResNTAB*)(buffer + offset);
            u16 entryNum = sec_s->mEntryNum;
            u16 lastOffset = sec_s->mEntries[entryNum - 1].mOffs;
            char* ptr = (char*)sec_s;
            u16 size = lastOffset;
            while (ptr[size] != 0) {
                size++;
            }
            size++;

            u8* nametab = JKR_NEW_ARRAY(u8, size);
            if (nametab == NULL) {
                goto failure;
            }
            for (u16 i = 0; i < size; i++) {
                nametab[i] = (buffer + offset)[i];
            }

            mNameTable = JKR_NEW JUTNameTab((ResNTAB*)nametab);
            if (mNameTable == NULL) {
                JKR_DELETE_ARRAY(nametab);
                goto failure;
            }
        }

    success:
        JKR_DELETE_ARRAY(buffer);
        return true;
    }

failure:
    JKR_DELETE_ARRAY(buffer);
    clean();
    return false;
}

bool J2DScreen::isUsed(ResTIMG const* p_timg) {
    return J2DPane::isUsed(p_timg);
}

bool J2DScreen::isUsed(ResFONT const* p_font) {
    return J2DPane::isUsed(p_font);
}

DUSK_GAME_DATA J2DDataManage* J2DScreen::mDataManage;

void* J2DScreen::getNameResource(char const* resName) {
    void* res = JKRGetNameResource(resName, NULL);

    if (res == NULL && mDataManage != NULL) {
        res = mDataManage->get(resName);
    }

    return res;
}

void J2DScreen::animation() {
    animationPane(mTransform);

    for (u16 i = 0; i < mMaterialNum; i++) {
        mMaterials[i].animation();
    }
}

void J2DScreen::setAnimation(J2DAnmColor* p_anmColor) {
    p_anmColor->searchUpdateMaterialID(this);
    u16 matNum = p_anmColor->getUpdateMaterialNum();

    for (u16 i = 0; i < matNum; i++) {
        u16 matID = p_anmColor->getUpdateMaterialID(i);
        if (matID < mMaterialNum) {
            mMaterials[matID].setAnimation(p_anmColor);
        }
    }
}

void J2DScreen::setAnimation(J2DAnmTextureSRTKey* p_anmSRTKey) {
    p_anmSRTKey->searchUpdateMaterialID(this);
    u16 matNum = p_anmSRTKey->getUpdateMaterialNum();

    for (u16 i = 0; i < matNum; i++) {
        u16 matID = p_anmSRTKey->getUpdateMaterialID(i);
        if (matID < mMaterialNum) {
            mMaterials[matID].setAnimation(p_anmSRTKey);
        }
    }
}

void J2DScreen::setAnimation(J2DAnmTexPattern* p_anmPattern) {
    p_anmPattern->searchUpdateMaterialID(this);
    u16 matNum = p_anmPattern->getUpdateMaterialNum();

    for (u16 i = 0; i < matNum; i++) {
        u16 matID = p_anmPattern->getUpdateMaterialID(i);
        if (matID < mMaterialNum) {
            mMaterials[matID].setAnimation(p_anmPattern);
        }
    }
}

void J2DScreen::setAnimation(J2DAnmTevRegKey* p_anmRegKey) {
    p_anmRegKey->searchUpdateMaterialID(this);

    u16 cMatNum = p_anmRegKey->getCRegUpdateMaterialNum();
    for (u16 i = 0; i < cMatNum; i++) {
        u16 matID = p_anmRegKey->getCRegUpdateMaterialID(i);
        if (matID < mMaterialNum) {
            mMaterials[matID].setAnimation(p_anmRegKey);
        }
    }

    u16 kMatNum = p_anmRegKey->getKRegUpdateMaterialNum();
    for (u16 i = 0; i < kMatNum; i++) {
        u16 matID = p_anmRegKey->getKRegUpdateMaterialID(i);
        if (matID < mMaterialNum) {
            mMaterials[matID].setAnimation(p_anmRegKey);
        }
    }
}

void J2DScreen::setAnimation(J2DAnmVtxColor* p_anmVtxColor) {
    J2DPane::setVtxColorAnimation(p_anmVtxColor);
}

void J2DScreen::setAnimation(J2DAnmVisibilityFull* p_anmVisibility) {
    J2DPane::setVisibileAnimation(p_anmVisibility);
}

J2DPane* J2DScreen::createPane(J2DScrnBlockHeader const& header, JSURandomInputStream* p_stream,
                               J2DPane* p_basePane, u32 param_3) {
    return createPane(header, p_stream, p_basePane, param_3, NULL);
}

u16 J2DScreen::getTypeID() const {
    return 8;
}

void J2DScreen::calcMtx() {
    makeMatrix(mTranslateX, mTranslateY);
}

void J2DScreen::setAnimation(J2DAnmBase* p_anm) {
    J2DPane::setAnimation(p_anm);
}

void J2DScreen::setAnimationVF(J2DAnmVisibilityFull*) {
    /* empty function */
}

void J2DScreen::setAnimationVC(J2DAnmVtxColor*) {
    /* empty function */
}
