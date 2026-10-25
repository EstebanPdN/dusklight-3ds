#include "runtime/copy_lifecycle_probe.hpp"
#include "gx_geometry_pica.hpp"
#include "gx_command_probe.hpp"
#include "scene_bootstrap.hpp"
#include <3ds.h>
#include <citro3d.h>
#include "diagnostics.hpp"
#include "runtime/bootstrap.hpp"
#include "runtime/cache_maintenance_profile.hpp"
#include "runtime/video_retrace.hpp"
#include "runtime/gpu_sync.hpp"
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
#include <cstdio>
#include <cstdlib>
#include <malloc.h>

// Small diagnostic admission, NOT the eventual game's memory budget.
// Both are counted within application memory; no 178 MiB mode is requested.
extern "C" {
#ifdef DUSK_GAME_STARTUP
u32 __stacksize__ = 256 * 1024;
u32 __ctru_heap_size = 80 * 1024 * 1024;
u32 __ctru_linear_heap_size = 16 * 1024 * 1024;
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
// sdmc:/3ds/DuskLight/log.txt only by the main thread (flushNativeLog, every
// 30 frames) or on fatal/exception. Per-line SD writes from many threads made
// the system FS module fault on hardware.
// The ring lives in the linear heap: the executable's static size must stay at
// the 114 baseline (code+bss+96 MiB heaps fill the New 3DS application region).
namespace { constexpr std::size_t logRingBytes=128*1024; char* logRing{}; std::size_t logHead{},logFlushed{}; LightLock logLock=1; FILE* sdLog{}; bool sdLogTried{}; }
void logText(const char* text) {
    const auto length=__builtin_strlen(text);
    svcOutputDebugString(text, length);
    LightLock_Lock(&logLock);
    if(!logRing)logRing=static_cast<char*>(linearAlloc(logRingBytes));
    if(!logRing){LightLock_Unlock(&logLock);return;}
    for(std::size_t n=0;n<length;++n)logRing[(logHead+n)%logRingBytes]=text[n];
    logHead+=length;
    LightLock_Unlock(&logLock);
}
void flushNativeLog(){
    LightLock_Lock(&logLock);
    if(!sdLogTried){sdLogTried=true;sdLog=std::fopen("sdmc:/3ds/DuskLight/log.txt","wb");}
    if(logHead-logFlushed>logRingBytes)logFlushed=logHead-logRingBytes;
    char chunk[1024];
    while(sdLog&&logRing&&logFlushed<logHead){
        std::size_t n=0;
        while(n<sizeof(chunk)&&logFlushed+n<logHead){chunk[n]=logRing[(logFlushed+n)%logRingBytes];++n;}
        std::fwrite(chunk,1,n,sdLog);logFlushed+=n;
    }
    if(sdLog)std::fflush(sdLog);
    LightLock_Unlock(&logLock);
}
// ARM11 exception report (data/prefetch abort, undefined instruction).
void logException(ERRF_ExceptionInfo* info,CpuRegisters* regs){
    char text[256];
    std::snprintf(text,sizeof(text),"Native EXCEPTION type=%u far=%08lx fsr=%08lx pc=%08lx lr=%08lx sp=%08lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx thread=%p\n",
        unsigned(info->type),static_cast<unsigned long>(info->far),static_cast<unsigned long>(info->fsr),
        static_cast<unsigned long>(regs->pc),static_cast<unsigned long>(regs->lr),static_cast<unsigned long>(regs->sp),
        static_cast<unsigned long>(regs->r[0]),static_cast<unsigned long>(regs->r[1]),static_cast<unsigned long>(regs->r[2]),static_cast<unsigned long>(regs->r[3]),
        static_cast<void*>(threadGetCurrent()));
    svcOutputDebugString(text,__builtin_strlen(text));
    // Best effort, no lock (the faulting thread may hold it): append to ring, write once.
    if(logRing)for(std::size_t n=0;text[n];++n)logRing[(logHead+n)%logRingBytes]=text[n];
    logHead+=__builtin_strlen(text);
    if(!sdLogTried){sdLogTried=true;sdLog=std::fopen("sdmc:/3ds/DuskLight/log.txt","wb");}
    if(sdLog&&logRing){
        if(logHead-logFlushed>logRingBytes)logFlushed=logHead-logRingBytes;
        while(logFlushed<logHead){std::fputc(logRing[logFlushed%logRingBytes],sdLog);++logFlushed;}
        std::fflush(sdLog);
    }
    svcExitProcess();
}
void installExceptionLogger(){threadOnException(logException,RUN_HANDLER_ON_FAULTING_STACK,WRITE_DATA_TO_HANDLER_STACK);}
[[noreturn]] void fatal(const char* text) {
    logText("Native fatal: ");logText(text);logText("\n");flushNativeLog();
    // A worker thread cannot own the console (the main loop redraws it) and
    // would hang silently: keep the log and terminate the whole process.
    if (threadGetCurrent() != nullptr) { logText("Native fatal on a worker thread: process exit\n"); flushNativeLog(); svcExitProcess(); }
    if (graphicsReady && !platformGpuFailurePending()) {
        std::printf("\x1b[2JNative fatal error\n%.200s\nSTART: exit\n", text);
        while (aptMainLoop()) {
            hidScanInput(); if (hidKeysDown() & KEY_START) break;
            gfxFlushBuffers(); gspWaitForVBlank();
        }
    }
    // A fatal path can still have live game workers, stacks and GPU consumers.
    // std::exit would run libctru heap teardown before killing those threads.
    // Let the kernel terminate the complete process with its storage intact.
    logText("Native fatal containment: kernel process exit; normal shutdown incomplete\n");
    svcExitProcess();
}
void waitForRetrace() { waitVideoRetrace(); }
void invalidateCache(void* memory, std::uint32_t size) {
    auto& profile=cacheMaintenanceProfile;
    const auto start=profile.active?svcGetSystemTick():0;
    // Game DCInvalidateRange on ordinary process memory. GSP rejects such ranges
    // on hardware (Azahar accepts them); the kernel call covers any mapped
    // process memory. A failure is reported once, never fatal: these ranges are
    // CPU data, and every GPU-consumed buffer is maintained by the renderer.
    if (R_FAILED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(memory), size))) {
        static bool reported; if (!reported) { reported = true; logText("Native cache invalidation failed once (ignored)\n"); }
    }
    if(profile.active){profile.invalidateTicks+=svcGetSystemTick()-start;profile.invalidateBytes+=size;++profile.invalidateCalls;}
}
void flushCache(void* memory, std::uint32_t size) {
    auto& profile=cacheMaintenanceProfile;
    const auto start=profile.active?svcGetSystemTick():0;
    if (R_FAILED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(memory), size))) {
        static bool reported; if (!reported) { reported = true; logText("Native cache flush failed once (ignored)\n"); }
    }
    if(profile.active){profile.flushTicks+=svcGetSystemTick()-start;profile.flushBytes+=size;++profile.flushCalls;}
}
}
int main() {
    using namespace dusk::native;
    installExceptionLogger();
    logText("Native entry: main\n");
    gfxInitDefault(); gfxSet3D(false); consoleInit(GFX_BOTTOM, nullptr); graphicsReady = true;
    logText("Native entry: LCD console ready\n");
    bool new3ds = false;
    if (R_FAILED(APT_CheckNew3DS(&new3ds)) || !new3ds) fatal("New Nintendo 3DS is required");
    osSetSpeedupEnable(true); // libctru requests New3DS speed/L2; not a measured clock claim.
    logText("Native entry: Citro3D initialization\n");
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) fatal("Citro3D initialization failed");
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
    if (!initializeGxPica()) fatal(gxPicaFailure());
    copyprobe::initialize(); // One fixed private SD opt-in; no render-loop I/O.
#ifdef DUSK_GAME_STARTUP
    const int gameResult=runOriginalGame();
    // runOriginalGame has joined audio and every managed game worker. Targets
    // and GPU/service workers must also be stopped before libc unmaps heaps.
    const auto remaining=threadStats();
    if(remaining.workers||remaining.stackBytes||remaining.waitingQueues||!alphaIntervalCopySafe()){
        logText("Native shutdown failure: retained workers or unsafe framebuffer; kernel process exit\n");
        svcExitProcess();
    }
    logText("Native shutdown phase=graphics stop\n");
    releaseCopyImages();
    C3D_Fini(); releaseFramebufferReadback(); releaseFramebufferRecords();
    destroyPreview(); destroyGxPica(); destroyJ2dPictureProbe(); destroyCommandProbe();
    copyprobe::writeReport(true); // After all copy/cache destruction; outside game timing.
    gfxExit(); graphicsReady=false;
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
