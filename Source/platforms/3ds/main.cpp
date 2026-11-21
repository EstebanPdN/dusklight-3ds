#include "runtime/durable_file.hpp"
#include "runtime/copy_lifecycle_probe.hpp"
#include "gx_geometry_pica.hpp"
#include "gx_command_probe.hpp"
#include "scene_bootstrap.hpp"
#if defined(DUSK_PICA_GAME_DRAW)
#include "native/game_draw_api.hpp"
#endif
#include "runtime/beta_screen.hpp"
#include <3ds.h>
#include <citro3d.h>
#include "runtime/ctru_abi.hpp"
#include "diagnostics.hpp"
#include "runtime/bootstrap.hpp"
#ifdef DUSK_GAME_STARTUP
#include "runtime/application_dump_ctru.hpp"
#include "runtime/frame_telemetry.hpp"
#include "runtime/benchmark_input.hpp"
#endif
#include "runtime/cache_maintenance_profile.hpp"
#include "runtime/video_retrace.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gpu_canary.hpp"
#include "runtime/gx_framebuffer.hpp"
#include "runtime/video_mode.hpp"
#include "runtime/game_startup.hpp"
#include "runtime/os_thread.hpp"
#include "runtime/pad_bootstrap.hpp"
#include "runtime/clock.hpp"
#include "dusk/game_clock.h"
#include "runtime/disc.hpp"
#include "gx_pica.hpp"
#include "gx_texture_pica.hpp"
#include "j2d_picture_probe.hpp"
#include "runtime/gx_state.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <malloc.h>

// Small diagnostic admission, NOT the eventual game's memory budget.
// Both are counted within application memory; no 178 MiB mode is requested.
extern "C" {
#ifdef DUSK_GAME_STARTUP
u32 __stacksize__ = 256 * 1024;
u32 __ctru_heap_size = 80 * 1024 * 1024;
// 20 MiB linear (was 16): the texture cache needs ~10 MiB at the title on top
// of geometry, captures and readbacks. The application region still had
// ~9.5 MiB free with the old split (measured in a full dump).
u32 __ctru_linear_heap_size = 20 * 1024 * 1024;
#else
u32 __ctru_heap_size = 24 * 1024 * 1024;
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;
#endif
}
namespace {
using namespace dusk::native;
bool graphicsReady, appSuspended, appSleeping, kernelReady, timingReset;
#ifdef DUSK_COMMAND_PROBE_STARTUP
bool commandProbeVisible=true;
#else
bool commandProbeVisible=false;
#endif
bool suspended() { return appSuspended || appSleeping; }
void onApt(APT_HookType type, void*) {
    if (type == APTHOOK_ONSUSPEND) appSuspended = true;
    if (type == APTHOOK_ONSLEEP) appSleeping = true;
    if (type == APTHOOK_ONRESTORE) appSuspended = false;
    if (type == APTHOOK_ONWAKEUP) appSleeping = false;
    if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) {
        timingReset = true;
        osSetSpeedupEnable(true);
    }
    // Citro3D's own APT hook drains its GPU queue before relinquishing GSP.
    // Do not wait for VBlank here: Citro3D disables those callbacks on suspend.
    if (kernelReady) setClockPaused(suspended() || dumpBusy());
}
void panel(const DiagnosticSnapshot& s, unsigned page, const DumpArm& arm, bool workerStarted) {
#ifdef DUSK_SCENE_STARTUP
    std::printf("\x1b[2JOrdon original J3D geometry\nTextures/actors pending\n%.90s\nFrame %llu | %.2f ms\nCircle pad: camera\nSTART: exit\n",nativeSceneStatus(),static_cast<unsigned long long>(s.frame),s.frameMs);
    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr),320*240*2);
    return;
