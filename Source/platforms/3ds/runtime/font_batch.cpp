#include "font_batch.hpp"
#include <dolphin/gx.h>
namespace dusk::native {
void FontBatch::flush(){
    if(!count)return;
    GXBegin(GX_QUADS,GX_VTXFMT0,count*4);
    for(unsigned i=0;i<count;++i){const auto& q=quads[i];
        GXPosition3f32(q.left,q.top,0);GXColor1u32(q.colors[0]);GXTexCoord2u16(q.uv[0],q.uv[1]);
        GXPosition3f32(q.right,q.top,0);GXColor1u32(q.colors[1]);GXTexCoord2u16(q.uv[2],q.uv[1]);
        GXPosition3f32(q.right,q.bottom,0);GXColor1u32(q.colors[3]);GXTexCoord2u16(q.uv[2],q.uv[3]);
        GXPosition3f32(q.left,q.bottom,0);GXColor1u32(q.colors[2]);GXTexCoord2u16(q.uv[0],q.uv[3]);
    }
    GXEnd();count=0;
}
}
