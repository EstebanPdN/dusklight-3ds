#include "gx_command_probe.hpp"
#include "gx_command_draw.hpp"
#include "runtime/gx_state.hpp"
#include "runtime/gx_stream.hpp"
#include <dolphin/gd.h>
#include <JSystem/J3DGraphBase/J3DGD.h>
#include <dolphin/gx.h>
#include <array>
#include <bit>
namespace dusk::native {
namespace {
using namespace gx;
CommandMemory memory;
CommandProcessor processor(memory);
std::array<u8,256> scratch;
CommandStream stream(processor,scratch);
CommandDrawContext context;
std::array<std::array<u8,96>,3> textures{};
std::array<TextureHandle,3> handles{};
GXTexObj objects[3];
alignas(32) std::array<u8,64> textureList{};
std::array<u8,163> list{}; // opcode/count + four 40-byte vertices
bool ready;
bool configure() {
    const float projection[]{.005f,0,0,-1,0,-2.0f/240,0,1,0,0,.5,-.5,0,0,0,1};
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0};
    GXSetProjection(projection,GX_ORTHOGRAPHIC);GXSetViewport(0,0,400,240,0,1);
    GXLoadPosMtxImm(identity,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
    GXSetScissor(0,0,400,240);GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_COPY);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);GXSetZCompLoc(GX_FALSE);GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,0,GX_DF_NONE,GX_AF_NONE);
    GXSetNumTexGens(3);GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    for(unsigned i=0;i<3;++i){GXSetVtxDesc(static_cast<GXAttr>(GX_VA_TEX0+i),GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0,static_cast<GXAttr>(GX_VA_TEX0+i),GX_TEX_ST,GX_F32,0);
        GXSetTexCoordGen2(static_cast<GXTexCoordID>(i),GX_TG_MTX2x4,static_cast<GXTexGenSrc>(GX_TG_TEX0+i),GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);}
    GXSetFog(GX_FOG_NONE,0,0,0,0,{0,0,0,255});GXSetFogRangeAdj(GX_FALSE,320,nullptr);
    GXSetColorUpdate(GX_TRUE);GXSetDither(GX_FALSE);GXSetCoPlanar(GX_FALSE);GXSetClipMode(GX_CLIP_ENABLE);
    for(unsigned i=0;i<8;++i)GXEnableTexOffsets(static_cast<GXTexCoordID>(i),GX_FALSE,GX_FALSE);
    GXInvalidateVtxCache();GXFlush();
    if (!stream.finish()) return false;
    if (!stream.writeBp(0x41,processor.registers().bp[0x41]|16)) return false;
    for(unsigned reg:{0x42u,0xf1u,0xf5u})if (!stream.writeBp(reg,0)) return false;
    alignas(32) u8 commands[1024];GDLObj gd;GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
    GDSetGenMode2(3,1,3,0,GX_CULL_NONE);
    for(unsigned i=0;i<4;++i)GDSetTevSwapModeTable(static_cast<GXTevSwapSel>(i),GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    GDSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD2,GX_TEXMAP0,GX_COLOR0A0,GX_TEXCOORD0,GX_TEXMAP1,GX_COLOR0A0);
    GDSetTevOrder(GX_TEVSTAGE2,GX_TEXCOORD1,GX_TEXMAP2,GX_COLOR0A0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);
    for(unsigned i=0;i<3;++i){auto stage=static_cast<GXTevStageID>(i);
        GDSetTevColorCalc(stage,GX_CC_ZERO,GX_CC_TEXC,i?GX_CC_CPREV:GX_CC_RASC,GX_CC_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        GDSetTevAlphaCalcAndSwap(stage,GX_CA_ZERO,GX_CA_TEXA,i?GX_CA_APREV:GX_CA_RASA,GX_CA_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV,GX_TEV_SWAP0,GX_TEV_SWAP0);
        GDSetTevDirect(stage);}
    const auto bytes=GDGetCurrOffset();GDSetCurrent(nullptr);if (!processor.execute({commands,bytes})) return false;
    return true;
}
}
bool initializeCommandProbe() {
    if (ready) return true;
    for (unsigned i=0;i<3;++i) {
        auto& t=textures[i];
        t[0]=1; t[3]=t[5]=8; t[24]=1; t[31]=32;
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
            const auto offset=32+(y/4)*32+(y%4)*8+x;
            const bool bright=i==0 ? x<4 : i==1 ? y<4 : (x<4)==(y<4);
            t[offset]=bright ? 255 : 128;
        }
        if (!registerTexture(t,handles[i])) { destroyCommandProbe(); return false; }
        GXInitTexObj(&objects[i],t.data()+32,8,8,GX_TF_I8,GX_CLAMP,GX_CLAMP,GX_FALSE);
    }
    GDLObj imageCommands;GDInitGDLObj(&imageCommands,textureList.data(),textureList.size());GDSetCurrent(&imageCommands);
    for(unsigned i=0;i<3;++i){const auto map=static_cast<GXTexMapID>(i);
        J3DGDSetTexImgPtr(map,textures[i].data()+32);
        J3DGDSetTexImgAttr(map,8,8,GX_TF_I8);
        J3DGDSetTexLookupMode(map,GX_CLAMP,GX_CLAMP,GX_LINEAR,GX_LINEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    }
    const auto textureBytes=GDGetCurrOffset();GDSetCurrent(nullptr);
    if(textureBytes!=60){destroyCommandProbe();return false;}
    if (!setCommandStream(&stream)) { destroyCommandProbe(); return false; }
    const bool configured=configure();
    const bool detached=setCommandStream(nullptr);
    if (!configured || !detached) { destroyCommandProbe(); return false; }
    processor.setSink(drawGxCommand,&context);
    std::size_t cursor=0;
    list[cursor++]=GX_QUADS; list[cursor++]=0; list[cursor++]=4;
    auto scalar=[&](float f) {
        auto bits=std::bit_cast<u32>(f);
        for (int shift=24;shift>=0;shift-=8) list[cursor++]=bits>>shift;
    };
    for (unsigned i=0;i<4;++i) {
        const float s=i==1||i==2 ? 1.f : 0.f, t=i>=2 ? 1.f : 0.f;
        scalar(20+360*s); scalar(20+200*t); scalar(0);
        for (u8 value : {u8(64),u8(128),u8(192),u8(255)}) list[cursor++]=value;
        scalar(s); scalar(t); scalar(t); scalar(s); scalar(1-s); scalar(t);
    }
    ready=cursor==list.size();
    return ready;
}
bool drawCommandProbe() {
    if (!ready || !setCommandStream(&stream)) return false;
    bool loaded=true;
    if(context.submitted%2) loaded=stream.call(textureList);
    else for (unsigned i=0;i<3;++i) GXLoadTexObj(&objects[i],static_cast<GXTexMapID>(i));
    bool drawn=false;
    if(loaded){
        // Same synthetic three-map material through raw, fixed, auto and indexed
        // emission. Visual output must remain identical across all four modes.
        const auto mode=context.submitted%4;
        if(mode==0)drawn=stream.call(list);
        else{
            const u16 indices[]{0,1,2,0,2,3};
            if(mode==3)GXBeginIndexed(GX_VTXFMT0,4,indices,6);
            else GXBegin(GX_QUADS,GX_VTXFMT0,mode==2?GX_AUTO:4);
            for(unsigned i=0;i<4;++i){
                const float s=i==1||i==2?1.f:0.f,t=i>=2?1.f:0.f;
                GXPosition3f32(20+360*s,20+200*t,0);GXColor4u8(64,128,192,255);
                GXTexCoord2f32(s,t);GXTexCoord2f32(t,s);GXTexCoord2f32(1-s,t);
            }
            GXEnd();drawn=true;
        }
    }
    const bool okay=loaded && drawn && stream.finish();
    const bool detached=setCommandStream(nullptr);
    return okay && detached;
}
void destroyCommandProbe() {
    for (unsigned i=0;i<3;++i) if (handles[i]) {
        GXDestroyTexObj(&objects[i]); unregisterTexture(handles[i]); handles[i]=0;
    }
    ready=false;
}
CommandProbeStatistics commandProbeStatistics() { return {context.failure,context.submitted,context.rejected}; }
}
