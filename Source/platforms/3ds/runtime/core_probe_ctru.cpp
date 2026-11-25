#include "core_probe.hpp"
#include "audio_control.hpp"
#include <3ds.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <initializer_list>

extern "C" void OSReport(const char*,...);

namespace dusk::native {
namespace {
constexpr std::uint64_t ticksPerMs=SYSCLOCK_ARM11/1000;
constexpr unsigned windowMs=100;
struct Task {
    std::atomic<std::uint32_t> loops{};
    std::atomic<bool> stop{},running{};
    std::uint32_t fpscr{};
    std::int32_t core{-1}; // svcGetProcessorID on the busy thread (Azahar: always 0)
};
// 4096 steps of a linear congruential generator: integer work that stays in
// registers, so the cores do not meet in memory.
__attribute__((noinline)) std::uint32_t spin(std::uint32_t x){
    for(unsigned i=0;i<4096;++i)x=x*1664525u+1013904223u;
    return x;
}
__attribute__((target("arm"))) std::uint32_t readFpscr(){std::uint32_t value;__asm__ volatile("vmrs %0, fpscr":"=r"(value));return value;}
void worker(void* argument){
    auto& task=*static_cast<Task*>(argument);
    task.fpscr=readFpscr();task.core=svcGetProcessorID();
    task.running.store(true,std::memory_order_release);
    std::uint32_t x=1,loops=0;
    while(!task.stop.load(std::memory_order_relaxed)){x=spin(x);task.loops.store(++loops,std::memory_order_relaxed);}
    // The generator's value is used so the loop is not removed.
    if(!x)task.loops.fetch_add(1,std::memory_order_relaxed);
}
void line(std::FILE* out,const char* format,...){
    char text[240];
    va_list arguments;va_start(arguments,format);
    std::vsnprintf(text,sizeof(text),format,arguments);
    va_end(arguments);
    // On the card line by line: a console the probe stops keeps what came before.
    if(out){std::fprintf(out,"%s\n",text);std::fflush(out);}
    OSReport("Native core probe: %s\n",text);
}
// Loops of the generator on the calling thread in one window.
std::uint32_t ownLoops(){
    const std::uint64_t end=svcGetSystemTick()+windowMs*ticksPerMs;
    std::uint32_t x=1,loops=0;
    while(svcGetSystemTick()<end){x=spin(x);++loops;}
    return loops+(x?0:1);
}
struct Gsp {double meanUs{},maxUs{};unsigned calls{};};
// Small GSP requests (a 64-byte cache flush: one IPC round trip to the GSP
// module) for one window, the calling thread sleeping 1 ms between them.
Gsp gspWindow(void* memory){
    Gsp result;
    const std::uint64_t end=svcGetSystemTick()+windowMs*ticksPerMs;
    std::uint64_t total=0,longest=0;
    while(svcGetSystemTick()<end){
        const std::uint64_t begin=svcGetSystemTick();
        GSPGPU_FlushDataCache(memory,64);
        const std::uint64_t taken=svcGetSystemTick()-begin;
        total+=taken;if(taken>longest)longest=taken;++result.calls;
        svcSleepThread(1000000);
    }
    if(result.calls){
        result.meanUs=double(total)*1e6/SYSCLOCK_ARM11/result.calls;
        result.maxUs=double(longest)*1e6/SYSCLOCK_ARM11;
    }
    return result;
}
// One busy thread on `core` for one window; the calling thread measures GSP
// requests meanwhile. False when the thread could not be made or never ran.
Task task;
bool busyWindow(int core,void* memory,std::uint32_t& loops,Gsp& gsp,std::uint32_t& fpscr){
    task.loops.store(0);task.stop.store(false);task.running.store(false);task.fpscr=0;task.core=-1;
    loops=0;gsp={};fpscr=0;
    // Below the main thread and the audio mixer (a higher number is a lower priority).
    const Thread thread=threadCreate(worker,&task,16*1024,0x38,core,false);
    if(!thread)return false;
    for(unsigned waited=0;waited<200&&!task.running.load(std::memory_order_acquire);++waited)svcSleepThread(1000000);
    const bool ran=task.running.load(std::memory_order_acquire);
    if(ran){
        const std::uint32_t before=task.loops.load(std::memory_order_relaxed);
        gsp=gspWindow(memory);
        loops=task.loops.load(std::memory_order_relaxed)-before;
        fpscr=task.fpscr;
    }
    task.stop.store(true,std::memory_order_release);
    // A thread that never got the core is left behind rather than waited for.
    if(R_SUCCEEDED(threadJoin(thread,2000000000ull)))threadFree(thread);
    else line(nullptr,"thread on core %d did not end within 2 s",core);
    return ran;
}
double flushUs(void* memory,std::size_t bytes,unsigned calls,bool kernel){
    const std::uint64_t begin=svcGetSystemTick();
    for(unsigned n=0;n<calls;++n){
        if(kernel)svcFlushProcessDataCache(CUR_PROCESS_HANDLE,reinterpret_cast<u32>(memory),bytes);
        else GSPGPU_FlushDataCache(memory,bytes);
    }
    return double(svcGetSystemTick()-begin)*1e6/SYSCLOCK_ARM11/calls;
}
}
void runCoreProbe(std::FILE* out){
    constexpr std::size_t bytes=256*1024;
    void* const memory=linearAlloc(bytes);
    if(!memory){line(out,"core_probe=skipped (no linear memory)");return;}
    const std::uint64_t began=svcGetSystemTick();
    const std::uint32_t own=ownLoops();
    line(out,"core_probe_core0=loops_per_%ums:%lu fpscr:%08lx",windowMs,static_cast<unsigned long>(own),static_cast<unsigned long>(readFpscr()));
    const Gsp idle=gspWindow(memory);
    line(out,"core_probe_gsp_request_idle=mean_us:%.1f max_us:%.1f calls:%u",idle.meanUs,idle.maxUs,idle.calls);
    auto share=[own](std::uint32_t loops){return own?double(loops)/own:0.0;};
    std::uint32_t loops,fpscr;Gsp gsp;
    // The audio mixer (audio-core.txt) has the higher priority on its core:
    // that core's figure below is what it leaves.
    const int audioCore=nativeAudioWorkerCore();
    line(out,"core_probe_audio_core=%d",audioCore);
    // Core 2: beside the GX worker (idle here) and, when audio_core is 2, the mixer.
    if(busyWindow(2,memory,loops,gsp,fpscr))
        line(out,"core_probe_core2=loops:%lu of_core0:%.2f gsp_mean_us:%.1f gsp_max_us:%.1f fpscr:%08lx",static_cast<unsigned long>(loops),share(loops),
            gsp.meanUs,gsp.maxUs,static_cast<unsigned long>(fpscr));
    else line(out,"core_probe_core2=no thread");
    // Core 1: the system core. An application thread runs there only after
    // APT_SetAppCpuTimeLimit, for that share of the core at most.
    u32 limitBefore=0;
    const Result got=APT_GetAppCpuTimeLimit(&limitBefore);
    line(out,"core_probe_limit_before=%lu (result %08lx)",static_cast<unsigned long>(limitBefore),static_cast<unsigned long>(got));
    // Core 1 takes one application thread, and changing the limit would
    // starve the mixer when it is that thread.
    if(audioCore==1)line(out,"core_probe_core1=skipped (the audio mixer runs there)");
    else for(const u32 limit:{30u,50u,80u}){
        const Result set=APT_SetAppCpuTimeLimit(limit);
        if(R_FAILED(set)){line(out,"core_probe_core1_limit%lu=refused (result %08lx)",static_cast<unsigned long>(limit),static_cast<unsigned long>(set));continue;}
        if(busyWindow(1,memory,loops,gsp,fpscr))
            line(out,"core_probe_core1_limit%lu=loops:%lu of_core0:%.2f gsp_mean_us:%.1f gsp_max_us:%.1f fpscr:%08lx",static_cast<unsigned long>(limit),
                static_cast<unsigned long>(loops),share(loops),gsp.meanUs,gsp.maxUs,static_cast<unsigned long>(fpscr));
        else line(out,"core_probe_core1_limit%lu=no thread",static_cast<unsigned long>(limit));
    }
    // As found; a console that had none keeps the smallest one (no
    // application thread is left on core 1 for it to matter).
    if(audioCore!=1){
        const Result restored=APT_SetAppCpuTimeLimit(limitBefore>=5?limitBefore:5);
        line(out,"core_probe_limit_restored=%lu (result %08lx)",static_cast<unsigned long>(limitBefore>=5?limitBefore:5),static_cast<unsigned long>(restored));
    }
    // libctru documents core 3 as closed to applications; the Alpha 11
    // console ran this thread there (0.86 of core 0). Beside the mixer when
    // audio_core is 3.
    if(busyWindow(3,memory,loops,gsp,fpscr))line(out,"core_probe_core3=loops:%lu of_core0:%.2f says_core:%ld",static_cast<unsigned long>(loops),share(loops),
        static_cast<long>(task.core));
    else line(out,"core_probe_core3=no thread");
    // A cache flush of 4 KiB and of 256 KiB, through GSP and through the kernel.
    line(out,"core_probe_flush_gsp=us_4k:%.1f us_256k:%.1f",flushUs(memory,4096,100,false),flushUs(memory,bytes,8,false));
    line(out,"core_probe_flush_kernel=us_4k:%.1f us_256k:%.1f",flushUs(memory,4096,100,true),flushUs(memory,bytes,8,true));
    linearFree(memory);
    line(out,"core_probe_ms=%.0f",double(svcGetSystemTick()-began)*1000.0/SYSCLOCK_ARM11);
}
}
