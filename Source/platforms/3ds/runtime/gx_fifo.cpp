#include "gx_state.hpp"
#include "gx_stream.hpp"
#include "gx_initial_state.hpp"
#include "gpu_sync.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <cstring>
namespace {
struct Fifo {void* base{};u32 size{};void* read{};void* write{};};
static_assert(sizeof(Fifo)<=sizeof(GXFifoObj));
GXFifoObj initial{},*cpu{},*gpu{};
Fifo read(const GXFifoObj* object){Fifo value{};if(object)std::memcpy(&value,object,sizeof(value));return value;}
void put(GXFifoObj* object,const Fifo& value){if(!object)OSPanic(__FILE__,__LINE__,"Null native GX FIFO");std::memset(object,0,sizeof(*object));std::memcpy(object,&value,sizeof(value));}
}
extern "C" {
GXFifoObj* GXInit(void* base,u32 size){
    GXInitFifoBase(&initial,base,size);cpu=gpu=&initial;
    dusk::native::gx::resetState();auto* stream=dusk::native::gx::commandStream();
    if(!stream||!stream->reset())OSPanic(__FILE__,__LINE__,"Native GX stream reset failed");
    dusk::native::gx::initializeGxDefaultState();return &initial;
}
void GXInitFifoBase(GXFifoObj* fifo,void* base,u32 size){if(!base||size<32||(reinterpret_cast<uintptr_t>(base)&31)||(size&31))OSPanic(__FILE__,__LINE__,"Invalid native GX FIFO storage");put(fifo,{base,size,base,base});}
void GXInitFifoPtrs(GXFifoObj* fifo,void* r,void* w){auto value=read(fifo);auto begin=reinterpret_cast<uintptr_t>(value.base);auto inside=[&](void* p){auto a=reinterpret_cast<uintptr_t>(p);return a>=begin&&a-begin<value.size;};if(!inside(r)||!inside(w))OSPanic(__FILE__,__LINE__,"Invalid native FIFO cursors");value.read=r;value.write=w;put(fifo,value);}
void GXSaveCPUFifo(GXFifoObj* fifo){if(!cpu||!dusk::native::gx::commandStream()->finish())OSPanic(__FILE__,__LINE__,"Cannot save incomplete native GX stream");put(fifo,read(cpu));}
void GXGetGPStatus(GXBool* high,GXBool* low,GXBool* readIdle,GXBool* cmdIdle,GXBool* breakpoint){
    const auto status=dusk::native::platformQueryGpu();
    if(status!=dusk::native::GpuCompletion::complete&&status!=dusk::native::GpuCompletion::notReady)OSPanic(__FILE__,__LINE__,"Native GPU status query failed");
    if(high)*high=GX_FALSE;if(low)*low=GX_TRUE;
    if(readIdle)*readIdle=status==dusk::native::GpuCompletion::complete?GX_TRUE:GX_FALSE;
    if(cmdIdle)*cmdIdle=status==dusk::native::GpuCompletion::complete?GX_TRUE:GX_FALSE;
    if(breakpoint)*breakpoint=GX_FALSE;
}
}
