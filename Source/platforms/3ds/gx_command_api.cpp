#include <cstring>
#include <array>
#include "runtime/gx_stream.hpp"
#include "runtime/gx_state.hpp"
#include "runtime/perf_features.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#if defined(DUSK_PICA_GAME_DRAW)
#include "native/game_draw_api.hpp"
#endif

namespace {
thread_local dusk::native::gx::CommandStream* current=nullptr;
[[noreturn]] void fail(const char* call){
    OSPanic(__FILE__,__LINE__,"Native GX: %s Stream=%u command=%u offset=%u opcode=%02x needed=%u peak=%u capacity=%u",call,
        current?unsigned(current->error()):0,current?unsigned(current->commandError()):0,current?unsigned(current->commandErrorOffset()):0,
        current?current->pendingOpcode():0,current?unsigned(current->pendingBytes()):0,current?unsigned(current->peakBytes()):0,current?unsigned(current->scratchCapacity()):0);
}
void write(u64 value,unsigned count,const char* call){
    if(!current)fail(call);
    u8 bytes[8];for(unsigned i=0;i<count;++i)bytes[i]=static_cast<u8>(value>>((count-i-1)*8));
    if(!current->writeValue(bytes,count))fail(call);
}
void resetOwnerState(){
    if(current&&!current->resetAutomaticSuOwnerState())fail("resetState/automatic SU busy");
}
}
namespace dusk::native::gx {
CommandStream* commandStream(){return current;}
bool setCommandStream(CommandStream* stream){
    if((current&&!current->idle())||(stream&&!stream->idle()))return false;
    current=stream;
    setStateResetObserver(stream?resetOwnerState:nullptr);
    return true;
}
}
extern "C" {
void GXCmd1u8(u8 value){write(value,1,"GXCmd1u8");}
void GXCmd1u16(u16 value){write(value,2,"GXCmd1u16");}
void GXCmd1u32(u32 value){write(value,4,"GXCmd1u32");}
void GXCmd1u64(u64 value){write(value,8,"GXCmd1u64");}
void GXNativeLoadXf(u16 address,const u32* values,u32 count){
    if(!current||!values||!count||!current->writeXf(address,{values,count}))fail("GXNativeLoadXf");
}
// Merge scope for loops of fixed-count quad/triangle draws (see CommandStream).
void GXNativeMergeImmediate(int enabled){
    if(current&&!current->setImmediateMerge(enabled!=0))fail("GXNativeMergeImmediate");
}
void GXCallDisplayList(const void* data,u32 size);
// J3D calls one vertex descriptor/format list per shape. Shapes of a model
// usually carry identical lists (same descriptor, formats and arrays);
// executing one again while nothing changed the vertex setup is a no-op, so
// its parse and array validation are skipped.
void duskNativeCallVertexSetup(const void* data,u32 size){
    static std::array<u8,0x180> last{};static u32 lastSize{},lastEpoch{};static bool valid{};
    if(!current||(!data&&size))fail("duskNativeCallVertexSetup");
    if(valid&&size==lastSize&&current->registers().vertexSetupEpoch==lastEpoch&&!std::memcmp(data,last.data(),size))return;
    // packetPaint: the epoch also moves when a shape binds its matrix arrays
    // (slots 12-15, which no vertex setup list touches), so the test above
    // misses on nearly every packet. The same bytes again, with every
    // descriptor, format and array binding they set still in place, are
    // held as well (CommandStream::callHeld).
    if(valid&&size==lastSize&&dusk::native::features::on(dusk::native::features::Id::packetPaint)&&
       !std::memcmp(data,last.data(),size)&&current->callHeld({static_cast<const u8*>(data),size})){
        lastEpoch=current->registers().vertexSetupEpoch;return;
    }
    GXCallDisplayList(data,size);
    valid=size&&size<=last.size();
    if(valid){std::memcpy(last.data(),data,size);lastSize=size;lastEpoch=current->registers().vertexSetupEpoch;}
}
// packetPaint: J3DCurrentMtx::load (CP 0x30, CP 0x40, XF 0x1018-0x1019) as
// the typed writes those three commands become once parsed: the same
// applyCp and XF store, the same command counts, a held merged draw drawn
// first in both. Only on an idle stream outside a native packet capture,
// whose records keep the commands' own byte form.
bool duskNativeLoadCurrentMtx(u32 a,u32 b){
    using namespace dusk::native;
    if(!current||!features::on(features::Id::packetPaint)||!current->idle()||current->capturing()||current->nativePendingRun())return false;
    const u32 both[2]{a,b};
    if(!current->writeCp(0x30,a)||!current->writeCp(0x40,b)||!current->writeXf(0x1018,both))fail("J3DCurrentMtx::load");
    return true;
}
void GXNativeLoadIndx(u8 cmd,u16 indx,u16 addr){
    if(!current||!current->loadIndexed(cmd,indx,addr))fail("GXNativeLoadIndx");
}
void GXCallDisplayList(const void* data,u32 size){
#if defined(DUSK_PICA_GAME_DRAW)
    dusk::pica::game::invalidateCamera(); // An unconverted list cannot inherit a native camera proof.
#endif
    if(!current||(!data&&size)||!current->flushAutomaticSu()||!current->call({static_cast<const u8*>(data),size}))fail("GXCallDisplayList");
}
}
