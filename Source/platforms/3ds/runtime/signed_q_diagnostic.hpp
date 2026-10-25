// Explicit default-off trial: bounded exact-bit diagnostics, no per-frame logging.
#pragma once
#include "../gx_command_draw.hpp"
#include "gx_commands.hpp"
#include "gx_pica_material.hpp"
#include <string_view>
extern "C" void OSReport(const char*,...);
namespace dusk::native {
inline bool parseSignedQDiagnostic(std::string_view text,bool& enabled){
    enabled=false;
    if(text=="enabled=0\n"||text=="enabled=0\r\n")return true;
    if(text=="enabled=1\n"||text=="enabled=1\r\n"){enabled=true;return true;}
    return false;
}
inline void reportSignedQTrial(const gx::CommandDraw& draw,const gx::PicaMaterialPlan& m,const SignedQDrawSample& s){
    OSReport("SIGNED_Q_ATTEMPT v=%u ix=%u neg=%u mixedtri=%u coord=%u map=%u target=%u,%u,%u,%u texgen=%08x post=%08x stages=%u textures=%u primitive=%u\n",
        s.vertices,s.indices,s.negativeVertices,s.mixedTriangles,s.coordinate,s.map,
        s.target[0],s.target[1],s.target[2],s.target[3],unsigned(s.texgen[0]),unsigned(s.texgen[1]),unsigned(m.stageCount),unsigned(m.textureCount),unsigned(draw.primitive));
    for(unsigned u=0;u<m.textureCount;++u)OSReport("SIGNED_Q_BINDING unit=%u map=%u coord=%u\n",u,unsigned(m.textures[u].map),unsigned(m.textures[u].coordinate));
    // Original bounded TEV words identify the material independently of pointer
    // addresses. The adapter hook never receives scene names or filters on them.
    const unsigned stages=((draw.registers.bp[0]>>10)&15)+1;
    for(unsigned t=0;t<stages;++t)OSReport("SIGNED_Q_MATERIAL stage=%u order=%08x rgb=%08x alpha=%08x indirect=%08x\n",
        t,unsigned(draw.registers.bp[0x28+t/2]),unsigned(draw.registers.bp[0xc0+2*t]),unsigned(draw.registers.bp[0xc1+2*t]),unsigned(draw.registers.bp[0x10+t]));
    for(unsigned n=0;n<s.samples;++n){const auto& v=s.vertex[n];
        OSReport("SIGNED_Q_VERTEX index=%u selector=%u clipbits=%08x,%08x,%08x,%08x transformedSTQbits=%08x,%08x,%08x actualUQVQQbits=%08x,%08x,%08x\n",
            v.index,v.textureMatrix,unsigned(v.clipBits[0]),unsigned(v.clipBits[1]),unsigned(v.clipBits[2]),unsigned(v.clipBits[3]),
            unsigned(v.transformedBits[0]),unsigned(v.transformedBits[1]),unsigned(v.transformedBits[2]),unsigned(v.gpuBits[0]),unsigned(v.gpuBits[1]),unsigned(v.gpuBits[2]));
        const unsigned post=s.texgen[1]&63;
        // Identity selector 60 takes q from the texgen input; post 61 is identity.
        // The row/type and normalize bit are retained in the texgen/post words.
        if(v.textureMatrix<60){const unsigned base=4*v.textureMatrix+8;
            OSReport("SIGNED_Q_SOURCE qrow=%08x,%08x,%08x,%08x\n",unsigned(draw.registers.xf[base]),unsigned(draw.registers.xf[base+1]),unsigned(draw.registers.xf[base+2]),unsigned(draw.registers.xf[base+3]));}
        if(post!=61){const unsigned base=0x500+4*post+8;
            OSReport("SIGNED_Q_SOURCE postqrow=%08x,%08x,%08x,%08x\n",unsigned(draw.registers.xf[base]),unsigned(draw.registers.xf[base+1]),unsigned(draw.registers.xf[base+2]),unsigned(draw.registers.xf[base+3]));}
    }
}
} // namespace dusk::native
