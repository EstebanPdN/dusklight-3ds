#include "gx_stream.hpp"
#include "gpu_sync.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
extern "C" void GXAbortFrame(){
    auto* stream=dusk::native::gx::commandStream();
    if(!stream||dusk::native::platformCompleteGpu(500000000)!=dusk::native::GpuCompletion::complete||!stream->reset())
        OSPanic(__FILE__,__LINE__,"Native GX frame abort/drain failed");
}
extern "C" void GXSetMisc(GXMiscToken token,u32 value){
    // XF flush words are a GCN FIFO padding policy. The command decoder knows
    // every complete primitive's exact extent; it never issues MMIO FIFO pads.
    if(token!=GX_MT_XF_FLUSH&&token!=GX_MT_DL_SAVE_CONTEXT)
        OSPanic(__FILE__,__LINE__,"Unsupported native GX misc token");
    if(token==GX_MT_DL_SAVE_CONTEXT&&value)
        OSPanic(__FILE__,__LINE__,"Native saved display-list context is unsupported");
}
