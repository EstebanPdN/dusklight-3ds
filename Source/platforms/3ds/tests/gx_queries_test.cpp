#include "../runtime/gx_state.hpp"
#include "../runtime/gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
template<class F> void rejected(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}assert(caught);}
void near(float a,float b){assert(std::abs(a-b)<.0001f);}
struct ShapeCapture {bool sphere{};unsigned draws{},vertices{};};
bool shapeSink(const CommandDraw& draw,void* user){
    auto& c=*static_cast<ShapeCapture*>(user);
    assert(draw.vertexCount==(c.sphere||c.draws==0?10:6));
    assert(draw.primitive==(c.sphere||c.draws==0?GX_TRIANGLESTRIP:GX_TRIANGLEFAN));
    for(unsigned i=0;i<draw.vertexCount;++i){
        DecodedVertex v;assert(draw.vertex(i,v)&&v.normalCount==1);
        const auto& n=v.normal[0];near(n[0]*n[0]+n[1]*n[1]+n[2]*n[2],1);
        if(c.sphere){
            near(v.position[0]*v.position[0]+v.position[1]*v.position[1]+v.position[2]*v.position[2],1);
            for(unsigned axis=0;axis<3;++axis)near(v.position[axis],n[axis]);
            assert(v.texcoord[0][0]>=0&&v.texcoord[0][0]<=1&&v.texcoord[0][1]>=0&&v.texcoord[0][1]<=1);
        }else{
            near(std::abs(v.position[2]),1);
            if(c.draws==0){near(v.position[0]*v.position[0]+v.position[1]*v.position[1],1);near(n[2],0);}
            else near(std::abs(n[2]),1);
        }
    }
    ++c.draws;c.vertices+=draw.vertexCount;return true;
}
void proceduralShapes(){
    CommandMemory memory;CommandProcessor processor(memory);std::array<u8,4096> bytes{};CommandStream stream(processor,bytes);
    assert(setCommandStream(&stream));
    GXAttrType descriptor;rejected([&]{GXGetVtxDesc(GX_VA_POS,&descriptor);});
    GXClearVtxDesc();
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)
        GXSetVtxAttrFmt(GX_VTXFMT3,static_cast<GXAttr>(attr),GX_POS_XY,GX_U8,3);
    GXSetVtxAttrFmt(GX_VTXFMT3,GX_VA_NRM,GX_NRM_NBT3,GX_S16,0);
    GXSetVtxDesc(GX_VA_NRM,GX_INDEX16);GXSetVtxDesc(GX_VA_NBT,GX_INDEX8);GXSetVtxDesc(GX_VA_TEX0,GX_INDEX16);
    GXGetVtxDesc(GX_VA_NRM,&descriptor);assert(descriptor==GX_INDEX8);
    GXGetVtxDesc(GX_VA_NBT,&descriptor);assert(descriptor==GX_INDEX8);
    GXCompCnt cnt;GXCompType type;u8 fraction;
    GXGetVtxAttrFmt(GX_VTXFMT3,GX_VA_NBT,&cnt,&type,&fraction);assert(cnt==GX_NRM_NBT3&&type==GX_S16&&fraction==14);
    GXVtxDescList beforeDesc[27]{};GXVtxAttrFmtList beforeFmt[27]{};
    GXGetVtxDescv(beforeDesc);GXGetVtxAttrFmtv(GX_VTXFMT3,beforeFmt);
    auto restored=[&]{
        GXVtxDescList afterDesc[27]{};GXVtxAttrFmtList afterFmt[27]{};
        GXGetVtxDescv(afterDesc);GXGetVtxAttrFmtv(GX_VTXFMT3,afterFmt);
        for(unsigned i=0;i<=GX_VA_TEX7+2;++i)assert(beforeDesc[i].attr==afterDesc[i].attr&&beforeDesc[i].type==afterDesc[i].type);
        for(unsigned i=0;i<=GX_VA_TEX7-GX_VA_POS+1;++i)assert(beforeFmt[i].attr==afterFmt[i].attr&&beforeFmt[i].cnt==afterFmt[i].cnt&&beforeFmt[i].type==afterFmt[i].type&&beforeFmt[i].frac==afterFmt[i].frac);
    };
    ShapeCapture capture{true};processor.setSink(shapeSink,&capture);
    GXDrawSphere(2,4);assert(capture.draws==2&&capture.vertices==20&&stream.idle());restored();
    capture={false};GXDrawCylinder(4);assert(capture.draws==3&&capture.vertices==22&&stream.idle());restored();
    rejected([&]{GXDrawSphere(0,4);});rejected([&]{GXDrawCylinder(0);});rejected([&]{GXDrawCylinder(100);});restored();
    // A raw list changes the NRM/NBT interpretation through XF vertex spec.
    assert(stream.writeCp(0x50,1u<<9|2u<<11));const u32 nbt=2u<<2;assert(stream.writeXf(0x1008,{&nbt,1}));
    GXGetVtxDesc(GX_VA_NRM,&descriptor);assert(descriptor==GX_NONE);GXGetVtxDesc(GX_VA_NBT,&descriptor);assert(descriptor==GX_INDEX8);
    const u32 normal=1u<<2;assert(stream.writeXf(0x1008,{&normal,1}));GXGetVtxDesc(GX_VA_NRM,&descriptor);assert(descriptor==GX_INDEX8);
    GXGetVtxDesc(GX_VA_NBT,&descriptor);assert(descriptor==GX_NONE);
    assert(setCommandStream(nullptr));
}
void testNativeGpuSync();
int main(){
    testNativeGpuSync();
    proceduralShapes();
    CommandMemory memory;CommandProcessor p(memory);std::array<u8,128> scratch{};CommandStream stream(p,scratch);
    u32 x=900,y=900,w=900,h=900;float projection[7]{},viewport[6]{};
    // No guessed viewport or projection is returned before initialization.
    assert(setCommandStream(&stream));rejected([&]{GXGetScissor(&x,&y,&w,&h);});assert(x==900&&y==900&&w==900&&h==900);
    rejected([&]{GXGetProjectionv(projection);});rejected([&]{GXGetViewportv(viewport);});
    unsigned cases=0;
    for(unsigned type=0;type<2;++type)for(float coefficient:{-.75f,0.f,1.25f}){
        const float compact[]{float(type),coefficient,.125f,1.75f,-.25f,-1.2f,-2.3f};
        GXSetProjectionv(compact);GXGetProjectionv(projection);
        for(unsigned i=0;i<7;++i)assert(projection[i]==compact[i]);++cases;
    }
    // Raw display-list changes are authoritative; getters do not read an old
    // typed setter shadow. A whole restoration remains valid afterwards.
    const u32 c=std::bit_cast<u32>(4.25f);assert(stream.writeXf(0x1020,{&c,1}));GXGetProjectionv(projection);assert(projection[1]==4.25f);
    for(auto rectangle:std::array<std::array<u32,4>,4>{{{0,0,400,240},{12,34,200,100},{0,0,0,0},{1705,1705,1,1}}}){
        GXSetScissor(rectangle[0],rectangle[1],rectangle[2],rectangle[3]);GXGetScissor(&x,&y,&w,&h);
        assert(x==rectangle[0]&&y==rectangle[1]&&w==rectangle[2]&&h==rectangle[3]);++cases;
    }
    assert(stream.writeBp(0x20,(355u<<12)|356));assert(stream.writeBp(0x21,(554u<<12)|455));
    GXGetScissor(&x,&y,&w,&h);assert(x==13&&y==14&&w==200&&h==100);
    for(auto values:std::array<std::array<float,6>,4>{{{0,0,400,240,0,1},{10.5f,20.25f,320,200,.125f,.875f},{-2,-4,608,448,1,0},{0,0,400,240,.75f,.75f}}}){
        GXSetViewport(values[0],values[1],values[2],values[3],values[4],values[5]);GXGetViewportv(viewport);
        for(unsigned i=0;i<6;++i)near(viewport[i],values[i]);++cases;
    }
    GXSetViewport(0,0,400,240,0,1);const u32 tx=std::bit_cast<u32>(550.f);assert(stream.writeXf(0x101d,{&tx,1}));GXGetViewportv(viewport);assert(viewport[0]==10);
    rejected([&]{GXGetScissor(nullptr,&y,&w,&h);});rejected([&]{GXGetProjectionv(nullptr);});rejected([&]{GXGetViewportv(nullptr);});
    const float bad[]{2,1,2,3,4,5,6};rejected([&]{GXSetProjectionv(bad);});
    assert(stream.writeBp(0x21,0));rejected([&]{GXGetScissor(&x,&y,&w,&h);});
    assert(setCommandStream(nullptr));resetState();rejected([&]{GXGetViewportv(viewport);});rejected([&]{GXGetProjectionv(projection);});rejected([&]{GXGetScissor(&x,&y,&w,&h);});
    GXSetScissor(13,14,200,100);GXGetScissor(&x,&y,&w,&h);assert(x==13&&y==14&&w==200&&h==100);
    const float compact[]{1,2,.5f,3,-.25f,4,-1};GXSetProjectionv(compact);GXGetProjectionv(projection);
    for(unsigned i=0;i<7;++i)assert(compact[i]==projection[i]);
    GXSetViewport(10,20,400,240,.125f,.875f);GXGetViewportv(viewport);near(viewport[0],10);near(viewport[4],.125f);near(viewport[5],.875f);
    printf("PASS: original sphere/cylinder geometry, normals/UVs, VCD/VAT restoration and NRM/NBT raw-list queries; %u projection/scissor/viewport round trips and immediate diagnostics\n",cases);
}
