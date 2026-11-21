#ifndef J3DNATIVEINSTANCES_H
#define J3DNATIVEINSTANCES_H

// Instance batches (New 3DS port; J3DShape.cpp, m_Do_ext.cpp).
//
// An actor that draws several models of one model data in a row, each lit by
// the same tevStr (the rope bridge's planks and knots), lets the last one
// carry the others: only that one is entered, and its shape draws every model
// of the batch where the original chain of shape packets would have drawn
// them, in the same order (J3DDrawBuffer prepends each entry, so the last
// entered draws first). The models share one material and one set of
// differed register values, so the per-model work that remains is the
// matrix of each model: models of one draw get matrix slots 0, 1, ... (the
// values J3DShapeMtxConcatView::loadMtxConcatView_PNGP computes) and a list
// built once holds that many copies of the shape's list, each vertex with its
// slot's matrix index in front. The PICA runs the same shader on the same
// matrix rows, selected per vertex (gx_rigid_mt.v.pica, v4).

#if defined(DUSK_TARGET_3DS)
#include <types.h>

class J3DModel;
class J3DShape;
class J3DShapePacket;

struct J3DNativeInstances {
    enum { maxModels = 64 };
    // In the order the actor would have entered them; the last one carries.
    J3DModel* const* models;
    u16 count;
};

// What the batches did since the last report (game thread only).
struct J3DNativeInstanceStats {
    u32 draws;          // palette draws made
    u32 instances;      // models they drew
    u32 alone;          // models drawn one by one by a carrier (fallback)
    u32 listsMade;      // merged lists built
    u32 recycled;       // times the list store emptied (all lists unused)
    const char* why;    // last reason a carrier drew its models one by one
};

// A new draw phase: the batches of the last one end (their picture has been
// painted). Counts the pictures the list store ages its lists by.
void J3DNativeInstancesBeginPicture();
// Draw phase, before anything of the batch is done: `carrier` draws
// `instances` this picture. False: no room; nothing was registered.
bool J3DNativeInstancesRegister(J3DModel* carrier, const J3DNativeInstances* instances);
// The batch `model` carries this picture, or NULL.
const J3DNativeInstances* J3DNativeInstancesOf(const J3DModel* model);
// Null when every model of a batch can be drawn through `shape`'s merged
// list; else why not (a matrix layout other than one view-concatenated
// matrix, direct or matrix-index attributes, NBT, normal texture matrices).
const char* J3DNativeInstancesShapeRefusal(J3DShape* shape);
// The paint side, in place of `shape`'s drawFast for the carrier packet
// (prepareDraw done): every model of `instances` in the original order.
void J3DNativeInstancesDraw(J3DShape* shape, const J3DShapePacket& carrier, const J3DNativeInstances& instances);
J3DNativeInstanceStats& J3DNativeInstancesStats();
// Merged lists kept and the bytes they use.
void J3DNativeInstancesStore(u32& lists, u32& bytes, u32& capacity);
#endif

#endif /* J3DNATIVEINSTANCES_H */
