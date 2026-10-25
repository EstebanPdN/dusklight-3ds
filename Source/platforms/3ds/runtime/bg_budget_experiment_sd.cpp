#include "bg_budget_experiment.hpp"
#include "bootstrap.hpp"
#include "m_Do/m_Do_ext.h"
#include <cstdio>
#include <cstdarg>
struct OSThread;
extern "C" OSThread* OSGetCurrentThread();
namespace dusk::bg76 {
namespace {
// Keep each complete row in one debug-SVC message; OSReport is only 256 bytes.
// This stack buffer is used only after the existing enabled/owner/event gates.
void logRow(const char* format, ...) __attribute__((format(printf, 1, 2)));
void logRow(const char* format, ...) {
    char text[768];
    va_list args; va_start(args, format);
    const int n=std::vsnprintf(text,sizeof(text),format,args);
    va_end(args);
    if(n<0){dusk::native::logText("BG76LOGERROR complete=0 reason=format\n");return;}
    if(static_cast<std::size_t>(n)>=sizeof(text)){
        dusk::native::logText("BG76LOGERROR complete=0 reason=truncated\n");return;
    }
    dusk::native::logText(text);
}
}

std::uintptr_t threadToken(){return reinterpret_cast<std::uintptr_t>(OSGetCurrentThread());}
void initializeFromSd(){
    static bool once=false;if(once)return;once=true;
    Config c{};char text[97];bool valid=false;
    if(auto* f=std::fopen("sdmc:/3ds/dusklight/bg-budget-experiment-76.txt","rb")){
        auto n=std::fread(text,1,sizeof(text),f);bool okay=!std::ferror(f);if(std::fclose(f))okay=false;
        valid=okay&&parse(text,n,c);
    }
    const bool initialized=initialize(valid?&c:nullptr,mDoExt_getGameHeap(),mDoExt_getArchiveHeap());
    // Once-only startup diagnostic, independent of the enabled event stream.
    if(valid&&!initialized)dusk::native::logText("BG76INIT rejected=1 requested=1 validconfig=1\n");
}
void flush(){
    // At most 8 rows/frame and 256 events/lifetime; no allocator hook I/O or heap walk.
    static bool announced=false;
    if(enabled()&&owner()&&!announced){announced=true;
        logRow("BG76CONFIG experimental=1 gameheap=%08lx archiveheap=%08lx actor=%lu aggregate=%lu headroom=%lu ledger=%lu fixed=%lu maxevents=256\n",
          (unsigned long)gameOwner(),(unsigned long)archiveOwner(),(unsigned long)configValues().actor,(unsigned long)configValues().aggregate,(unsigned long)configValues().headroom,
          (unsigned long)ledgerBytes(),(unsigned long)fixedBytes());
    }
    Event e{};for(unsigned i=0;i<8&&take(e);++i){
        logRow("BG76 scope=instance resourcecomplete=0 frame=%lu gen=%lu kind=%lu id=%lu room=%d block=%d actor=%08lx heap=%08lx roomheap=%08lx archiveowner=%08lx resourceowner=%08lx cap=%lu live=%lu request=%lu charged=%lu used=%lu payload=%lu result=%ld callback=%ld posttotal=%ld postok=%d drops=%lu ledgercomplete=%d\n",
          (unsigned long)e.frame,(unsigned long)e.generation,(unsigned long)e.kind,(unsigned long)e.id,(int)e.room,(int)e.block,(unsigned long)e.actor,(unsigned long)e.heap,(unsigned long)e.roomHeap,(unsigned long)e.archiveOwner,(unsigned long)e.resourceOwner,
          (unsigned long)e.cap,(unsigned long)e.live,(unsigned long)e.request,(unsigned long)e.charged,(unsigned long)e.used,(unsigned long)e.payload,(long)e.result,(long)e.callback,(long)e.postTotal,e.postTotal>=0&&std::uint32_t(e.postTotal)>=configValues().headroom,(unsigned long)dropped(),dropped()==0);
        logRow("BG76FREE gen=%lu kind=%lu before=%ld,%ld,%ld,%ld,%ld,%ld after=%ld,%ld,%ld,%ld,%ld,%ld\n",
          (unsigned long)e.generation,(unsigned long)e.kind,(long)e.before.gameTotal,(long)e.before.gameLargest,(long)e.before.archiveTotal,(long)e.before.archiveLargest,(long)e.before.roomTotal,(long)e.before.roomLargest,
          (long)e.after.gameTotal,(long)e.after.gameLargest,(long)e.after.archiveTotal,(long)e.after.archiveLargest,(long)e.after.roomTotal,(long)e.after.roomLargest);
    }
}
}