#endif
    std::printf("\x1b[2J\x1b[1;1H");
    if (s.visible) {
        std::printf("DUSKLIGHT New3DS 422d7bb1 dirty\nNative bootstrap O2 | page %u/4\nNOT GAMEPLAY / NO TITLE YET\n\n", page + 1);
        if (page == 0) {
            std::printf("N3DS: %s  clock/L2 requested\nScene/tick: n/a  Frame: %llu\n",
                s.new3ds ? "yes" : "NO", static_cast<unsigned long long>(s.frame));
            std::printf("FPS %.1f | %.2f ms\np50 %.2f p95 %.2f p99 %.2f\nTarget 33.33 ms (shell only)\n",
                s.frameMs > 0 ? 1000.0 / s.frameMs : 0, s.frameMs, s.p50, s.p95, s.p99);
            std::printf("Game update/draw/GPU wait: n/a\nAudio/I-O timings: n/a\nJKR: %s worker TLS/heap: %s\n",
                s.kernelOkay ? "OK" : "FAIL", !startupFinished() ? "pending" : s.workerOkay ? "OK" : "FAIL");
            std::printf("JKR used/peak*/free %lu/%lu/%lu K\nLinear free %lu K; used/peak n/a\n",
                static_cast<unsigned long>(s.gameHeap.used / 1024), static_cast<unsigned long>(s.gameHeap.peak / 1024),
                static_cast<unsigned long>(s.gameHeap.free / 1024), static_cast<unsigned long>(s.linearFree / 1024));
            std::printf("PICA texture cache %lu KiB\n", static_cast<unsigned long>(gxTextureBytes()/1024));
            std::printf("Indexed arena %lu / 524288 B\n", static_cast<unsigned long>(gxGeometryBytes()));
            if (commandProbeVisible) {
                const auto& command=commandProbeStatistics();
                std::printf("SYNTHETIC 3-map command probe\nCmd drawn/reject %llu/%llu\n%.38s\n",
                    static_cast<unsigned long long>(command.submitted),
                    static_cast<unsigned long long>(command.rejected),command.failure);
            }
            const auto gxStats = gx::statistics();
            std::printf("GX drawn/drop %llu/%llu\nGX: %.28s | %.24s\nPICA: %.32s\n",
                static_cast<unsigned long long>(gxStats.drawsSubmitted),
                static_cast<unsigned long long>(gxStats.drawsDropped),
                gx::errorText(gxStats.firstError), gxStats.firstErrorCall ? gxStats.firstErrorCall : "none", gxPicaFailure());
        } else if (page == 1) {
            const auto gx = dusk::native::gx::statistics();
            std::printf("Citro3D top target: 400x240 2D\nFallback IA8 quad: %s\nOriginal J2D picture/primitive path\nGX draws %llu dropped %llu\nGX error: %.25s\nPICA: %.32s\nJ3D/actors: not linked\nNOT A GAME TITLE SCREEN\n", s.previewReady ? "allocated" : "not allocated",
                static_cast<unsigned long long>(gx.drawsSubmitted), static_cast<unsigned long long>(gx.drawsDropped),
                dusk::native::gx::errorText(gx.firstError), gxPicaFailure());
        } else if (page == 2) {
            std::printf("App region: %lu MiB (launcher)\nLibc/linear reserved: 24/8 MiB\nApp heap used/peak/free: n/a\nLinear free: %lu KiB\n",
                static_cast<unsigned long>(s.appRegionBytes / (1024 * 1024)), static_cast<unsigned long>(s.linearFree / 1024));
            std::printf("JKR budget/free: %lu/%lu KiB\nJKR used/peak*: %lu/%lu KiB\n*sampled; bootstrap arena only\nPrepared data cache: not built\nStreaming queue/latency: n/a\n",
                static_cast<unsigned long>(s.gameHeap.budget / 1024), static_cast<unsigned long>(s.gameHeap.free / 1024),
                static_cast<unsigned long>(s.gameHeap.used / 1024), static_cast<unsigned long>(s.gameHeap.peak / 1024));
        } else {
            std::printf("Audio/NDSP: not initialized\nRate/voices/underruns: n/a\nSaving: disabled; no save writes\nL+R+A: quick (no RAM)\nL+R+B: full private app RAM\nHold 1 second, release to rearm\nReserve: >=8 MiB, preflight\nPRIVATE - NEVER UPLOAD OR COMMIT\n");
        }
        const auto discText = s.discStatus < 0 ? "validation pending" :
            s.discStatus == static_cast<int>(DiscStatus::missing) ? "Missing zelda.twilight on SD" :
            discStatusText(static_cast<DiscStatus>(s.discStatus));
        std::printf("\nISO: %.34s\n%.39s\n%.39s\n", discText, startupResourceStatus(), j2dPictureProbeStatus());
        std::printf("Buttons: %08lx\n", static_cast<unsigned long>(s.buttons));
        const auto pad = nativePadSnapshot();
        std::printf("Original PAD %08lx / %08lx\nCP %.2f %.2f | CS %.2f %.2f\n",
            static_cast<unsigned long>(pad.held), static_cast<unsigned long>(pad.pressed),
            pad.circleX, pad.circleY, pad.cstickX, pad.cstickY);
        std::printf("\nDump: %s\n", !workerStarted ? "worker unavailable" : arm.arming() ? "arming (hold 1 second)" : dumpStateText());
        if (dumpBusy()) std::printf("%lu / %lu KiB\n", static_cast<unsigned long>(dumpProgressKiB()), static_cast<unsigned long>(dumpTotalKiB()));
        if (completedDumpPath()[0]) std::printf("Saved under dusklight-3ds/dumps\n");
        std::printf("\x1b[27;1HSELECT+Left/Right: page\nX: command probe | START: exit\n");
    } else if (arm.arming() || dumpBusy()) {
        std::printf("\x1b[28;1HPRIVATE DUMP %s", arm.arming() ? "ARMING" : "ACTIVE");
    }
    std::printf("\x1b[29;36H[DBG]");
    // Bottom console uses an unbuffered RGB565 surface; flush only after refresh.
    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr), 320 * 240 * 2);
}
}
namespace dusk::native {
// Hardware diagnosis: the native log is kept in a RAM ring and written to
// sdmc:/3ds/DuskLight/log.txt only by the main thread (flushNativeLog from the
// frame loop) or on fatal/exception. Per-line SD writes from many threads made
// the system FS module fault on hardware.
// The ring lives in the linear heap: the executable's static size must stay at
// the 114 baseline (code+bss+96 MiB heaps fill the New 3DS application region).
namespace { constexpr std::size_t logRingBytes=128*1024,flushChunkBytes=32*1024; char* logRing{}; char* flushChunk{};
std::size_t logHead{},logFlushed{}; LightLock logLock=1; DurableLog sdLog{}; bool sdLogTried{};
// One SD writer at a time (frame loop, fatal paths); logLock only guards the ring.
LightLock flushLock=1; u64 lastFlushTick{};
struct { unsigned calls,writes; std::uint64_t bytes,ticks,longest; } flushStats{};
// Each start keeps the logs of the three before it (log-1.txt is the last
// one): a console that froze and was switched off keeps its evidence.
void openSdLog(){ if(sdLogTried)return; sdLogTried=true; durableRotate("/3ds/DuskLight/log","txt",3); durableLogOpen(sdLog,"/3ds/DuskLight/log.txt"); }
// Ring bytes [from, from+n) into out; logLock held.
void copyRing(std::size_t from,char* out,std::size_t n){
    const auto at=from%logRingBytes,first=std::min(n,logRingBytes-at);
    __builtin_memcpy(out,logRing+at,first);
    __builtin_memcpy(out+first,logRing,n-first);
} }
void logText(const char* text) {
    const auto length=__builtin_strlen(text);
    svcOutputDebugString(text, length);
    LightLock_Lock(&logLock);
    if(!logRing){logRing=static_cast<char*>(linearAlloc(logRingBytes));flushChunk=static_cast<char*>(linearAlloc(flushChunkBytes));}
    if(!logRing){LightLock_Unlock(&logLock);return;}
    for(std::size_t n=0;n<length;++n)logRing[(logHead+n)%logRingBytes]=text[n];
    logHead+=length;
    LightLock_Unlock(&logLock);
}
void flushNativeLog(){
    // SD writes take tens of milliseconds each. They used to run under
    // logLock, so every thread that logged meanwhile (loaders, the audio
    // mixer) waited for the card; Alpha 6 saw fills of hundreds of ms right
    // after this flush. Now the ring is copied under the lock and written
    // outside it, in chunks of up to 32 KiB: one FS call each instead of one
    // per 4 KiB. Each write is on the card when it returns (FS flush), even if
    // the console has to be switched off next.
    LightLock_Lock(&flushLock);
    const u64 start=svcGetSystemTick();
    openSdLog();
    char fallback[2048];
    char* chunk=flushChunk?flushChunk:fallback;
    const std::size_t capacity=flushChunk?flushChunkBytes:sizeof(fallback);
    std::size_t total=0;
    // Up to what was waiting on entry: other threads keep logging during the
    // SD writes, and chasing them would give the flush no fixed end.
    LightLock_Lock(&logLock);const std::size_t target=logHead;LightLock_Unlock(&logLock);
    while(sdLog.open){
        LightLock_Lock(&logLock);
        if(logHead-logFlushed>logRingBytes)logFlushed=logHead-logRingBytes;
        const std::size_t from=logFlushed,n=logRing&&target>from?std::min(capacity,target-from):0;
        if(n)copyRing(from,chunk,n);
        LightLock_Unlock(&logLock);
        if(!n||!durableLogAppend(sdLog,chunk,n))break;
        LightLock_Lock(&logLock);logFlushed=from+n;LightLock_Unlock(&logLock);
        total+=n;++flushStats.writes;
    }
    const u64 end=svcGetSystemTick();lastFlushTick=end;
    ++flushStats.calls;flushStats.bytes+=total;flushStats.ticks+=end-start;flushStats.longest=std::max<std::uint64_t>(flushStats.longest,end-start);
    const auto stats=flushStats;
    if(stats.calls>=64)flushStats={};
    LightLock_Unlock(&flushLock);
    // Its cost is outside the frame telemetry (the flush follows the sample).
    if(stats.calls>=64){
        char line[160];
        std::snprintf(line,sizeof(line),"Native log flush: %u calls, %llu bytes in %u SD writes, %.1f ms in all, longest %.1f ms\n",stats.calls,
            static_cast<unsigned long long>(stats.bytes),stats.writes,stats.ticks*1000.0/SYSCLOCK_ARM11,stats.longest*1000.0/SYSCLOCK_ARM11);
        logText(line);
    }
}
// The frame loop's flush: at most every 5 seconds (it was every 30 pictures),
// or sooner once a quarter of the ring is waiting so nothing is overwritten.
// The SD writer runs below the game on its default core. Only bounded
// capture admissions and explicit receipt fences wait for capture I/O.
// Fatal paths still call flushNativeLog themselves (flushLock orders the two).
namespace { LightEvent sdWriterKick, captureProgress; Thread sdWriter{}; bool sdWriterTried{}; std::atomic<bool> sdWriterLogDue{},sdWriterStop{};
void sdWriterMain(void*){
    for(;;){
        LightEvent_Wait(&sdWriterKick);
        // If shutdown was signalled during a drain, service that final wake
        // too. The producer has stopped before the shutdown wake is issued.
        const bool stopping=sdWriterStop.load(std::memory_order_acquire);
        if(sdWriterLogDue.exchange(false,std::memory_order_acq_rel))flushNativeLog();
        gpuCanaryWriteSessionRecord();
#ifdef DUSK_GAME_STARTUP
        if (!writePendingZoneFrameCsv()) logText("Native story frame CSV write failed\n");
        writeBenchmarkCapturePending();
#endif
        if(stopping)return;
    }
}
bool sdWriterReady(){
    if(!sdWriterTried){
        sdWriterTried=true;LightEvent_Init(&sdWriterKick,RESET_ONESHOT);
        LightEvent_Init(&captureProgress,RESET_ONESHOT);
        s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
        // Joinable: stopNativeSdWriter ends and joins it before libc unmaps
        // the heap its stack is in.
        sdWriter=threadCreate(sdWriterMain,nullptr,16*1024,priority+8<0x3f?priority+8:0x3f,-2,false);
        logText(sdWriter?"Native SD writer thread started\n":"Native SD writer thread refused: log flushes stay on the game thread\n");
    }
    return sdWriter!=nullptr;
}
// Shutdown, main thread, after the game workers are joined. Its flushes and
// session records go back to their callers (sdWriterReady is false after).
void stopNativeSdWriter(){
    if(!sdWriter)return;
    // Only the stop flag below can let this worker return. Capture the
    // handle while libctru still exposes it, even if the final wake preempts us.
    const Handle kernel=threadGetHandle(sdWriter);
    sdWriterStop.store(true,std::memory_order_release);LightEvent_Signal(&sdWriterKick);
    // All producers have stopped. A healthy SD card took over seven seconds
    // for individual visual pairs in Alpha19. Keep the heap and FS alive for
    // the complete final drain. The kernel handle signals actual thread exit.
    const Result joined=svcWaitSynchronization(kernel,U64_MAX);
    if(joined!=0)containShutdownFailure("SD writer wait handle=%08lx result=%08lx",
        static_cast<unsigned long>(kernel),static_cast<unsigned long>(joined));
    threadFree(sdWriter);sdWriter=nullptr;
    logText("Native shutdown phase=SD writer stop: joined\n");
}}
void containShutdownFailure(const char* format,...){
    char reason[176],line[256];
    std::va_list args;
    va_start(args,format);
    std::vsnprintf(reason,sizeof(reason),format,args);
    va_end(args);
    std::snprintf(line,sizeof(line),"Native shutdown failure: %s; kernel process exit\n",reason);
    // An unjoined worker may own the ordinary log's locks. Issue the debug
    // string and a separate FS_WRITE_FLUSH receipt without acquiring either.
    svcOutputDebugString(line,std::strlen(line));
    durableAppendText("/3ds/DuskLight/shutdown-errors.txt",line);
    gpuCanarySessionFatal(reason);
    svcExitProcess();
}
bool kickNativeSdWriterAsync(){
    if(!sdWriterReady())return false;
    LightEvent_Signal(&sdWriterKick);
    return true;
}
void waitForNativeCaptureProgress(){LightEvent_Wait(&captureProgress);}
void signalNativeCaptureProgress(){LightEvent_Signal(&captureProgress);}
void kickNativeSdWriter(){if(!kickNativeSdWriterAsync())gpuCanaryWriteSessionRecord();}
void flushNativeLogPeriodic(){
    std::size_t pending;
    LightLock_Lock(&logLock);pending=logHead-logFlushed;LightLock_Unlock(&logLock);
    if(svcGetSystemTick()-lastFlushTick<5*SYSCLOCK_ARM11&&pending<logRingBytes/4)return;
    if(!sdWriterReady()){flushNativeLog();return;}
    // Due again only after this flush ends (lastFlushTick), not every picture.
    if(!sdWriterLogDue.exchange(true,std::memory_order_acq_rel))LightEvent_Signal(&sdWriterKick);
}
std::size_t nativeLogPosition(){return logHead;}
void writeNativeLogSince(std::size_t mark,std::FILE* file){
    LightLock_Lock(&logLock);
    if(logHead-mark>logRingBytes)mark=logHead-logRingBytes;
    char chunk[1024];
    while(file&&logRing&&mark<logHead){
        std::size_t n=0;
        while(n<sizeof(chunk)&&mark+n<logHead){chunk[n]=logRing[(mark+n)%logRingBytes];++n;}
        std::fwrite(chunk,1,n,file);mark+=n;
    }
    LightLock_Unlock(&logLock);
}
// ARM11 exception report (data/prefetch abort, undefined instruction).
void logException(ERRF_ExceptionInfo* sdkInfo,CpuRegisters* regs){
    // The SDK struct has an enum member: read it with the SDK's layout (ctru_abi.hpp).
    static char text[2048]; // not on the faulting stack
    std::size_t used=0;
    auto add=[&](const char* format,auto... values){
        const int n=std::snprintf(text+used,sizeof(text)-used,format,values...);
        if(n>0)used=std::min(sizeof(text)-1,used+std::size_t(n));
    };
    add("Native EXCEPTION type=%u far=%08lx fsr=%08lx pc=%08lx lr=%08lx sp=%08lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx thread=%p\n",
        abi::exceptionKind(sdkInfo),static_cast<unsigned long>(abi::exceptionFar(sdkInfo)),static_cast<unsigned long>(abi::exceptionFsr(sdkInfo)),
        static_cast<unsigned long>(regs->pc),static_cast<unsigned long>(regs->lr),static_cast<unsigned long>(regs->sp),
        static_cast<unsigned long>(regs->r[0]),static_cast<unsigned long>(regs->r[1]),static_cast<unsigned long>(regs->r[2]),static_cast<unsigned long>(regs->r[3]),
        static_cast<void*>(threadGetCurrent()));
    // The rest of the registers and the call chain: every stack word that
    // points into the program's code is a return-address candidate
    // (arm-none-eabi-addr2line -f -C -i -e dusklight-native.elf <address>).
    add("Native EXCEPTION registers r4=%08lx r5=%08lx r6=%08lx r7=%08lx r8=%08lx r9=%08lx r10=%08lx r11=%08lx r12=%08lx cpsr=%08lx\n",
        static_cast<unsigned long>(regs->r[4]),static_cast<unsigned long>(regs->r[5]),static_cast<unsigned long>(regs->r[6]),
        static_cast<unsigned long>(regs->r[7]),static_cast<unsigned long>(regs->r[8]),static_cast<unsigned long>(regs->r[9]),
        static_cast<unsigned long>(regs->r[10]),static_cast<unsigned long>(regs->r[11]),static_cast<unsigned long>(regs->r[12]),
        static_cast<unsigned long>(regs->cpsr));
    {MemInfo memory{};PageInfo page{};u32 codeEnd=0x00100000;
     if(R_SUCCEEDED(svcQueryMemory(&memory,&page,0x00100000)))codeEnd=memory.base_addr+memory.size;
     const u32 sp=regs->sp&~3u;
     if(R_SUCCEEDED(svcQueryMemory(&memory,&page,sp))&&(memory.perm&MEMPERM_READ)){
        const u32 end=memory.base_addr+memory.size;unsigned found=0;
        add("Native EXCEPTION stack code addresses:");
        for(u32 at=sp;at+4<=end&&at-sp<0x3000&&found<48;at+=4){
            const u32 word=*reinterpret_cast<const u32*>(at);
            if(word>=0x00100000&&word<codeEnd){add(" +%lx=%08lx",static_cast<unsigned long>(at-sp),static_cast<unsigned long>(word));++found;}
        }
        add("\n");
     }}
    svcOutputDebugString(text,__builtin_strlen(text));
    // Best effort, no lock (the faulting thread may hold it): append to ring, write once.
    if(logRing)for(std::size_t n=0;text[n];++n)logRing[(logHead+n)%logRingBytes]=text[n];
    logHead+=__builtin_strlen(text);
    openSdLog();
    if(sdLog.open&&logRing){
        if(logHead-logFlushed>logRingBytes)logFlushed=logHead-logRingBytes;
        char chunk[256];
        while(logFlushed<logHead){
            std::size_t n=0;
            while(n<sizeof(chunk)&&logFlushed+n<logHead){chunk[n]=logRing[(logFlushed+n)%logRingBytes];++n;}
            if(!durableLogAppend(sdLog,chunk,n))break;
            logFlushed+=n;
        }
    }
    // Last, after the log is on the card: the GPU guard's session record, so
    // the next start does not report a power-off or freeze. Once only, in
    // case the handler itself faults.
    static bool sessionRecorded;
    if(!sessionRecorded){
        sessionRecorded=true;char note[64];
        std::snprintf(note,sizeof(note),"CPU exception pc=%08lx",static_cast<unsigned long>(regs->pc));
        gpuCanarySessionFatal(note);
    }
    svcExitProcess();
}
void installExceptionLogger(){threadOnException(logException,RUN_HANDLER_ON_FAULTING_STACK,WRITE_DATA_TO_HANDLER_STACK);}
namespace { bool fatalUnattended; }
void setFatalUnattended(bool unattended){fatalUnattended=unattended;}
[[noreturn]] void fatal(const char* text) {
    if(const auto relay=fatalRelayHook;relay&&relay(text))for(;;)svcSleepThread(1000000000LL);
    logText("Native fatal: ");logText(text);logText("\n");flushNativeLog();
    // The GPU guard's session record would otherwise still say "running",
    // and the next start would report a power-off or freeze (Alpha 6 zone 84).
    gpuCanarySessionFatal(text);
    // A worker thread cannot own the console (the main loop redraws it) and
    // would hang silently: keep the log and terminate the whole process.
    if (threadGetCurrent() != nullptr) { logText("Native fatal on a worker thread: process exit\n"); flushNativeLog(); svcExitProcess(); }
    if (fatalUnattended && graphicsReady) {
        std::printf("\x1b[2JNative fatal error\n%.200s\n\nAutomatic test: restarting.\nIf the game does not reopen,\nopen it again: the test resumes.\n", text);
        gfxFlushBuffers(); svcSleepThread(3000000000LL);
        // Leave through APT so the system relaunches this title; the kernel
        // then ends the process with its workers and storage intact.
        aptSetChainloaderToSelf(); aptExit();
        svcExitProcess();
    }
    if (graphicsReady && !platformGpuFailurePending()) {
        std::printf("\x1b[2JNative fatal error\n%.200s\nSTART: exit\n", text);
        while (aptMainLoop()) {
#ifdef DUSK_GAME_STARTUP
            scanNativeGameInput(); if(nativeGameInput().down&KEY_START)break;
#else
            hidScanInput(); if (hidKeysDown() & KEY_START) break;
#endif
            gfxFlushBuffers(); gspWaitForVBlank();
        }
    }
    // A fatal path can still have live game workers, stacks and GPU consumers.
    // std::exit would run libctru heap teardown before killing those threads.
    // Let the kernel terminate the complete process with its storage intact.
    logText("Native fatal containment: kernel process exit; normal shutdown incomplete\n");
    svcExitProcess();
}
// newlib's abort, exit and _exit end the process without a word: operator
// new out of memory, a pure virtual call or std::terminate (Azahar showed the
// game simply vanishing in the first test zone). Linked with --wrap
// (CMakeLists.txt), each now leaves its caller in the log first; abort and a
// nonzero exit also close the GPU session record as a fatal, so the next
// start does not report a power-off or a GPU stop.
namespace {
void reportProcessEnd(const char* what,int code,void* caller){
    static std::atomic<bool> reported{};
    if(reported.exchange(true))return; // exit calls _exit: one line
    char line[128];
    std::snprintf(line,sizeof(line),"Native process end: %s(%d) called from %p\n",what,code,caller);
    logText(line);flushNativeLog();
    if(code||std::strcmp(what,"abort")==0){
        char note[64];std::snprintf(note,sizeof(note),"%s(%d) from %p",what,code,caller);
        gpuCanarySessionFatal(note);
    }
}
}
extern "C" {
[[noreturn]] void __real_abort();
[[noreturn]] void __real_exit(int);
[[noreturn]] void __real__exit(int);
[[noreturn]] void __wrap_abort(){reportProcessEnd("abort",0,__builtin_return_address(0));__real_abort();}
[[noreturn]] void __wrap_exit(int code){reportProcessEnd("exit",code,__builtin_return_address(0));__real_exit(code);}
[[noreturn]] void __wrap__exit(int code){reportProcessEnd("_exit",code,__builtin_return_address(0));__real__exit(code);}
}
// Console GPU guard: the PICA stopped on a draw. The evidence is on the SD;
// with `restart` the system is asked to start the game again (the guard leaves
// that draw out), otherwise the message stays until START.
[[noreturn]] void restartAfterGpuHang(const char* text,bool restart) {
    if(const auto relay=gpuHangRelayHook;relay&&relay(text,restart))for(;;)svcSleepThread(1000000000LL);
    logText("Native GPU guard: ");logText(text);logText("\n");flushNativeLog();
    if (graphicsReady) {
        // Restarting the title does not reset a stopped PICA (CIA g: the next
        // start stopped on its first clear), so ask for a power cycle.
        std::printf("\x1b[2JDuskLight: the GPU stopped.\n%.300s\n\nSaved on the SD card.\n%s\n", text,
            restart ? "\nHold POWER until the console\nswitches off, switch it on and\nopen DuskLight again: it goes on\nwithout that draw."
                    : "\nPlease send the /3ds/DuskLight\nfolder. Hold POWER to switch off.");
        gfxFlushBuffers();
        for (;;) svcSleepThread(1000000000LL);
    }
    svcExitProcess();
}
void waitForRetrace() { waitVideoRetrace(); }
namespace { std::atomic<std::uintptr_t> ignoredBegin{},ignoredEnd{}; std::atomic<std::uint32_t> ignoredCount{}; }
void ignoreCacheInvalidation(const void* memory,std::uint32_t bytes){
    const auto begin=reinterpret_cast<std::uintptr_t>(memory);
    ignoredEnd.store(0,std::memory_order_relaxed);ignoredBegin.store(begin,std::memory_order_relaxed);ignoredEnd.store(begin+bytes,std::memory_order_relaxed);
}
std::uint32_t ignoredCacheInvalidations(){return ignoredCount.load(std::memory_order_relaxed);}
void invalidateCache(void* memory, std::uint32_t size) {
    // A range only this process's CPU threads use (ignoreCacheInvalidation):
    // the cores' data caches are coherent, so the kernel call buys nothing.
    const auto at=reinterpret_cast<std::uintptr_t>(memory);
    if(at>=ignoredBegin.load(std::memory_order_relaxed)&&at+size<=ignoredEnd.load(std::memory_order_relaxed)){
        ignoredCount.fetch_add(1,std::memory_order_relaxed);return;
    }
    auto& profile=cacheMaintenanceProfile;
    const auto start=profile.active?svcGetSystemTick():0;
    // Game DCInvalidateRange on ordinary process memory. GSP rejects such ranges
    // on hardware (Azahar accepts them); the kernel call covers any mapped
    // process memory. A failure is reported once, never fatal: these ranges are
    // CPU data, and every GPU-consumed buffer is maintained by the renderer.
    // Clean before invalidating: on the GameCube a DMA filled these ranges, here
    // the CPU did (fread, Yaz0, ARAM copies) and the game's paired DCStoreRange
    // is no longer forwarded. A plain invalidate discarded those dirty lines on
    // a New 3DS (archives rejected as malformed, then a crash); Azahar has no
    // cache, so it never showed. Only loads call this; gameplay frames make 0.
    if (R_FAILED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(memory), size))) {
        static bool reported; if (!reported) { reported = true; logText("Native cache invalidation failed once (ignored)\n"); }
    }
    if(profile.active){profile.invalidateTicks+=svcGetSystemTick()-start;profile.invalidateBytes+=size;++profile.invalidateCalls;}
}
static bool gameCacheFlush; // SD switch game-cache-flush.txt: the previous behaviour, for a console A/B.
void flushCache(void* memory, std::uint32_t size) {
    // Game DCFlushRange/DCStoreRange on ordinary process memory (display
    // lists, deformed vertices, CPU-drawn images). On the GameCube this
    // published the bytes to the GPU; here the translator reads them on the
    // CPU, and every buffer the PICA or the DSP consumes is flushed by its
    // owner (renderer, audio). The kernel call was pure cost: about 100 calls
    // per frame in a field scene. Only the diagnostic count is kept.
    auto& profile=cacheMaintenanceProfile;
    if(profile.active){profile.flushBytes+=size;++profile.flushCalls;}
    if(gameCacheFlush)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(memory), size);
}
void setGameCacheFlush(bool forward) { gameCacheFlush = forward; }
}
int main() {
    using namespace dusk::native;
    installExceptionLogger();
    logText("Native entry: main\n");
    gfxInitDefault(); gfxSet3D(false); consoleInit(GFX_BOTTOM, nullptr); graphicsReady = true;
#ifdef DUSK_GAME_STARTUP
    startBetaScreen();
#endif
    logText("Native entry: LCD console ready\n");
    bool new3ds = false;
    if (R_FAILED(APT_CheckNew3DS(&new3ds)) || !new3ds) fatal("New Nintendo 3DS is required");
    osSetSpeedupEnable(true); // libctru requests New3DS speed/L2; not a measured clock claim.
    logText("Native entry: Citro3D initialization\n");
#if defined(DUSK_GAME_STARTUP)
    // The game's PICA command arena. A dungeon frame emits more than 256 KiB of
    // commands, and each time the arena runs out platformPrepareGpuDraw drains
    // the whole GPU to reuse it (Alpha 6 console: 0.5 drains per frame, about
    // 1.25 ms of waiting per frame). The earlier 512 KiB request depended on
    // DUSK_PICA_GAME_DRAW, which the hardware build sets OFF, so the console
    // ran Citro3D's 256 KiB default (the log's "65536 words"). 1 MiB of the
    // 20 MiB linear heap; the least free linear memory seen in Alpha 6 was
    // 5.9 MiB. SD switch gpu-arena-256k.txt restores the old size for an A/B.
    unsigned commandBytes=1024*1024;
    if(auto* small=std::fopen("sdmc:/3ds/DuskLight/gpu-arena-256k.txt","rb")){std::fclose(small);commandBytes=C3D_DEFAULT_CMDBUF_SIZE;}
#else
    constexpr unsigned commandBytes=C3D_DEFAULT_CMDBUF_SIZE;
#endif
    if (!C3D_Init(commandBytes)) fatal("Citro3D initialization failed");
    {
        char arena[128];
        std::snprintf(arena,sizeof(arena),"Native GPU command arena: %u KiB (%u words); linear free %lu KiB\n",commandBytes/1024,commandBytes/4,
            static_cast<unsigned long>(linearSpaceFree()/1024));
        logText(arena);
    }
    logText("Native entry: Citro3D ready; waiting for real VI IRQ\n");
    const auto retraceBefore = platformVideoRetraceCount();
    waitVideoRetrace();
    const auto retraceAfter = platformVideoRetraceCount();
    if (retraceAfter == retraceBefore) fatal("Native VI did not observe a real VBlank");
    char retraceLog[96];
    std::snprintf(retraceLog, sizeof(retraceLog), "Native VI real IRQ: %lu -> %lu\n",
        static_cast<unsigned long>(retraceBefore), static_cast<unsigned long>(retraceAfter));
    logText(retraceLog);
    auto* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!target) fatal("Top render target allocation failed");
    if(!initializeFramebuffers(target,400,240))fatal("Native framebuffer registration failed");
    constexpr u32 transfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, transfer);
    if (!initializeGxPica()) fatal(gxPicaFailure()); // Transitional immediate UI/effects only.
