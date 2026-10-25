#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_raster_plan.hpp"
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <limits>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){
    char actual[512];va_list args;va_start(args,message);std::vsnprintf(actual,sizeof(actual),message,args);va_end(args);
    throw std::runtime_error(actual);
}
extern "C" void DCFlushRange(void*,u32){}
u32 OriginalGXGetTexBufferSize(u16,u16,u32,GXBool,u8);
u16 OriginalGXGetNumXfbLines(u16,f32);
f32 OriginalGXGetYScaleFactor(u16,u16);
void OriginalPixelReset(u32,u32,u32,u32);
u32 OriginalPixelRegister(unsigned);
void OriginalGXSetAlphaUpdate(bool);
void OriginalGXSetDstAlpha(bool,u8);
void OriginalGXSetPixelFmt(GXPixelFmt,GXZFmt16);
template<class F> void rejected(F action){bool failed=false;try{action();}catch(const std::runtime_error&){failed=true;}assert(failed);}
struct Capture {unsigned draws=0,count=0,indices=0;std::array<DecodedVertex,16> vertices{};std::array<u16,12> triangles{};};
bool sink(const CommandDraw& draw,void* pointer){
    auto& c=*static_cast<Capture*>(pointer);++c.draws;c.count=draw.vertexCount;c.indices=draw.indexCount;
    assert(c.count<=c.vertices.size()&&c.indices<=c.triangles.size());
    for(unsigned i=0;i<c.count;++i)if(!draw.vertex(i,c.vertices[i]))return false;
    for(unsigned i=0;i<c.indices;++i)if(!draw.triangleIndex(i,c.triangles[i]))return false;
    return true;
}
void format(){
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_NRM,GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_POS,GX_POS_XY,GX_F32,0);
    GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_NRM,GX_NRM_XYZ,GX_F32,0);
    GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_TEX0,GX_TEX_ST,GX_S16,8);
}
void vertex(unsigned i){GXPosition2f32(float(i),-float(i));GXNormal3f32(0,0,1);GXColor4u8(16+i,32,48,64);GXTexCoord2s16(-128,256);}
int main(){
    CommandMemory memory;CommandProcessor p(memory);std::array<u8,256> scratch{};CommandStream stream(p,scratch);Capture capture;
    assert(setCommandStream(&stream));p.setSink(sink,&capture);format();
    GXBegin(GX_TRIANGLES,GX_VTXFMT3,3);for(unsigned i=0;i<3;++i)vertex(i);GXEnd();
    assert(stream.idle()&&capture.draws==1&&capture.count==3);
    for(unsigned i=0;i<3;++i){auto& v=capture.vertices[i];assert(v.position[0]==float(i)&&v.position[1]==-float(i)&&v.position[2]==0);
        assert(v.normalCount==1&&v.normal[0][2]==1&&v.texcoord[0][0]==-.5f&&v.texcoord[0][1]==1);
        assert(std::abs(v.color[0][0]-(16+i)/255.f)<1e-6&&std::abs(v.color[0][3]-64/255.f)<1e-6);}
    GXBegin(GX_TRIANGLES,GX_VTXFMT3,GX_AUTO);assert(stream.recordingAuto()&&!stream.idle());
    for(unsigned i=0;i<3;++i)vertex(i);assert(capture.draws==1);GXEnd();
    assert(stream.idle()&&capture.draws==2&&capture.count==3);
    GXBegin(GX_TRIANGLES,GX_VTXFMT3,GX_AUTO);GXEnd();assert(stream.idle()&&capture.draws==2);
    GXBegin(GX_TRIANGLES,GX_VTXFMT3,0);GXEnd();assert(capture.draws==2);
    u16 indices[]{0,1,2,0,2,3};GXBeginIndexed(GX_VTXFMT3,4,indices,6);
    indices[1]=3;for(unsigned i=0;i<4;++i)vertex(i);GXEnd();
    assert(capture.draws==3&&capture.count==4&&capture.indices==6&&capture.triangles[1]==1&&capture.triangles[5]==3);
    // Three-coordinate positions and float UVs use the same packed decoder.
    GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    GXBegin(GX_TRIANGLES,GX_VTXFMT3,3);
    for(unsigned i=0;i<3;++i){GXPosition3f32(i,2,3);GXNormal3f32(1,0,0);GXColor1u32(0x11223344);GXTexCoord2f32(.25f,.75f);}GXEnd();
    assert(capture.draws==4&&capture.vertices[2].position[2]==3&&capture.vertices[2].texcoord[0][1]==.75f);
    // Complete enough vertices, but no GXEnd, is still an unfinished auto draw.
    format();GXBegin(GX_TRIANGLES,GX_VTXFMT3,GX_AUTO);for(unsigned i=0;i<3;++i)vertex(i);
    rejected([&]{GXSetColorUpdate(GX_TRUE);});assert(capture.draws==4&&stream.error()==StreamError::truncated);
    assert(stream.reset());p.setSink(sink,&capture);format();
    GXBegin(GX_TRIANGLES,GX_VTXFMT3,3);GXPosition2f32(0,0);rejected([&]{GXEnd();});assert(stream.error()==StreamError::truncated);
    assert(stream.reset());format();GXBegin(GX_TRIANGLES,GX_VTXFMT3,GX_AUTO);GXPosition2f32(0,0);rejected([&]{GXEnd();});
    assert(stream.reset());format();GXBegin(GX_TRIANGLES,GX_VTXFMT3,GX_AUTO);
    for(unsigned i=0;i<12;++i)vertex(i);GXEnd();
    assert(stream.idle()&&capture.draws==6&&capture.count==3&&capture.vertices[0].position[0]==9);
    assert(stream.reset());format();GXBegin(GX_TRIANGLES,GX_VTXFMT3,GX_AUTO);
    assert(!setCommandStream(nullptr));assert(stream.reset());
    // Pixel state compares directly against original GDSetFog command words.
    alignas(32) std::array<u8,64> gdBytes{};GDLObj gd;unsigned fogCases=0;
    const unsigned modes[]{0,2,4,5,6,7,10,12,13,14,15};
    const std::array<std::array<float,4>,7> cases{{{100,800,1,1000},{100,800,100,1000},{50,50,1,1000},{100,800,1,1},{100,800,0,1000},{800,100,1,1000},{-100,800,1,1000}}};
    for(unsigned mode:modes)for(auto values:cases){
        assert(stream.reset());GXSetFog(static_cast<GXFogType>(mode),values[0],values[1],values[2],values[3],{12,34,56,255});
        const auto expected=p.registers();CommandProcessor original(memory);
        GDInitGDLObj(&gd,gdBytes.data(),gdBytes.size());GDSetCurrent(&gd);
        GDSetFog(static_cast<GXFogType>(mode),values[0],values[1],values[2],values[3],{12,34,56,255});
        const auto count=GDGetCurrOffset();GDSetCurrent(nullptr);assert(original.execute({gdBytes.data(),count}));
        for(unsigned reg=0xee;reg<=0xf2;++reg)assert(original.registers().bp[reg]==expected.bp[reg]);++fogCases;
    }
    assert(stream.reset());GXFogAdjTable range{};for(unsigned i=0;i<10;++i)range.r[i]=256+i*100;
    GXSetFogRangeAdj(GX_TRUE,320,&range);assert(p.registers().bp[0xe8]==(662u|1024));
    for(unsigned i=0;i<5;++i)assert(p.registers().bp[0xe9+i]==(range.r[2*i]|(u32(range.r[2*i+1])<<12)));
    const auto previous=p.registers().bp[0xe9];GXSetFogRangeAdj(GX_FALSE,320,nullptr);assert(p.registers().bp[0xe8]==662&&p.registers().bp[0xe9]==previous);
    GXSetFogColor({1,2,3,4});assert(p.registers().bp[0xf2]==0x010203);
    assert(stream.writeBp(0x41,0xa5a5a5));GXSetColorUpdate(GX_FALSE);GXSetDither(GX_TRUE);assert(p.registers().bp[0x41]==((0xa5a5a5u&~8u)|4u));
    assert(stream.writeBp(0,0x12345));assert(stream.writeBp(0xfe,1));GXSetCoPlanar(GX_TRUE);
    assert(p.registers().bp[0]==(0x12345|(1u<<19))&&p.registers().bpMask==0xffffff);
    GXSetCoPlanar(GX_FALSE);assert(p.registers().bp[0]==0x12345);
    GXSetClipMode(GX_CLIP_ENABLE);assert(p.registers().xfValid[0x1005]&&p.registers().xf[0x1005]==0);
    GXSetClipMode(GX_CLIP_DISABLE);assert(p.registers().xf[0x1005]==1);
    for(unsigned i=0;i<8;++i){assert(stream.writeBp(0x30+2*i,0x12345));GXEnableTexOffsets(static_cast<GXTexCoordID>(i),GX_TRUE,GX_FALSE);assert(p.registers().bp[0x30+2*i]==((0x12345u&~(3u<<18))|(1u<<18)));}
    const auto epoch=p.registers().vertexCacheEpoch;GXInvalidateVtxCache();GXFlush();assert(stream.idle()&&p.registers().vertexCacheEpoch==epoch+1);
    rejected([&]{GXSetFog(static_cast<GXFogType>(3),0,1,0,1,{});});
    rejected([&]{GXSetFog(GX_FOG_PERSP_LIN,0,1,2,1,{});});
    rejected([&]{GXSetFog(GX_FOG_PERSP_LIN,0,std::numeric_limits<float>::quiet_NaN(),0,1,{});});
    rejected([&]{GXSetFogRangeAdj(GX_TRUE,320,nullptr);});range.r[9]=4096;rejected([&]{GXSetFogRangeAdj(GX_TRUE,320,&range);});
    rejected([&]{GXEnableTexOffsets(static_cast<GXTexCoordID>(8),GX_FALSE,GX_FALSE);});
    CommandRegisters raster{};for(unsigned reg:{0u,0x20u,0x21u,0x40u,0x41u,0x42u,0x43u,0xf1u,0xf3u,0xf5u})raster.bpValid.set(reg);
    raster.bp[0x20]=(342u<<12)|342;raster.bp[0x21]=(741u<<12)|581;
    RasterPlan plan;RasterError error;assert(planRaster(raster,plan,&error));
    for(unsigned enabled=0;enabled<2;++enabled)for(unsigned subtract=0;subtract<2;++subtract)
    for(unsigned src=0;src<8;++src)for(unsigned dst=0;dst<8;++dst){
        raster.bp[0x41]=enabled|(subtract<<11)|(src<<8)|(dst<<5)|24;
        assert(planRaster(raster,plan,&error)&&plan.blend.mode==enabled&&plan.subtractBlend==bool(enabled&&subtract));
        assert(plan.blend.src==src&&plan.blend.dst==dst&&plan.writeRgb&&!plan.writeAlpha);
    }
    // Captured exterior draw: alpha-only RGBA6 subtract, depth read/no write.
    raster.bp[0x41]=0x935;raster.bp[0x43]=0x41;raster.bp[0x40]=7;raster.bp[0xf3]=0x7f0000;
    assert(planRaster(raster,plan,&error,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6));
    assert(plan.subtractBlend&&!plan.writeRgb&&plan.writeAlpha&&plan.storesAlpha&&plan.depth.enabled&&!plan.depth.write);
    raster.bp[0x41]=raster.bp[0x43]=raster.bp[0x40]=raster.bp[0xf3]=0;
    // Fog inspection is an explicit diagnostic policy. Ordinary calls remain
    // strict, and inspection must still reject unrelated unsupported state.
    raster.bp[0xf1]=2u<<21;
    assert(!planRaster(raster,plan,&error)&&error==RasterError::fog);
    assert(planRaster(raster,plan,&error,FogPolicy::diagnosticDisabled));
    raster.bp[0]=1u<<19;
    assert(!planRaster(raster,plan,&error,FogPolicy::diagnosticDisabled)&&error==RasterError::coPlanar);
    raster.bp[0]=0;raster.bp[0xf1]=0;
    raster.bp[0]=1u<<19;assert(!planRaster(raster,plan,&error)&&error==RasterError::coPlanar);
    raster.bp[0]=0;raster.xfValid.set(0x1005);raster.xf[0x1005]=1;
    assert(!planRaster(raster,plan,&error)&&error==RasterError::clipMode);
    raster.xf[0x1005]=0;raster.bp[0xf1]=2u<<21;assert(!planRaster(raster,plan,&error)&&error==RasterError::fog);
    // Original actors mix signed fixed-point vertices and packed colors. Their
    // VAT owns the scaling; encoding must preserve every signed wire bit.
    assert(stream.reset());
    GXClearVtxDesc();
    GXVtxDescList packedDesc[]{{GX_VA_POS,GX_DIRECT},{GX_VA_NRM,GX_DIRECT},
        {GX_VA_CLR0,GX_DIRECT},{GX_VA_TEX0,GX_DIRECT},{GX_VA_NULL,GX_NONE}};
    GXSetVtxDescv(packedDesc);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_S16,4);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_S8,0);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGB,GX_RGB565,0);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_U8,3);
    GXBegin(GX_TRIANGLES,GX_VTXFMT0,3);
    for(unsigned i=0;i<3;++i){GXPosition3s16(-32768,32767,-16);GXNormal3s8(-64,0,64);
        GXColor1u16(0xf800);GXTexCoord2u8(4,8);}GXEnd();
    const auto& packed=capture.vertices[0];
    assert(packed.position[0]==-2048&&packed.position[1]==32767/16.f&&packed.position[2]==-1);
    assert(packed.normal[0][0]==-1&&packed.normal[0][2]==1);
    assert(packed.color[0][0]==1&&packed.color[0][1]==0&&packed.color[0][3]==1);
    assert(packed.texcoord[0][0]==.5f&&packed.texcoord[0][1]==1);
    // Position/normal/color/UV indices have independently registered bounds.
    // Position and UV arrays are disc big-endian; native normal floats are LE.
    std::array<u8,18> positions{0xff,0xf0,0,0x20,0,0x30,0,0x40,0,0x50,0,0x60,0,0x70,0,0x80,0,0x90};
    std::array<float,9> normals{0,0,1,0,1,0,1,0,0};
    std::array<u8,12> colors{255,0,0,255,0,255,0,128,0,0,255,64};
    std::array<u8,12> uv{0,0,0,0,0,0x80,0,0x40,1,0,0,0x80};
    std::array<MemoryHandle,4> handles{};
    assert(memory.add(positions,handles[0]));
    assert(memory.add({reinterpret_cast<const u8*>(normals.data()),sizeof(normals)},handles[1]));
    assert(memory.add(colors,handles[2]));assert(memory.add(uv,handles[3]));
    for(unsigned bits:{8u,16u}){
        assert(stream.reset());GXClearVtxDesc();
        const auto type=bits==8?GX_INDEX8:GX_INDEX16;
        for(auto& d:packedDesc)if(d.attr!=GX_VA_NULL)d.type=type;
        GXSetVtxDescv(packedDesc);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_S16,4);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_F32,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_U16,8);
        GXSetArray(GX_VA_POS,positions.data(),positions.size(),6,false);
        GXSetArray(GX_VA_NRM,normals.data(),sizeof(normals),12,true);
        GXSetArray(GX_VA_CLR0,colors.data(),colors.size(),4,false);
        GXSetArray(GX_VA_TEX0,uv.data(),uv.size(),4,false);
        GXBegin(GX_TRIANGLES,GX_VTXFMT0,3);
        for(unsigned i=0;i<3;++i){
            if(bits==8){GXPosition1x8(i);GXNormal1x8(2-i);GXColor1x8(i);GXTexCoord1x8(i);}
            else{GXPosition1x16(i);GXNormal1x16(2-i);GXColor1x16(i);GXTexCoord1x16(i);}
        }GXEnd();
        assert(capture.vertices[0].position[0]==-1&&capture.vertices[2].position[2]==9);
        assert(capture.vertices[0].normal[0][0]==1&&capture.vertices[2].normal[0][2]==1);
        assert(capture.vertices[1].color[0][1]==1&&capture.vertices[1].color[0][3]==128/255.f);
        assert(capture.vertices[2].texcoord[0][0]==1&&capture.vertices[2].texcoord[0][1]==.5f);
    }
    rejected([&]{GXBegin(GX_TRIANGLES,GX_VTXFMT0,3);
        for(unsigned i=0;i<3;++i){GXPosition1x16(3);GXNormal1x16(0);GXColor1x16(0);GXTexCoord1x16(0);}GXEnd();});
    assert(stream.reset());for(auto handle:handles)assert(memory.remove(handle));
    rejected([&]{GXSetVtxDescv(nullptr);});
    assert(setCommandStream(nullptr));rejected([&]{GXPosition1x8(0);});
    rejected([&]{GXSetFog(GX_FOG_NONE,0,0,0,0,{});});
    // Tile allocation is essential to original map, font and JUT resources.
    // Exercise every format in the pinned pure allocator over partial tiles,
    // full-sized frames, rectangular mip chains and the level-count boundary.
    const unsigned formats[]{GX_TF_I4,GX_TF_C4,GX_TF_CMPR,GX_CTF_R4,GX_CTF_Z4,
        GX_TF_I8,GX_TF_IA4,GX_TF_C8,GX_TF_Z8,GX_CTF_RA4,GX_CTF_A8,GX_CTF_R8,GX_CTF_G8,GX_CTF_B8,GX_CTF_Z8M,GX_CTF_Z8L,
        GX_TF_IA8,GX_TF_RGB565,GX_TF_RGB5A3,GX_TF_RGBA8,GX_TF_C14X2,GX_TF_Z16,GX_TF_Z24X8,GX_CTF_RA8,GX_CTF_RG8,GX_CTF_GB8,GX_CTF_Z16L};
    unsigned sizeCases=0;
    for(auto f:formats){
        for(u16 w:{1,7,8,9,64,608,1024})for(u16 h:{1,3,4,5,448,1024}){
            assert(GXGetTexBufferSize(w,h,f,GX_FALSE,0)==OriginalGXGetTexBufferSize(w,h,f,GX_FALSE,0));++sizeCases;
        }
        for(u16 w:{1,8,64,1024})for(u16 h:{1,4,32,256})for(u8 levels:{0,1,2,5,11,255}){
            assert(GXGetTexBufferSize(w,h,f,GX_TRUE,levels)==OriginalGXGetTexBufferSize(w,h,f,GX_TRUE,levels));++sizeCases;
        }
    }
    rejected([&]{GXGetTexBufferSize(0,8,GX_TF_I8,GX_FALSE,0);});
    rejected([&]{GXGetTexBufferSize(1025,8,GX_TF_I8,GX_FALSE,0);});
    rejected([&]{GXGetTexBufferSize(8,8,255,GX_FALSE,0);});
    rejected([&]{GXGetTexBufferSize(7,8,GX_TF_I8,GX_TRUE,3);});
    // An unregistered image can be described, but it cannot be drawn. Using an
    // unreadable address here proves these read-only queries need no texel read.
    GXTexObj object{};Texture metadata;
    GXInitTexObj(&object,reinterpret_cast<const void*>(1),608,448,GX_TF_RGB565,GX_MIRROR,GX_REPEAT,GX_FALSE);
    assert(GXGetTexObjWidth(&object)==608&&GXGetTexObjHeight(&object)==448);
    assert(GXGetTexObjWrapS(&object)==GX_MIRROR&&GXGetTexObjWrapT(&object)==GX_REPEAT);
    assert(queryTextureObject(&object,metadata)&&metadata.width==608&&metadata.image.empty()&&!metadata.handle);
    GXLoadTexObj(&object,GX_TEXMAP0);assert(!textureBinding(0,metadata));
    GXDestroyTexObj(&object);assert(!queryTextureObject(&object,metadata)&&!metadata.width);
    rejected([&]{GXGetTexObjWidth(&object);});rejected([&]{GXGetTexObjHeight(nullptr);});
    GXInitTexObj(&object,nullptr,0,0,GX_TF_RGBA8,GX_CLAMP,GX_CLAMP,GX_FALSE);
    assert(GXGetTexObjWidth(&object)==0&&GXGetTexObjHeight(&object)==0);
    assert(queryTextureObject(&object,metadata)&&metadata.image.empty()&&!metadata.handle);
    GXLoadTexObj(&object,GX_TEXMAP0);assert(!textureBinding(0,metadata));
    GXDestroyTexObj(&object);assert(!queryTextureObject(&object,metadata));
    // Actual pinned GX encoders are compiled as an independent reference.
    // Seed unrelated bits, then test format transitions and disabled alpha.
    unsigned pixelCases=0;
    assert(setCommandStream(&stream));
    for(u32 seed:{0u,0xa5a5a5u,0xffffffu}){
        assert(stream.reset());
        const u32 control=seed&~63u,gen=seed&~(1u<<9);
        OriginalPixelReset(seed,seed,control,gen);
        assert(stream.writeBp(0x41,seed)&&stream.writeBp(0x42,seed)&&stream.writeBp(0x43,control)&&stream.writeBp(0,gen));
        for(unsigned fmt=0;fmt<8;++fmt)for(unsigned z=0;z<4;++z)for(bool enable:{false,true}){
            GXSetAlphaUpdate(enable);OriginalGXSetAlphaUpdate(enable);
            const auto alpha=u8((fmt*31+z*17)&255);
            GXSetDstAlpha(enable,alpha);OriginalGXSetDstAlpha(enable,alpha);
            GXSetPixelFmt(static_cast<GXPixelFmt>(fmt),static_cast<GXZFmt16>(z));
            OriginalGXSetPixelFmt(static_cast<GXPixelFmt>(fmt),static_cast<GXZFmt16>(z));
            for(unsigned reg:{0u,0x41u,0x42u,0x43u})assert(p.registers().bp[reg]==OriginalPixelRegister(reg));
            ++pixelCases;
        }
    }
    assert(stream.reset());assert(stream.writeBp(0x22,0xa5a5a5));
    GXSetPointSize(255,GX_TO_ONE);assert(p.registers().bp[0x22]==((0xa5a5a5u&~(255u<<8|7u<<19))|255u<<8|5u<<19));
    rejected([&]{GXSetPointSize(1,static_cast<GXTexOffset>(6));});
    // Exhaust the representable height pairs, including1024-line clamping.
    // The original divides by zero for impossible quantizations; native calls
    // explicitly reject those inputs before reaching the same integer math.
    unsigned scaleCases=0;
    for(unsigned efb=1;efb<=1024;++efb)for(unsigned xfb=efb;xfb<=1024;++xfb){
        if((efb-1)*256+1<xfb)continue;
        const auto expected=OriginalGXGetYScaleFactor(efb,xfb);
        const auto actual=GXGetYScaleFactor(efb,xfb);
        assert(std::bit_cast<u32>(actual)==std::bit_cast<u32>(expected));
        if(actual>=1)assert(GXGetNumXfbLines(efb,actual)==OriginalGXGetNumXfbLines(efb,actual));
        ++scaleCases;
    }
    for(u16 height:{1,2,128,240,448,480,528,1024})for(unsigned q=1;q<=256;++q){
        const float scale=256.f/q;
        assert(GXGetNumXfbLines(height,scale)==OriginalGXGetNumXfbLines(height,scale));++scaleCases;
    }
    rejected([&]{GXGetNumXfbLines(0,1);});rejected([&]{GXGetNumXfbLines(1025,1);});
    rejected([&]{GXGetNumXfbLines(240,0);});rejected([&]{GXGetNumXfbLines(240,.5f);});
    rejected([&]{GXGetNumXfbLines(240,257);});rejected([&]{GXGetNumXfbLines(240,std::numeric_limits<float>::quiet_NaN());});
    rejected([&]{GXGetYScaleFactor(0,240);});rejected([&]{GXGetYScaleFactor(240,0);});
    rejected([&]{GXGetYScaleFactor(480,240);});rejected([&]{GXGetYScaleFactor(480,1025);});
    rejected([&]{GXGetYScaleFactor(1,2);});rejected([&]{GXGetYScaleFactor(2,1024);});
    assert(setCommandStream(nullptr));
    std::printf("PASS: packed/direct/indexed vertices; %u original GD fog cases; %u original texture allocations; metadata lifetime; %u original pixel transitions; %u original copy-scale cases\n",fogCases,sizeCases,pixelCases,scaleCases);
}
