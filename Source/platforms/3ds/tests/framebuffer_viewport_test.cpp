#include "../runtime/gx_framebuffer.hpp"
#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_vertex_transform.hpp"
#include "../runtime/gx_raster_plan.hpp"
#include <dolphin/gx/GXAurora.h>
#include <dolphin/os.h>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace dusk::native;
using namespace dusk::native::gx;
namespace {
unsigned created{},binds{},clears{};
bool failCreate{},failBind{},failClear{};
struct Snapshot {
    std::array<u32,6> viewport{};
    std::array<u32,2> scissor{};
    explicit Snapshot(const CommandStream& stream){
        const auto& r=stream.registers();
        for(unsigned i=0;i<6;++i)viewport[i]=r.xf[0x101a+i];
        for(unsigned i=0;i<2;++i)scissor[i]=r.bp[0x20+i];
    }
    bool operator==(const Snapshot&)const=default;
};
template<class F> void rejected(F action){
    bool failed=false;try{action();}catch(const std::runtime_error&){failed=true;}assert(failed);
}
void close(float actual,float expected){assert(std::isfinite(actual)&&std::abs(actual-expected)<2e-5f);}
void floats(CommandStream& stream,unsigned address,std::span<const float> values){
    std::array<u32,12> words{};assert(values.size()<=words.size());
    for(unsigned i=0;i<values.size();++i)words[i]=std::bit_cast<u32>(values[i]);
    assert(stream.writeXf(address,std::span{words}.first(values.size())));
}
void viewport(CommandStream& stream,float x,float y,float w,float h,float n,float f){
    const float values[]{w/2,-h/2,(f-n)*16777215.f,340+x+w/2,340+y+h/2,f*16777215.f};
    floats(stream,0x101a,values);
}
void scissor(CommandStream& stream,unsigned x,unsigned y,unsigned w,unsigned h){
    assert(stream.writeBp(0x20,((x+342)<<12)|(y+342)));
    assert(stream.writeBp(0x21,((x+w+341)<<12)|(y+h+341)));
}
void initializeScene(CommandStream& stream){
    // Independent asymmetric position, identity model, orthographic reversed
    // depth and projective texture matrix. The framebuffer API must change
    // only raster placement, never the light projection or receiver UVs.
    const float projection[]{1,0,1,0,-1,0};floats(stream,0x1020,projection);
    const u32 ortho=1;assert(stream.writeXf(0x1026,{&ortho,1}));
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0};floats(stream,0,identity);
    const float receiver[]{.5f,0,0,.5f,0,-.5f,0,.5f,0,0,0,1};floats(stream,120,receiver);
    assert(stream.writeCp(0x30,30u<<6));
    const u32 count=1,config=2,post=61;
    assert(stream.writeXf(0x103f,{&count,1}));
    assert(stream.writeXf(0x1040,{&config,1}));
    assert(stream.writeXf(0x1050,{&post,1}));
    for(unsigned reg:{0u,0x40u,0x41u,0x42u,0x43u,0xf1u,0xf5u})assert(stream.writeBp(reg,0));
    assert(stream.writeBp(0xf3,(7u<<16)|(7u<<19)));
    assert(stream.writeBp(0x59,171u|(171u<<9)));
}
TransformedVertex transformed(const CommandStream& stream,float x,float y){
    VertexTransformPlan plan;VertexTransformError error;
    const bool okay=planVertexTransform(stream.registers(),1,plan,&error);
    if(!okay)std::fprintf(stderr,"transform plan: %s\n",vertexTransformErrorText(error));
    assert(okay);
    DecodedVertex input{};input.present=1u<<GX_VA_POS;
    input.position[0]=x;input.position[1]=y;input.position[2]=.25f;
    TransformedVertex output;assert(transformVertex(plan,input,output));return output;
}
void fullTarget(const CommandStream& stream,unsigned w,unsigned h){
    assert(currentTargetExtent.logicalWidth==w&&currentTargetExtent.logicalHeight==h);
    const auto& r=stream.registers();
    const float expected[]{w/2.f,-float(h)/2,16777215.f,340+w/2.f,340+h/2.f,16777215.f};
    for(unsigned i=0;i<6;++i){assert(r.xfValid[0x101a+i]);close(std::bit_cast<float>(r.xf[0x101a+i]),expected[i]);}
    assert((r.bp[0x20]&0x7ff7ff)==((342u<<12)|342u));
    assert((r.bp[0x21]&0x7ff7ff)==(((w+341)<<12)|(h+341)));
    RasterPlan raster;assert(planRaster(r,raster));assert(!raster.scissor.empty);
    assert(raster.scissor.left==0&&raster.scissor.top==0);
    assert(raster.scissor.right==currentTargetExtent.pixelHeight&&raster.scissor.bottom==currentTargetExtent.pixelWidth);
    const auto center=transformed(stream,0,0);
    close(center.clip[0],0);close(center.clip[1],0);close(center.clip[2],-.25f);close(center.clip[3],1);
    close(center.texcoord[0][0],.5f);close(center.texcoord[0][1],.5f);close(center.texcoord[0][2],1);
    const auto asymmetric=transformed(stream,.25f,-.5f);
    close(asymmetric.clip[0],-.5f);close(asymmetric.clip[1],-.25f);
    // Undo PICA rotation independently: raster position and receiver texel
    // position must agree, including the 192-square center at (96,96).
    close((1-asymmetric.clip[1])*w/2,asymmetric.texcoord[0][0]*w);
    close((1-asymmetric.clip[0])*h/2,asymmetric.texcoord[0][1]*h);
}
}
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
FramebufferHandle dusk::native::platformCreateFramebuffer(unsigned w,unsigned h){
    assert(!(w&7)&&!(h&7));return failCreate?nullptr:reinterpret_cast<void*>(uintptr_t(++created+1));
}
bool dusk::native::platformBindFramebuffer(FramebufferHandle handle){assert(handle);++binds;return !failBind;}
bool dusk::native::platformClearFramebuffer(FramebufferHandle handle,u32,u32){assert(handle);++clears;return !failClear;}
bool dusk::native::platformReadFramebufferDepth(FramebufferHandle,unsigned,unsigned,u32& value){value=0x123456;return true;}
bool dusk::native::platformReadFramebufferDepthImage(FramebufferHandle,std::span<const std::byte>&){assert(false);return false;}
int main(){
    CommandMemory memory;CommandProcessor processor(memory);std::array<u8,256> scratch{};
    CommandStream stream(processor,scratch);assert(setCommandStream(&stream));
    rejected([]{GXCreateFrameBuffer(192,192);});rejected([]{GXRestoreFrameBuffer();});
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),400,240));
    assert(configureMainFramebuffer(608,448));assert(beginFramebufferFrame());
    const auto main=activeFramebuffer();
    initializeScene(stream);viewport(stream,17,29,530,370,.125f,.875f);scissor(stream,37,41,510,330);
    // Preserve non-scissor bits, exact float words, and validity across restore.
    assert(stream.writeBp(0x20,stream.registers().bp[0x20]|0x800800));
    assert(stream.writeBp(0x21,stream.registers().bp[0x21]|0x800800));
    const Snapshot saved(stream);const auto baseline=transformed(stream,.25f,-.5f);
    RasterPlan mainRaster;assert(planRaster(stream.registers(),mainRaster));
    assert(stream.writeBp(0xfe,1));
    GXCreateFrameBuffer(192,192);fullTarget(stream,192,192);assert(stream.registers().bpMask==1);
    const auto shadow=activeFramebuffer();
    // Pending game mask survives create and still affects exactly one write.
    assert(stream.writeBp(0x41,0xffffff));assert(stream.registers().bp[0x41]==1&&stream.registers().bpMask==0xffffff);
    assert(stream.writeBp(0x41,0));
    // Ordinary command setters inside offscreen remain effective (Aurora's
    // override is initial state, not a lock). Every subsequent create resets it.
    viewport(stream,5,7,40,30,.25f,.75f);scissor(stream,5,7,40,30);
    assert(transformed(stream,0,0).clip[0]!=0);
    GXCreateFrameBuffer(192,192);assert(activeFramebuffer()==shadow);fullTarget(stream,192,192);
    GXCreateFrameBuffer(64,64);fullTarget(stream,64,64);assert(!endFramebufferFrame());
    // Failed sequential creates and restore leave the saved main state intact.
    const auto second=activeFramebuffer();const Snapshot beforeFailure(stream);
    auto unchanged=[&]{assert(activeFramebuffer()==second&&Snapshot(stream)==beforeFailure);fullTarget(stream,64,64);};
    rejected([]{GXCreateFrameBuffer(0,64);});unchanged();
    rejected([]{GXCreateFrameBuffer(1024,1024);});unchanged();
    failCreate=true;rejected([]{GXCreateFrameBuffer(80,80);});failCreate=false;unchanged();
    failClear=true;rejected([]{GXCreateFrameBuffer(192,192);});failClear=false;unchanged();
    failBind=true;rejected([]{GXCreateFrameBuffer(192,192);});unchanged();
    rejected([]{GXRestoreFrameBuffer();});failBind=false;unchanged();
    assert(stream.writeBp(0xfe,0xabcdef));GXRestoreFrameBuffer();
    assert(activeFramebuffer()==main&&Snapshot(stream)==saved&&stream.registers().bpMask==0xabcdef);
    const auto restored=transformed(stream,.25f,-.5f);
    assert(restored.clip==baseline.clip&&restored.texcoord==baseline.texcoord);
    RasterPlan restoredRaster;assert(planRaster(stream.registers(),restoredRaster));
    assert(restoredRaster.scissor.left==mainRaster.scissor.left&&restoredRaster.scissor.top==mainRaster.scissor.top);
    assert(restoredRaster.scissor.right==mainRaster.scissor.right&&restoredRaster.scissor.bottom==mainRaster.scissor.bottom);
    rejected([]{GXRestoreFrameBuffer();});assert(Snapshot(stream)==saved);
    // First-create failure must not suspend state or prevent a clean retry.
    failClear=true;rejected([]{GXCreateFrameBuffer(64,64);});failClear=false;
    assert(activeFramebuffer()==main&&Snapshot(stream)==saved);
    failBind=true;rejected([]{GXCreateFrameBuffer(64,64);});failBind=false;
    assert(activeFramebuffer()==main&&Snapshot(stream)==saved);
    GXCreateFrameBuffer(61,29);fullTarget(stream,61,29);GXRestoreFrameBuffer();assert(Snapshot(stream)==saved);
    assert(endFramebufferFrame());releaseFramebufferRecords();
    // Discarding records while offscreen, then reinitializing, must overwrite
    // any abandoned snapshot at the next first main -> offscreen transition.
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),400,240));assert(beginFramebufferFrame());
    GXCreateFrameBuffer(64,64);releaseFramebufferRecords();
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),400,240));assert(beginFramebufferFrame());
    viewport(stream,11,13,300,200,0,1);assert(stream.writeBp(0xfe,0xffffff));scissor(stream,23,31,270,150);
    const Snapshot restarted(stream);GXCreateFrameBuffer(192,192);fullTarget(stream,192,192);
    GXRestoreFrameBuffer();assert(Snapshot(stream)==restarted);
    // An incomplete raw command rejects before any bind/clear or state write.
    const auto oldBinds=binds,oldClears=clears;
    const u8 partial=0x61;assert(stream.write({&partial,1}));
    rejected([]{GXCreateFrameBuffer(192,192);});assert(binds==oldBinds&&clears==oldClears);
    assert(activeFramebuffer()==main&&Snapshot(stream)==restarted);
    assert(stream.reset());
    // Missing main viewport/scissor rejects before allocation or target change.
    const auto oldCreated=created;
    rejected([]{GXCreateFrameBuffer(192,192);});
    assert(activeFramebuffer()==main&&created==oldCreated&&binds==oldBinds&&clears==oldClears);
    viewport(stream,0,0,400,240,0,1);
    rejected([]{GXCreateFrameBuffer(192,192);});
    assert(activeFramebuffer()==main&&created==oldCreated&&binds==oldBinds&&clears==oldClears);
    assert(endFramebufferFrame());releaseFramebufferRecords();
    assert(setCommandStream(nullptr));rejected([]{GXCreateFrameBuffer(192,192);});
    std::puts("PASS: framebuffer viewport/scissor command override, 192->64 restore, transform/receiver correspondence, masks, padded targets and failure lifecycle");
}