#if defined(DUSK_PICA_GAME_DRAW)
    if(!dusk::pica::game::initialize(target))fatal(dusk::pica::game::failure());
    logText("Renderer: native model dispatch selected; prepared asset bindings required; legacy model packet fallback disabled\n");
#endif
    copyprobe::initialize(); // One fixed private SD opt-in; no render-loop I/O.
#ifdef DUSK_GAME_STARTUP
    const int gameResult=runOriginalGame();
    // runOriginalGame has joined audio and every managed game worker. Targets
    // and GPU/service workers must also be stopped before libc unmaps heaps.
    stopNativeSdWriter();
    const auto remaining=threadStats();
    if(remaining.workers||remaining.stackBytes||remaining.waitingQueues||sdWriter||!alphaIntervalCopySafe()){
        containShutdownFailure("retained workers or unsafe framebuffer");
    }
    logText("Native shutdown phase=graphics stop\n");
    releaseCopyImages();
    C3D_Fini();
#if defined(DUSK_PICA_GAME_DRAW)
    if(!dusk::pica::game::releaseAfterContextDestroyed())containShutdownFailure("graphics resource release failed");
#endif
    releaseFramebufferReadback(); releaseFramebufferRecords();
    destroyPreview(); destroyGxPica(); destroyJ2dPictureProbe(); destroyCommandProbe();
    copyprobe::writeReport(true); // After all copy/cache destruction; outside game timing.
    gfxExit(); graphicsReady=false;
    // No heartbeat writer, GPU work or graphics owner can still race the final record.
    gpuCanarySessionEnd();
    logText("Native shutdown complete: workers=0 stackBytes=0 waitingQueues=0 graphics=stopped\n");
    return gameResult;
