#include "gx_initial_state.hpp"
#include "gx_state.hpp"
#include "gx_stream.hpp"
#include "gx_target_extent.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>

namespace dusk::native::gx {
namespace {
void initializeSdkDefaultTexture(){
    // Original GXInit.c DefaultTexData: sixteen IA8 words 0xffff, loaded
    // into all eight maps. This is a real, bounded process-lifetime image,
    // not a fallback for an unknown image encountered by a later draw.
    alignas(32) static const std::array<u8,64> bti=[]{
        std::array<u8,64> value{};value[0]=GX_TF_IA8;value[3]=value[5]=4;
        value[20]=value[21]=1;value[24]=1;value[31]=32;
        for(unsigned i=32;i<64;++i)value[i]=255;
        return value;
    }();
    static GXTexObj object{};
    u32 token{};Texture texture{};
    if(!textureCommandToken(bti.data()+32,token)){
        TextureHandle handle{};
        if(!registerTexture(bti,handle,true)||!textureCommandToken(bti.data()+32,token))
            OSPanic(__FILE__,__LINE__,"Native GX SDK default texture registration failed");
    }
    if(!textureFromCommandToken(token,texture)||texture.bti.data()!=bti.data()||
       !texture.immutablePixels||texture.width!=4||texture.height!=4||texture.format!=GX_TF_IA8)
        OSPanic(__FILE__,__LINE__,"Native GX SDK default texture identity mismatch");
    GXInitTexObj(&object,bti.data()+32,4,4,GX_TF_IA8,GX_CLAMP,GX_CLAMP,GX_FALSE);
    for(unsigned map=0;map<8;++map)GXLoadTexObj(&object,static_cast<GXTexMapID>(map));
}
}
void initializeGxDefaultState(){
    if(!commandStream()||!commandStream()->idle()||!currentTargetExtent.valid())
        OSPanic(__FILE__,__LINE__,"Native GX initialization requires an idle stream and target");
    // Retain the original baseline when the experiment is OFF. Automatic SU
    // needs the original SDK's explicit initial sampler state, not invented
    // dimensions when a later draw/flush encounters a missing typed binding.
    if(commandStream()->automaticSuEnabled())initializeSdkDefaultTexture();
    // Match the pinned original initialization without PPC MMIO/cache addresses.
    const GXColor black{0,0,0,0},white{255,255,255,255};
    const float identity[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    const float projection[7]{1,0,1,0,-1,-2,0};
    GXClearVtxDesc();
    for(unsigned f=0;f<8;++f){
        const auto format=static_cast<GXVtxFmt>(f);
        GXSetVtxAttrFmt(format,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
        GXSetVtxAttrFmt(format,GX_VA_NRM,GX_NRM_XYZ,GX_F32,0);
        GXSetVtxAttrFmt(format,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
        GXSetVtxAttrFmt(format,GX_VA_CLR1,GX_CLR_RGBA,GX_RGBA8,0);
        for(unsigned i=0;i<8;++i)GXSetVtxAttrFmt(format,static_cast<GXAttr>(GX_VA_TEX0+i),GX_TEX_ST,GX_F32,0);
    }
    GXLoadPosMtxImm(identity,GX_PNMTX0);GXLoadNrmMtxImm(identity,GX_PNMTX0);
    GXLoadTexMtxImm(identity,GX_IDENTITY,GX_MTX3x4);
    GXLoadTexMtxImm(identity,GX_PTIDENTITY,GX_MTX3x4);
    GXSetCurrentMtx(GX_PNMTX0);GXSetProjectionv(projection);
    GXSetViewport(0,0,currentTargetExtent.logicalWidth,currentTargetExtent.logicalHeight,0,1);
    GXSetScissor(0,0,currentTargetExtent.logicalWidth,currentTargetExtent.logicalHeight);
    GXSetCoPlanar(GX_FALSE);GXSetCullMode(GX_CULL_BACK);GXSetClipMode(GX_CLIP_ENABLE);
    GXSetLineWidth(6,GX_TO_ZERO);GXSetPointSize(6,GX_TO_ZERO);
    GXSetNumChans(0);
    for(auto channel:{GX_COLOR0A0,GX_COLOR1A1}){
        GXSetChanCtrl(channel,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetChanAmbColor(channel,black);GXSetChanMatColor(channel,white);
    }
    for(unsigned i=0;i<8;++i){
        GXSetTexCoordGen(static_cast<GXTexCoordID>(i),GX_TG_MTX2x4,static_cast<GXTexGenSrc>(GX_TG_TEX0+i),GX_IDENTITY);
        GXEnableTexOffsets(static_cast<GXTexCoordID>(i),GX_FALSE,GX_FALSE);
    }
    GXSetNumTexGens(1);GXSetNumTevStages(1);GXSetNumIndStages(0);
    for(unsigned i=0;i<16;++i){
        const auto stage=static_cast<GXTevStageID>(i);
        GXSetTevOrder(stage,i<8?static_cast<GXTexCoordID>(i):GX_TEXCOORD_NULL,
                      i<8?static_cast<GXTexMapID>(i):GX_TEXMAP_NULL,i<8?GX_COLOR0A0:GX_COLOR_NULL);
        GXSetTevKColorSel(stage,GX_TEV_KCSEL_1_4);GXSetTevKAlphaSel(stage,GX_TEV_KASEL_1);
        GXSetTevSwapMode(stage,GX_TEV_SWAP0,GX_TEV_SWAP0);GXSetTevDirect(stage);
    }
    GXSetTevOp(GX_TEVSTAGE0,GX_REPLACE);
    GXSetTevSwapModeTable(GX_TEV_SWAP0,GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP1,GX_CH_RED,GX_CH_RED,GX_CH_RED,GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2,GX_CH_GREEN,GX_CH_GREEN,GX_CH_GREEN,GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3,GX_CH_BLUE,GX_CH_BLUE,GX_CH_BLUE,GX_CH_ALPHA);
    for(unsigned i=0;i<4;++i)GXSetIndTexCoordScale(static_cast<GXIndTexStageID>(i),GX_ITS_1,GX_ITS_1);
    GXSetFog(GX_FOG_NONE,0,1,.1f,1,black);GXSetFogRangeAdj(GX_FALSE,0,nullptr);
    GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GXSetZTexture(GX_ZT_DISABLE,GX_TF_Z8,0);GXSetDstAlpha(GX_FALSE,0);
    GXSetBlendMode(GX_BM_NONE,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
    GXSetColorUpdate(GX_TRUE);GXSetAlphaUpdate(GX_TRUE);GXSetDither(GX_TRUE);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);GXSetZCompLoc(GX_TRUE);
    GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);
}
}
