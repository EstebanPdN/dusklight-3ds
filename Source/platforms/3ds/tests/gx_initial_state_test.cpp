#include "../runtime/gx_initial_state.hpp"
#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_raster_plan.hpp"
#include "../runtime/gx_raster_color.hpp"
#include "../runtime/gx_vertex_transform.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
int main(){
    static CommandMemory memory;static CommandProcessor processor(memory);
    std::array<u8,4096> bytes{};CommandStream stream(processor,bytes);
    assert(setCommandStream(&stream));
    currentTargetExtent={640,480,400,240};assert(currentTargetExtent.valid());
    RasterPlan raster;assert(!planRaster(processor.registers(),raster));
    initializeGxDefaultState();assert(planRaster(processor.registers(),raster));
    for(unsigned i=0;i<12;++i){
        const auto at=0x500+(GX_PTIDENTITY-GX_PTTEXMTX0)*4+i;
        assert(processor.registers().xfValid[at]);
        assert(std::bit_cast<float>(processor.registers().xf[at])==(i==0||i==5||i==10?1:0));
    }
    // Reproduce the original first J2D fader: no camera, no texture and an
    // opaque black box. The sink executes every planner and vertex transform;
    // it never substitutes missing registers with an assumed default.
    const float ortho[16]{2.f/640,0,0,-1,0,-2.f/480,0,1,0,0,-.5f,-.5f,0,0,0,1};
    GXSetProjection(ortho,GX_ORTHOGRAPHIC);
    GXSetNumIndStages(0);for(unsigned i=0;i<16;++i)GXSetTevDirect(static_cast<GXTevStageID>(i));
    GXSetZCompLoc(GX_FALSE);GXSetAlphaCompare(GX_GREATER,0,GX_AOP_OR,GX_GREATER,0);
    GXSetZMode(GX_FALSE,GX_LEQUAL,GX_FALSE);GXSetTevOp(GX_TEVSTAGE0,GX_PASSCLR);
    GXSetNumChans(1);GXSetNumTevStages(1);GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR0A0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_SET);
    GXSetPixelFmt(GX_PF_RGBA6_Z24,GX_ZC_LINEAR);
    RasterError error;
    assert(!planRaster(processor.registers(),raster,&error)&&error==RasterError::pixelFormat);
    assert(planRaster(processor.registers(),raster,&error,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6));
    assert(raster.storesAlpha&&raster.writeAlpha);
    GXSetAlphaUpdate(GX_FALSE);
    assert(planRaster(processor.registers(),raster,&error,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6)&&!raster.writeAlpha);
    GXSetPixelFmt(GX_PF_RGB565_Z16,GX_ZC_LINEAR);
    assert(!planRaster(processor.registers(),raster,&error,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6)&&error==RasterError::pixelFormat);
    GXSetPixelFmt(GX_PF_RGBA6_Z24,GX_ZC_LINEAR);
    unsigned draws=0;
    processor.setSink([](const CommandDraw& draw,void* user){
        RasterPlan rp;MaterialState state;PicaMaterialPlan material;
        RasterColorPlan color;VertexTransformPlan transform;
        assert(planRaster(draw.registers,rp,nullptr,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6)&&decodeMaterial(draw.registers,state));
        assert(rp.storesAlpha&&!rp.writeAlpha);
        assert(planPicaMaterial(state,material)&&material.textureCount==0);
        assert(planRasterColor(draw.registers,material,color)&&planVertexTransform(draw.registers,0,transform));
        assert(draw.vertexCount==4&&draw.primitive==GX_QUADS);
        for(unsigned i=0;i<4;++i){
            DecodedVertex decoded;TransformedVertex vertex;std::array<float,4> rgba;
            assert(draw.vertex(i,decoded)&&transformVertex(transform,decoded,vertex));
            assert(evaluateRasterColor(color,decoded,rgba));
            assert(rgba[0]==0&&rgba[1]==0&&rgba[2]==0&&rgba[3]==1);
            for(auto f:vertex.clip)assert(std::isfinite(f));
        }
        ++*static_cast<unsigned*>(user);return true;
    },&draws);
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for(unsigned i=0;i<4;++i){GXPosition3f32(i==1||i==2?640:0,i>=2?480:0,0);GXColor1u32(255);}
    GXEnd();assert(stream.finish()&&draws==1);
    // Reset intentionally returns to unknown state until GXInit initializes it.
    assert(stream.reset()&&!planRaster(processor.registers(),raster));
    initializeGxDefaultState();assert(planRaster(processor.registers(),raster));
    assert(setCommandStream(nullptr));
    std::puts("PASS native GX default state and original untextured J2D fader command, material, color and transforms; no GPU submission");
}
