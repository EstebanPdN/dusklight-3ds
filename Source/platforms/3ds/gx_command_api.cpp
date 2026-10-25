#include "runtime/gx_stream.hpp"
#include "runtime/gx_state.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>

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
    if(!current->write({bytes,count}))fail(call);
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
void GXNativeLoadIndx(u8 cmd,u16 indx,u16 addr){
    if(!current||!current->loadIndexed(cmd,indx,addr))fail("GXNativeLoadIndx");
}
void GXCallDisplayList(const void* data,u32 size){
    if(!current||(!data&&size)||!current->flushAutomaticSu()||!current->call({static_cast<const u8*>(data),size}))fail("GXCallDisplayList");
}
}
