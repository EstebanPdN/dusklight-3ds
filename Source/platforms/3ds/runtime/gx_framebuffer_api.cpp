#include "gx_framebuffer.hpp"
#include "../gx_pica.hpp"
#include "gx_stream.hpp"
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCpu2Efb.h>
#include <dolphin/os.h>
#include <array>
#include <bit>
#if defined(__3DS__)
#include "developer_script.hpp"
#endif
namespace {
using dusk::native::gx::CommandStream;
CommandStream& finish(const char* name){if(!dusk::native::alphaIntervalCopySafe())OSPanic(__FILE__,__LINE__,"Alpha interval unsafe framebuffer boundary: %s",name);auto* stream=dusk::native::gx::commandStream();if(!stream||!stream->finish())OSPanic(__FILE__,__LINE__,"Native framebuffer command boundary: %s",name);return *stream;}
void check(bool okay,const char* name){if(!okay)OSPanic(__FILE__,__LINE__,"Native framebuffer lifecycle: %s",name);}
struct RasterState {
    std::array<u32,6> viewport{};
    std::array<u32,2> scissor{};
};
RasterState suspended{};
CommandStream* suspendedOwner{};
RasterState saveRasterState(const CommandStream& stream){
    const auto& r=stream.registers();RasterState state;
    // Reject an uninitialized main render state before changing targets. The
    // saved words must be restorable exactly, including noncanonical floats.
    for(unsigned i=0;i<6;++i){check(r.xfValid[0x101a+i],"save viewport");state.viewport[i]=r.xf[0x101a+i];}
    for(unsigned i=0;i<2;++i){check(r.bpValid[0x20+i],"save scissor");state.scissor[i]=r.bp[0x20+i];}
    return state;
}
void applyRasterState(CommandStream& stream,const RasterState& state){
    check(stream.writeXf(0x101a,state.viewport),"apply viewport");
    // This render-target transition is not a GX BP write from the game. Do
    // not consume a pending one-write mask or let it truncate the scissor.
    const auto mask=stream.registers().bpMask;
    if(mask!=0xffffff)check(stream.writeBp(0xfe,0xffffff),"scissor mask");
    for(unsigned i=0;i<2;++i)check(stream.writeBp(0x20+i,state.scissor[i]),"apply scissor");
    if(mask!=0xffffff)check(stream.writeBp(0xfe,mask),"restore scissor mask");
}
}
extern "C" void GXCreateFrameBuffer(u32 width,u32 height){
    auto& stream=finish("begin");
    const auto active=dusk::native::activeFramebuffer();
    check(active!=nullptr,"begin outside frame");
    const bool suspend=active==dusk::native::mainFramebuffer();
    RasterState mainState;
    if(suspend)mainState=saveRasterState(stream);
    else check(suspendedOwner==&stream,"begin without suspended state");
    check(dusk::native::beginOffscreenFramebuffer(width,height),"begin");
    if(suspend){suspended=mainState;suspendedOwner=&stream;}
    // Aurora recording.cpp suspends cached effective render state once and
    // replaces it on EVERY offscreen create. In native command mode the XF/BP
    // words are the effective state used by planVertexTransform/planRaster;
    // changing only the physical C3D viewport leaves casters misplaced.
    const float values[]{width/2.f,-float(height)/2,16777215.f,340+width/2.f,340+height/2.f,16777215.f};
    RasterState target;
    for(unsigned i=0;i<6;++i)target.viewport[i]=std::bit_cast<u32>(values[i]);
    constexpr u32 scissorMask=0x7ff7ff;
    target.scissor[0]=(stream.registers().bp[0x20]&~scissorMask)|(342u<<12)|342u;
    target.scissor[1]=(stream.registers().bp[0x21]&~scissorMask)|((341+width)<<12)|(341+height);
    applyRasterState(stream,target);
}
extern "C" void GXRestoreFrameBuffer(){
    auto& stream=finish("restore");
    check(suspendedOwner==&stream,"restore without suspended state");
    check(dusk::native::restoreMainFramebuffer(),"restore");
    applyRasterState(stream,suspended);suspendedOwner=nullptr;
}
extern "C" void GXPeekZ(u16 x,u16 y,u32* output){
    if(!output)return;
    finish("depth read");u32 value;
    check(dusk::native::platformReadFramebufferDepth(dusk::native::activeFramebuffer(),x,y,value),"depth read");
    *output=value;
}
void dusk::native::peekFramebufferDepthBatch(std::span<const FramebufferDepthQuery> queries){
    check(queries.size()<=maxFramebufferDepthQueries,"depth batch count");
    const auto extent=gx::currentTargetExtent;
    bool needed=false;
    for(const auto& query:queries)if(query.output){
        check(extent.valid()&&query.x<extent.logicalWidth&&query.y<extent.logicalHeight,"depth batch coordinates");
        needed=true;
    }
    if(!needed)return; // Preserve GXPeekZ's null-output and empty-queue behavior.
    finish("depth batch");
    std::span<const std::byte> snapshot;
    check(platformReadFramebufferDepthImage(activeFramebuffer(),snapshot),"depth batch read");
    std::array<u32,maxFramebufferDepthQueries> values{};
    for(std::size_t i=0;i<queries.size();++i)if(queries[i].output)
        check(sampleFramebufferDepthImage(extent,snapshot,queries[i].x,queries[i].y,values[i]),"depth batch sample");
    // Publish only after every sample succeeds; no caller code runs while the
    // borrowed readback is live. Generic GXPeekZ and GXCopyTex still read fresh.
    for(std::size_t i=0;i<queries.size();++i)if(queries[i].output)*queries[i].output=values[i];
#if defined(__3DS__)
    captureDeveloperDepthQueries(extent,snapshot,queries);
#endif
}
