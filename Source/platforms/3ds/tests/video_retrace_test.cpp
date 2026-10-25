#include "../runtime/video_retrace.hpp"
#include "helpers/native_frame_wait.h"
#include <dolphin/vi.h>
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <stdexcept>
using namespace dusk::native;
namespace {
u32 count,preCount,postCount;
unsigned waits,preCalls,postCalls,spurious;
std::int64_t slept;
OSMessageQueue queue;
OSMessage messages[1];
bool replacePost;
bool recurse;
void post(u32 c){postCount=c;++postCalls;assert(VIGetRetraceCount()==c);assert(OSSendMessage(&queue,reinterpret_cast<void*>(std::uintptr_t(c)),OS_MESSAGE_NOBLOCK));}
void anotherPost(u32 c){post(c);}
void pre(u32 c){
    preCount=c;++preCalls;assert(VIGetRetraceCount()==c);
    if(replacePost){assert(VISetPostRetraceCallback(anotherPost)==post);replacePost=false;}
    // Hardware IRQs may advance during dispatch; callbacks retain one VI sample.
    if(recurse){bool rejected=false;try{waitVideoRetrace();}catch(const std::runtime_error&){rejected=true;}assert(rejected);recurse=false;}
}
}
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
namespace dusk::native {
u32 platformVideoRetraceCount(){return count;}
void platformWaitVideoRetrace(){++waits;if(spurious)--spurious;else ++count;}
void platformSleepNanoseconds(std::int64_t n){slept=n;}
}
template<class F>void rejected(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}assert(caught);}
int main(){
    OSInitMessageQueue(&queue,messages,1);FrameRetraceDeadline d;
    assert(VISetPreRetraceCallback(pre)==nullptr);assert(VISetPostRetraceCallback(post)==nullptr);
    auto first=waitForFrameRetraces(&queue,2,d);assert(first==1&&waits==1&&preCalls==1&&postCalls==1&&preCount==postCount);
    assert(waitForFrameRetraces(&queue,2,d)==3&&waits==3);
    // A slow frame must consume an already observed real later IRQ, without an
    // extra VBlank wait that would unnecessarily reduce the frame rate.
    count=10;const auto before=waits;assert(waitForFrameRetraces(&queue,2,d)==10&&waits==before);
    spurious=2;assert(waitForFrameRetraces(&queue,1,d)==12&&waits==before+4);
    replacePost=true;recurse=true;assert(waitForFrameRetraces(&queue,0,d)==13&&d.next==14);
    assert(preCount==postCount&&VISetPostRetraceCallback(post)==anotherPost);
    // The previous queue token can be consumed by the original display waiter.
    waitVideoRetrace();auto queueCalls=postCalls;assert(waitForFrameRetraces(&queue,1,d)==14&&postCalls==queueCalls);
    count=UINT32_MAX-1;d.next=UINT32_MAX-1;assert(waitForFrameRetraces(&queue,2,d)==UINT32_MAX-1&&d.next==0);
    assert(waitForFrameRetraces(&queue,2,d)==0&&d.next==2);assert(waitForFrameRetraces(&queue,2,d)==2);
    assert(VISetPreRetraceCallback(nullptr)==pre);assert(VISetPostRetraceCallback(nullptr)==post);
    rejected([&]{waitForFrameRetraces(&queue,2,d);}); // no success without original queue producer
    rejected([&]{waitForFrameRetraces(nullptr,1,d);});rejected([&]{waitForFrameRetraces(&queue,65536,d);});
    sleepGameTicks(0);assert(slept==0);sleepGameTicks(1);assert(slept==25);
    sleepGameTicks(40500000);assert(slept==1000000000);sleepGameTicks(40500001);assert(slept==1000000025);
    rejected([&]{sleepGameTicks(-1);});rejected([&]{sleepGameTicks(INT64_MAX);});
    puts("PASS: real retrace adapter with synthetic IRQ source, original OS queue, pre/post ownership, deadline/drop/spurious/wrap/reentrant/error cases and tick sleep conversion");
}
