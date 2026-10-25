#include "helpers/batch.hpp"

#if !defined(DUSK_TARGET_3DS)
#include "dusk/logging.h"
#else
#include <dolphin/os.h>
#endif

#include <aurora/dl.hpp>
#include <dolphin/gx/GXEnum.h>

namespace batch {

void decode_leaf_template(const u8* dl, u32 size, LeafTemplate& out) {
    out.vtxCount = 0;
    out.posRefCount = 0;
    bool posSeen[256] = {};

    static constexpr GXVtxDescList kLeafDesc[] = {
        {GX_VA_POS, GX_INDEX8},
        {GX_VA_NRM, GX_INDEX8},
        {GX_VA_CLR0, GX_INDEX8},
        {GX_VA_TEX0, GX_INDEX8},
        {GX_VA_NULL, GX_NONE},
    };

    aurora::gx::dl::Reader reader{dl, size, kLeafDesc};
    while (const auto cmd = reader.next()) {
        if (cmd->kind == aurora::gx::dl::Command::Kind::Passthrough) {
            if (cmd->data[0] != GX_NOP) {
#if defined(DUSK_TARGET_3DS)
                OSPanic(__FILE__, __LINE__, "decode_leaf_template: unexpected opcode 0x%x", cmd->data[0]);
#else
                DuskLog.fatal("decode_leaf_template: unexpected opcode {:#x}", cmd->data[0]);
#endif
            }
            continue;
        }
        if (cmd->kind != aurora::gx::dl::Command::Kind::Draw) {
#if defined(DUSK_TARGET_3DS)
            OSPanic(__FILE__, __LINE__, "decode_leaf_template: unexpected pre-optimized draw");
#else
            DuskLog.fatal("decode_leaf_template: unexpected pre-optimized draw");
#endif
        }

        const auto& draw = cmd->draw;
        bool overflow = false;
        const bool expanded =
            aurora::gx::dl::expand_triangles(draw.prim, draw.vtxCount, [&](u16 i0, u16 i1, u16 i2) {
                if (overflow || out.vtxCount + 3 > LeafTemplate::kMaxVtx) {
                    overflow = true;
                    return;
                }
                for (const u16 elem : {i0, i1, i2}) {
                    LeafTemplate::Vtx& v = out.vtx[out.vtxCount++];
                    v.pos = draw.attr_idx(elem, GX_VA_POS);
                    v.nrm = draw.attr_idx(elem, GX_VA_NRM);
                    v.clr = draw.attr_idx(elem, GX_VA_CLR0);
                    v.tex = draw.attr_idx(elem, GX_VA_TEX0);
                    if (!posSeen[v.pos]) {
                        posSeen[v.pos] = true;
                        if (out.posRefCount >= LeafTemplate::kMaxPosRefs) {
                            overflow = true;
                            return;
                        }
                        out.posRefs[out.posRefCount++] = v.pos;
                    }
                }
            });
        if (!expanded) {
#if defined(DUSK_TARGET_3DS)
            OSPanic(__FILE__, __LINE__, "decode_leaf_template: untriangulable draw (primitive 0x%x, %u vertices)",
                static_cast<u32>(draw.prim), draw.vtxCount);
#else
            DuskLog.fatal("decode_leaf_template: untriangulable draw (prim {:#x}, {} verts)",
                static_cast<u32>(draw.prim), draw.vtxCount);
#endif
        }
        if (overflow) {
#if defined(DUSK_TARGET_3DS)
            OSPanic(__FILE__, __LINE__, "decode_leaf_template: overflow (%u vertices, %u positions)",
                out.vtxCount, out.posRefCount);
#else
            DuskLog.fatal("decode_leaf_template: template overflow ({} verts, {} positions)",
                out.vtxCount, out.posRefCount);
#endif
        }
    }
    if (reader.failed()) {
#if defined(DUSK_TARGET_3DS)
        OSPanic(__FILE__, __LINE__, "decode_leaf_template: failed to walk display list");
#else
        DuskLog.fatal("decode_leaf_template: failed to walk display list");
#endif
    }
}

}  // namespace batch
