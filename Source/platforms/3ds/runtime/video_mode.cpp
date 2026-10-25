#include "video_mode.hpp"
#include "os_system.hpp"
#include <dolphin/vi.h>
#include <dolphin/os.h>
namespace {
GXRenderModeObj pending{};
bool configured{},black{};
void* nextFramebuffer{};
}
extern "C" {
// Pinned original default descriptor; VI maps this logical EFB to the LCD.
GXRenderModeObj GXNtsc480IntDf={VI_TVMODE_NTSC_INT,640,480,480,40,0,640,480,VI_XFBMODE_DF,0,0,
    {{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6}},
    {8,8,10,12,10,8,8}};
void VIInit(){configured=false;black=false;nextFramebuffer=nullptr;}
void VISetNextFrameBuffer(void* address){nextFramebuffer=address;}
void* VIGetNextFrameBuffer(){return nextFramebuffer;}
void VIConfigure(const GXRenderModeObj* mode){
    if(!mode||!mode->fbWidth||!mode->efbHeight||mode->fbWidth>1024||mode->efbHeight>1024||!mode->xfbHeight)
        OSPanic(__FILE__,__LINE__,"Invalid native VI render mode");
    pending=*mode;configured=true;
}
void VISetBlack(BOOL value){black=value!=0;}
void VIFlush(){
    if(!configured||!dusk::native::platformCommitVideoMode(pending.fbWidth,pending.efbHeight,black))
        OSPanic(__FILE__,__LINE__,"Native VI mode commit failed");
}
u32 VIGetDTVStatus(){return dusk::native::platformDigitalVideo()?1:0;}
u32 VIGetNextField(){return 0;} // The native LCD is progressive, without interlaced fields.
}