#endif
    // Retained until process exit: JKR root itself resides inside this allocation.
    void* arena = memalign(32, 8 * 1024 * 1024);
    if (!arena || !initializeKernel(arena, 8 * 1024 * 1024)) fatal("JKernel bootstrap failed");
    kernelReady = true;
    if (!initializeNativePad()) fatal("Native HID/IRRST input initialization failed");
#ifdef DUSK_SCENE_STARTUP
    if (!initializeNativeScene()) fatal(nativeSceneStatus());
#endif
#ifndef DUSK_SCENE_STARTUP
    if (!initializeCommandProbe()) fatal("Command probe initialization failed");
#endif
    aptHookCookie hook{}; aptHook(&hook, onApt, nullptr);
    DiagnosticSnapshot snapshot{};
    snapshot.new3ds = new3ds; snapshot.kernelOkay = true; snapshot.visible = true;
    snapshot.appRegionBytes = osGetMemRegionSize(MEMREGION_APPLICATION);
    snapshot.gameHeap = kernelStats();
    snapshot.discStatus = -1;
    bool workerStarted = startDiagnosticWorker();
    DumpArm arm;
    std::array<double, 120> samples{};
    unsigned sampleCount = 0, sampleCursor = 0, page = 0;
    std::uint64_t last = svcGetSystemTick(), next = last, refresh = 0;
    bool wasBusy = false, dirty = true, captureFrameOpen = false, previewAttempted = false, pictureReady = false;
    std::uint64_t pictureFrame = 0;
    while (aptMainLoop()) {
        hidScanInput(); auto held = hidKeysHeld(), down = hidKeysDown();
        if ((down & KEY_START) && !dumpBusy()) break;
        if ((down & KEY_X) && !dumpBusy()) { commandProbeVisible=!commandProbeVisible; dirty=true; }
        if (down & KEY_TOUCH) {
            touchPosition touch; hidTouchRead(&touch);
            if (touch.px >= 272 && touch.py >= 216) { snapshot.visible = !snapshot.visible; dirty = true; }
        }
        if (held & KEY_SELECT) {
            if (down & KEY_DLEFT) { page = (page + 3) % 4; dirty = true; }
            if (down & KEY_DRIGHT) { page = (page + 1) % 4; dirty = true; }
        }
        const auto now = svcGetSystemTick();
        bool busy = dumpBusy();
        if (busy != wasBusy || timingReset) {
            if (timingReset) arm = DumpArm{}; // A held chord must be released after resume.
            setClockPaused(busy || suspended()); last = next = now;
            sampleCount = sampleCursor = 0; wasBusy = busy; dirty = true; timingReset = false;
        }
        if (captureFrameOpen && !busy) {
            if(platformReserveGpuEntries(4,500000000)!=GpuCompletion::complete)fatal("Native GPU presentation queue unavailable");
            if(!platformEndGpuFrame(0))fatal("Native GPU frame completion unavailable");
            captureFrameOpen = false;
        }
#ifndef DUSK_SCENE_STARTUP
        if (!previewAttempted && startupFinished() && !busy && !captureFrameOpen) {
            StartupJ2dResources resources;
            if (startupJ2dResources(resources))
                pictureReady = initializeJ2dPictureProbe(resources.nintendo, resources.warning, resources.prompt);
            // Original pictures own their cached texture upload. Allocate the
            // isolated 256 KiB preview only when that path cannot initialize.
            if (!pictureReady) {
                PreviewImage image;
                if (startupPreviewImage(image)) snapshot.previewReady = initializePreview(image);
            }
            previewAttempted = true; dirty = true;
        }
#endif
        snapshot.buttons = held;
        snapshot.discStatus = startupDiscStatus(); snapshot.workerOkay = startupWorkerOkay();
        if (!busy && startupFinished()) snapshot.gameHeap = kernelStats();
        snapshot.linearFree = linearSpaceFree();
        int chord = ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) ?
            ((held & KEY_B) ? 2 : (held & KEY_A) ? 1 : 0) : 0;
        bool beforeArm = arm.arming();
        const auto request = arm.update(chord, now / (SYSCLOCK_ARM11 / 1000), busy || !startupFinished());
        if (request && workerStarted) {
            // FrameSync only waits for VBlank. FrameBegin(0) actually waits for
            // the previous command/transfer queue, then opens an empty frame.
            // Keep it empty and open throughout capture: no GPU/top rendering
            // overlaps the dump. The bottom progress UI remains live; the RAM
            // manifest explicitly identifies its snapshot as non-atomic.
            if (!platformBeginGpuFrame(0)) fatal("Cannot drain GPU queue for capture");
            captureFrameOpen = true;
            setClockPaused(true);
            if (!requestDump(request, snapshot)) setClockPaused(suspended());
            // Track the transition immediately: a quick dump can finish before
            // the next frame, which must still resume the paused game clock.
            busy = dumpBusy(); wasBusy = true; dirty = true;
            last = next = now; sampleCount = sampleCursor = 0;
        }
        dirty |= beforeArm != arm.arming();
        if (!busy && !captureFrameOpen) {
            updateNativePad();
            // The current renderer uses the original combined 30 Hz policy.
            // This advances timing only; player/actor simulation is not wired yet.
            static bool gameTimingReady = false;
            if (startupFinished()) {
                if (!gameTimingReady) {
                    dusk::game_clock::initialize();
                    dusk::game_clock::reset();
                    gameTimingReady = true;
                }
                dusk::game_clock::advance();
            }
            snapshot.frameMs = (now - last) * 1000.0 / SYSCLOCK_ARM11; last = now;
            if (snapshot.frameMs > 0 && snapshot.frameMs < 1000) {
                samples[sampleCursor++ % samples.size()] = snapshot.frameMs;
                sampleCount = std::min<unsigned>(sampleCount + 1, samples.size());
            }
            if (!platformBeginGpuFrame(C3D_FRAME_SYNCDRAW)) fatal("Cannot begin render frame");
            if (!beginGxGeometryFrame()) fatal(gxGeometryFailure());
            C3D_RenderTargetClear(target, C3D_CLEAR_ALL,
                pictureReady ? 0x000000ff : snapshot.previewReady ? 0xf0ede6ff : held & KEY_A ? 0x287050ff : 0x182c48ff, 0);
            if(!beginFramebufferFrame())fatal("Native framebuffer frame begin failed");
#ifdef DUSK_COMMAND_PROBE_STARTUP
            // Dedicated regression build alternates both sinks automatically.
            // This catches stale multi-unit/material state across original J2D.
            commandProbeVisible=(snapshot.frame/180)%2==0;
#endif
#ifdef DUSK_SCENE_STARTUP
            circlePosition circle;hidCircleRead(&circle);
            if(!drawNativeScene(circle.dx/156.f,-circle.dy/156.f))fatal(nativeSceneStatus());
#else
            if (commandProbeVisible) {
                if (!drawCommandProbe()) fatal(commandProbeStatistics().failure);
            } else if (pictureReady) drawJ2dPictureProbe(pictureFrame++);
            else { if (snapshot.previewReady) drawPreview(); drawJ2dPrimitiveProbe(); }
#endif
            if (!endGxGeometryFrame()) fatal(gxGeometryFailure());
            if (!snapshot.frame) {
                if (platformCompleteGpu(500000000) != GpuCompletion::complete)
                    fatal("Native GPU completion admission failed");
                logText("Native GPU actual queue complete after first diagnostic frame\n");
            }
            if(!endFramebufferFrame())fatal("Native offscreen surface left active at frame end");
            updateVideoPresentation();
            if(platformReserveGpuEntries(4,500000000)!=GpuCompletion::complete)fatal("Native GPU presentation queue unavailable");
            if(!platformEndGpuFrame(0))fatal("Native GPU frame completion unavailable");
            ++snapshot.frame;
#ifdef DUSK_DIAGNOSTIC_EXIT_FRAMES
            if (snapshot.frame >= DUSK_DIAGNOSTIC_EXIT_FRAMES) {
                const auto command = commandProbeStatistics();
                const auto stats = gx::statistics();
                if (!startupWorkerOkay() || !pictureReady || !pictureFrame ||
                    !command.submitted || command.rejected || stats.drawsDropped)
                    fatal("Bounded diagnostic resource/draw admission failed");
                char admission[160];
                std::snprintf(admission, sizeof(admission),
                    "Native bounded diagnostic: %llu frames, J2D %llu, command %llu, dropped %llu; normal exit\n",
                    static_cast<unsigned long long>(snapshot.frame),
                    static_cast<unsigned long long>(pictureFrame),
                    static_cast<unsigned long long>(command.submitted),
                    static_cast<unsigned long long>(stats.drawsDropped));
                logText(admission);
                break;
            }
#endif
        }
        if (dirty || now >= refresh) {
            if (snapshot.visible || dirty || arm.arming() || busy) {
                if (sampleCount && snapshot.visible) {
                    auto sorted = samples;
                    std::sort(sorted.begin(), sorted.begin() + sampleCount);
                    snapshot.p50 = sorted[(sampleCount * 50 + 99) / 100 - 1];
                    snapshot.p95 = sorted[(sampleCount * 95 + 99) / 100 - 1];
                    snapshot.p99 = sorted[(sampleCount * 99 + 99) / 100 - 1];
                }
                panel(snapshot, page, arm, workerStarted);
            }
            refresh = now + SYSCLOCK_ARM11 / 5; dirty = false;
        }
        next += SYSCLOCK_ARM11 / 30;
        const auto after = svcGetSystemTick();
        if (next > after) svcSleepThread((next - after) * 1000000000ULL / SYSCLOCK_ARM11);
        else next = after;
    }
    if (captureFrameOpen) {
        if(platformReserveGpuEntries(4,500000000)!=GpuCompletion::complete)fatal("Native GPU capture queue unavailable");
        if(!platformEndGpuFrame(0))fatal("Native GPU frame completion unavailable");
    }
    aptUnhook(&hook);
    // C3D_Fini drains GPU work before it destroys its render targets. Deleting
    // target after FrameSync alone could free VRAM while a transfer was active.
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)fatal("Native final GPU drain failed");
    releaseCopyImages();
    C3D_Fini(); releaseFramebufferReadback(); releaseFramebufferRecords();
    destroyPreview(); destroyGxPica(); destroyJ2dPictureProbe(); destroyCommandProbe();
    copyprobe::writeReport(true);
    if (workerStarted) stopDiagnosticWorker();
    gfxExit();
    return 0;
}
