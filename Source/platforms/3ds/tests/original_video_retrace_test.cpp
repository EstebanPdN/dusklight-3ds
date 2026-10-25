#include "../runtime/bootstrap.hpp"
#include "../runtime/clock.hpp"
#include "../runtime/video_retrace.hpp"
#include "helpers/native_frame_wait.h"
#include "JSystem/JUtility/JUTVideo.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
namespace {u32 retrace;unsigned waits,configures,blackCalls,flushes;bool viStarted;GXDrawDoneCallback drawDone;}
namespace dusk::native {
void logText(const char* t){fputs(t,stderr);}
[[noreturn]]void fatal(const char* t){logText(t);abort();}
void flushCache(void*,std::uint32_t){}
void invalidateCache(void*,std::uint32_t){}
void waitForRetrace(){waitVideoRetrace();}
std::uint64_t counterTicks(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()*(nativeTickFrequency/1000);}
std::uint64_t wallMilliseconds(){return epoch1900To2000Ms;}
u32 platformVideoRetraceCount(){return retrace;}
void platformWaitVideoRetrace(){++retrace;++waits;}
void platformSleepNanoseconds(std::int64_t){}
}
// Explicit host hardware backends. The production VI mode/framebuffer/copy
// providers remain unresolved; an unexpected unimplemented copy aborts here.
extern "C" {
void VIInit(){viStarted=true;}
void VIConfigure(const GXRenderModeObj* m){assert(viStarted&&m&&m->fbWidth==608&&m->efbHeight==448);++configures;}
void VISetBlack(BOOL){++blackCalls;}
void VIFlush(){++flushes;}
void VISetNextFrameBuffer(void*){abort();}
void* VIGetNextFrameBuffer(){abort();}
void GXCopyDisp(void*,GXBool){abort();}
void GXFlush(){abort();}
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb){auto previous=drawDone;drawDone=cb;return previous;}
}
int main(){
    alignas(32)static std::array<u8,256*1024> arena;
    assert(dusk::native::initializeKernel(arena.data(),arena.size()));const auto heapBefore=dusk::native::kernelStats();
    GXRenderModeObj mode{};mode.fbWidth=608;mode.efbHeight=448;mode.xfbHeight=448;
    auto* video=JUTVideo::createManager(&mode);assert(video&&configures==1&&waits==2&&drawDone==JUTVideo::drawDoneCallback);
    dusk::native::FrameRetraceDeadline deadline;
    assert(dusk::native::waitForFrameRetraces(video->getMessageQueue(),2,deadline)==3);
    assert(dusk::native::waitForFrameRetraces(video->getMessageQueue(),2,deadline)==5);
    const auto before=waits;retrace=12;
    assert(dusk::native::waitForFrameRetraces(video->getMessageQueue(),2,deadline)==12&&waits==before);
    assert(blackCalls>=4&&flushes>=4&&JUTVideo::getVideoInterval()>0);
    JUTVideo::destroyManager();assert(!JUTVideo::getManager());
    assert(VISetPreRetraceCallback(nullptr)==nullptr&&VISetPostRetraceCallback(nullptr)==nullptr);
    const auto after=dusk::native::kernelStats();assert(after.healthy&&after.free==heapBefore.free);
    puts("PASS: actual JUTVideo construction/two startup waits/pre-post queue/frame deadline/slow-frame/destruction with real JKR and OS; synthetic hardware source only");
}
