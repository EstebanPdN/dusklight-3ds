#include <cstdlib>
#include "copy_lifecycle_probe.hpp"
#include "cache_maintenance_profile.hpp"
#include "game_startup.hpp"
#if defined(DUSK_PICA_GAME_DRAW)
#include "../native/game_draw_api.hpp"
#endif
#include "application_dump_ctru.hpp"
#include "measured_dump.hpp"
#include "benchmark_capture.hpp"
#include "benchmark_input.hpp"
#include "story_chapter_checkpoint.hpp"
#include "story_chapter_game.hpp"
#include "story_portal_game.hpp"
#include "c/c_damagereaction.h"
#include "d/d_msg_object.h"
#include "bootstrap.hpp"
#include "boot_options.hpp"
#include "clock.hpp"
#include "os_arena.hpp"
#include "os_system.hpp"
#include "os_thread.hpp"
#include "thread_backend.hpp"
#include "pad_bootstrap.hpp"
#include "dvd_provider.hpp"
#include "gx_framebuffer.hpp"
#include "alpha_interval_target.hpp"
#include "signed_q_diagnostic.hpp"
#include "../gx_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "gpu_sync.hpp"
#include "gpu_list_capture.hpp"
#include "gpu_canary.hpp"
#include "release.hpp"
#include "gx_stream.hpp"
#include "gx_material.hpp"
#include "gx_transform_commands.hpp"
#include "gx_vertex_transform.hpp"
#include "gx_profile.hpp"
#include "fused_color_config.hpp"
#include "gx_compact_cache.hpp"
#include "gx_rigid_vertex.hpp"
#include "../gx_command_draw.hpp"
#include "gx_worker_runtime.hpp"
#include "gx_worker_sync.hpp"
#include "helpers/native_effects.h"
#include "shadow_casters.hpp"
#include "screen_effects.hpp"
#include "gx_i4_bake_cache68.hpp"
#include "../gx_geometry_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "video_mode.hpp"
#include "texture_resources.hpp"
#include "texture_half_policy.hpp"
#include "audio_control.hpp"
#include "hud_tape.hpp"
#include "screen_tape.hpp"
#include "gx_indirect_gpu.hpp"
#include "developer_script.hpp"
#include "frame_telemetry.hpp"
#include "tour_cadence.hpp"
#include "scene_transition.hpp"
#include "dev_console.hpp"
#include "helpers/native_frame_wait.h"
#include "font_pages.hpp"
#include "gx_copy_images.hpp"
#include "dusk/game_clock.h"
#include "dusk/version.hpp"
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_midna.h"
#include "SSystem/SComponent/c_math.h"
#include "m_Do/m_Do_Reset.h"
#include "dusk/audio/DuskAudioSystem.h"
#include "helpers/gx_helper.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/JUtility/JUTNameTab.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "c/c_dylink.h"
#include "m_Do/m_Do_machine.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_dvd_thread.h"
#include "m_Do/m_Do_ext.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_create_iter.h"
#include "f_pc/f_pc_create_req.h"
#include "f_op/f_op_overlap_mng.h"
#include "d/d_s_room.h"
#include "d/d_bg_s_gnd_chk.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_camera_mng.h"
#include "d/d_demo.h"
#include "f_op/f_op_actor_mng.h"
#include "d/d_stage.h"
#include "d/d_item.h"
#include "d/d_item_data.h"
#include "d/d_save.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_main.h"
#include "f_ap/f_ap_game.h"
#include "f_op/f_op_actor_mng.h"
#include "DynamicLink.h"
#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include "model_lod.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <bit>
extern s32 LOAD_COPYDATE(void*);
extern "C" { extern u8 __start__[],__end__[],__exidx_end[]; }
namespace dusk { bool IsShuttingDown=false; }
extern std::uint32_t duskNativeDynamicReasons[4];
// m_Do_dvd_thread.cpp reads it when the DVD thread is created.
extern "C" { bool duskNativeLoaderOnSecondCore=true; }
namespace dusk::native { void optionalCopyStatistics(std::uint64_t&,std::uint64_t&,std::uint64_t&); }
extern "C" std::uint32_t duskNativeHeapEpoch;
// Defined below; written by instrumented original scene-change requests.
extern "C" int duskNativeSceneExit;
extern "C" const void* duskNativeSceneCaller;
namespace dusk::native {
namespace {
bool liveGameArray(std::uint64_t address,u32 size){
    GxProfileScope profile(GxProfilePart::arrayValidate,gxSparseVertexProfiling);
    if(!address||!size||address>UINTPTR_MAX||size>UINTPTR_MAX-address)return false;
    // The same model arrays are bound for every shape. A proof stays valid
    // until some heap frees, shrinks or destroys an allocation.
    // Per thread: the second-core worker resolves the arrays of the lists
    // it runs while the game thread binds the next shape's.
    struct Proof {std::uint32_t address{},size{},epoch{};};
    static thread_local std::array<Proof,64> proofs{};
    auto& proof=proofs[(std::uint32_t(address)>>4)*2654435761u>>26];
    // Read once, before the heap walk: a free on another thread during the
    // walk then leaves this proof already stale.
    const auto epoch=__atomic_load_n(&duskNativeHeapEpoch,__ATOMIC_RELAXED);
    if(proof.epoch==epoch&&proof.address==std::uint32_t(address)&&proof.size==size)return true;
    const auto remember=[&](bool live){if(live)proof={std::uint32_t(address),size,epoch};return live;};
    auto* pointer=reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    auto* heap=JKRHeap::findFromRoot(pointer);
    s32 remaining=-1;
    if(heap&&heap->getHeapType()=='EXPH')remaining=static_cast<JKRExpHeap*>(heap)->getRemainingAllocationSize(pointer);
    else if(heap&&heap->getHeapType()=='SLID')remaining=static_cast<JKRSolidHeap*>(heap)->getRemainingUsedSize(pointer);
    return remember(remaining>=0&&size<=static_cast<u32>(remaining));
}
// Second-core worker (runtime/gx_worker.hpp): the mode of this start.
// - SD switch gx-worker.txt: `0` off, `w` the worker on core 2, `d` the same
//   records drawn by the game thread itself (no second thread; proves the
//   records), a `v` behind `w` or `d` adds the verify checks.
// - No switch: the worker, unless this build learned to leave it off. The
//   start before this one had it on and ended on a fatal error, a CPU
//   exception or a GPU stop (the GPU guard's session record tells). A confirmed
//   font-pane OOM with an idle journal leaves the worker eligible. Otherwise,
//   the worker may be involved, so it is left off from then on and
//   gx-worker-learned.txt says why. Deleting that file, or `w` in the
//   switch, turns it on again. A console that is switched off while playing
//   teaches nothing.
struct WorkerChoice {gxworker::Mode mode{};bool verify{};};
WorkerChoice chooseGxWorker(){
    constexpr char switchPath[]="sdmc:/3ds/DuskLight/gx-worker.txt",learnedPath[]="sdmc:/3ds/DuskLight/gx-worker-learned.txt";
    char text[8]{},learned[200]{};
    if(auto* file=std::fopen(switchPath,"rb")){(void)std::fread(text,1,sizeof(text)-1,file);std::fclose(file);}
    if(auto* file=std::fopen(learnedPath,"rb")){(void)std::fread(learned,1,sizeof(learned)-1,file);std::fclose(file);}
    // Learned by this build only.
    char label[40]{};
    bool learnedOff=std::sscanf(learned,"off build=%39[^;\n]",label)==1&&!std::strcmp(label,DUSKLIGHT_RELEASE);
    if(learned[0]&&!learnedOff){std::remove(learnedPath);learned[0]=0;}
    if(!learnedOff&&gpuCanaryPreviousSessionWord("core2")){
        const auto end=gpuCanaryPreviousEnd();
        const char* how=end==GpuPreviousEnd::fatal?"a fatal error or CPU exception":end==GpuPreviousEnd::gpuStop?"a GPU stop":nullptr;
        if(how){
            std::snprintf(learned,sizeof(learned),"off build=%s; the start before had the second-core worker on and ended on %s (see gpu-hangs.txt and log-1.txt)\n",
                DUSKLIGHT_RELEASE,how);
            if(auto* file=std::fopen(learnedPath,"wb")){std::fputs(learned,file);std::fclose(file);}
            learnedOff=true;
        }
    }
    WorkerChoice choice;
    const bool forced=text[0]=='w'||text[0]=='d';
    if(text[0]=='0')OSReport("Native GX worker: off (SD switch gx-worker.txt)\n");
    else if(learnedOff&&!forced){
        if(char* end=std::strchr(learned,'\n'))*end=0;
        OSReport("Native GX worker: off, learned: %.160s\n",learned);
        OSReport("Native GX worker: delete gx-worker-learned.txt or put w in gx-worker.txt to turn it on again\n");
    }else{
        choice.mode=text[0]=='d'?gxworker::Mode::deferred:gxworker::Mode::threaded;
        choice.verify=forced&&text[1]=='v';
    }
    return choice;
}
// Alpha 15, SD switch gx-overlap.txt (the second-core worker's share of a
// picture): `0` Alpha 14 as it was (294 KiB of records, a full ring hands
// the whole queue to the game thread, the painter's 2D passes and its end
// sync); `1` within the painter only: 1 MiB of records, a full ring draws on
// the game thread only until the record fits, a texture already held needs no
// sync, retained screens met while runs are queued are drawn without one; `2`
// (default) adds the overlap: the 2D passes go to the queue too (HUD tape as
// jobs) and the painter ends without a sync, the worker draws on during the
// next logic tick; game code that frees, creates or rewrites what a queued
// shape reads syncs first (gxOverlapSyncHook), the picture's close syncs.
unsigned gxOverlapLevel=2;
unsigned readGxOverlapLevel(){
    char text[4]{};
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gx-overlap.txt","rb")){(void)std::fread(text,1,sizeof(text)-1,file);std::fclose(file);}
    return text[0]=='0'?0u:text[0]=='1'?1u:2u;
}
BootStage bootStage;bool hasBootStage{},bootStageFromTest{};
bool readRasterCarrierDiagnostic(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/raster-carrier.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native raster carrier config unreadable; diagnostic disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    const std::string_view value{text,bytes};
    if(valid&&(value=="enabled=0\n"||value=="enabled=0\r\n"))return false;
    if(valid&&(value=="enabled=1\n"||value=="enabled=1\r\n"))return true;
    OSReport("Native raster carrier config invalid/unreadable; diagnostic disabled\n");
    return false;
}
bool readSignedQDiagnostic(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/signed-q.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native signed-q config unreadable; diagnostic disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native signed-q config invalid/unreadable; diagnostic disabled\n");
        return false;
    }
    return enabled;
}
bool readAutomaticSuConfig77(){
    errno=0;auto* file=std::fopen("sdmc:/3ds/dusklight/gx-auto-su77.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native automatic SU77 config unreadable; experiment disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native automatic SU77 config invalid/unreadable; experiment disabled\n");return false;
    }
    return enabled;
}
bool readPositiveQGpuConfig(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/positive-q-gpu.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native positive-q GPU config unreadable; experiment disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native positive-q GPU config invalid/unreadable; experiment disabled\n");
        return false;
    }
    return enabled;
}
void reportPositiveQGpu(const CommandDrawContext& context){
    if(!context.allowSignedQPositiveRigidGpu)return;
    const auto& counts=context.signedQPositiveRigid;
    auto row=[](const char* phase,const SignedQPositiveRigidCounts::Total& total){
        OSReport("Native positive-q GPU phase=%s draws=%llu vertices=%llu\n",phase,
            static_cast<unsigned long long>(total.draws),static_cast<unsigned long long>(total.vertices));
    };
    row("attempt",counts.attempts);row("plan-fail",counts.planningFailures);
    row("pack-fail",counts.packingFailures);row("proof-pass",counts.proofPasses);
    row("proof-fail",counts.proofFailures);row("submitted",counts.submitted);
}
// Spliced into the existing anonymous startup namespace; no new translation unit.
bool readCameraAlignedProofConfig(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/camera-aligned-indirect.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native camera-aligned proof config unreadable; observation disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native camera-aligned proof config invalid/unreadable; observation disabled\n");
        return false;
    }
    return enabled;
}
bool readExactI4BakeConfig(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/i4-quarter-bake.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native exact I4 bake config unreadable; experiment disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native exact I4 bake config invalid/unreadable; experiment disabled\n");return false;
    }
    return enabled;
}
bool readCameraCpuClipConfig(){
    // On unless the SD file says enabled=0: water that refracts the frame copy
    // crosses the camera plane whenever the player stands in it, and without
    // the clip those patches of the surface are left out.
    auto* file=std::fopen("sdmc:/3ds/dusklight/camera-cpu-clip.txt","rb");
    if(!file){
        if(errno!=ENOENT){OSReport("Native camera CPU clipping config unreadable; disabled\n");return false;}
        return true;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native camera CPU clipping config invalid/unreadable; experiment disabled\n");
        return false;
    }
    return enabled;
}
void reportCameraAlignedProof(const CommandDrawContext& context){
    if(context.allowExactI4Bake)OSReport("I4_BAKE submitted=%llu vertices=%llu cpuImages=%u cacheStorage=%u\n",
        static_cast<unsigned long long>(context.exactI4Draws),static_cast<unsigned long long>(context.exactI4Vertices),
        context.exactI4CpuImages,unsigned(sizeof(gx::bake68::Cache)));
    if(context.depthlessOrthoDraws)OSReport("DEPTHLESS_ORTHO submitted=%llu vertices=%llu depth=disabled transport=interior-z\n",
        static_cast<unsigned long long>(context.depthlessOrthoDraws),static_cast<unsigned long long>(context.depthlessOrthoVertices));
    if(!context.allowCameraAlignedIndirectQ&&!context.allowCameraAlignedCpuClip&&!context.positiveQDepthFallbackDraws)return;
    if(context.positiveQDepthFallbackDraws)OSReport("POSQ_DEPTH submitted=%llu vertices=%llu policy=positive-only approximation=world-depth-clip\n",
        static_cast<unsigned long long>(context.positiveQDepthFallbackDraws),static_cast<unsigned long long>(context.positiveQDepthFallbackVertices));
    const auto& counts=context.cameraAlignedIndirect;
    auto row=[](const char* phase,const CameraAlignedIndirectCounts::Total& total){
        OSReport("CAMQ_PROOF phase=%s draws=%llu vertices=%llu\n",phase,
            static_cast<unsigned long long>(total.draws),static_cast<unsigned long long>(total.vertices));
    };
    row("attempt",counts.attempts);row("proof-pass",counts.proofPasses);
    row("proof-fail",counts.proofFailures);row("submitted",counts.submitted);
    if(context.allowCameraAlignedCpuClip)OSReport("CAMERA_CLIP attempts=%llu submitted=%llu culled=%llu kernelRejected=%llu preparedOriginal=%llu preparedOutput=%llu preparedIndices=%llu error=%lu vertex=%lu triangle=%lu\n",
        static_cast<unsigned long long>(context.cameraClipAttempts),static_cast<unsigned long long>(context.cameraClipDraws),
        static_cast<unsigned long long>(context.cameraClipCulled),static_cast<unsigned long long>(context.cameraClipKernelRejected),
        static_cast<unsigned long long>(context.cameraClipOriginalVertices),static_cast<unsigned long long>(context.cameraClipOutputVertices),
        static_cast<unsigned long long>(context.cameraClipOutputIndices),static_cast<unsigned long>(context.cameraClipFirstError),
        static_cast<unsigned long>(context.cameraClipFirstVertex),static_cast<unsigned long>(context.cameraClipFirstTriangle));
    const auto& f=context.cameraAlignedIndirectFirstFailure;
    OSReport("CAMQ_PROOF failure reason=%u index=%lu unit=%lu valid=%lx q=%08lx w=%08lx sourceq=%08lx transform=%lu\n",
        unsigned(f.reason),static_cast<unsigned long>(f.index),static_cast<unsigned long>(f.unit),
        static_cast<unsigned long>(f.validBits),static_cast<unsigned long>(f.baseQBits),
        static_cast<unsigned long>(f.clipWBits),static_cast<unsigned long>(f.sourceQBits),
        static_cast<unsigned long>(f.transformError));
}

bool readAlphaIntervalConfig(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/alpha-interval.txt","rb");
    if(!file){if(errno!=ENOENT)fatal("Cannot read alpha-interval.txt");return false;}
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseAlphaIntervalConfig({text,bytes},enabled))fatal("Invalid alpha-interval.txt; expected enabled=0 or enabled=1 followed by newline");
    return enabled;
}

std::size_t readTextureHalfPolicy(std::array<NativeTextureFingerprint,64>& policy){
    auto* file=std::fopen("sdmc:/3ds/dusklight/texture-half-sha256.txt","rb");
    if(!file){if(errno!=ENOENT)fatal("Cannot read texture-half-sha256.txt");return 0;}
    char text[8193];const auto bytes=std::fread(text,1,sizeof(text),file);
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    std::size_t count=0;
    if(!valid||!parseTextureHalfPolicy({text,bytes},policy,count))fatal("Invalid texture-half-sha256.txt; expected up to 64 unique SHA256 hex tokens");
    return count;
}
void readBootStage(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/boot-stage.txt","rb");
    if(!file){
        // An ordinary boot retains the original opening scene. A resumed or
        // unattended test starts directly in its next zone.
        if(devConsoleBootStage(bootStage)){
            hasBootStage=bootStageFromTest=true;
            OSReport("Native test stage requested: %s room=%d point=%d\n",bootStage.name,bootStage.room,bootStage.point);
        }
        return;
    }
    char text[96];const auto count=std::fread(text,1,sizeof(text),file);
    const bool valid=count<sizeof(text)&&!std::ferror(file)&&parseBootStage({text,count},bootStage);
    std::fclose(file);
    if(!valid)fatal("Invalid boot-stage.txt; expected STAGE ROOM POINT LAYER");
    hasBootStage=true;
    OSReport("Native developer stage requested: %s room=%d point=%d layer=%d\n",bootStage.name,bootStage.room,bootStage.point,bootStage.layer);
}
// ---- CPU mode proof ----
// osSetSpeedupEnable asks for the New 3DS 804 MHz clock and L2 cache and
// reports nothing; Alpha 6 logged no proof that either was granted (a silent
// refusal costs about 3x CPU). The GameCube rounds to nearest; the FPSCR a
// thread starts with is logged, not assumed. ARM encoding: Thumb-1 (ARMv6K)
// has no VFP instructions.
__attribute__((target("arm"))) std::uint32_t readFpscr(){std::uint32_t value;__asm__ volatile("vmrs %0, fpscr":"=r"(value));return value;}
// 64 independent adds plus the loop's subs and bne: about 66 cycles a round
// on the in-order, single-issue ARM11 MPCore.
constexpr unsigned aluRoundCycles=66;
__attribute__((target("arm"),noinline)) void aluRounds(std::uint32_t rounds){
    std::uint32_t a=1,b=2,c=3,d=4;
    __asm__ volatile("1:\n\t.rept 16\n\tadd %0,%0,#1\n\tadd %1,%1,#1\n\tadd %2,%2,#1\n\tadd %3,%3,#1\n\t.endr\n\t"
        "subs %4,%4,#1\n\tbne 1b":"+r"(a),"+r"(b),"+r"(c),"+r"(d),"+r"(rounds)::"cc");
}
// Core clock in MHz from the fastest of three timed runs (the system tick
// counts 268,111,856 per second in both clock modes).
double measureCoreMhz(){
    constexpr std::uint32_t rounds=12000;
    std::uint64_t best=~0ull;
    for(int run=0;run<3;++run){
        const auto start=svcGetSystemTick();aluRounds(rounds);
        best=std::min<std::uint64_t>(best,svcGetSystemTick()-start);
    }
    return best?double(rounds)*aluRoundCycles*SYSCLOCK_ARM11/double(best)/1e6:0.0;
}
volatile std::uint32_t chaseSink;
// Nanoseconds per dependent load over one random cycle through count 32-byte
// lines (Sattolo's shuffle: every line visited, no prefetchable stride).
// The whole cycle is walked once before timing: the shuffle leaves about half
// of a 4 MiB set in a 2 MiB L2, and a full walk turns that into the steady
// state, where each line was last read a whole cycle (4 MiB) earlier.
double chaseNanoseconds(std::uint32_t* lines,std::uint32_t count,std::uint32_t steps){
    for(std::uint32_t i=0;i<count;++i)lines[i*8]=i;
    std::uint32_t state=0x2545F491u;
    for(std::uint32_t i=count-1;i>0;--i){
        state=state*1664525u+1013904223u;
        const auto j=std::uint32_t((std::uint64_t(state)*i)>>32);
        std::swap(lines[i*8],lines[j*8]);
    }
    std::uint32_t at=0;
    for(std::uint32_t i=0;i<count;++i)at=lines[at*8];
    const auto start=svcGetSystemTick();
    for(std::uint32_t i=0;i<steps;++i)at=lines[at*8];
    const auto ticks=svcGetSystemTick()-start;
    chaseSink=at;
    return double(ticks)*1e9/SYSCLOCK_ARM11/steps;
}
void describeFpscr(char* out,std::size_t capacity,const char* name,bool valid,std::uint32_t value){
    static const char* const rounding[4]{"nearest","toward +inf","toward -inf","toward zero"};
    if(!valid){std::snprintf(out,capacity,"%s=unavailable",name);return;}
    std::snprintf(out,capacity,"%s=%08lx (flush-to-zero %d, default NaN %d, rounding %s)",name,static_cast<unsigned long>(value),
        int((value>>24)&1),int((value>>25)&1),rounding[(value>>22)&3]);
}
// Boot: clock, L2 evidence and the FPSCR of the main thread and of a thread
// created on core 2 like the audio and worker threads. After a resume
// (full=false): the clock and the main FPSCR only, a few milliseconds.
void reportCpuMode(bool full){
    const auto begin=svcGetSystemTick();
    char mainText[96],threadText[112];
    describeFpscr(mainText,sizeof(mainText),"main",true,readFpscr());
    std::snprintf(threadText,sizeof(threadText),"core-2 thread=not measured");
    if(full){
        // Static: a probe that misses its 100 ms may still write it later.
        struct Probe {std::uint32_t fpscr{};bool done{};};static Probe probe;probe={};
        s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
        if(const auto thread=platformCreateThread([](void* argument){auto* p=static_cast<Probe*>(argument);p->fpscr=readFpscr();p->done=true;},
                &probe,4096,priority,2)){
            // The kernel handle, result 0 only: libctru's threadJoin returns
            // once its finished flag is set, before the thread leaves its
            // stack, and a timeout (0x09401BFE) passes R_SUCCEEDED.
            if(svcWaitSynchronization(thread.kernel,100000000ll)==0){
                platformFreeThread(thread);describeFpscr(threadText,sizeof(threadText),"core-2 thread",probe.done,probe.fpscr);
            }else{
                // Neither freed nor detached (detaching races its exit): a
                // late exit still finds its stack; 4 KiB are left once, only here.
                std::snprintf(threadText,sizeof(threadText),"core-2 thread=did not finish in 100 ms (left running)");
            }
        }else std::snprintf(threadText,sizeof(threadText),"core-2 thread=not created (no core 2)");
    }
    const double mhz=measureCoreMhz();
    bool new3ds=false;APT_CheckNew3DS(&new3ds);
    if(full){
        // L1 data cache 16 KiB, New 3DS L2 2 MiB: a 512 KiB set fits only the
        // L2, a 4 MiB set fits neither. Azahar models no cache: all equal.
        // Evidence, not a latency table: with a random-replacement L2 a
        // fifth of the 4 MiB loads may still hit, and both large sets pay
        // TLB walks, so the rule is "clearly below 1", not a fixed figure.
        constexpr std::uint32_t bigLines=(4u<<20)/32;
        double cachedNs=0,l2Ns=0,memoryNs=0;bool measured=false;
        if(auto* buffer=static_cast<std::uint32_t*>(std::malloc(std::size_t(bigLines)*32))){
            cachedNs=chaseNanoseconds(buffer,(16u<<10)/32,16384);
            l2Ns=chaseNanoseconds(buffer,(512u<<10)/32,16384);
            memoryNs=chaseNanoseconds(buffer,bigLines,16384);
            std::free(buffer);measured=true;
        }
        OSReport("Native CPU mode: core clock about %.0f MHz (timed %u-cycle loop; 804 with the New 3DS speedup, 268 without); new3ds=%d\n",
            mhz,aluRoundCycles,int(new3ds));
        if(measured)OSReport("Native CPU mode: L2 evidence, ns per dependent load: 16 KiB %.1f, 512 KiB %.1f, 4 MiB %.1f; 16K/512K %.2f, 512K/4M %.2f "
            "(512K/4M clearly below 1: L2 active; near 1: no L2, or no cache model as in Azahar)\n",cachedNs,l2Ns,memoryNs,
            l2Ns>0?cachedNs/l2Ns:0.0,memoryNs>0?l2Ns/memoryNs:0.0);
        else OSReport("Native CPU mode: L2 evidence not measured (no 4 MiB block free)\n");
    }else OSReport("Native CPU mode after resume: core clock about %.0f MHz; new3ds=%d\n",mhz,int(new3ds));
    OSReport("Native FPSCR: %s; %s; probe took %.1f ms\n",mainText,threadText,double(svcGetSystemTick()-begin)*1000.0/SYSCLOCK_ARM11);
}
bool gameSuspended{},gameSleeping{},resetTiming{},cpuModeCheck{};
void onGameApt(APT_HookType type,void*){
    if(type==APTHOOK_ONSUSPEND)gameSuspended=true;
    if(type==APTHOOK_ONSLEEP)gameSleeping=true;
    if(type==APTHOOK_ONRESTORE)gameSuspended=false;
    if(type==APTHOOK_ONWAKEUP)gameSleeping=false;
    // The speedup is requested again; the next frame measures the clock.
    if(type==APTHOOK_ONRESTORE||type==APTHOOK_ONWAKEUP){resetTiming=true;cpuModeCheck=true;osSetSpeedupEnable(true);}
    const bool paused=gameSuspended||gameSleeping||applicationDumpActive();
    setClockPaused(paused);dusk::audio::SetPaused(paused||transitionWarming());
}
}
const BootStage* requestedBootStage(){return hasBootStage?&bootStage:nullptr;}
// Defined with the real-speed scheduling below; written to the zone reports.
extern "C" { extern unsigned duskNativeLogicTick,duskNativeStaleCollidersRemoved,duskNativeSceneLogicRuns; }
namespace {
// A cutscene camera drives the view. The demo object exists only inside a
// play scene: on the title screens dDemo_c::getCamera() would read through a
// null object (its debug-only assertion is compiled out). The scene-change
// log of 2026-11-16 called it there, a fault on the console at the first
// scene change of every start; the emulator title gate reported the read.
bool cutsceneCameraActive(){return dDemo_c::m_object&&dDemo_c::getCamera();}
// Actor-driven demos can suspend the player camera without a JStudio camera.
bool playerCameraSuspended(camera_process_class* camera) {
    return camera && !camera->mCamera.Active();
}
// Test session state: the save in memory was replaced by an empty one with the
// test equipment, so travelling may change it freely. Never set for a save the
// player loaded, unless the automatic test starts (it must not be saved after).
bool developerSession;
// A travel's new save state waits for the next PLAY_SCENE (applyTravelLoadout,
// d_s_play phase_00): until the fade ends, the previous scene's Link still
// draws with what he loaded for the clothes, form and items he was created
// with. The random test once put the Magic Armor on in the save under a Link
// created in the Zora Armor; his draw used armor animations he never loaded.
struct PendingTravel {bool active{},clearSave{};int form{};std::uint32_t seed{};story::Checkpoint checkpoint{};} pendingTravel;
// Scene change log: every request for a new stage is described once, so a
// test zone that ends "moved" explains itself without the disc (Alpha 6:
// eight moved zones, explained only by reading the start data and dumps).
// travelRequested: the pending request is a test, console or boot-stage
// travel. gameSceneChangeCount counts only the game's own requests.
bool nextStagePending,travelRequested;
std::uint32_t sceneChangeCount,gameSceneChangeCount;
// At most 199 characters: OSReport formats into 256 bytes, and the log line
// adds a 25-character prefix and the newline.
char sceneChangeText[200]="none";
story::Change storyChange;
story::ChapterWatch chapterWatch;
CommandDrawContext* devDrawContext;
// The random test (dev_console.cpp) gives every zone a seed; the state below
// follows from it alone, so a seed from a console run replays the same zone.
char loadoutText[200]="fixed: Ordon sword, lantern (X), boomerang (Y)";
std::uint32_t loadoutRandom(std::uint32_t& state){
    state+=0x9E3779B9u;std::uint32_t z=state;
    z=(z^(z>>16))*0x85EBCA6Bu;z=(z^(z>>13))*0xC2B2AE35u;return z^(z>>16);
}
int itemSlot(u8 item){for(int slot=SLOT_0;slot<=SLOT_23;++slot)if(dComIfGs_getItem(slot,false)==item)return slot;return -1;}
// Life (quarter hearts) the current test zone starts with; each phase of the
// zone restores it (dev_console refillLife). 0 outside a test session.
u16 zoneStartLife;
unsigned lifeRefills;
// Random equipment and state on the test's empty save: clothes, sword,
// shield, two items on X and Y, hearts, rupees (the Magic Armor spends them)
// and the time of day. Equipment is selected directly: the original get
// functions of several shields and armors are empty (the item-get event
// equips them).
void randomLoadout(int form,std::uint32_t seed){
    struct Gear {u8 item;u8 collect;const char* name;};
    static constexpr Gear clothes[]{{dItemNo_WEAR_CASUAL_e,0,"Ordon clothes"},{dItemNo_WEAR_KOKIRI_e,0,"Hero's clothes"},
        {dItemNo_WEAR_ZORA_e,0,"Zora Armor"},{dItemNo_ARMOR_e,0,"Magic Armor"}};
    static constexpr Gear swords[]{{dItemNo_WOOD_STICK_e,COLLECT_WOODEN_SWORD,"wooden sword"},{dItemNo_SWORD_e,COLLECT_ORDON_SWORD,"Ordon sword"},
        {dItemNo_MASTER_SWORD_e,COLLECT_MASTER_SWORD,"Master Sword"},{dItemNo_LIGHT_SWORD_e,COLLECT_LIGHT_SWORD,"light sword"}};
    static constexpr Gear shields[]{{dItemNo_WOOD_SHIELD_e,COLLECT_WOODEN_SHIELD,"wooden shield"},{dItemNo_SHIELD_e,COLLECT_ORDON_SHIELD,"Ordon shield"},
        {dItemNo_HYLIA_SHIELD_e,COLLECT_HYLIAN_SHIELD,"Hylian shield"}};
    // Items that work without a story flag. The two clawshots share a slot.
    // The fishing rod is FISHING_ROD_1 (slot 20): Alpha 6 listed BEE_ROD,
    // whose get function is empty, so six zones claimed a rod they never had
    // and kept the previous zone's X or Y item.
    static constexpr Gear items[]{{dItemNo_KANTERA_e,0,"lantern"},{dItemNo_BOOMERANG_e,0,"Gale Boomerang"},{dItemNo_SPINNER_e,0,"Spinner"},
        {dItemNo_IRONBALL_e,0,"Ball and Chain"},{dItemNo_BOW_e,0,"bow"},{dItemNo_HOOKSHOT_e,1,"clawshot"},{dItemNo_W_HOOKSHOT_e,1,"double clawshots"},
        {dItemNo_HVY_BOOTS_e,0,"Iron Boots"},{dItemNo_COPY_ROD_e,0,"Dominion Rod"},{dItemNo_PACHINKO_e,0,"slingshot"},{dItemNo_FISHING_ROD_1_e,0,"fishing rod"}};
    constexpr unsigned itemCount=sizeof(items)/sizeof(*items);
    std::uint32_t state=seed;
    const auto& cloth=clothes[loadoutRandom(state)%4];
    const auto& sword=swords[loadoutRandom(state)%4];
    const auto& shield=shields[loadoutRandom(state)%3];
    dComIfGs_setCollectClothes(KOKIRI_CLOTHES_FLAG);dComIfGs_setSelectEquipClothes(cloth.item);
    dComIfGs_setCollectSword(sword.collect);dComIfGs_setSelectEquipSword(sword.item);
    dComIfGs_setCollectShield(shield.collect);dComIfGs_setSelectEquipShield(shield.item);
    const unsigned first=loadoutRandom(state)%itemCount;
    unsigned second=loadoutRandom(state)%(itemCount-1);if(second>=first)++second;
    if(items[first].collect&&items[second].collect)second=(second+2)%itemCount; // never both clawshots
    if(second==first)second=(first+1)%itemCount;
    execItemGet(items[first].item);execItemGet(items[second].item);
    const int slotX=itemSlot(items[first].item),slotY=itemSlot(items[second].item);
    // The save is kept from zone to zone: an item that was not given must
    // clear its button (0xff, as the original does), not leave the last
    // zone's selection there.
    dComIfGs_setSelectItemIndex(SELECT_ITEM_X,slotX>=0?u8(slotX):u8(0xff));
    dComIfGs_setSelectItemIndex(SELECT_ITEM_Y,slotY>=0&&slotY!=slotX?u8(slotY):u8(0xff));
    if(slotX<0||slotY<0)OSReport("Native test loadout: item get gave nothing (X %02x slot %d, Y %02x slot %d); that button is cleared\n",
        unsigned(items[first].item),slotX,unsigned(items[second].item),slotY);
    // At least 3 full hearts. The same number of draws as before keeps the
    // rupees and time of day of every seed (Alpha 6: 12 of 93 zones died on
    // 0.25 to 5.5 hearts). Life is in quarter hearts; max life in fifths.
    const unsigned hearts=3+loadoutRandom(state)%18,quarters=12+loadoutRandom(state)%(hearts*4-11);
    dComIfGs_setMaxLife(u8(hearts*5));dComIfGs_setLife(u16(quarters));
    const unsigned rupees=(loadoutRandom(state)%3)?loadoutRandom(state)%501:0;
    dComIfGs_setRupee(u16(rupees));
    const float time=float(loadoutRandom(state)%360);
    dComIfGs_setTime(time);
    // X and Y as the save holds them now, so the report never claims gear
    // the zone did not have.
    auto onButton=[](int button)->const char*{
        const u8 slot=dComIfGs_getSelectItemIndex(button);
        if(slot==0xff)return "none";
        const u8 item=dComIfGs_getItem(slot,false);
        for(const auto& gear:items)if(gear.item==item)return gear.name;
        return "other";
    };
    std::snprintf(loadoutText,sizeof(loadoutText),"%s, %s, %s, %s, X=%s, Y=%s, hearts %u.%02u/%u, rupees %u, time %.0f",
        form?"wolf":"human",cloth.name,sword.name,shield.name,onButton(SELECT_ITEM_X),onButton(SELECT_ITEM_Y),
        quarters/4,(quarters%4)*25,hearts,rupees,double(time));
}
void developerLoadout(int form,std::uint32_t seed){
    if(seed){
        randomLoadout(form,seed);
        OSReport("Native test loadout seed=%08lx: %s\n",static_cast<unsigned long>(seed),loadoutText);
    }else{
        // Sword, shield, lantern and boomerang: the scripted B, R, X and Y
        // inputs then exercise the original attack, guard, light and
        // projectile code.
        execItemGet(dItemNo_SWORD_e);execItemGet(dItemNo_HYLIA_SHIELD_e);
        execItemGet(dItemNo_KANTERA_e);execItemGet(dItemNo_BOOMERANG_e);
        dComIfGs_setSelectItemIndex(SELECT_ITEM_X,SLOT_1);dComIfGs_setSelectItemIndex(SELECT_ITEM_Y,SLOT_0);
        // The save carries over between zones: start each one with full life.
        dComIfGs_setLife(dComIfGs_getMaxLifeGauge());
        std::snprintf(loadoutText,sizeof(loadoutText),"%s, fixed: Ordon sword, lantern (X), boomerang (Y)",form?"wolf":"human");
    }
    zoneStartLife=dComIfGs_getLife();
    dComIfGs_setTransformStatus(form?TF_STATUS_WOLF:TF_STATUS_HUMAN);
    // The form only follows that status once transforming is unlocked (event
    // 0x0D04); before it the game makes Link a wolf wherever a twilight he
    // has entered is not cleared, so after the first twilight zone every
    // later "human" zone showed the wolf. Human zones are the normal world
    // (the three twilights cleared), wolf zones the twilight one.
    dComIfGs_onEventBit(0x0D04);
    for(int level=0;level<3;++level){if(form)dComIfGs_offDarkClearLV(level);else dComIfGs_onDarkClearLV(level);}
    // No dComIfGp_itemDataInit here: it clears the live message heaps of a
    // running PLAY_SCENE. The next stage's meters read the save themselves.
}
// Restore each slice independently. These flags are original quest milestones
// (d_save_bit_labels.inc), and each item uses the original acquisition path.
void storyCheckpoint(story::Checkpoint checkpoint, int form) {
    const auto base = story::chapterBase(checkpoint);
    using Checkpoint = story::Checkpoint;
    using Flags = dSv_event_flag_c;
    auto mark = [](std::initializer_list<u16> flags) {
        for (u16 flag : flags) dComIfGs_onEventBit(flag);
    };
    if (base >= Checkpoint::ordon) mark({Flags::M_001, Flags::M_002, Flags::F_0700});
    if (base >= Checkpoint::faron) {
        mark({Flags::F_0575, Flags::F_0600, Flags::M_095});
        execItemGet(dItemNo_WOOD_STICK_e);
        execItemGet(dItemNo_PACHINKO_e);
        execItemGet(dItemNo_KANTERA_e);
        dComIfGs_setSelectItemIndex(SELECT_ITEM_X, SLOT_1);
    }
    if (base >= Checkpoint::faronCleared) {
        mark({Flags::F_0565, Flags::M_008, Flags::M_009, Flags::M_010, Flags::M_011, Flags::M_012,
            Flags::M_013, Flags::M_014, Flags::M_015, Flags::M_016, Flags::M_017, Flags::M_019,
            Flags::M_022, Flags::M_065, Flags::M_072});
        dComIfGs_onDarkClearLV(0);
        execItemGet(dItemNo_WEAR_KOKIRI_e);
        execItemGet(dItemNo_SWORD_e);
        execItemGet(dItemNo_WOOD_SHIELD_e);
        execItemGet(dItemNo_BOOMERANG_e);
        dComIfGs_setSelectItemIndex(SELECT_ITEM_Y, SLOT_0);
    }
    if (base >= Checkpoint::eldinTwilight) {
        mark({Flags::F_0279, Flags::F_0804});
    }
    if (base >= Checkpoint::eldinCleared) {
        mark({Flags::M_018, Flags::M_021, Flags::M_023, Flags::M_024, Flags::M_027, Flags::M_028,
            Flags::M_029, Flags::M_031});
        dComIfGs_onDarkClearLV(1);
        execItemGet(dItemNo_HVY_BOOTS_e);
        execItemGet(dItemNo_BOW_e);
    }
    if (base >= Checkpoint::lanayruTwilight) mark({Flags::M_032});
    if (base >= Checkpoint::lanayruCleared) {
        mark({Flags::M_064, Flags::M_070});
        dComIfGs_onDarkClearLV(2);
    }
    if (form) {
        mark({Flags::M_067});
        dComIfGs_onTransformLV(base >= Checkpoint::lanayruTwilight ? 2 : 1);
    }
    dComIfGs_setTransformStatus(form ? TF_STATUS_WOLF : TF_STATUS_HUMAN);
    dComIfGs_setTime(165.0f);
    story::applyChapterCheckpoint(checkpoint);
    chapterWatch = {};
    const auto* step = devConsoleStoryContext().definition;
    if (step && step->chapter.introInput == story::IntroInput::argorok) {
        // The Aeralfos intro leaves this process-global flag set. A fresh
        // Argorok checkpoint needs B_dr to create its original director.
        OSReport("Native Argorok checkpoint: incoming demo skip=%u\n", unsigned(cDmr_SkipInfo));
        cDmr_SkipInfo = 0;
    }
    dComIfGs_setLife(dComIfGs_getMaxLifeGauge());
    zoneStartLife = dComIfGs_getLife();
    cM_initRnd(17, 53, 113);
    cM_initRnd2(19, 59, 127);
    std::snprintf(loadoutText, sizeof(loadoutText),
        "story checkpoint %u, %s, clothes=%02x sword=%02x shield=%02x X=%02x Y=%02x",
        unsigned(checkpoint), form ? "wolf" : "human", unsigned(dComIfGs_getSelectEquipClothes()),
        unsigned(dComIfGs_getSelectEquipSword()), unsigned(dComIfGs_getSelectEquipShield()),
        unsigned(dComIfGs_getSelectItemIndex(SELECT_ITEM_X)), unsigned(dComIfGs_getSelectItemIndex(SELECT_ITEM_Y)));
}

bool developerWarp(const char* stage,int room,int point,int layer,int form,bool test){
    auto* play=fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    scene_class* other{};
    if(!play){
        for(const s16 name:{s16(fpcNm_NAME_SCENE_e),s16(fpcNm_OPENING_SCENE_e),s16(fpcNm_MENU_SCENE_e)})
            if(auto* found=fpcM_SearchByName(name)){other=reinterpret_cast<scene_class*>(found);break;}
        if(!other)return false; // logo and warnings: startup resources may still be loading
    }else if(dComIfGp_isEnableNextStage())return false;
    const bool noSave=!play||!std::strncmp(dComIfGp_getStartStageName(),"F_SP102",8);
    const bool clearSave=noSave||(test&&!developerSession);
    if(other){
        if(!fopScnM_ChangeReq(other,fpcNm_PLAY_SCENE_e,0,5))return false;
        // As the file menu does before PLAY. itemInit clears the whole item
        // block and keeps only the 2D heap; the title (opening scene) is a
        // play scene that still owns the message heap, the sub heaps and its
        // message object and releases them when it is deleted. Alpha 1 lost
        // them here: START+SELECT in the title ended in a null heap destroy.
        auto& state=g_dComIfG_gameInfo.play;
        auto* message=state.getMsgExpHeap();auto* sub0=state.getSubExpHeap2D(0);auto* sub1=state.getSubExpHeap2D(1);
        auto* messageObject=state.getMsgObjectClass();
        dComIfGp_itemDataInit();
        state.setMsgExpHeap(message);state.setSubExpHeap2D(0,sub0);state.setSubExpHeap2D(1,sub1);
        state.setMsgObjectClass(messageObject);
        dComIfGp_setNextStage(stage,point,room,layer);
        g_dComIfG_gameInfo.play.mNextStage.getStartStage()->set(stage,room,point,layer);
    }else dComIfGp_setNextStage(stage,point,room,layer);
    if(clearSave)developerSession=true;
    travelRequested=true;
    const auto zone = test ? devConsoleZoneLoadout() : DevZoneLoadout{};
    if (zone.checkpoint != story::Checkpoint::none) developerSession = true;
    pendingTravel={developerSession,clearSave,form,zone.seed,zone.checkpoint};
    OSReport("Native developer travel: %s room=%d point=%d layer=%d form=%d session=%d\n",stage,room,point,layer,form,int(developerSession));
    return true;
}
void developerStuckReport(){
    OSReport("Native stuck stage: %.8s room=%d drive=%ld overlap-peek=%d overlap-done=%d overlap-request=%d next-stage=%d player=%p\n",
        dComIfGp_getStartStageName(),dComIfGp_roomControl_getStayNo(),static_cast<long>(DVDGetDriveStatus()),fopOvlpM_IsPeek(),fopOvlpM_IsDone(),
        fopOvlpM_IsDoingReq(),int(dComIfGp_isEnableNextStage()),dComIfGp_getPlayer(0));
    for(const auto& [name,heap]:{std::pair<const char*,JKRHeap*>{"game",mDoExt_getGameHeap()},{"zelda",mDoExt_getZeldaHeap()},
            {"archive",mDoExt_getArchiveHeap()},{"j2d",mDoExt_getJ2dHeap()},{"command",mDoExt_getCommandHeap()}})
        if(heap)OSReport("Native stuck stage: %s heap size=%lu free=%lu largest=%lu\n",name,static_cast<unsigned long>(heap->getHeapSize()),
            static_cast<unsigned long>(heap->getTotalFreeSize()),static_cast<unsigned long>(heap->getFreeSize()));
    if(auto* player=dComIfGp_getPlayer(0)){
        // The player waits in creation until collision exists under its start point.
        dBgS_GndChk check;cXyz position=player->current.pos;position.y+=100.0f;check.SetPos(&position);
        const f32 ground=dComIfG_Bgsp().GroundCross(&check);
        OSReport("Native stuck stage: player at %.1f %.1f %.1f room=%d ground-below=%.1f event-data=%d\n",player->current.pos.x,player->current.pos.y,
            player->current.pos.z,int(fopAcM_GetRoomNo(player)),ground,int(dComIfGp_getEventManager().dataLoaded()));
    }
    static int backgrounds;backgrounds=0;
    fopAcIt_Judge(+[](void* actor,void*)->void*{
        auto* a=static_cast<fopAc_ac_c*>(actor);
        if(fopAcM_GetProfName(a)==fpcNm_BG_e){++backgrounds;OSReport("Native stuck stage: background actor room=%d heap=%p\n",int(fopAcM_GetParam(a)),a->heap);}
        return nullptr;
    },nullptr);
    OSReport("Native stuck stage: %d background actors exist\n",backgrounds);
    static int listed;listed=0;
    fpcCtIt_Method(+[](void* tag,void*)->int{
        const auto* request=static_cast<const create_request*>(tag);
        if(listed++<24)OSReport("Native stuck stage: creating profile=%d id=%lu doing=%d cancel=%d\n",
            request->process?int(fpcM_GetProfName(request->process)):-1,static_cast<unsigned long>(request->id),int(request->is_doing),int(request->is_cancel));
        if(request->process&&fpcM_GetProfName(request->process)==fpcNm_ROOM_SCENE_e){
            auto* room=reinterpret_cast<room_of_scene_class*>(request->process);
            const int number=int(fopScnM_GetParam(room));
            auto* block=dStage_roomControl_c::getMemoryBlock(number);
            OSReport("Native stuck stage: room scene room=%d phase=%d bank-step=%d block=%p used=%lu size=%lu flag8=%d flag20=%d\n",number,room->phase.id,
                int(room->field_0x1d4),block,static_cast<unsigned long>(block?block->getTotalUsedSize():0),
                static_cast<unsigned long>(block?block->getHeapSize():0),int(dComIfGp_roomControl_checkStatusFlag(number,8)),
                int(dComIfGp_roomControl_checkStatusFlag(number,0x20)));
        }
        return 1;
    },nullptr);
    OSReport("Native stuck stage: %d processes still being created\n",listed);
}
void developerReport(std::FILE* file);
// sdmc:/3ds/DuskLight/measure-at.txt: picture numbers at which a measurement
// is taken (measured_dump.hpp), for scripted runs: "600 900r 1200 1500-lod"
// (r: a reference measurement, the improvements of perf_features.hpp off;
// -name: without that improvement alone). Each is appended to
// sdmc:/3ds/DuskLight/measure.txt when it is complete.
struct MeasureTrigger {std::uint64_t frame;bool reference;unsigned without;bool taken;};
std::array<MeasureTrigger,16> measureTriggers{};
unsigned measureTriggerCount{};
// The game state written into a dump's info.txt: what would otherwise have to
// be dug out of the RAM image by hand (camera, cutscene, player, heaps, actors).
void dumpSceneReport(std::FILE* file){
    std::fprintf(file,"stage=%.8s room=%d point=%d layer=%d next_stage_pending=%d\n",dComIfGp_getStartStageName(),
        int(dComIfGp_roomControl_getStayNo()),int(dComIfGp_getStartStagePoint()),int(dComIfGp_getStartStageLayer()),int(dComIfGp_isEnableNextStage()));
    const char* scene="none";
    for(const auto& [name,id]:{std::pair<const char*,s16>{"play",fpcNm_PLAY_SCENE_e},{"opening (title)",fpcNm_OPENING_SCENE_e},
            {"name (file select)",fpcNm_NAME_SCENE_e},{"menu",fpcNm_MENU_SCENE_e},{"logo",fpcNm_LOGO_SCENE_e}})
        if(fpcM_SearchByName(id)){scene=name;break;}
    std::fprintf(file,"scene=%s\ntime_of_day=%.2f day=%u\n",scene,dComIfGs_getTime(),unsigned(dComIfGs_getDate()));
    if(auto* camera=dComIfGp_getCamera(0)){
        auto* view=reinterpret_cast<camera_class*>(camera);
        const cXyz eye=*fopCamM_GetEye_p(view),center=*fopCamM_GetCenter_p(view);
        std::fprintf(file,"camera_eye=%.3f %.3f %.3f\ncamera_center=%.3f %.3f %.3f\ncamera_fovy=%.2f bank=%d\n",
            eye.x,eye.y,eye.z,center.x,center.y,center.z,fopCamM_GetFovy(view),int(fopCamM_GetBank(view)));
    }else std::fprintf(file,"camera=none\n");
    std::fprintf(file,"cutscene_camera=%d cutscene_frame=%lu cutscene_mode=%ld event_running=%d\n",int(cutsceneCameraActive()),
        static_cast<unsigned long>(dDemo_c::getFrame()),static_cast<long>(dDemo_c::getMode()),int(dComIfGp_event_runCheck()));
}
void dumpStateReport(std::FILE* file){
    dumpSceneReport(file);
    if (chapterWatch.step == devConsoleStoryContext().definition && chapterWatch.step &&
        chapterWatch.observedActor != fpcM_ERROR_PROCESS_ID_e) {
        if (auto* target = fopAcM_SearchByID(chapterWatch.observedActor)) {
            auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
            std::fprintf(file,"story_subject=%s id=%u health=%d room=%d pos=%.1f %.1f %.1f target_lock=%u\n",
                chapterWatch.step->chapter.subject,unsigned(chapterWatch.observedActor),int(target->health),
                int(fopAcM_GetRoomNo(target)),target->current.pos.x,target->current.pos.y,target->current.pos.z,
                unsigned(link && link->getAtnActorID() == chapterWatch.observedActor));
        }
    }
    if(auto* player=dComIfGp_getPlayer(0)){
        std::fprintf(file,"player_position=%.3f %.3f %.3f angle_y=%d speed=%.2f room=%d life=%u\n",player->current.pos.x,player->current.pos.y,
            player->current.pos.z,int(player->shape_angle.y),player->speedF,int(fopAcM_GetRoomNo(player)),unsigned(dComIfGs_getLife()));
        if(auto* link=dComIfGp_getLinkPlayer())std::fprintf(file,"link_demo_type=%d demo_mode=%lu wolf=%d\n",link->mDemo.getDemoType(),
            static_cast<unsigned long>(link->getDemoMode()),int(dComIfGs_getTransformStatus()));
    }else std::fprintf(file,"player=none\n");
    for(const auto& [name,heap]:{std::pair<const char*,JKRHeap*>{"game",mDoExt_getGameHeap()},{"zelda",mDoExt_getZeldaHeap()},
            {"archive",mDoExt_getArchiveHeap()},{"j2d",mDoExt_getJ2dHeap()},{"command",mDoExt_getCommandHeap()}})
        if(heap)std::fprintf(file,"heap_%s=size %lu free %lu largest %lu\n",name,static_cast<unsigned long>(heap->getHeapSize()),
            static_cast<unsigned long>(heap->getTotalFreeSize()),static_cast<unsigned long>(heap->getFreeSize()));
    std::fprintf(file,"linear_free=%lu vram_free=%lu\n",static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()));
    developerReport(file);
    // Every actor: profile number (d_procname.h), room and position.
    static std::FILE* out;static unsigned listed;out=file;listed=0;
    std::fprintf(file,"\n[actors] profile room x y z\n");
    fopAcIt_Judge(+[](void* actor,void*)->void*{
        auto* a=static_cast<fopAc_ac_c*>(actor);
        if(listed++<240)std::fprintf(out,"%d %d %.0f %.0f %.0f\n",int(fopAcM_GetProfName(a)),int(fopAcM_GetRoomNo(a)),
            a->current.pos.x,a->current.pos.y,a->current.pos.z);
        return nullptr;
    },nullptr);
    std::fprintf(file,"actors=%u\n",listed);
    // What the pictures before this dump cost, process by process.
    measured::write(file);
}
void developerReport(std::FILE* file){
    if(const auto* c=devDrawContext)
        std::fprintf(file,"draws_submitted_total=%llu\ndraws_rejected_total=%llu\ndraws_simplified_total=%llu\ndraws_skipped_total=%llu\n"
            "quantized_draws=%llu\nvertex_fog_draws=%llu\ntriangle_fog_draws=%llu\nvertex_indirect_draws=%llu\nculled_draws=%llu\n"
            "camera_culled=%llu\nalpha_union_draws=%llu\nconstant_early_depth_draws=%llu\n",
            static_cast<unsigned long long>(c->submitted),static_cast<unsigned long long>(c->rejected),
            static_cast<unsigned long long>(c->simplifiedDraws),static_cast<unsigned long long>(c->skippedDraws),
            static_cast<unsigned long long>(c->quantizedDraws),static_cast<unsigned long long>(c->vertexFogDraws),
            static_cast<unsigned long long>(c->triangleFogDraws),static_cast<unsigned long long>(c->vertexIndirectDraws),
            static_cast<unsigned long long>(c->culled),static_cast<unsigned long long>(c->cameraCulled),
            static_cast<unsigned long long>(c->alphaUnionDraws),static_cast<unsigned long long>(c->constantEarlyDepthDraws));
    // Degraded draws by reason, running totals (DrawVisualFallback order);
    // zone differences attribute them per zone.
    // Running total; a zone that adds to it between its first and last
    // report had actors deleted with colliders still registered.
    std::fprintf(file,"stale_colliders_removed_total=%u\n",duskNativeStaleCollidersRemoved);
    std::fprintf(file,"scene_logic_only_ticks_total=%u\nlogic_ticks_total=%u\n",duskNativeSceneLogicRuns,duskNativeLogicTick);
    // Alpha 8 paths, running totals (a zone's share is the difference between
    // two reports): retained 2D screens, repeated texture retentions answered
    // from the memo, warped draws given a constant offset on the GPU route.
    {const auto t=screenTapeStatistics();
     std::fprintf(file,"retained_screens_total=draws:%llu replayed:%llu recorded:%llu plain:%llu volatile:%llu refused:%llu dropped:%llu recycled:%llu\n"
        "retained_screens_store=kept:%lu words:%lu largest:%lu geometry_bytes:%lu\n",
        static_cast<unsigned long long>(t.begins),static_cast<unsigned long long>(t.replayed),static_cast<unsigned long long>(t.recorded),
        static_cast<unsigned long long>(t.plain),static_cast<unsigned long long>(t.volatileScreens),
        static_cast<unsigned long long>(t.refusedSpan+t.refusedState+t.refusedGeometry+t.refusedSize),
        static_cast<unsigned long long>(t.dropped),static_cast<unsigned long long>(t.recycled),
        static_cast<unsigned long>(t.tapes),static_cast<unsigned long>(t.words),static_cast<unsigned long>(t.largestWords),
        static_cast<unsigned long>(t.geometryBytes));
     std::fprintf(file,"retained_screens_panes=split_screens:%llu children:%llu in_runs:%llu changed_lately:%llu not_recordable:%llu "
        "base_changes:%llu left_out:%llu\n",
        static_cast<unsigned long long>(t.notes[duskScreenNoteSplitScreens]),static_cast<unsigned long long>(t.notes[duskScreenNoteChildren]),
        static_cast<unsigned long long>(t.notes[duskScreenNoteInRun]),static_cast<unsigned long long>(t.notes[duskScreenNoteUnsettled]),
        static_cast<unsigned long long>(t.notes[duskScreenNoteUnrecordable]),static_cast<unsigned long long>(t.notes[duskScreenNoteBaseChanged]),
        static_cast<unsigned long long>(t.notes[duskScreenNoteOverflow]));}
    {const auto t=textureResourceStatistics();
     std::fprintf(file,"texture_retention_total=memo_hits:%lu conflicts:%lu images:%u owners:%u bytes:%lu\n",t.memoHits,t.conflicts,t.images,t.owners,
        static_cast<unsigned long>(t.bytes));}
    {const auto& c=gx::indirectConstantCounters;
     std::fprintf(file,"indirect_constant_total=planned:%llu gpu:%llu vertices:%llu per_pixel:%llu restarts:%llu refused_scale:%llu refused_other:%llu "
        "camera_divisor:%llu\n",
        static_cast<unsigned long long>(c.planned),static_cast<unsigned long long>(c.gpu),static_cast<unsigned long long>(c.vertices),
        static_cast<unsigned long long>(c.perPixel),static_cast<unsigned long long>(c.restarts),
        static_cast<unsigned long long>(c.refused[unsigned(gx::IndirectConstantRefusal::scale)]),
        static_cast<unsigned long long>(c.refused[unsigned(gx::IndirectConstantRefusal::deviation)]+c.refused[unsigned(gx::IndirectConstantRefusal::shared)]+
            c.refused[unsigned(gx::IndirectConstantRefusal::texture)]+c.refused[unsigned(gx::IndirectConstantRefusal::units)]+
            c.refused[unsigned(gx::IndirectConstantRefusal::projected)]),static_cast<unsigned long long>(c.cameraDivisor));
     const auto& w=gx::vertexWarpCounters;std::uint64_t refused=0;for(auto v:w.refused)refused+=v;
     using W=gx::VertexWarpRefusal;
     std::fprintf(file,"vertex_warp_total=draws:%llu vertices:%llu integer_pass:%llu copied_draws:%llu copied_vertices:%llu units:%llu "
        "constant_units:%llu refused:%llu refused_role:%llu refused_texgen:%llu refused_uniform:%llu\n",
        static_cast<unsigned long long>(w.draws),static_cast<unsigned long long>(w.vertices),
        static_cast<unsigned long long>(gx::vertexWarpIntegerVertices),static_cast<unsigned long long>(gx::vertexWarpRepeatDraws),
        static_cast<unsigned long long>(gx::vertexWarpRepeatVertices),
        static_cast<unsigned long long>(w.units),static_cast<unsigned long long>(w.constantUnits),static_cast<unsigned long long>(refused),
        static_cast<unsigned long long>(w.refused[unsigned(W::role)]),static_cast<unsigned long long>(w.refused[unsigned(W::texgen)]),
        static_cast<unsigned long long>(w.refused[unsigned(W::uniform)]));}
    // Arena bytes that draws of compact records returned (running total).
    if(const auto* context=devDrawContext)
        std::fprintf(file,"arena_returned_total=bytes:%llu\n",static_cast<unsigned long long>(context->compactReservationBytes));
    // Second-core worker (runtime/gx_worker.hpp), running totals. unmarked,
    // sum_mismatches, damaged, binding_refusals and list_failures must be
    // zero; shadow_job_failures are queued shadow caster passes that drew
    // nothing usable after all (shadow_casters.hpp).
    {char text[560];describeGxWorker(text,sizeof(text));
     std::uint64_t queued{},lists{},failures{},runs{},refusedRuns{},shadowJobs{},shadowFailures{};nativeWorkerStatistics(queued,lists,failures);
     nativeRunStatistics(runs,refusedRuns);gxShadowCasterJobStatistics(shadowJobs,shadowFailures);
     std::fprintf(file,"gx_worker_total=%s lists:%llu list_failures:%llu runs:%llu runs_kept_for_arrays:%llu shadow_jobs:%llu shadow_job_failures:%llu copy_jobs:%llu\n",text,
        static_cast<unsigned long long>(lists),static_cast<unsigned long long>(failures),
        static_cast<unsigned long long>(runs),static_cast<unsigned long long>(refusedRuns),
        static_cast<unsigned long long>(shadowJobs),static_cast<unsigned long long>(shadowFailures),
        static_cast<unsigned long long>(queuedCopyTextures()));}
    // Perspective draws left on the CPU vertex route: by the gate that kept
    // them (draws/vertices; gx_command_draw.cpp: 2 lookup carrier, 5 alpha
    // union or interval, 6 triangle fog, 7 clip disabled, 8 lines and points,
    // 9 indirect warp, 10 more than three textures, 11-12 projected texture,
    // 13 plan refused, 14 refused after the plan, 15 all gates but 14), and
    // the reject codes of the plans and of the compact decoders.
    {std::fprintf(file,"cpu_route_gates_total=");
     for(unsigned g=0;g<gxPerspCpuGateTotal.size();++g)if(gxPerspCpuGateTotal[g])
        std::fprintf(file,"%u:%llu/%llu ",g,static_cast<unsigned long long>(gxPerspCpuGateTotal[g]),static_cast<unsigned long long>(gxPerspCpuGateVerticesTotal[g]));
     std::fprintf(file,"\nrigid_plan_rejects_total=");
     for(unsigned c=0;c<gx::gxRigidPlanRejects.size();++c)if(const auto n=gx::gxRigidPlanRejectsCleared[c]+gx::gxRigidPlanRejects[c])
        std::fprintf(file,"%u:%llu ",c,static_cast<unsigned long long>(n));
     std::fprintf(file,"\ncompact_decode_rejects_total=");
     for(unsigned c=0;c<gx::gxCompactDecodeRejects.size();++c)if(const auto n=gx::gxCompactDecodeRejectsCleared[c]+gx::gxCompactDecodeRejects[c])
        std::fprintf(file,"%u:%llu ",c,static_cast<unsigned long long>(n));
     std::fprintf(file,"\ngpu_route_additions_total=coarse_coordinates:%llu unread_attributes:%llu per_bone_texgen_plans:%llu per_bone_texgen_refused:%llu "
        "swapped_arrays:%llu three_coordinate_sets:%llu fog_curve_ramps:%llu\n",
        static_cast<unsigned long long>(gx::gxCoarseTexcoordDecodes),static_cast<unsigned long long>(gx::gxUnreadAttributeDecodes),
        static_cast<unsigned long long>(gx::gxPairedTexgenPlans),static_cast<unsigned long long>(gx::gxPairedTexgenRefusals),
        static_cast<unsigned long long>(gx::gxSwappedArrayDecodes),static_cast<unsigned long long>(gx::gxWideRecordPlans),
        static_cast<unsigned long long>(devDrawContext?devDrawContext->fogAlphaRampDraws:0));}
    if(const auto* c=devDrawContext){const auto& r=c->simplifiedByReason;
        std::fprintf(file,"simplified_by_reason_total=tev:%llu clipped_depth:%llu projected_divisor:%llu dual_alpha:%llu shared_indirect:%llu indirect_divisor:%llu\n",
            static_cast<unsigned long long>(r[0]),static_cast<unsigned long long>(r[1]),static_cast<unsigned long long>(r[2]),
            static_cast<unsigned long long>(r[3]),static_cast<unsigned long long>(r[4]),static_cast<unsigned long long>(r[5]));}
    // Rejections since the previous zone report, by reason. The first draw of
    // each reason is described in the log again in the next zone.
    if(auto* c=devDrawContext)for(unsigned n=0;n<c->rejectReasonCount;++n){
        auto& reason=c->rejectReasons[n];
        if(reason.draws!=reason.mark)std::fprintf(file,"rejected_reason=%llu draws since the last report (%llu total, %llu vertices): %s\n",
            static_cast<unsigned long long>(reason.draws-reason.mark),static_cast<unsigned long long>(reason.draws),
            static_cast<unsigned long long>(reason.vertices),reason.why);
        reason.mark=reason.draws;reason.reported=false;
    }
    auto* gameHeap=mDoExt_getGameHeap();auto* zeldaHeap=mDoExt_getZeldaHeap();auto* archiveHeap=mDoExt_getArchiveHeap();
    std::fprintf(file,"texture_linear_evictions=%llu\n",static_cast<unsigned long long>(gxTextureLinearEvictions()));
    {   // worldMips: resident levels now; decisions since start.
        using ull=unsigned long long;const auto m=gxWorldMipStatistics();
        std::fprintf(file,"world_mips_images=%u\nworld_mips_bytes=%lu\nworld_mips_made=%llu\nworld_mips_binds=%llu sampled=%llu base_level=%llu without_levels=%llu\n"
            "world_mips_refused=shape %llu alpha %llu cap %llu linear %llu allocation %llu\n",
            m.images,static_cast<unsigned long>(m.bytes),ull(m.made),ull(m.worldBinds),ull(m.sampledBinds),ull(m.levelBaseBinds),ull(m.residentWithout),
            ull(m.refusedShape),ull(m.refusedAlpha),ull(m.refusedCap),ull(m.refusedLinear),ull(m.refusedAllocation));
    }
    std::fprintf(file,"game_heap_free=%lu\nzelda_heap_free=%lu\narchive_heap_free=%lu\ngpu_texture_bytes=%lu\nowned_bti_bytes=%lu\n"
        "font_page_bytes=%lu\ncopy_image_bytes=%lu\noffscreen_bytes=%lu\ntransient_geometry_bytes=%lu\n",
        static_cast<unsigned long>(gameHeap?gameHeap->getTotalFreeSize():0),static_cast<unsigned long>(zeldaHeap?zeldaHeap->getTotalFreeSize():0),
        static_cast<unsigned long>(archiveHeap?archiveHeap->getTotalFreeSize():0),static_cast<unsigned long>(gxTextureBytes()),
        static_cast<unsigned long>(textureResourceBytes()),static_cast<unsigned long>(fontPageBytes()),
        static_cast<unsigned long>(copyImageBytes()),static_cast<unsigned long>(offscreenFramebufferBytes()),
        static_cast<unsigned long>(gxGeometryBytes()));
    // resets: full pool during a scene; stage_clears: emptied for a new play
    // scene. palette_*: compact-palette batches (A5), arena = later batches
    // of a list copied instead of bound resident.
    if(auto* sc=gx::staticCompactCache())std::fprintf(file,"static_geometry_hits=%llu\nstatic_geometry_misses=%llu\nstatic_geometry_stores=%llu\n"
        "static_geometry_resets=%llu\nstatic_geometry_bytes=%u\nstatic_geometry_capacity=%u\nstatic_geometry_stage_clears=%llu\n"
        "static_geometry_palette_hits=%llu\nstatic_geometry_palette_stores=%llu\nstatic_geometry_palette_arena=%llu\n"
        "static_geometry_palette_replays=%llu\nstatic_geometry_palette_refusals=%llu\n",
        static_cast<unsigned long long>(sc->hits),static_cast<unsigned long long>(sc->misses),static_cast<unsigned long long>(sc->stores),
        static_cast<unsigned long long>(sc->resets),unsigned(sc->bytesUsed()),unsigned(sc->capacity()),static_cast<unsigned long long>(sc->stageClears),
        static_cast<unsigned long long>(sc->paletteHits),static_cast<unsigned long long>(sc->paletteStores),static_cast<unsigned long long>(sc->paletteArena),
        static_cast<unsigned long long>(sc->paletteReplays),static_cast<unsigned long long>(sc->paletteRefusals));
    {std::uint64_t nh,nm,nr,nk;nativeShapeStatistics(nh,nm,nr,nk);
     std::fprintf(file,"direct_shape_hits=%llu\ndirect_shape_misses=%llu\ndirect_shape_records=%llu\ndirect_shape_key_misses=%llu\n",
        static_cast<unsigned long long>(nh),static_cast<unsigned long long>(nm),static_cast<unsigned long long>(nr),static_cast<unsigned long long>(nk));}
    const auto audio=nativeAudioStatus();
    std::fprintf(file,"audio_output=%s\naudio_blocks=%lu\naudio_underruns=%lu\n",audio.output==AudioOutputState::ndsp?"ndsp":"muted",
        static_cast<unsigned long>(audio.blocks),static_cast<unsigned long>(audio.underruns));
    std::fprintf(file,"scene_changes_total=%lu\ngame_scene_changes_total=%lu\nlast_scene_change=%s\nlife_quarters=%u\nzone_start_life=%u\n"
        "life_refills_total=%u\n",static_cast<unsigned long>(sceneChangeCount),static_cast<unsigned long>(gameSceneChangeCount),sceneChangeText,
        unsigned(dComIfGs_getLife()),unsigned(zoneStartLife),lifeRefills);
}
// Test zones: each phase starts with the zone's starting life. Never at zero:
// the death sequence has begun and the zone is reported "died".
void refillTestLife(){
    if(!developerSession||!zoneStartLife||!dComIfGp_getPlayer(0))return;
    const u16 life=dComIfGs_getLife();
    if(!life||life>=zoneStartLife)return;
    dComIfGs_setLife(zoneStartLife);++lifeRefills;
    OSReport("Native test life restored: %u -> %u quarter hearts (refill %u)\n",unsigned(life),unsigned(zoneStartLife),lifeRefills);
}
// Exits of one scene list that lead to the requested stage, point and room.
int matchingExits(stage_scls_info_dummy_class* list,char tag,const char* stage,int point,int room,char* out,int capacity,int at){
    if(!list)return at;
    const int count=list->num;
    stage_scls_info_class* entries=list->m_entries;
    if(!entries||count<=0||count>255)return at;
    for(int i=0;i<count&&at<capacity-8;++i){
        const auto& e=entries[i];
        if(!std::strncmp(e.mStage,stage,8)&&int(e.mStart)==point&&int(e.mRoom)==room)
            at+=std::snprintf(out+at,std::size_t(capacity-at),"%s%c%d",at?",":"",tag,i);
    }
    return at;
}
void observeStoryPad(DevGameStatus& status, camera_class* camera) {
    status.eventRunning = dComIfGp_event_runCheck();
    status.dialogueActive = status.eventRunning && dMsgObject_isTalkNowCheck();
    if (camera) {
        const auto yaw = dCam_getControledAngleY(camera);
        status.cameraForwardX = cM_ssin(yaw);
        status.cameraForwardZ = cM_scos(yaw);
    }
}

// Compare health on the same live enemy process. Actor removal or room changes
// alone cannot witness a hit. Only count health loss near an observed Link attack.
void observeStory(DevGameStatus& status) {
    status.storyChange = storyChange;
    auto* scene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    auto* link = status.playScene ? dComIfGp_getLinkPlayer() : nullptr;
    if (!scene || !link) return;
    status.playSceneId = fpcM_GetID(scene);
    status.wolf = link->checkWolf();
    status.attacked = link->getCutType() != daPy_py_c::CUT_TYPE_NONE;
    status.lanternLit = link->getKandelaarFlamePos() != nullptr;
    status.cutsceneCamera = cutsceneCameraActive();
    auto* camera = dComIfGp_getCamera(0);
    observeStoryPad(status, camera);
    status.cutsceneFrame = status.cutsceneCamera ? unsigned(dDemo_c::getFrame()) : 0;
    if (auto* midna = daPy_py_c::getMidnaActor())
        status.midnaVisible = !midna->checkNoDrawState();
    if (status.room == 8 && !std::strcmp(status.stage, "F_SP122")) {
        status.portalOpen = dComIfGs_isSwitch(story::castleWestSwitch, status.room);
        const auto& alink = *static_cast<daAlink_c*>(link);
        status.portalLockCount = alink.mWolfLockNum;
        status.portalChargeActive = alink.mProcID == daAlink_c::PROC_WOLF_ROLL_ATTACK_CHARGE ||
                                    alink.mProcID == daAlink_c::PROC_WOLF_ROLL_ATTACK_MOVE;
    }

    if (camera) {
        // Actor-driven scenes such as portal activation own the camera
        // directly and don't run a JStudio demo.
        if (!status.cutsceneCamera && status.eventRunning && playerCameraSuspended(camera)) {
            status.cutsceneCamera = true;
            status.cutsceneFrame = unsigned(status.frame);
        }
    }
    if (std::strcmp(status.stage, "F_SP121") && std::strcmp(status.stage, "F_SP122")) return;

    struct Enemy { fpc_ProcID id{}; int health{}; bool seen{}; };
    struct FightSample {
        std::uint32_t scene{};
        unsigned attackWindow{};
        Enemy enemies[32]{};
    };
    static FightSample fight;
    if (fight.scene != status.playSceneId) {
        fight = {};
        fight.scene = status.playSceneId;
    }
    if (status.attacked) fight.attackWindow = 5;
    else if (fight.attackWindow) --fight.attackWindow;
    struct Scan { DevGameStatus& status; FightSample& fight; daAlink_c& link; } scan{status, fight, *static_cast<daAlink_c*>(link)};
    status.enemyDistance = 3500.0f;
    fopAcIt_Judge(+[](void* actor, void* data) -> void* {
        auto* enemy = static_cast<fopAc_ac_c*>(actor);
        auto& [sample, history, wolf] = *static_cast<Scan*>(data);
        if (fopAcM_GetGroup(enemy) != fopAc_ENEMY_e || fopAcM_GetRoomNo(enemy) != sample.room ||
            fopAcM_CheckCondition(enemy, fopAcCnd_NODRAW_e)) return nullptr;
        if (sample.room == 8 && fopAcM_GetName(enemy) != fpcNm_E_S1_e) return nullptr;
        const float dx = enemy->current.pos.x - sample.position[0];
        const float dz = enemy->current.pos.z - sample.position[2];
        const float distance = std::hypot(dx, dz);
        if (distance > 3500 || std::abs(enemy->current.pos.y - sample.position[1]) > 600) return nullptr;
        if (enemy->health > 0) {
            ++sample.enemyCount;
            // The dome retains a tagged beast as Link moves. With one tag
            // in the final pair, steer toward the beast still outside it.
            if (distance < sample.enemyDistance && (!sample.portalLockCount || !wolf.checkWolfLock(enemy))) {
                sample.enemyDistance = distance;
                sample.enemyX = enemy->current.pos.x;
                sample.enemyZ = enemy->current.pos.z;
            }
        }
        const auto id = fopAcM_GetID(enemy);
        for (auto& entry : history.enemies) {
            if (entry.seen && entry.id != id) continue;
            if (entry.seen && history.attackWindow && distance < 650 && enemy->health < entry.health)
                ++sample.damagedEnemies;
            entry = {id, enemy->health, true};
            break;
        }
        return nullptr;
    }, &scan);
}

// Once per picture after the game ran: a new next-stage request is logged
// with where it leads, who asked (test travel, or the game in a cutscene, in
// an event or in play), the exit number (from the instrumented request, else
// -1) with the matching entries of the room 'r' and stage 's' exit lists, the
// calling code address, and Link's speed and mode as the request recorded
// them. A request stays pending until the new scene starts (fade and load,
// many ticks), so this once-per-picture check sees each one.
void observeSceneChange(){
    const bool pending=dComIfGp_isEnableNextStage();
    if(pending&&!nextStagePending){
        const bool own=travelRequested;
        ++sceneChangeCount;if(!own)++gameSceneChangeCount;
        const char* next=dComIfGp_getNextStageName();
        const int point=dComIfGp_getNextStagePoint(),room=dComIfGp_getNextStageRoomNo(),layer=dComIfGp_getNextStageLayer();
        const int stay=dComIfGp_roomControl_getStayNo();
        if (!own && devConsoleStoryContext().step) {
            std::snprintf(storyChange.fromStage, sizeof(storyChange.fromStage), "%.8s", dComIfGp_getStartStageName());
            std::snprintf(storyChange.toStage, sizeof(storyChange.toStage), "%.8s", next);
            storyChange.fromRoom=stay;
            storyChange.toRoom=room;
            storyChange.point=point;
            storyChange.layer=layer;
            storyChange.exit=duskNativeSceneExit;
            storyChange.warpPortal=point==story::playerWarpPoint?dComIfGp_SelectWarpPt_get():-1;
            storyChange.warpSpawn=point==story::playerWarpPoint?dComIfGs_getTurnRestartAngleY():-1;
        }
        char exits[48]="none";int at=0;
        if(!own&&fpcM_SearchByName(fpcNm_PLAY_SCENE_e)){
            if(stay>=0&&stay<64)if(auto* data=dComIfGp_roomControl_getStatusRoomDt(stay))
                at=matchingExits(data->getSclsInfo(),'r',next,point,room,exits,int(sizeof(exits)),at);
            at=matchingExits(dComIfGp_getStageSclsInfo(),'s',next,point,room,exits,int(sizeof(exits)),at);
            if(!at)std::snprintf(exits,sizeof(exits),"none");
        }
        auto* player=dComIfGp_getPlayer(0);
        const char* by=own?"test travel":cutsceneCameraActive()?"game in cutscene":dComIfGp_event_runCheck()?"game in event":"game";
        std::snprintf(sceneChangeText,sizeof(sceneChangeText),
            "%.8s r%d -> %.8s r%d p%d layer %d wipe %d, by %s, exit %d (lists %s), caller %p, speed %.1f mode %08lx, at %.0f %.0f %.0f",
            dComIfGp_getStartStageName(),stay,next,room,point,layer,int(dComIfGp_getNextStageWipe()),by,duskNativeSceneExit,exits,
            duskNativeSceneCaller,dComIfGs_getLastSceneSpeedF(),static_cast<unsigned long>(dComIfGs_getLastSceneMode()),
            player?player->current.pos.x:0.0f,player?player->current.pos.y:0.0f,player?player->current.pos.z:0.0f);
        OSReport("Native scene change %lu: %s\n",static_cast<unsigned long>(sceneChangeCount),sceneChangeText);
        travelRequested=false;duskNativeSceneExit=-1;duskNativeSceneCaller=nullptr;
    }else if(!pending){
        // A request that never set the next stage (the skipped attract movie)
        // must not lend its exit, caller or travel mark to a later one.
        travelRequested=false;duskNativeSceneExit=-1;duskNativeSceneCaller=nullptr;
    }
    nextStagePending=pending;
}
}
// d_s_play phase_00 of a PLAY_SCENE: the previous scene and its actors are
// deleted, and no actor of the new one exists yet.
void applyTravelLoadout(){
    if(!pendingTravel.active)return;
    const auto travel=pendingTravel;pendingTravel={};
    if (travel.checkpoint != story::Checkpoint::none) {
        dComIfGs_init();
        storyCheckpoint(travel.checkpoint, travel.form);
    } else {
        if(travel.clearSave)dComIfGs_init();
        developerLoadout(travel.form,travel.seed);
    }
}
// d_s_logo, after the direct stage entry has initialized an empty save.
void applyBootStageLoadout(){
    // The boot stage's own request follows at once: not a change by the game.
    travelRequested=true;
    if(!bootStageFromTest){
        // boot-loadout.txt (scripted runs): "SEED [wolf]", the automatic test's
        // random state for that hexadecimal seed, the three twilights cleared
        // for a human zone; without it the stage starts with an empty save
        // (the fields and Snowpeak still in twilight).
        if(auto* file=std::fopen("sdmc:/3ds/dusklight/boot-loadout.txt","rb")){
            char text[32]{};const auto count=std::fread(text,1,sizeof(text)-1,file);std::fclose(file);text[count]=0;
            char* end=nullptr;const unsigned long seed=std::strtoul(text,&end,16);
            if(end!=text&&seed)developerLoadout(std::strstr(text,"wolf")?1:0,std::uint32_t(seed));
            else fatal("Invalid boot-loadout.txt; expected a hexadecimal seed and optionally wolf");
        }
        return;
    }
    // The resumed zone's own form and random state (a wolf zone used to come
    // back human after a restart).
    const auto zone=devConsoleZoneLoadout();
    developerSession=true;
    if (zone.checkpoint != story::Checkpoint::none) storyCheckpoint(zone.checkpoint, zone.form);
    else developerLoadout(zone.form,zone.seed);
}
}
// Real-speed scheduling, read by fpcM_Management (f_pc_manager.cpp): which
// tick of the picture paints, which one enters the draw lists, and how many
// original ticks each of those phases stands for.
extern "C" {
bool duskNativeTickPaint=true,duskNativeTickDraw=true;
float duskNativePaintTicks=1,duskNativeDrawTicks=1;
// A tick that enters no draw lists runs the logic of the play scene's draw
// phase (f_pc_manager.cpp, d_s_play.cpp). False (SD switch tick-logic-off.txt)
// restores Alpha 7: once per picture.
bool duskNativeTickLogic=true;
// Serial of the logic tick in progress, and the colliders taken out of the
// collision sets because their actor was deleted before the next collision
// pass (f_op_actor.cpp). In play they stay at zero while duskNativeTickLogic
// is on; scene changes add some (actors execute while the scene does not draw).
unsigned duskNativeLogicTick=0,duskNativeStaleCollidersRemoved=0;
// Times the play scene's draw-phase logic ran on a tick that drew nothing
// (d_s_play.cpp). Zero in a zone with several ticks per picture means the
// scene was not found: the zone report carries it for that reason.
unsigned duskNativeSceneLogicRuns=0;
// Developer log of the cutscene camera, one line per logic tick (SD switch camera-log.txt).
bool duskNativeCameraLog=false;
// The exit number and the code address behind the latest original scene
// change request. The 3DS lines in dStage_changeScene and
// dStage_changeScene4Event record the exit and their caller; those in
// dComIfGp_setNextStage record their caller only (exit -1), so the event
// exit of d_event_data.cpp and direct actor requests are named too. The
// first record wins; the scene change log reads and clears them. They stay
// -1 and null where no request records them.
int duskNativeSceneExit=-1;
const void* duskNativeSceneCaller=nullptr;
}
namespace {
// The original logic runs 30 ticks per second of real time, whatever the
// picture rate. A picture that took longer than one tick is followed by the
// ticks it owes (at most maxTicks per picture, so pictures never fall below
// about a quarter of the logic rate); a stall of more than staleTicks
// (loading, a dump, the HOME menu) is not caught up. Alpha 0 ran one tick per
// picture: at the console's ~14 pictures per second the whole game, its
// cutscene cameras included, ran at 47 % of its speed.
struct LogicPace {
    static constexpr unsigned maxTicks=4,staleTicks=8;
    std::uint64_t period{},clock{};bool anchored{};
    unsigned due(std::uint64_t now){
        if(!period)period=dusk::native::nativeTickFrequency/30;
        if(!anchored||now<clock){clock=now-period;anchored=true;}
        std::uint64_t owed=(now-clock)/period;
        if(owed>staleTicks){clock=now-period;owed=1;}
        const unsigned ticks=owed<1?1:owed>maxTicks?maxTicks:unsigned(owed);
        // Running ahead of real time earns no credit for later pictures.
        clock+=ticks*period;if(clock>now)clock=now;
        return ticks;
    }
    void reset(){anchored=false;}
};
LogicPace logicPace;
}
// The process manager calls this when the painter has issued the frame's draws.
// Submitting them now lets the PICA render during actor update and draw entry;
// the frame is still closed (LCD transfer) by the loop after fapGm_Execute.
extern "C" void duskNativePainterDone(){
    using namespace dusk::native;
    auto& queue=gxworker::queue();
    // Alpha 15 overlap (gx-overlap.txt 2): the worker goes on with the shapes
    // it has not drawn while the next logic tick runs; the game code that
    // frees, creates or rewrites what they read syncs first
    // (gxOverlapSyncHook), the picture's close syncs. It submits what it
    // drew once it is through (Platform::drained). A native packet capture in
    // progress ends first, as a sync would end it (its shapes are queued).
    bool overlap=false;
    if(gxOverlapLevel>=2&&queue.busy()&&!gxworker::Queue::consuming()){
        if(const auto hook=gxworker::Queue::captureHook)hook();
        overlap=queue.beginOverlap();
    }
    // Otherwise the worker has drawn every queued shape before the game
    // updates actors (they free and rewrite what the shapes read).
    if(!overlap)gxworker::sync(gxworker::SyncCause::painterEnd);
    setNativePainterOpen(false);
    auto* stream=gx::commandStream();
    // Native packets still pending are sealed into the game bank now: the
    // seal reads the display lists their records point at, which the game
    // may rewrite once actors run (runtime/native_packets.hpp).
    if(stream)(void)stream->registers();
    if(!overlap&&stream&&stream->idle())(void)platformSubmitGpu();
}
namespace dusk::native {
int runOriginalGame(){
    static FrameTelemetryInput completedPicture; // used only by the synchronous render-owner hook
    // Before the log file is first opened: a resumed test keeps the previous log.
    {DevGameHooks hooks;
     hooks.warp=developerWarp;
     hooks.report=developerReport;hooks.stuck=developerStuckReport;hooks.fullDump=requestApplicationDump;
     hooks.fullDumpBusy=+[]{return applicationDumpRequested()||applicationDumpActive();};
     hooks.benchmarkCapture=+[]{
         const auto zone=devConsoleZoneCaptureContext();
         if(!zone.loadout.zone)return;
         float fps{};int speed{};devConsoleRates(fps,speed);
         captureBenchmarkPicture(completedPicture,fps,speed,zone,zone.loadout.tour,
             developerSession?loadoutText:"none (normal play)",BenchmarkCaptureSource::automatic);
     };
     hooks.tourVisualCapture=+[](TourVisualReason reason){
         const auto zone=devConsoleZoneCaptureContext();
         if(zone.loadout.zone)captureTourVisualPicture(completedPicture,zone,reason);
     };
     hooks.loadout=+[]()->const char*{return loadoutText;};
     hooks.refillLife=refillTestLife;
     hooks.secondCore=+[](SecondCoreTest state){wantGxWorker(state!=SecondCoreTest::off,state==SecondCoreTest::measured);};
     setApplicationDumpReport(dumpStateReport,dumpSceneReport);
     initializeDevConsole(hooks);}
    // A boot-to-benchmark can spend time loading before its first picture.
    // Publish the requested zone now; the first render observation supplies
    // an actual stage and room later.
    BenchmarkInputSite initialInput;
    initialInput.zone=devConsoleZoneLoadout().zone;
    const auto initialStory = devConsoleStoryContext();
    initialInput.storyStep = initialStory.step;
    initialInput.storyPhase = initialStory.phase;
    initialInput.storyTick = initialStory.tick;
    setBenchmarkInputSite(initialInput);
    const auto startupBegin = svcGetSystemTick();

    // Before the 48 MiB arena: the L2 probe borrows a 4 MiB block for a moment.
    reportCpuMode(true);
    const auto cpuReady = svcGetSystemTick();
    const BootOptions options;
    logText("Original native startup: GX stream\n");
    bool rgb565Reference=false;
    if(auto* file=std::fopen("sdmc:/3ds/dusklight/rgb565-rgba8-reference.txt","rb")){
        std::fclose(file);rgb565Reference=true;
    }
    std::array<NativeTextureFingerprint,64> halfPolicy{};const auto halfCount=readTextureHalfPolicy(halfPolicy);
    const bool fusedColorCopy=readFusedColorCopyConfigFromSd();
    {const bool compactOff=options.present("texture-compact-off.txt");
     setGxTextureCompact(!compactOff);
     OSReport("Native texture storage: %s\n",compactOff?"RGBA8 for RGB5A3/CMPR/palettes (texture-compact-off.txt)":"16-bit where the picture is the same (RGBA5551/RGB565/LA8)");}
    // 12 MiB: the title shots need about 10 MiB of images at once (they
    // cycled through a 10 MiB cache); the linear heap grew by 4 MiB for it.
    if(!configureGxTextureCache(12*1024*1024,256,rgb565Reference,{halfPolicy.data(),halfCount},fusedColorCopy))fatal(gxTextureFailure());
    OSReport("Native GX RGB565 storage: %s\n",rgb565Reference?"RGBA8 diagnostic reference":"packed GPU_RGB565");
    OSReport("Native texture half policy: %lu source fingerprints; authored mip1, original CPU assets retained\n",static_cast<unsigned long>(halfCount));
    static gx::CommandMemory memory;static gx::CommandProcessor processor(memory);
    gx::MemoryHandle executableMemory{};
    if(!memory.add({__start__,static_cast<std::size_t>(__end__-__start__)},executableMemory))
        fatal("Original static GX array registration failed");
    processor.setLiveArrayResolver(liveGameArray);
    static std::array<u8,64*1024> batchVertices{};
    static std::array<u16,16*1024> batchIndices{};
    static std::array<u16,8192> batchHash{};
    static std::array<u16,4096> batchRemap{};
    processor.setPrimitiveBatchScratch({batchVertices,batchIndices,4096,batchHash,batchRemap});
    static std::array<u8,1024*1024> topologyStorage{};
    static gx::PrimitiveBatchCache topology;
    if(!topology.attach(topologyStorage))fatal("Original topology cache setup failed");
    processor.setPrimitiveBatchCache(&topology);
    // Immediate effects can exceed the tiny diagnostic scratch. Keep one
    // bounded 64 KiB CPU command buffer; original display lists still borrow
    // their archive data directly. Record the actual high-water requirement.
    static std::array<u8,64*1024> scratch{};static gx::CommandStream stream(processor,scratch);
    static CommandDrawContext drawContext;
    devDrawContext=&drawContext;
    drawContext.allowExactI4Bake=readExactI4BakeConfig();
    OSReport("Native exact I4 quarter bake: %s; strict nonzero nearest/clamp, physical PICA unverified\n",
        drawContext.allowExactI4Bake?"explicit SD opt-in":"disabled");
    drawContext.diagnosticRasterCarrier=readRasterCarrierDiagnostic();
    OSReport("Native dual-raster carrier diagnostic: %s; host semantics only, physical PICA unverified\n",
        drawContext.diagnosticRasterCarrier?"explicit SD opt-in":"disabled");
    drawContext.allowDirectSignedQDiagnostic=readSignedQDiagnostic();
    if(drawContext.allowDirectSignedQDiagnostic)drawContext.reportSignedQ=reportSignedQTrial;
    OSReport("Native direct signed-q diagnostic: %s; physical PICA and mip/pole behavior unverified\n",
        drawContext.allowDirectSignedQDiagnostic?"explicit SD opt-in":"disabled");
    drawContext.allowSignedQPositiveRigidGpu=readPositiveQGpuConfig()&&drawContext.allowDirectSignedQDiagnostic;
    OSReport("Native positive-q GPU experiment: %s; unchanged strict proof, physical equivalence unverified\n",
        drawContext.allowSignedQPositiveRigidGpu?"explicit SD opt-in":"disabled");
    drawContext.allowCameraAlignedIndirectQ=readCameraAlignedProofConfig();
    drawContext.allowCameraAlignedCpuClip=readCameraCpuClipConfig();
    OSReport("Native camera CPU clipping: %s; GPU output positive-only\n",drawContext.allowCameraAlignedCpuClip?"enabled":"disabled");
    drawContext.cameraAlignedIndirectProofOnly=true; // HIGH107: no SD switch permits admission.
    OSReport("Native camera-aligned indirect: %s; signed admission BLOCKED\n",
        drawContext.allowCameraAlignedIndirectQ?"proof observation only":"disabled");
    if(!configureFramebufferAlphaIntervals(readAlphaIntervalConfig(),drawContext.alphaIntervalPolicy))
        fatal("Alpha interval target ownership setup failed");
    OSReport("Native exact alpha interval policy: %s; native stencil ordering unverified\n",
        drawContext.alphaIntervalPolicy.enabled?"explicit SD opt-in":"disabled");
    drawContext.allowRigidGpu=true;
    // Shared source snapshots avoid copying the same arrays for every shape.
    // Keep a bounded 512 KiB input pool and 1 MiB decoded-output pool; every
    // hit still compares fresh consumed bytes and per-draw state stays live.
    // Measured unused since the GPU-resident static cache (stores=0 in all six
    // zones); kept small so code+bss+96 MiB heaps fit the New 3DS app region.
    static std::array<u8,192*1024> compactStorage{};
    static gx::CompactVertexCache compactCache;
    if(!compactCache.attach(compactStorage,64*1024))fatal("Compact cache attachment failed");
    drawContext.compactCache=&compactCache;
    // Native static geometry: decoded vertices of unmodified J3D models are
    // kept by vertex-data generation, without per-frame byte validation.
    static gx::StaticCompactCache staticCompact;
    // Prefer GPU-readable linear memory so cached vertices are bound directly.
    for(const std::size_t bytes:{std::size_t(2)<<20,std::size_t(1)<<20})
        if(attachResidentStaticGeometry(staticCompact,bytes)){
            gx::setStaticCompactCache(&staticCompact);
            OSReport("Native static geometry cache resident bytes=%u\n",unsigned(bytes));break;
        }
    if(!gx::staticCompactCache())
        for(const std::size_t bytes:{std::size_t(3)<<20,std::size_t(2)<<20,std::size_t(1)<<20})
            if(auto* pool=static_cast<u8*>(std::malloc(bytes))){
                staticCompact.attach({pool,bytes});gx::setStaticCompactCache(&staticCompact);
                OSReport("Native static geometry cache copied bytes=%u\n",unsigned(bytes));break;
            }
    drawContext.allowBehindCameraCull=true;
    OSReport("Native original GX policy: fully behind-camera triangles culled with clipping disabled; Tier D counted\n");
    drawContext.allowRgba6Storage=true;
    drawContext.allowQuantizedConstants=true;
    drawContext.allowVertexFog=true;
    drawContext.allowTriangleFog=true;
    drawContext.allowAlphaUnion=true;
    OSReport("Native original GX policy: opaque LEQUAL triangle-interleaved fog fallback; Tier D extra RGB store counted\n");
    drawContext.allowVertexIndirect=true;
    drawContext.allowConstantEarlyDepth=true;
    drawContext.allowDegradedDraws=true;
    drawContext.allowProjectiveDivisorFallback=true;
    drawContext.allowAlphaCompareFallback=true;
    drawContext.allowIndirectDivisorFallback=true;
    drawContext.preferFogRamp=true;
    {auto* off=options.open("fog-alpha-ramp-off.txt");
     drawContext.preferFogRampForAlpha=!off;if(off)std::fclose(off);}
    OSReport("Native fog curves other than the linear one on materials that leave the vertex alpha unread: %s\n",drawContext.preferFogRampForAlpha?
        "ramp texture (GPU vertex route)":"primary alpha (CPU vertex route; SD switch fog-alpha-ramp-off.txt)");
    // Strict rejection (the game ends on the first unsupported draw) only on request.
    {auto* strict=options.open("strict-draws.txt");
     drawContext.skipUnsupportedDraws=!strict;if(strict)std::fclose(strict);}
    gx::commandSkipUnavailableTextures=drawContext.skipUnsupportedDraws;
    OSReport("Native original GX policy: unsupported draws are %s\n",drawContext.skipUnsupportedDraws?
        "left out and counted (rejected); strict-draws.txt on the SD restores the fatal rejection":"fatal (strict-draws.txt)");
    OSReport("Native original GX policy: visible nonpositive projected divisor clamped; lower-bound dual alpha keeps one test; Tier D counted\n");
    OSReport("Native original GX policy: classified perspective TEV/clipping approximations counted; resource/protocol/submission failures remain fatal; UI strict\n");
    OSReport("Native original GX policy: vertex depth/range fog plus final RGB TEV; Tier D counted\n");
    OSReport("Native original GX policy: nominal TEV fractions use nearest UNORM8; Tier D counted\n");
    OSReport("Native original GX policy: RGBA6 uses actual RGBA8 storage; 6-bit quantization pending\n");
    drawContext.reportFailure=[](const gx::CommandDraw& draw,const char* why){
        if(std::string_view(why).starts_with("GX visual fallback: ")){
            OSReport("Native degraded draw: %s\n",why);return;
        }
        reportCameraAlignedProof(drawContext);
        auto* player=dComIfGp_getPlayer(0);
        OSReport("Native rejected draw world: stage=%.8s room=%d player=%p\n",dComIfGp_getStartStageName(),dComIfGp_getStartStageRoomNo(),player);
        OSReport("Native draw rejected: %s primitive=%u vertices=%u\n",why,unsigned(draw.primitive),unsigned(draw.vertexCount));
        // Inside a J3D display list the current model is live: name it.
        if(gx::staticGeometryHint.list)if(auto* model=j3dSys.getModel())if(const auto* data=model->getModelData()){
            auto* joints=data->getJointName();auto* materials=data->getMaterialName();
            auto* packet=j3dSys.getMatPacket();auto* material=packet?packet->getMaterial():nullptr;
            const unsigned index=material?material->getIndex():0xffff;
            OSReport("Native rejected draw model: joints=%u envelopes=%u shapes=%u joint0=%s material=%u/%s\n",unsigned(data->getJointNum()),
                unsigned(data->getWEvlpMtxNum()),unsigned(data->getShapeNum()),joints?joints->getName(0):"?",index,
                materials&&index<data->getMaterialNum()?materials->getName(u16(index)):"?");
        }
        OSReport("Native rejected XF clip valid=%u value=%lu\n",unsigned(draw.registers.xfValid[0x1005]),static_cast<unsigned long>(draw.registers.xf[0x1005]));
        for(unsigned reg=0x1009;reg<=0x1011;++reg)
            OSReport("Native rejected XF %04x valid=%u value=%08lx\n",reg,unsigned(draw.registers.xfValid[reg]),static_cast<unsigned long>(draw.registers.xf[reg]));
        gx::VertexTransformPlan transform;
        const bool reportTransform=gx::planVertexTransform(draw.registers,0,transform);
        if(reportTransform&&drawContext.allowCameraAlignedIndirectQ){
            const auto& p=transform.clipProjection();
            for(unsigned row=0;row<4;++row)
                OSReport("CAMQ_PROOF projection row=%u bits=%08lx,%08lx,%08lx,%08lx\n",row,
                    static_cast<unsigned long>(std::bit_cast<std::uint32_t>(p[row*4])),
                    static_cast<unsigned long>(std::bit_cast<std::uint32_t>(p[row*4+1])),
                    static_cast<unsigned long>(std::bit_cast<std::uint32_t>(p[row*4+2])),
                    static_cast<unsigned long>(std::bit_cast<std::uint32_t>(p[row*4+3])));
            const auto& e=gx::currentTargetExtent;
            OSReport("CAMQ_PROOF target logical=%u,%u pixel=%u,%u\n",e.logicalWidth,e.logicalHeight,e.pixelWidth,e.pixelHeight);
        }
        if(reportTransform)for(unsigned i=0;i<draw.vertexCount&&i<16;++i){
            gx::DecodedVertex input;gx::TransformedVertex vertex;
            if(draw.vertex(i,input)&&gx::transformVertex(transform,input,vertex))
                OSReport("Native rejected clip %u: %.6f %.6f %.6f %.6f\n",i,vertex.clip[0],vertex.clip[1],vertex.clip[2],vertex.clip[3]);
            if(draw.vertex(i,input))OSReport("Native rejected color %u: present=%lx %.6f %.6f %.6f %.6f\n",i,static_cast<unsigned long>(input.present),input.color[0][0],input.color[0][1],input.color[0][2],input.color[0][3]);
        }
        gx::MaterialState state;
        if(gx::decodeMaterial(draw.registers,state)){
            OSReport("Native material counts %u %u %u %u masks %u %u %u\n",state.stageCount,state.texgenCount,state.channelCount,state.indirectCount,state.colorValid,state.konstValid,state.swapValid);
            for(unsigned i=0;i<4;++i){
                const auto& c=state.colors[i];const auto& k=state.konst[i];const auto& w=state.swaps[i];
                OSReport("Native material register %u color %d %d %d %d konst %u %u %u %u swap %u %u %u %u\n",i,c[0],c[1],c[2],c[3],k[0],k[1],k[2],k[3],w[0],w[1],w[2],w[3]);
            }
        }
        for(unsigned reg=0;reg<256;++reg)if(draw.registers.bpValid[reg])
            OSReport("Native raster BP %02x valid=%u value=%06lx\n",reg,unsigned(draw.registers.bpValid[reg]),static_cast<unsigned long>(draw.registers.bp[reg]));
    };
    processor.setSink(drawGxCommand,&drawContext);
    const bool automaticSu77=readAutomaticSuConfig77();
    if(!stream.configureAutomaticSu(automaticSu77))fatal("Original GX automatic SU77 configuration failed");
    OSReport("Native automatic SU77: %s; typed SDK updates, raw display-list writes retained\n",
        automaticSu77?"enabled":"disabled");
    if(!gx::setCommandStream(&stream))fatal("Original game GX stream installation failed");
    constexpr unsigned arenaBytes=48*1024*1024;
    void* arena=memalign(32,arenaBytes);
    if(!arena||!initializeClock()||!configureGameArena(arena,arenaBytes))fatal("Original game arena initialization failed");
    logText("Original native startup: disc validation\n");
    const auto discBegin = svcGetSystemTick();
    if(!initializeOriginalDisc())fatal("Original startup needs validated sdmc:/3ds/DuskLight/zelda.twilight");
    const auto discReady = svcGetSystemTick();
    logText("Original native startup: disc ready; game state\n");
    readBootStage();loadDeveloperScripts();runDeveloperCalibration();setDeveloperDumpRegion(arena,arenaBytes);
    dusk::version::init();OSInit();
    mDoMain::sPowerOnTime=OSGetSystemTime();
    static mDoRstData resetData{};
    mDoRst::setResetData(&resetData);mDoRst::offReset();mDoRst::setLogoScnFlag(0);
    dComIfG_ct();mDoDvdThd::SyncWidthSound=false;
    aptHookCookie hook{};aptHook(&hook,onGameApt,nullptr);
    mDoMain::developmentMode=0;
    // The DVD thread (archive reads and decompression) goes to the second
    // core unless loader-core0.txt asks for the old place (mDoDvdThd::create).
    if(auto* file=options.open("loader-core0.txt")){std::fclose(file);duskNativeLoaderOnSecondCore=false;}
    {bool holdOff=false;if(auto* file=options.open("transition-hold-off.txt")){std::fclose(file);holdOff=true;}
     initializeSceneTransition(!holdOff);}
    OSReport("Original native startup: machine\n");
    const auto machineBegin = svcGetSystemTick();
    if(!mDoMch_Create())fatal("Original machine initialization failed");
    const auto machineReady = svcGetSystemTick();
    OSReport("Original native startup: graphics\n");
    if(!mDoGph_Create())fatal("Original graphics initialization failed");
    const auto graphicsReady = svcGetSystemTick();
    // JFWSystem owns PAD initialization and mDoCPd owns the gameplay JUT pad.
    // The diagnostic pad requires an existing JKR heap and must not be created
    // before mDoMch_Create or sampled in parallel with the original controller.
    if(!PADInit())fatal("Original PAD services are unavailable");
    mDoCPd_c::create();
    mDoDvdThd_callback_c::create((mDoDvdThd_callback_func)LOAD_COPYDATE,nullptr);
    fapGm_Create();fopAcM_initManager();cDyl_InitAsync();
    g_mDoAud_audioHeap=JKRCreateSolidHeap(0x14D800*2,JKRGetCurrentHeap(),false);
    if(!g_mDoAud_audioHeap)fatal("Original audio heap allocation failed");
    dusk::game_clock::initialize();
    std::uint64_t frame=0;
    std::uint64_t ordinaryFrames=0,ordinaryExecutionTicks=0,ordinaryLoopTicks=0;
    // Ordinary play keeps detailed profiling, the periodic report and SD writes
    // out of the frame loop; developer runs retain them (see developer_script).
    const bool detailedDiagnostics=developerDiagnosticsRequested();

    // Console A/B switches. Azahar models neither CPU caches nor GPU time.
    // The whole-heap frame-end flush is Citro3D's default and the only one
    // proven on a console; flush-ranges-only.txt keeps the cheaper A/B path.
    const bool flushAllLinear=!options.present("flush-ranges-only.txt");
    const bool gpuChunksOff=options.present("gpu-chunks-off.txt");
    const bool stackedWaits=options.present("stacked-waits.txt");
    // Draws per incremental submission. 64 (24 until Alpha 7): each one is a
    // GSP cache flush request, a command-list split and a queue start, and the
    // PICA keeps up (Alpha 6 console: 0.59 ms GPU tail after the last part).
    // The part after the painter overlaps the actor update in any case.
    // sdmc:/3ds/DuskLight/gpu-chunk-draws.txt holds another count (A/B).
    unsigned chunkDraws=64;
    if(auto* file=options.open("gpu-chunk-draws.txt")){
        unsigned value=0;if(std::fscanf(file,"%u",&value)==1&&value>0&&value<=4096)chunkDraws=value;
        std::fclose(file);
    }
    // Alpha 16: the first part of a picture holds 8 draws, then 16, 32, 64.
    // The console's PICA idled 7-9 ms at the start of every picture of the
    // owner's Alpha 15 dumps (the clear and the sky waited for 64 prepared
    // draws). gpu-first-part.txt holds another count; 0 restores Alpha 15.
    unsigned firstPart=8;
    if(auto* file=options.open("gpu-first-part.txt")){
        unsigned value=0;if(std::fscanf(file,"%u",&value)==1&&value<=4096)firstPart=value;
        std::fclose(file);
    }
    platformSetGpuChunkDraws(gpuChunksOff?0:chunkDraws);
    platformSetGpuFirstPart(firstPart);
    if(!gpuChunksOff)OSReport("Native GPU submission part: %u draws%s; first part of a picture %u draws%s\n",chunkDraws,chunkDraws!=64?" (SD switch)":"",
        firstPart&&firstPart<chunkDraws?firstPart:chunkDraws,firstPart!=8?" (SD switch)":"");
    // The performance improvements of this session (perf_features.hpp).
    if(auto* file=options.open("features.txt")){
        char words[160]{};(void)std::fread(words,1,sizeof(words)-1,file);std::fclose(file);
        const int refused=features::parse(words);
        if(refused)OSReport("Native features.txt: %d words not understood\n",refused);
    }
    OSReport("Native improvements: %s%s\n",features::text(),options.present("features.txt")?" (SD switch features.txt)":"");
    // Levels of detail of the models (switches lod and shadow).
    model_lod::open();
    // Scripted measurements (measured_dump.hpp).
    if(auto* file=options.open("measure-at.txt")){
        unsigned long long value=0;
        while(measureTriggerCount<measureTriggers.size()&&std::fscanf(file,"%llu",&value)==1){
            const int next=std::fgetc(file);
            unsigned without=0;
            if(next=='-'){
                char name[16]{};
                if(std::fscanf(file,"%15[a-z]",name)!=1||!(without=features::maskOf(name)))
                    fatal("Invalid measure-at.txt; after '-' comes the name of an improvement (perf_features.hpp)");
            }
            measureTriggers[measureTriggerCount++]={value,next=='r',without,false};
            if(next!='r'&&next!='-'&&next!=EOF)std::ungetc(next,file);
        }
        std::fclose(file);
        OSReport("Native scripted measurements: %u (measure-at.txt), written to measure.txt\n",measureTriggerCount);
    }
    // GPU time diagnostics (gpu_sync.hpp): per painter phase, or one draw per
    // command list for a dump that times every draw.
    {const bool phases=options.present("gpu-phases.txt"),drawTimes=options.present("gpu-draw-times.txt");
     measured::setBaseDiagnostics(phases,drawTimes);
     platformSetGpuDiagnostics(phases,drawTimes);
     if(auto* file=options.open("shadow-profile.txt")){
         const bool runs=std::fgetc(file)=='2';
         std::fclose(file);
         gpuShadowProfileMode=runs?GpuShadowProfileMode::runs:GpuShadowProfileMode::pass;
         OSReport("Native shadow GPU profiling: %s\n",runs?"atlas and mixed-caster run boundaries":"atlas boundaries");
     }
     const bool cullOff=options.present("gpu-cull-off.txt");
     const bool cullRefine=options.present("gpu-cull-refine.txt");
     setGxCulling(!cullOff,cullRefine);
     OSReport("Native off-screen culling of resident draws: %s\n",
        cullOff?"off (SD switch)":cullRefine?"on (refined, SD switch)":"on (blocks of 64 triangles)");
     if(phases||drawTimes)OSReport("Native GPU time diagnostics: %s (SD switch; command lists split %s)\n",
        drawTimes?"one draw per command list, painter phases":"painter phases",drawTimes?"before every draw":"at every painter phase");}
    const bool kernelFlush=options.present("svc-flush.txt");
    setGxGeometryKernelFlush(kernelFlush);
    OSReport("Native geometry publication: %s\n",kernelFlush?"kernel cache flush (SD switch)":"GSP cache flush request");
    const bool gameFlush=options.present("game-cache-flush.txt");
    setGameCacheFlush(gameFlush);
    OSReport("Native game cache flushes: %s\n",gameFlush?"forwarded to the kernel (SD switch)":"not forwarded");
    const bool nativeChainOff=options.present("native-chain-off.txt");
    setNativeShapeChain(!nativeChainOff);
    OSReport("Native shape replay continuation: %s\n",nativeChainOff?"off (SD switch)":"on");
    OSReport("Native frame pacing: %s; GPU submission: %s; frame-end flush: %s\n",
        stackedWaits?"legacy stacked retrace waits (SD switch)":"single display deadline",
        gpuChunksOff?"whole frame at frame end (SD switch)":"incremental, in parts of N draws and after the painter",
        flushAllLinear?"complete linear heap":"command list and written ranges (SD switch)");
    // Game speed: 30 logic ticks per second of real time (see LogicPace).
    // real-speed-off.txt restores one tick per picture (Alpha 0, slow motion);
    // draw-every-tick.txt also enters the draw lists on the extra ticks.
    const bool realSpeed=!options.present("real-speed-off.txt");
    const bool drawEveryTick=options.present("draw-every-tick.txt");
    duskNativeCameraLog=options.present("camera-log.txt");
    duskNativeTickLogic=!options.present("tick-logic-off.txt");
    OSReport("Native game speed: %s%s\n",realSpeed?"30 logic ticks per second of real time, up to 4 per picture":
        "one logic tick per picture (SD switch real-speed-off.txt)",drawEveryTick?"; draw lists on every tick (SD switch)":"");
    // Picture rate lock, first step. The game speed does not depend on the
    // picture rate (LogicPace). picture-rate.txt holds pictures per second:
    // the start of a picture is held to that whole number of display
    // refreshes after the previous start. Incomplete: a picture is presented
    // when its painting ends, so the rate on screen still follows the load. A
    // steady lock needs the wait before the presentation and a rate that is
    // a whole number of logic ticks (30, 15, 10). Off without the file.
    unsigned pictureRefreshes=0;
    if(auto* file=options.open("picture-rate.txt")){
        unsigned rate=0;
        if(std::fscanf(file,"%u",&rate)==1&&rate>=5&&rate<=30)pictureRefreshes=(60+rate/2)/rate;
        std::fclose(file);
    }
    if(pictureRefreshes>1)OSReport("Native picture rate: locked to one picture every %u display refreshes (%.1f per second; SD file picture-rate.txt)\n",
        pictureRefreshes,59.834/pictureRefreshes);
    else OSReport("Native picture rate: unlocked (as fast as the console paints, at most the game's 30 per second)\n");
    // 59.834 refreshes per second on the top screen.
    const std::uint64_t refreshTicks=std::uint64_t(nativeTickFrequency*1000ull/59834ull);
    std::uint64_t picturePaced=0;
    OSReport("Native scene logic (collisions, moving backgrounds, particles, frame counter): %s\n",
        duskNativeTickLogic?"every logic tick":"once per picture (SD switch tick-logic-off.txt, as Alpha 7)");
    OSReport("Native GX syncs: %s\n",nativeGxSyncWaits()?"wait for the whole GPU (SD switch gx-sync-wait.txt)":
        "ordered on the GPU; only readbacks, eviction and texture re-decode wait");
    unsigned previousTicks=1;
    const auto telemetryBegin = svcGetSystemTick();
    initializeFrameTelemetry();
    initializeGpuCanary();
    const auto telemetryReady = svcGetSystemTick();
    // A model-shadow pass stopped the PICA with this build: blob shadows only.
    if(gpuCanaryShadowRefused()&&native_effects::shadows>1){native_effects::shadows=1;OSReport("Native shadows: 1 (blobs only; the GPU guard recorded a stop in the model-shadow pass)\n");}
    else OSReport("Native shadows: %d (%s)\n",native_effects::shadows,native_effects::shadows>1?"blobs and model shadows":
        native_effects::shadows?"blobs only (shadows.txt)":"off");
    // Rigid routes. The multi-texture shader has completed on a console in
    // every run and draws everything by default, its projected unit included
    // (new on 2026-11-14: the guard records a stop of that unit).
    drawContext.allowMultiProjected=!gpuCanaryMultiProjectedRefused();
    setVertexVariantsAllowed(!gpuCanaryVertexVariantsRefused());
    if(auto* off=options.open("mt-projected-off.txt")){std::fclose(off);drawContext.allowMultiProjected=false;}
    // The single-texture rigid shader stopped the console PICA in every build
    // that used it (2026-11-13). Its outputs were completed on 2026-11-14,
    // unproven on a console: it is used only when rigid-single-on.txt asks for
    // it, or as the second choice once the projected unit above was refused
    // (the CPU route is the third). A stop of it is learned and refuses it.
    gx::multiTextureWidePalette=!options.present("mt-wide-palette-off.txt");
    gx::rigidConstantFogRamp=!options.present("constant-fog-ramp-off.txt");
    // A/B: texture units generated from the normal (J3D environment maps) on
    // the multi-texture shader, or on the CPU vertex route as before.
    gx::multiTextureNormalTexgen=!options.present("mt-normal-texgen-off.txt");
    OSReport("Native multi-texture texgen from the normal: %s\n",gx::multiTextureNormalTexgen?
        "GPU (unit role -1)":"CPU vertex route (SD switch mt-normal-texgen-off.txt)");
    // Environment maps on skinned models: one texture matrix per bone
    // (gx_rigid_vertex.hpp, pairedTexgenRows).
    gx::multiTexturePairedTexgen=!options.present("mt-paired-texgen-off.txt");
    OSReport("Native multi-texture texgen from the normal with a texture matrix per bone: %s\n",gx::multiTexturePairedTexgen?
        "GPU (the normal after the bone's matrix)":"CPU vertex route (SD switch mt-paired-texgen-off.txt)");
    // A material whose texgens read three GX coordinates: wide records
    // (gx_rigid_vertex.hpp, RigidVertexPlan::wideRecord).
    gx::multiTextureThirdCoordinate=!options.present("mt-third-coordinate-off.txt");
    OSReport("Native multi-texture materials reading three coordinate sets: %s\n",gx::multiTextureThirdCoordinate?
        "GPU (36-byte records)":"CPU vertex route (SD switch mt-third-coordinate-off.txt)");
    // Indexed arrays stored in the console's byte order (gx_rigid_decode.cpp).
    gx::gxCompactSwappedArrays=!options.present("swapped-arrays-off.txt");
    OSReport("Native multi-texture draws with arrays in the console's byte order: %s\n",gx::gxCompactSwappedArrays?
        "GPU (converted while decoding)":"CPU vertex route (SD switch swapped-arrays-off.txt)");
    drawContext.allowProjectionMode=!options.present("projection-mode-off.txt");
    OSReport("Native projected textures on the CPU route: %s\n",drawContext.allowProjectionMode?
        "PICA projection mode (per-pixel divide)":"per-vertex divide (SD switch)");
    gx::vertexWarpRepeats=!options.present("warp-repeat-off.txt");
    drawContext.shrinkCompactReservations=!options.present("arena-shrink-off.txt");
    OSReport("Native compact records in the frame arena: %s\n",drawContext.shrinkCompactReservations?
        "the unused part of each reservation is returned":"whole 60-byte vertices kept (SD switch arena-shrink-off.txt)");
    // The warp of each vertex on the GPU route (gx_indirect_gpu.hpp).
    gx::vertexWarpEnabled=!options.present("vertex-warp-off.txt");
    OSReport("Native indirect warps on the GPU route: %s\n",gx::vertexWarpEnabled?
        "sampled per vertex":"one constant offset per draw (SD switch vertex-warp-off.txt)");
    // Warped water projected through the camera (gx_command_draw.hpp).
    drawContext.allowCameraDivisor=!options.present("camera-divisor-off.txt");
    OSReport("Native warped units projected through the camera: %s\n",drawContext.allowCameraDivisor?
        "divided per pixel on both sides of the camera plane":"divisor clamped per vertex (SD switch camera-divisor-off.txt)");
    // Second-core worker (runtime/gx_worker.hpp): the 3D shapes are drawn on
    // core 2 (chooseGxWorker has the switch and what a bad start teaches).
    {
        const auto choice=chooseGxWorker();
        const auto mode=choice.mode;
        if(mode!=gxworker::Mode::off){
            // Linear memory, not static storage (see the log ring in main.cpp).
            constexpr std::size_t vertexBytes=64*1024,indexCount=16*1024,hashCount=8192,remapCount=4096,logWords=2048;
            const std::size_t processorBytes=(sizeof(gx::CommandProcessor)+7)&~std::size_t(7);
            // Alpha 15 (gx-overlap.txt 1, 2): records for a whole picture.
            // Azahar village, Alpha 14: 74-92k words of bank deltas and
            // 32-49 KB of runs a picture (the 294 KiB ring filled and handed
            // the queue back, 3-29 ms a picture); 1 MiB holds twice that.
            // The records are read by the CPU only (a copied run is decoded
            // into the geometry arena before the GPU sees it): the larger
            // ring comes from the libc heap, so the linear heap the GPU needs
            // keeps its room (village: 5.7 MB free in Alpha 14).
            gxOverlapLevel=readGxOverlapLevel();
            std::size_t bytes=gxOverlapLevel?2*sizeof(gx::CommandRegisters)+1024*1024:gxworker::Queue::minimumStorage;
            void* storage=gxOverlapLevel?memalign(32,bytes):linearMemAlign(bytes,8);
            bool linearStorage=!gxOverlapLevel;
            if(!storage&&bytes!=gxworker::Queue::minimumStorage){
                OSReport("Native GX worker: no heap memory for 1 MiB of records; the Alpha 14 size\n");
                bytes=gxworker::Queue::minimumStorage;storage=linearMemAlign(bytes,8);linearStorage=true;
            }
            auto* block=static_cast<std::uint8_t*>(linearMemAlign(processorBytes+vertexBytes+2*(indexCount+hashCount+remapCount)+4*logWords,8));
            if(!storage||!block){
                OSReport("Native GX worker: no linear memory for its records (%lu bytes free); off\n",static_cast<unsigned long>(linearSpaceFree()));
                if(storage){if(linearStorage)linearFree(storage);else std::free(storage);}
                if(block)linearFree(block);
            }else{
                // The processor queued display lists run on: the game
                // processor's memory registry, resolver, topology cache and
                // sink, its own batch scratch. The game processor waits for
                // the worker before it prepares a draw of its own (both use
                // that cache and everything below the sink).
                auto* fallback=new(block)gx::CommandProcessor(memory);
                processor.setDrawBarrier(syncGxWorkerForDraw);
                // Its runs of primitives may go to the queue as well, while
                // the painter walks its 3D lists (gx_command_draw.cpp). Code
                // and constant data end with the unwind index.
                processor.setImmutableMemory(__start__,__exidx_end);
                processor.setRunDeferral(deferNativeRun,&processor);
                auto* at=block+processorBytes;
                auto* vertices=at;at+=vertexBytes;
                auto* indices=reinterpret_cast<u16*>(at);at+=2*indexCount;
                auto* hash=reinterpret_cast<u16*>(at);at+=2*hashCount;
                auto* remap=reinterpret_cast<u16*>(at);at+=2*remapCount;
                // The game processor logs its stores into the bank for the
                // worker's copy (gx_bank_delta.hpp).
                stream.attachBankLog({reinterpret_cast<std::uint32_t*>(at),logWords});
                fallback->setLiveArrayResolver(liveGameArray);
                fallback->setPrimitiveBatchScratch({{vertices,vertexBytes},{indices,indexCount},4096,{hash,hashCount},{remap,remapCount}});
                fallback->setPrimitiveBatchCache(&topology);
                fallback->setSink(drawGxCommand,&drawContext);
                setNativeListProcessor(fallback);
                // Alpha 14: less of the queue handed back to the game thread
                // (gx_worker_sync.hpp), each with its SD switch; set before
                // the worker thread starts.
                const bool lateRuns=!options.present("gx-late-runs-off.txt");
                const bool sections=!options.present("gx-sections-off.txt");
                const bool bloomJobs=!options.present("gx-bloom-job-off.txt");
                const bool armJobs=!options.present("gx-shadow-arm-job-off.txt");
                const bool paletteFast=!options.present("gx-palette-fast-off.txt");
                const bool measured=!options.present("gx-rest-measured-off.txt");
                setNativeLateRuns(lateRuns);setNativeSections(sections);setGxBloomJobs(bloomJobs);setGxShadowArmJobs(armJobs);
                setCopyPaletteFastPath(paletteFast);setGxWorkerMeasuredRests(measured);
                if(!startGxWorker(mode,choice.verify,nativeWorkerAdapter(),storage,bytes))
                    OSReport("Native GX worker: not started; every draw stays on the game thread\n");
                else{
                    gxWorkerJobHook=queueNativeJob;
                    if(sections)gxWorkerSectionHook=nativeSection;
                    gxworker::queue().setPartialDrain(gxOverlapLevel>=1);
                    setTextureRetainWithoutOwner(gxOverlapLevel>=1);
                    setScreenTapeQueuedPlain(gxOverlapLevel>=1);
                    const bool overlapOn=gxOverlapLevel>=2&&gxworker::queue().mode()==gxworker::Mode::threaded;
                    setNativeLate2D(overlapOn);
                    gxOverlapSyncHook=overlapOn?gxOverlapSync:nullptr;
                    OSReport("Native GX overlap: level %u (SD switch gx-overlap.txt: 0 Alpha 14, 1 within the painter, 2 into the next logic tick); "
                        "%lu KiB of records (%s heap); full ring %s; painter's end %s\n",gxOverlapLevel,static_cast<unsigned long>(bytes/1024),
                        linearStorage?"linear":"libc",
                        gxOverlapLevel>=1?"draws until the record fits":"hands the queue back",
                        gxOverlapLevel>=2?"without a sync, 2D passes queued":"syncs");
                    OSReport("Native GX worker jobs: late runs %s, refraction sections %s, bloom %s, shadow start %s, palette check %s; rests %s "
                        "(SD switches gx-late-runs-off, gx-sections-off, gx-bloom-job-off, gx-shadow-arm-job-off, gx-palette-fast-off, gx-rest-measured-off)\n",
                        lateRuns?"on":"off",sections?"on":"off",bloomJobs?"queued":"synced",armJobs?"queued":"synced",paletteFast?"without the GPU":"with the GPU",
                        measured?"measured":"by share");
                }
            }
        }
    }
    const bool singleAsked=options.present("rigid-single-on.txt");
    drawContext.allowSingleRigidShader=(singleAsked||!drawContext.allowMultiProjected)&&!gpuCanaryRigidSingleRefused()&&
        !options.present("rigid-single-off.txt");
    OSReport("Native rigid shaders: multi-texture palette %s; projected unit %s; single-texture shader %s\n",
        gx::multiTextureWidePalette?"up to the rows of unused units":"below row 81 (SD switch)",drawContext.allowMultiProjected?"on":"off",
        drawContext.allowSingleRigidShader?(singleAsked?"on (SD switch rigid-single-on.txt)":"on (second choice after the projected unit was refused)"):
        "off (the multi-texture shader or the CPU route draws its share)");
    std::uint64_t telemetryPaint=0,telemetryUpdate=0,telemetryDraw=0,telemetryDrawn=0;
    u32 telemetryAudio=nativeAudioStatus().mixTicks;
    std::uint64_t consolePaintTicks=0,consoleUpdateTicks=0,consoleGpuWaitTicks=0;
    std::uint32_t consoleDraws=0,consoleGameFree=0,consoleLinearFree=0;
    auto tagBenchmarkInput=[&](std::uint64_t previousFrame){
        BenchmarkInputSite site;
        site.zone=devConsoleZoneLoadout().zone;
        site.previousFrame=previousFrame;
        if(site.zone){
            const auto story=devConsoleStoryContext();
            site.storyStep=story.step;site.storyPhase=story.phase;site.storyTick=story.tick;
            site.room=dComIfGp_roomControl_getStayNo();
            std::snprintf(site.stage,sizeof(site.stage),"%.8s",dComIfGp_getStartStageName());
        }
        setBenchmarkInputSite(site);
    };
    BenchmarkInputSite deferredEdge;
    bool haveDeferredEdge{};
    auto drainBenchmarkInput=[&](const FrameTelemetryInput& sample,bool completed){
        if(!haveDeferredEdge&&!takeBenchmarkInputEdge(deferredEdge))return;
        haveDeferredEdge=true;
        const auto context=devConsoleZoneCaptureContext();
        float fps{};int speed{};
        if(completed)devConsoleRates(fps,speed);
        // A press detected after this submission belongs to a later picture.
        // Bound the batch even if the scanner is still supplying new edges.
        for(unsigned n=0;n<33&&haveDeferredEdge;++n){
            if(completed&&!benchmarkInputReady(deferredEdge,sample.submitTick))return;
            captureBenchmarkPicture(sample,fps,speed,context,context.loadout.tour,
                developerSession?loadoutText:"none (normal play)",BenchmarkCaptureSource::manual,&deferredEdge,completed);
            haveDeferredEdge=n+1<33&&takeBenchmarkInputEdge(deferredEdge);
        }
    };
    const auto startupMs = [](u64 first, u64 last) { return double(last-first)*1000.0/SYSCLOCK_ARM11; };
    OSReport("Native startup ms: cpu=%.2f gx-arena=%.2f disc=%.2f game-state=%.2f machine=%.2f graphics=%.2f pad-audio-config=%.2f telemetry-guard=%.2f renderer-worker=%.2f\n",
        startupMs(startupBegin,cpuReady),startupMs(cpuReady,discBegin),startupMs(discBegin,discReady),
        startupMs(discReady,machineBegin),startupMs(machineBegin,machineReady),startupMs(machineReady,graphicsReady),
        startupMs(graphicsReady,telemetryBegin),startupMs(telemetryBegin,telemetryReady),startupMs(telemetryReady,svcGetSystemTick()));
    try{
        while(aptMainLoop()){
            const auto loopStart=gxProfileTick();
            // Developer runs and the test's profile phase take the sparse detailed profile.
            // A measurement (measured_dump.hpp) owns the timers while it runs: the
            // diagnostic mode's sparse profile and its report wait for it.
            const bool detailFrames=(detailedDiagnostics||devConsoleProfiling())&&!measured::active();
            gxSparseVertexProfiling=(detailFrames&&frame%30==29)||measured::sectionTimers();
            gxProcessProfiling=measured::processTimers();
            tagBenchmarkInput(frame);
            scanNativeGameInput();
            // Scripted input is merged after physical capture ownership.
            const auto& physical=nativeGameInput();
            u32 heldKeys=physical.held;
            {std::int16_t sx{},sy{};applyScriptedInput(heldKeys,sx,sy);}
            updateApplicationDumpInput(heldKeys);
            // START and SELECT stay with the game and the console: START+SELECT
            // toggles the automatic test; the EXIT tab leaves the game.
            if(!applicationDumpInputSuppressed())updateDevConsoleInput(heldKeys,physical.down);
            tagBenchmarkInput(frame); // START+SELECT can change benchmark ownership.
            if(devConsoleExitRequested())break;
            // The automatic test finished: the game stands still on that
            // screen until an input or its power-off (dev_console.hpp).
            if(devConsoleHolding()){
                finishTourVisualPicture();
                finishBenchmarkPicture();
                if(developerExitRequested(frame)){OSReport("Native developer exit after frame %llu\n",static_cast<unsigned long long>(frame));break;}
                serviceDevConsoleHold();
                if(devConsoleExitRequested())break;
                gspWaitForVBlank();
                resetTiming=true;
                // The frame count goes on (one per display refresh): scripted
                // developer input and the developer exit are told by it.
                ++frame;setDeveloperFrame(frame);
                continue;
            }
            if(developerExitRequested(frame)){OSReport("Native developer exit after frame %llu\n",static_cast<unsigned long long>(frame));break;}
            // Reported frame numbers start at 1 after the first completed frame.
            setDeveloperFrame(frame+1);
            if(gpuCanaryAutoAdvance()&&dComIfGp_getStartStageName()[0])gpuCanaryWorldEntered();
            if(copyprobe::enabled)copyprobe::setFrame(frame+1);
            // The game's display wait (JFWDisplay::beginRender) is the single
            // pacing point of a frame. Wait here only if the previous iteration
            // never reached it, so an unpaced state cannot spin.
            if(stackedWaits||!consumeFramePaced())VIWaitForRetrace();
            // Picture rate lock: this picture starts no sooner than
            // pictureRefreshes refreshes after the one before (half a refresh
            // of tolerance, so a picture that is on time never waits one more).
            if(pictureRefreshes>1){
                for(unsigned waited=0;waited<pictureRefreshes&&picturePaced&&
                    gxProfileTick()-picturePaced+refreshTicks/2<pictureRefreshes*refreshTicks;++waited)gspWaitForVBlank();
                picturePaced=gxProfileTick();
            }
            // After HOME or sleep the speedup was requested again: prove it held.
            if(cpuModeCheck){cpuModeCheck=false;reportCpuMode(false);}
            if(resetTiming){dusk::game_clock::reset();logicPace.reset();resetTiming=false;}
            // Logic ticks this picture carries; the first paints the previous
            // picture's lists, the last enters this picture's lists.
            // Behind the curtain (scene_transition.hpp) one tick per picture:
            // the hidden pictures warm what the first visible ones draw.
            const unsigned ticks=transitionWarming()?1:realSpeed?logicPace.due(loopStart):1;
            duskNativePaintTicks=float(previousTicks);duskNativeDrawTicks=drawEveryTick?1.0f:float(ticks);
            const auto timing=dusk::game_clock::advance();
            if(!platformBeginGpuFrame(stackedWaits?C3D_FRAME_SYNCDRAW:0))fatal("Original game GPU frame admission failed");
            if(gpuListCaptureBoundary(frame+1))flagNextFrameTelemetry(telemetryDiagnosticFrame|telemetryCaptureOwner);
            finishTourVisualPicture(true);
            finishBenchmarkPicture(true);
            finishDeveloperRetainedDepth(true); // Before any new pre/depth capture.
            if(!collectTextureResources())fatal(textureResourceFailure());
            // Native static geometry: each new play scene empties the pool.
            // dScnPly phase_1 gives every new scene (a reload of the same
            // stage too) a new process id once the previous scene and its
            // actors are deleted; beginGxGeometryFrame applies the clear
            // behind the loading fade. In Alpha 6 the pool instead filled
            // across stages and reset in play (54 of 93 zones), dropping
            // every native record at once.
            {static fpc_ProcID poolScene=dStage_roomControl_c::getProcID();
             const fpc_ProcID scene=dStage_roomControl_c::getProcID();const bool stageChanged=scene!=poolScene;
             if(stageChanged){poolScene=scene;if(auto* sc=gx::staticCompactCache())sc->clearAtStageChange();}
             // Levels of detail of the deleted scene's models: retired now,
             // freed at the next picture (the lists entered last still
             // point at them while this picture's first tick paints).
             model_lod::frameBegin(stageChanged);}
            if(!beginGxGeometryFrame())fatal(gxGeometryFailure());
            // The previous picture's GPU work is complete here: the retained
            // screens may start their geometry buffer again.
            beginScreenTapeFrame();
            if(!beginFramebufferFrame())fatal("Original game framebuffer begin failed");
#if defined(DUSK_PICA_GAME_DRAW)
            // platformBeginGpuFrame has completed prior shared GPU work before
            // either renderer may recycle its resident/retired allocations.
            if(!dusk::pica::game::beginFrame())fatal(dusk::pica::game::failure());
            invalidatePicaCommandState();
#endif
            if(developerDepthAuditRequested(frame+1))captureDeveloperDepthBoundary(false);
            mDoGph_gInf_c::updateRenderSize();
            const auto workerBefore=nativeAudioStatus();
            const auto profileBefore=gxProfileTotals;const auto executionStart=gxProfileTick();
            gxSparseVertexProfiling=(detailFrames&&frame%30==29)||measured::sectionTimers();resetGxNamedProfiles();
            native_effects::emitterReport=gxSparseVertexProfiling;native_effects::emitterRowCount=0;
            cacheMaintenanceProfile={};cacheMaintenanceProfile.active=gxSparseVertexProfiling;
            // One diagnostic frame only; exclude this traced frame from timing
            // comparisons. All ordinary frames keep concise aggregate profiles.
            gxVertexLayoutTracing=frame==359;
            drawContext.cpuVertexReasons={};
            std::uint64_t audioTicks=0;
            const auto* chapterStep = devConsoleStoryContext().definition;
            const bool tickChapterPad = chapterStep && story::usesTickChapterPad(*chapterStep);
            DevGameStatus chapterPadStatus;
            setNativePainterOpen(true);
            for(unsigned tick=0;tick<ticks;++tick){
                duskNativeTickPaint=tick==0;duskNativeTickDraw=drawEveryTick||tick+1==ticks;
                ++duskNativeLogicTick;
                // Physical input is latched per picture. The two chapter
                // controllers can react to the preceding simulation tick.
                devConsolePadTick(tick, tickChapterPad && tick != 0 ? &chapterPadStatus : nullptr);
                dusk::game_clock::begin_sim_tick();mDoCPd_c::read();
                if(mDoCPd_c::getTrig(PAD_1))OSReport("Original native PAD trigger=%04lx frame=%llu\n",
                    static_cast<unsigned long>(mDoCPd_c::getTrig(PAD_1)),static_cast<unsigned long long>(frame));
                fapGm_Execute();
                if (chapterStep) chapterWatch.observe(*chapterStep);
                if (tickChapterPad && tick + 1 < ticks) {
                    chapterPadStatus = {};
                    if (auto* link = dComIfGp_getLinkPlayer(); link && chapterWatch.step == chapterStep &&
                        story::sameArea(chapterStep->start,
                            {dComIfGp_getStartStageName(), dComIfGp_roomControl_getStayNo()})) {
                        const auto& pos = link->current.pos;
                        chapterPadStatus.position[0] = pos.x;
                        chapterPadStatus.position[2] = pos.z;
                        observeStoryPad(chapterPadStatus, dComIfGp_getCamera(0));
                        chapterPadStatus.chapterControl = chapterWatch.control;
                        chapterPadStatus.chapterEvidence = chapterWatch.proof.evidence;
                    }
                }
                const auto audioStart=gxProfileTick();mDoAud_Execute();audioTicks+=gxProfileTick()-audioStart;
                dusk::game_clock::commit_sim_tick();
            }
            devConsolePadTick(0);
            duskNativeTickPaint=duskNativeTickDraw=true;
            // The next paint shows this picture: it follows the previous paint by these ticks.
            previousTicks=ticks;
            const auto executionTicks=gxProfileTick()-executionStart;
            // The picture is closed by this thread alone. The painter ends
            // with a sync (duskNativePainterDone) or, with the overlap, the
            // worker drew on during the logic tick: this one takes what it
            // has not drawn yet (a shape some code issued after the painter
            // too).
            gxworker::sync(gxworker::SyncCause::frameEnd);
            gx::compactTextureRegistry();
            if(!stream.finish())fatal("Original game left an incomplete GX command");
            if(!endGxGeometryFrame())fatal(gxGeometryFailure());
            if(!endFramebufferFrame())fatal("Original game left an offscreen target active");
            if(developerDepthAuditRequested(frame+1))captureDeveloperDepthBoundary(true);
            updateVideoPresentation();
            if(developerCaptureRequested(frame+1))OSReport("Native developer capture frame=%llu %s\n",
                static_cast<unsigned long long>(frame+1),captureMainFramebuffer(frame+1)?"written":"FAILED");
            if(copyprobe::enabled&&developerCaptureRequested(frame+1))copyprobe::writeReport(false);
            observeDeveloperRetainedDepth(RetainedDepthStage::afterColor);
            if(!alphaIntervalCopySafe())fatal("Alpha interval unsafe FrameEnd");
            // One final PICA split plus the SDK's three possible LCD transfers.
            if(platformReserveGpuEntries(4,500000000)!=GpuCompletion::complete)
                fatal("Original game GPU presentation queue unavailable");
            if(devConsoleTourVisualActive())noteTourVisualTarget();
#if defined(DUSK_PICA_GAME_DRAW)
            if(!dusk::pica::game::endFrame())fatal(dusk::pica::game::failure());
            if(!platformEndGpuFrame(GX_CMDLIST_FLUSH))fatal("Native GPU frame completion unavailable");
#else
            // Every CPU-written GPU input (geometry arena, resident caches,
            // textures) is flushed where it is written; the whole-heap flush
            // stays the default until a console run confirms the cheaper path.
            if(!platformEndGpuFrame(flushAllLinear?0:GX_CMDLIST_FLUSH))fatal("Native GPU frame completion unavailable");
#endif
            observeDeveloperRetainedDepth(RetainedDepthStage::afterFrameEnd);
            reportGxWorker(false);reportGxCulling();reportVertexPlans();
            ++frame;
            {
                // One RAM sample per frame; SD output only at a stage change or exit.
                auto& sample=completedPicture;
                sample={};
                sample.frame=frame;sample.submitTick=gxProfileTick();sample.loopTicks=sample.submitTick-loopStart;
                if(captureWriterOverlappedSubmission())flagNextFrameTelemetry(telemetryCaptureWriter);
                if(tourCadenceActive()){
                    sample.linearFree=consoleLinearFree;sample.gameHeapFree=consoleGameFree;
                    const bool eventRunning = dComIfGp_event_runCheck();
                    const bool timelineCamera = cutsceneCameraActive();
                    const bool actorCamera = !timelineCamera && eventRunning && playerCameraSuspended(dComIfGp_getCamera(0));
                    sample.demoFrame=timelineCamera?dDemo_c::getFrame():actorCamera?std::uint32_t(frame):0;
                    sample.sceneFlags=((timelineCamera||actorCamera)?1:0)|(eventRunning?2:0)|
                        ((dComIfGp_isEnableNextStage()||transitionHeld())?4:0);
                }
                sample.executionTicks=executionTicks;
                const auto paint=gxProfileTotals.ticks[unsigned(GxProfilePart::gamePaint)];
                const auto update=gxProfileTotals.ticks[unsigned(GxProfilePart::gameUpdate)];
                const auto draw=gxProfileTotals.ticks[unsigned(GxProfilePart::gameDraw)];
                sample.paintTicks=paint-telemetryPaint;sample.updateTicks=update-telemetryUpdate;sample.drawTicks=draw-telemetryDraw;
                telemetryPaint=paint;telemetryUpdate=update;telemetryDraw=draw;
                sample.paceTicks=takeFramePaceTicks();
                const u32 audioNow=nativeAudioStatus().mixTicks;sample.audioMixTicks=u32(audioNow-telemetryAudio);telemetryAudio=audioNow;
                sample.gpu=platformTakeGpuTelemetry();sample.gpuTailMilliseconds=C3D_GetDrawingTime();
                sample.draws=std::uint32_t(drawContext.submitted-telemetryDrawn);telemetryDrawn=drawContext.submitted;
                // The worker's rest rule measures the painter per draw.
                gxWorkerPictureEnd(sample.paintTicks,sample.draws);
                sample.stage=dComIfGp_getStartStageName();sample.room=dComIfGp_roomControl_getStayNo();
                sample.ticks=ticks;
                closeGpuListCapture(frame,sample.submitTick,sample.stage,sample.room);
                recordFrameTelemetry(sample);
                consolePaintTicks=sample.paintTicks;consoleUpdateTicks=sample.updateTicks;consoleDraws=sample.draws;
                switch(sceneTransitionPictureEnd(float(sample.loopTicks*1000.0/nativeTickFrequency))){
                case TransitionEvent::warmingStarted:dusk::audio::SetPaused(true);break;
                case TransitionEvent::released:
                    // No catch-up ticks for the hidden time, then the sound.
                    dusk::audio::SetPaused(gameSuspended||gameSleeping);resetTiming=true;break;
                case TransitionEvent::none:break;
                }
                consoleGpuWaitTicks=sample.gpu.beginWaitTicks+sample.gpu.endWaitTicks+sample.gpu.completeWaitTicks+sample.gpu.presentWaitTicks;
                drainBenchmarkInput(sample,true);
            }
            // Main owner, completed GPU frame, before bottom progress UI.
            // No game/filesystem lock held and no worker suspension.
            // The request first measures a burst of ordinary pictures.
            // The rate shown when the dump was asked for: the burst's own
            // pictures are slower.
            static float askedFps;static int askedSpeed;
            if(applicationDumpRequested()&&measured::state.stage==measured::Stage::idle)devConsoleRates(askedFps,askedSpeed);
            // At its picture or the first one after it that can take it (another
            // measurement still running, the worker resting: measured_dump.hpp).
            for(unsigned n=0;n<measureTriggerCount;++n)if(!measureTriggers[n].taken&&measureTriggers[n].frame<=frame&&!applicationDumpRequested()){
                char label[96];std::snprintf(label,sizeof(label),"trigger=frame-%llu stage=%.8s room=%d",static_cast<unsigned long long>(frame),
                    dComIfGp_getStartStageName(),int(dComIfGp_roomControl_getStayNo()));
                auto& trigger=measureTriggers[n];
                trigger.taken=measured::request({8,trigger.reference||trigger.without?0u:6u,2,trigger.reference,trigger.without},label);
                break;
            }
            // A scripted measurement is written as soon as it is complete (SD time: the pacing starts again).
            if(measureTriggerCount&&!measured::active()&&measured::flush("sdmc:/3ds/DuskLight/measure.txt")){dusk::game_clock::reset();resetTiming=true;}
            if(measured::advance(completedPicture,audioTicks,applicationDumpRequested())){
                finishBenchmarkPicture();
                {char note[512];const float fps=askedFps;const int speed=askedSpeed;auto* player=dComIfGp_getPlayer(0);
                 const auto zone=devConsoleZoneLoadout();
                 // test_zone: dump folders are numbered by capture, so after a
                 // crashed zone (no dump) they no longer match zone numbers.
                 std::snprintf(note,sizeof(note),"fps=%.1f\ngame_speed_percent=%d\nlogic_ticks_last_picture=%u\nplayer_position=%.0f %.0f %.0f\n"
                    "test_zone=%u\ntest_tour=%s\ntest_state_seed=%08lx\ntest_loadout=%s\n",
                    fps,speed,ticks,player?player->current.pos.x:0.0f,player?player->current.pos.y:0.0f,player?player->current.pos.z:0.0f,
                    zone.zone,zone.tour[0]?zone.tour:"none",
                    static_cast<unsigned long>(zone.seed),developerSession?loadoutText:"none (normal play)");
                 setApplicationDumpNote(note);}
                setClockPaused(true);dusk::audio::SetPaused(true);
                const bool captured=serviceApplicationDump(frame,dComIfGp_getStartStageName(),dComIfGp_roomControl_getStayNo());
                measured::finish();
                if(captured)flagNextFrameTelemetry(telemetryFullDump);
                setClockPaused(gameSuspended||gameSleeping);
                dusk::audio::SetPaused(gameSuspended||gameSleeping||transitionWarming());
                devConsoleRedraw();
                if(captured){dusk::game_clock::reset();resetTiming=true;continue;}
            }
            if(developerDumpRequested(frame))dumpDeveloperMemory(frame);
            if(!detailedDiagnostics&&frame%60==0)ordinaryFrames=ordinaryExecutionTicks=ordinaryLoopTicks=0;
            // Console evidence: without this a freeze left no log at all (the
            // log stayed in RAM until a clean exit). Written by the SD writer thread.
            if(!detailedDiagnostics)flushNativeLogPeriodic();
            gpuCanaryFrameEnd(frame,dComIfGp_getStartStageName());
            if(detailedDiagnostics&&(frame==1||frame%30==0)){
                const double perfLoopMs=ordinaryFrames?ordinaryLoopTicks*1000.0/(nativeTickFrequency*ordinaryFrames):0.0;
                const double perfExecutionMs=ordinaryFrames?ordinaryExecutionTicks*1000.0/(nativeTickFrequency*ordinaryFrames):0.0;
                auto* gameHeap=mDoExt_getGameHeap();
                const auto audio=nativeAudioStatus();
                const char* audioText=audio.output==AudioOutputState::ndsp?"NDSP active":
                    audio.output==AudioOutputState::componentUnavailable?"MUTED: DSP component missing":"not initialized";
                observeDeveloperRetainedDepth(RetainedDepthStage::beforeConsole);
                (void)gameHeap;(void)audioText;(void)timing;
                flushNativeLog();
                if(auto* perfLog=std::fopen("sdmc:/3ds/DuskLight/perf.log","ab")){
                    // Real hardware timing, one line per 30 frames, for remote diagnosis.
                    std::fprintf(perfLog,"frame=%llu stage=%.8s loop_ms=%.2f fps=%.1f exec_ms=%.2f drawn=%llu rejected=%llu linear_free_kib=%lu\n",
                        static_cast<unsigned long long>(frame),dComIfGp_getStartStageName(),perfLoopMs,perfLoopMs>0?1000.0/perfLoopMs:0.0,perfExecutionMs,
                        static_cast<unsigned long long>(drawContext.submitted),static_cast<unsigned long long>(drawContext.rejected),
                        static_cast<unsigned long>(linearSpaceFree()/1024));
                    std::fclose(perfLog);
                }
                observeDeveloperRetainedDepth(RetainedDepthStage::afterConsoleWrites);
                observeDeveloperRetainedDepth(RetainedDepthStage::afterConsoleFlush);
            }
            if(detailFrames&&(frame==1||frame%30==0)){
                if(ordinaryFrames)OSReport("Native ordinary frame window: count=%llu execution_mean=%.3f loop_mean=%.3f ms detailed=%s\n",
                    static_cast<unsigned long long>(ordinaryFrames),ordinaryExecutionTicks*1000.0/(nativeTickFrequency*ordinaryFrames),
                    ordinaryLoopTicks*1000.0/(nativeTickFrequency*ordinaryFrames),gxProfileEveryFrame?"all":"sparse");
                ordinaryFrames=ordinaryExecutionTicks=ordinaryLoopTicks=0;
                auto* measuredGameHeap=mDoExt_getGameHeap();
                OSReport("Native renderer memory: game_free=%lu linear_free=%lu gpu_textures=%lu archive_bti=%lu font_bti=%lu copy_bti=%lu offscreen=%lu geometry=%lu bytes\n",
                    static_cast<unsigned long>(measuredGameHeap?measuredGameHeap->getTotalFreeSize():0),
                    static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(gxTextureBytes()),
                    static_cast<unsigned long>(textureResourceBytes()),static_cast<unsigned long>(fontPageBytes()),
                    static_cast<unsigned long>(copyImageBytes()),static_cast<unsigned long>(offscreenFramebufferBytes()),
                    static_cast<unsigned long>(gxGeometryBytes()));
                const auto halfStatus=gxTextureHalfStatus();
                if(halfStatus.policyEntries)OSReport("Native texture half residency: policy=%lu images=%lu gpu_bytes=%lu same_images_full_bytes=%lu (CPU assets unchanged)\n",
                    static_cast<unsigned long>(halfStatus.policyEntries),static_cast<unsigned long>(halfStatus.residentImages),
                    static_cast<unsigned long>(halfStatus.residentBytes),static_cast<unsigned long>(halfStatus.sameImagesFullBytes));
                OSReport("Original native frame %llu complete; drawn=%llu rejected=%llu quantized=%llu vertex-fog=%llu vertex-indirect=%llu culled=%llu triangle-fog=%llu\n",static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(drawContext.submitted),static_cast<unsigned long long>(drawContext.rejected),static_cast<unsigned long long>(drawContext.quantizedDraws),static_cast<unsigned long long>(drawContext.vertexFogDraws),static_cast<unsigned long long>(drawContext.vertexIndirectDraws),static_cast<unsigned long long>(drawContext.culled),static_cast<unsigned long long>(drawContext.triangleFogDraws));
                OSReport("Native original cache flush=%.3f ms calls=%u bytes=%llu invalidate=%.3f ms calls=%u bytes=%llu\n",
                    cacheMaintenanceProfile.flushTicks*1000.0/nativeTickFrequency,cacheMaintenanceProfile.flushCalls,cacheMaintenanceProfile.flushBytes,
                    cacheMaintenanceProfile.invalidateTicks*1000.0/nativeTickFrequency,cacheMaintenanceProfile.invalidateCalls,cacheMaintenanceProfile.invalidateBytes);
                reportPositiveQGpu(drawContext);
                reportCameraAlignedProof(drawContext);
                OSReport("Native audio execution=%.3f ms\n",audioTicks*1000.0/nativeTickFrequency);
                const auto workerAfter=nativeAudioStatus();
                OSReport("Native audio worker elapsed=%.3f ms blocks=%lu (overlaps main profiling)\n",
                    u32(workerAfter.mixTicks-workerBefore.mixTicks)*1000.0/nativeTickFrequency,
                    static_cast<unsigned long>(workerAfter.blocks-workerBefore.blocks));
                OSReport("Native GX immediate scratch peak=%u capacity=%u split-draws=%llu split-chunks=%llu largest-split=%u merged=%llu merged-lines=%llu\n",
                    unsigned(stream.peakBytes()),unsigned(stream.scratchCapacity()),
                    static_cast<unsigned long long>(stream.splitDraws()),static_cast<unsigned long long>(stream.splitChunks()),
                    unsigned(stream.largestSplitBytes()),static_cast<unsigned long long>(stream.mergedDraws()),
                    static_cast<unsigned long long>(stream.mergedLineDraws()));
                OSReport("Native GX primitives grouped=%llu submissions=%llu vertices-reused=%llu\n",static_cast<unsigned long long>(processor.statistics().mergedPrimitives),static_cast<unsigned long long>(processor.statistics().draws),static_cast<unsigned long long>(processor.statistics().reusedVertices));
                auto milliseconds=[&](GxProfilePart part){const auto n=unsigned(part);return 1000.0*(gxProfileTotals.ticks[n]-profileBefore.ticks[n])/nativeTickFrequency;};
                OSReport("Native sparse array validation=%.3f ms calls=%llu\n",milliseconds(GxProfilePart::arrayValidate),static_cast<unsigned long long>(gxProfileTotals.calls[unsigned(GxProfilePart::arrayValidate)]-profileBefore.calls[unsigned(GxProfilePart::arrayValidate)]));
                for(const auto& e:gxNamedProfiles)if(e.calls)OSReport("Native painter %s %.3f ms calls=%llu\n",e.name,e.ticks*1000.0/nativeTickFrequency,static_cast<unsigned long long>(e.calls));
                {OSReport("Native rigid plan rejects:");for(unsigned c=0;c<gx::gxRigidPlanRejects.size();++c)if(gx::gxRigidPlanRejects[c])OSReport(" %u=%lu",c,static_cast<unsigned long>(gx::gxRigidPlanRejects[c]));
                 OSReport("\n");
                 const bool refusedFog=gx::gxRigidPlanRejects[20]!=0;
                 if(gx::gxRigidPlanRejects[25]){
                     const auto& d=gx::gxRigidLastRefusedUniform;
                     const auto& f=d.fogWords;
                     OSReport("Native rigid uniform refused (reject 25): u[%u][%u]=%g fog BP e8/ee/ef/f0/f1=%06lx/%06lx/%06lx/%06lx/%06lx\n",
                         d.row,d.component,double(d.value),static_cast<unsigned long>(f[0]),static_cast<unsigned long>(f[1]),
                         static_cast<unsigned long>(f[2]),static_cast<unsigned long>(f[3]),static_cast<unsigned long>(f[4]));
                 }
                 for(unsigned c=0;c<gx::gxRigidPlanRejects.size();++c)gx::gxRigidPlanRejectsCleared[c]+=gx::gxRigidPlanRejects[c];
                 gx::gxRigidPlanRejects={};
                 if(gx::gxRigidConstantFogRampPlans)
                     OSReport("Native rigid constant fog ramp plans=%llu\n",gx::gxRigidConstantFogRampPlans);
                 OSReport("Native rigid texgen from the normal plans=%lu (per-bone texture matrices: plans=%llu bones refused=%llu); "
                     "last refused texgen gen=%08lx post=%08lx reject=%lu\n",
                     static_cast<unsigned long>(gx::gxRigidNormalTexgenPlans),gx::gxPairedTexgenPlans,gx::gxPairedTexgenRefusals,
                     static_cast<unsigned long>(gx::gxRigidLastRefusedTexgen.gen),
                     static_cast<unsigned long>(gx::gxRigidLastRefusedTexgen.post),static_cast<unsigned long>(gx::gxRigidLastRefusedTexgen.code));
                 if(const auto& d=gx::gxRigidLastRefusedFog;refusedFog&&d.set)
                     OSReport("Native rigid plan, fog of the last draw refused for it (reject 20): function=%u orthographic=%u a=%g b=%g c=%g "
                         "fog unit=%d texture units=%u vertex colour=%u lit=%u\n",d.function,d.orthographic,double(d.a),double(d.b),double(d.c),
                         d.fogUnit,d.units,d.vertexColor,d.lit);
                 if(const auto& d=gx::gxRigidLastRefusedSources;d.mask)
                     OSReport("Native rigid plan, last draw reading more coordinate sets than a record carries (reject 61): TEX mask=%02x units=%u "
                         "gens=%08lx %08lx %08lx vertex colour=%u ambient from vertex=%u lit=%u required=%u lights=%02x fog=%u\n",
                         d.mask,d.units,static_cast<unsigned long>(d.gen[0]),static_cast<unsigned long>(d.gen[1]),static_cast<unsigned long>(d.gen[2]),
                         d.vertexColor,d.ambientVertex,d.lit,d.required,d.lightMask,d.fog);
                 OSReport("Native compact decode rejects:");for(unsigned c=0;c<gx::gxCompactDecodeRejects.size();++c)if(gx::gxCompactDecodeRejects[c])OSReport(" %u=%lu",c,static_cast<unsigned long>(gx::gxCompactDecodeRejects[c]));
                 OSReport("\n");
                 for(unsigned c=0;c<gx::gxCompactDecodeRejects.size();++c)gx::gxCompactDecodeRejectsCleared[c]+=gx::gxCompactDecodeRejects[c];
                 gx::gxCompactDecodeRejects={};}
                {static const char* names[5]{"staticRigid","dynamicRigid","ortho2D","perspCpu","other"};
                 for(unsigned c=0;c<5;++c)if(gxDrawClassCount[c])OSReport("Native draw class %s count=%llu %.3f ms\n",names[c],
                    static_cast<unsigned long long>(gxDrawClassCount[c]),gxDrawClassTicks[c]*1000.0/nativeTickFrequency);
                 gxDrawClassTicks={};gxDrawClassCount={};}
                for(unsigned op=0;op<256;++op)if(gxFifoOpcodeCounts[op])OSReport("Native FIFO opcode %02x count=%lu bytes=%lu\n",op,
                    static_cast<unsigned long>(gxFifoOpcodeCounts[op]),static_cast<unsigned long>(gxFifoOpcodeBytes[op]));
                gxFifoOpcodeCounts={};gxFifoOpcodeBytes={};
                OSReport("Native game phases paint=%.3f update=%.3f draw=%.3f ms\n",milliseconds(GxProfilePart::gamePaint),milliseconds(GxProfilePart::gameUpdate),milliseconds(GxProfilePart::gameDraw));
                OSReport("Native GX stream total=%.3f ms\n",milliseconds(GxProfilePart::stream));
                OSReport("Native proven constant early depth draws=%llu alpha-union=%llu\n",static_cast<unsigned long long>(drawContext.constantEarlyDepthDraws),static_cast<unsigned long long>(drawContext.alphaUnionDraws));
                OSReport("Native sparse vertex samples decode=%.3f transform=%.3f lighting=%.3f fog=%.3f ms samples=%llu\n",
                    milliseconds(GxProfilePart::vertexDecode),milliseconds(GxProfilePart::vertexTransform),milliseconds(GxProfilePart::vertexLighting),milliseconds(GxProfilePart::vertexFog),
                    static_cast<unsigned long long>(gxProfileTotals.calls[unsigned(GxProfilePart::vertexDecode)]-profileBefore.calls[unsigned(GxProfilePart::vertexDecode)]));
                OSReport("Native GPU rigid vertices=%llu direct-decode=%llu compact=%llu\n",static_cast<unsigned long long>(gxProfileTotals.rigidVertices-profileBefore.rigidVertices),static_cast<unsigned long long>(gxProfileTotals.rigidDecodedVertices-profileBefore.rigidDecodedVertices),static_cast<unsigned long long>(gxProfileTotals.compactVertices-profileBefore.compactVertices));
                OSReport("Native topology cache hits=%llu misses=%llu bytes=%u\n",static_cast<unsigned long long>(processor.statistics().batchCacheHits),static_cast<unsigned long long>(processor.statistics().batchCacheMisses),unsigned(topologyStorage.size()));
                OSReport("Native compact snapshot hits=%llu misses=%llu stores=%llu evictions=%llu bytes=%llu ranges=%llu output=%llu packed-changes=%llu array-changes=%llu\n",
                    compactCache.hits,compactCache.misses,compactCache.stores,compactCache.evictions,compactCache.storeBytes,compactCache.rangeBytes,compactCache.outputBytes,compactCache.packedChanges,compactCache.arrayChanges);
                {std::uint64_t cs,cd,cm;optionalCopyStatistics(cs,cd,cm);OSReport("Native optional capture deferred=%llu done=%llu materialized=%llu\n",
                    static_cast<unsigned long long>(cs),static_cast<unsigned long long>(cd),static_cast<unsigned long long>(cm));}
                {std::uint64_t sk,ap;picaStateFilterStatistics(sk,ap);OSReport("Native PICA state filter skips=%llu applies=%llu\n",
                    static_cast<unsigned long long>(sk),static_cast<unsigned long long>(ap));}
                {OSReport("Native dynamic shapes pos=%lu nrm=%lu col=%lu nodata=%lu\n",
                    static_cast<unsigned long>(duskNativeDynamicReasons[0]),static_cast<unsigned long>(duskNativeDynamicReasons[1]),
                    static_cast<unsigned long>(duskNativeDynamicReasons[2]),static_cast<unsigned long>(duskNativeDynamicReasons[3]));}
                {OSReport("Native perspective CPU gates:");
                 for(unsigned g=0;g<16;++g)if(gxPerspCpuGate[g])OSReport(" %u=%llu/%llu",g,static_cast<unsigned long long>(gxPerspCpuGate[g]),static_cast<unsigned long long>(gxPerspCpuGateVertices[g]));
                 OSReport("\n");gxPerspCpuGate={};gxPerspCpuGateVertices={};}
                gxTextureDecodeReport();
                {std::uint64_t bs,ba;gxTextureBindStatistics(bs,ba);OSReport("Native texture binds skipped=%llu applied=%llu\n",
                    static_cast<unsigned long long>(bs),static_cast<unsigned long long>(ba));}
                {std::uint64_t us,uw;picaUniformStatistics(us,uw);OSReport("Native uniform rows skipped=%llu written=%llu\n",
                    static_cast<unsigned long long>(us),static_cast<unsigned long long>(uw));}
                reportPicaSubmission();
                reportHudTape();
                reportScreenTape();
                {std::uint64_t nh,nm,nr,nk;nativeShapeStatistics(nh,nm,nr,nk);OSReport("Native direct shapes hits=%llu misses=%llu records=%llu keyMisses=%llu\n",
                    static_cast<unsigned long long>(nh),static_cast<unsigned long long>(nm),static_cast<unsigned long long>(nr),static_cast<unsigned long long>(nk));}
                {std::uint64_t refreshed,refused;std::array<std::uint64_t,7> parts;nativeShapeDiagnostics(refreshed,refused,parts);
                 OSReport("Native direct shape constants refreshed=%llu refused=%llu; profiled key misses by part bp=%llu texture=%llu cp=%llu xf=%llu matrix=%llu target=%llu unrecorded=%llu\n",
                    static_cast<unsigned long long>(refreshed),static_cast<unsigned long long>(refused),
                    static_cast<unsigned long long>(parts[0]),static_cast<unsigned long long>(parts[1]),static_cast<unsigned long long>(parts[2]),
                    static_cast<unsigned long long>(parts[3]),static_cast<unsigned long long>(parts[4]),static_cast<unsigned long long>(parts[5]),
                    static_cast<unsigned long long>(parts[6]));}
                for(unsigned kind=0;kind<2;++kind){
                    // Ten most expensive process profiles of this frame (ids: d_procname.h).
                    char line[256];int at=std::snprintf(line,sizeof(line),"Native process %s top:",kind?"draw":"execute");
                    std::uint64_t total=0;for(const auto ticks:gxProcessTicks[kind])total+=ticks;
                    for(unsigned rank=0;rank<10&&at<int(sizeof(line))-24;++rank){
                        unsigned best=0;for(unsigned n=1;n<gxProcessProfileCapacity;++n)if(gxProcessTicks[kind][n]>gxProcessTicks[kind][best])best=n;
                        if(!gxProcessTicks[kind][best])break;
                        at+=std::snprintf(line+at,sizeof(line)-at," %u=%.2f/%u",best,gxProcessTicks[kind][best]*1000.0/nativeTickFrequency,unsigned(gxProcessCalls[kind][best]));
                        gxProcessTicks[kind][best]=0;
                    }
                    OSReport("%s total=%.2f ms\n",line,total*1000.0/nativeTickFrequency);
                    gxProcessTicks[kind]={};gxProcessCalls[kind]={};
                    // The owner-indexed kinds (paint, replay) are read by the measured dump only.
                    gxProcessTicks[kind+2]={};gxProcessCalls[kind+2]={};
                }
                {std::uint64_t d2,z2,b2;native2dStatistics(d2,z2,b2);
                 OSReport("Native profiled 2D draws=%llu zero-vertex-alpha=%llu alpha-blend=%llu\n",d2,z2,b2);}
                {auto& e=native_effects::counters;
                 OSReport("Native effects since last report: blob-shadows=%u draws=%u model-shadows=%u triangles=%u demoted=%u particle-emitters=%u batches=%u batched=%u off-screen=%u no-warp=%u single=%u (kind=%u projected=%u callback=%u formula=%u) bloom=%u\n",
                    e.simpleShadows,e.simpleShadowDraws,e.realShadows,e.realShadowTriangles,e.realShadowDemoted,e.particleEmitters,e.particleBatches,e.particleBatched,e.particleCulled,e.particleDirect,e.particleSingle,
                    e.particleUnbatched[0],e.particleUnbatched[1],e.particleUnbatched[2],e.particleUnbatched[3],e.bloomPasses);
                 OSReport("Native particle batches since last report: extra-draws env=%u texture=%u refraction=%u\n",e.particleBreaks[0],e.particleBreaks[1],e.particleRefraction);
                 // The profiled frame's particle resources, most expensive first (flags: see native_effects.h).
                 {auto* rows=native_effects::emitterRows;const unsigned count=native_effects::emitterRowCount;
                  std::sort(rows,rows+count,[](const auto& a,const auto& b){return a.ticks>b.ticks;});
                  for(unsigned i=0;i<count&&i<24;++i)OSReport("Native particle resource id=%04x flags=%05x emitters=%u particles=%u draws=%u single=%u ms=%.3f\n",
                      rows[i].id,rows[i].flags,rows[i].emitters,rows[i].particles,rows[i].batches,rows[i].singles,rows[i].ticks*1000.0/nativeTickFrequency);}
                 e={};
                 const auto shadow=takeGxShadowCasterStatistics();
                 OSReport("Native shadow casters since last report: passes=%u remembered-draws=%u dropped=%u retained=%u retained-replays=%u reclaimed-replays=%u retain-failed=%u publish-failed=%u\n",
                     shadow.passes,shadow.remembered,shadow.dropped,shadow.retained,shadow.retainedReplays,shadow.reclaimedReplays,shadow.retainFailed,shadow.publishFailed);}
                {unsigned tu,tc,tr;gx::textureObjectUsage(tu,tc,&tr);
                 OSReport("Native GX texture objects used=%u/%u reclaimed=%u\n",tu,tc,tr);}
                {std::uint64_t ch,cc,ci,cm;gx::compiledListStatistics(ch,cc,ci,cm);
                 OSReport("Native compiled lists: replayed=%llu compiled=%llu interpreted=%llu mismatches=%llu\n",ch,cc,ci,cm);
                 std::uint64_t pf,ph,pm;gx::paintListStatistics(pf,ph,pm);
                 OSReport("Native paint lists (packetPaint): fast replays=%llu held vertex setups=%llu held mismatches=%llu\n",pf,ph,pm);}
                {std::array<std::uint64_t,8> g{};const auto n=rigidTriangleFogStatistics(&g);
                 OSReport("Native GPU triangle-fog draws=%llu cpu-gates=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",n,g[0],g[1],g[2],g[3],g[4],g[5],g[6],g[7]);}
                {std::uint64_t ih,im,is,ifb;immediateDrawStatistics(ih,im,is,ifb);
                 OSReport("Native immediate draw records: hits=%llu misses=%llu stores=%llu fallbacks=%llu\n",ih,im,is,ifb);}
                {std::array<std::uint64_t,13> o;nativeShapeRecordOutcomes(o);
                 OSReport("Native static batch outcomes recorded=%llu batch-limit=%llu not-compact=%llu not-resident=%llu projected=%llu carrier-fallback=%llu indirect=%llu special-route=%llu clip-disabled=%llu fog-substituted=%llu expanded=%llu pool=%llu cpu-vertex=%llu\n",
                    o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7],o[8],o[9],o[10],o[11],o[12]);}
                {std::uint64_t chained,keyHits;nativeShapeChainStatistics(chained,keyHits);
                 OSReport("Native direct shape continuations=%llu key-cache-hits=%llu\n",static_cast<unsigned long long>(chained),static_cast<unsigned long long>(keyHits));}
                {std::uint64_t ph,pm,ps,pf;preparedDrawStatistics(ph,pm,ps,pf);
                 OSReport("Native prepared draws hits=%llu misses=%llu stores=%llu fallbacks=%llu\n",static_cast<unsigned long long>(ph),
                    static_cast<unsigned long long>(pm),static_cast<unsigned long long>(ps),static_cast<unsigned long long>(pf));}
                // New fields go last: older log readers match the prefix.
                if(auto* sc=gx::staticCompactCache()){
                    OSReport("Native static geometry hits=%llu misses=%llu stores=%llu resets=%llu bytes=%u/%u stage-clears=%llu\n",
                        static_cast<unsigned long long>(sc->hits),static_cast<unsigned long long>(sc->misses),static_cast<unsigned long long>(sc->stores),
                        static_cast<unsigned long long>(sc->resets),unsigned(sc->bytesUsed()),unsigned(sc->capacity()),static_cast<unsigned long long>(sc->stageClears));
                    OSReport("Native static geometry compact palettes hits=%llu stores=%llu arena=%llu replays=%llu refusals=%llu\n",
                        static_cast<unsigned long long>(sc->paletteHits),static_cast<unsigned long long>(sc->paletteStores),static_cast<unsigned long long>(sc->paletteArena),
                        static_cast<unsigned long long>(sc->paletteReplays),static_cast<unsigned long long>(sc->paletteRefusals));
                }
                OSReport("Native shared arrays hits=%llu stores=%llu evictions=%llu copied=%llu compared=%llu\n",compactCache.arrayHits,compactCache.arrayStores,compactCache.arrayEvictions,compactCache.arrayBytes,compactCache.comparedBytes);
                OSReport("Native camera compatibility culled=%llu\n",static_cast<unsigned long long>(drawContext.cameraCulled));
                {const auto& v=drawContext.cpuVertexReasons;
                OSReport("Native CPU vertex reasons union=%llu trifog=%llu fogred=%llu linepoint=%llu noclip=%llu indirect=%llu untextured=%llu multitex=%llu coord=%llu plan=%llu\n",
                    v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);}
                OSReport("Native degraded draws simplified=%llu skipped=%llu\n",
                    static_cast<unsigned long long>(drawContext.simplifiedDraws),static_cast<unsigned long long>(drawContext.skippedDraws));
                OSReport("Native vertex paths direct=%.3f cpu=%.3f position=%.3f ms position-only=%llu\n",milliseconds(GxProfilePart::vertexRigid),milliseconds(GxProfilePart::vertexCpu),milliseconds(GxProfilePart::vertexPosition),static_cast<unsigned long long>(gxProfileTotals.positionOnlyVertices-profileBefore.positionOnlyVertices));
                for(const auto& e:gxDecodeCensus)if(e.draws)
                    OSReport("Native decode census: draws=%lu vertices=%lu from the static cache=%lu %.3f ms; attributes%s\n",
                        static_cast<unsigned long>(e.draws),static_cast<unsigned long>(e.vertices),static_cast<unsigned long>(e.reused),
                        e.ticks*1000.0/nativeTickFrequency,e.text);
                gxDecodeCensus={};
                OSReport("Native EFB copies=%.3f encode=%.3f ms vertices=%llu lit=%llu fogged=%llu\n",milliseconds(GxProfilePart::copy),milliseconds(GxProfilePart::encode),
                    static_cast<unsigned long long>(gxProfileTotals.vertices-profileBefore.vertices),static_cast<unsigned long long>(gxProfileTotals.litVertices-profileBefore.litVertices),static_cast<unsigned long long>(gxProfileTotals.fogVertices-profileBefore.fogVertices));
                // Conversion attempts include preflight, clone encode and native stores;
                // checked flush calls include failures. Neither is legacy encode/decode.
                if(fusedColorCopy)OSReport("Native fused copy phases: conversion-attempt=%.3f ms calls=%llu checked-flush=%.3f ms calls=%llu (includes failures)\n",
                    milliseconds(GxProfilePart::fusedConvert),
                    static_cast<unsigned long long>(gxProfileTotals.calls[unsigned(GxProfilePart::fusedConvert)]-profileBefore.calls[unsigned(GxProfilePart::fusedConvert)]),
                    milliseconds(GxProfilePart::fusedCheckedFlush),
                    static_cast<unsigned long long>(gxProfileTotals.calls[unsigned(GxProfilePart::fusedCheckedFlush)]-profileBefore.calls[unsigned(GxProfilePart::fusedCheckedFlush)]));
                OSReport("Native frame profile: execution=%.3f command=%.3f texture=%.3f decode=%.3f vertex=%.3f submit=%.3f wait=%.3f ms decodes=%llu bytes=%llu evictions=%llu\n",
                    executionTicks*1000.0/nativeTickFrequency,milliseconds(GxProfilePart::command),milliseconds(GxProfilePart::texture),milliseconds(GxProfilePart::decode),
                    milliseconds(GxProfilePart::vertices),milliseconds(GxProfilePart::submit),milliseconds(GxProfilePart::wait),
                    static_cast<unsigned long long>(gxProfileTotals.calls[unsigned(GxProfilePart::decode)]-profileBefore.calls[unsigned(GxProfilePart::decode)]),
                    static_cast<unsigned long long>(gxProfileTotals.decodedBytes-profileBefore.decodedBytes),static_cast<unsigned long long>(gxProfileTotals.evictions-profileBefore.evictions));
                auto* player=dComIfGp_getPlayer(0);
                OSReport("Original world stage=%.8s room=%d player=%p\n",dComIfGp_getStartStageName(),dComIfGp_getStartStageRoomNo(),player);
                if(player)OSReport("Original player position %.3f %.3f %.3f stick %.3f angle=%d\n",
                    player->current.pos.x,player->current.pos.y,player->current.pos.z,mDoCPd_c::getStickValue(PAD_1),mDoCPd_c::getStickAngle3D(PAD_1));
                if(auto* link=dComIfGp_getLinkPlayer())OSReport("Original Link control demo-type=%d demo-mode=%lu speed=%.3f\n",
                    link->mDemo.getDemoType(),static_cast<unsigned long>(link->getDemoMode()),link->speedF);
            }
            observeDeveloperRetainedDepth(RetainedDepthStage::afterReporting);
            {
                // Bottom-screen console and automatic test, between frames.
                observeSceneChange();
                DevGameStatus status;
                status.submitTick=completedPicture.submitTick;
                status.submittedTransition=(completedPicture.sceneFlags&4)!=0;
                status.frame=frame;status.stage=dComIfGp_getStartStageName();status.room=dComIfGp_roomControl_getStayNo();
                status.point=dComIfGp_getStartStagePoint();status.layer=dComIfGp_getStartStageLayer();
                auto* player=dComIfGp_getPlayer(0);
                status.playerReady=player!=nullptr;status.stageChanging=dComIfGp_isEnableNextStage();
                status.playScene=status.playerReady&&fpcM_SearchByName(fpcNm_PLAY_SCENE_e)!=nullptr;
                status.transitionHeld=transitionHeld();
                if(player){status.position[0]=player->current.pos.x;status.position[1]=player->current.pos.y;status.position[2]=player->current.pos.z;}
                status.life=player?int(dComIfGs_getLife()):-1;
                status.menuOpen=status.playScene&&dComIfGp_isPauseFlag();
                status.sceneChanges=gameSceneChangeCount;status.sceneChange=sceneChangeText;
                if (const auto* step = devConsoleStoryContext().definition) {
                    observeStory(status);
                    if (step->script == story::Script::warp &&
                        story::sameArea(step->start, {status.stage, status.room})) {
                        if (auto* link = dComIfGp_getLinkPlayer())
                            status.portalWarp = story::observePortalWarp(*step, *static_cast<daAlink_c*>(link));
                    }
                    if (chapterWatch.step == step) {
                        status.chapterScene = chapterWatch.scene;
                        status.chapterEvidence = chapterWatch.proof.evidence;
                        if (status.playSceneId == chapterWatch.scene)
                            status.chapterControl = chapterWatch.control;
                    }
                }
                const double scale=1000.0/nativeTickFrequency;
                status.loopMs=float((gxProfileTick()-loopStart)*scale);status.execMs=float(executionTicks*scale);
                status.paintMs=float(consolePaintTicks*scale);status.updateMs=float(consoleUpdateTicks*scale);status.gpuWaitMs=float(consoleGpuWaitTicks*scale);
                status.ticks=ticks;
                status.draws=consoleDraws;status.rejected=drawContext.rejected;status.degraded=drawContext.simplifiedDraws;
                status.textureBytes=std::uint32_t(gxTextureBytes());status.geometryBytes=std::uint32_t(gxGeometryBytes());
                if(frame%30==0){auto* heap=mDoExt_getGameHeap();consoleGameFree=heap?std::uint32_t(heap->getTotalFreeSize()):0;consoleLinearFree=linearSpaceFree();}
                status.gameHeapFree=consoleGameFree;status.linearFree=consoleLinearFree;
                if(serviceDevConsole(status)){dusk::game_clock::reset();resetTiming=true;}
                tagBenchmarkInput(frame);
            }
            // Measure complete ordinary loops separately from detailed profiler,
            // console/log refresh, vertex tracing, screenshot and SD dump frames.
            if(!detailedDiagnostics||(frame!=1&&frame%30!=0&&!gxVertexLayoutTracing&&!developerCaptureRequested(frame)&&!developerDumpRequested(frame)&&!developerDepthAuditRequested(frame)&&!developerRetainedDepthDiagnosticFrame(frame))){
                ++ordinaryFrames;ordinaryExecutionTicks+=executionTicks;ordinaryLoopTicks+=gxProfileTick()-loopStart;
            }
        }
    }catch(const ResetSignal& signal){OSReport("Original reset requested mode=%d code=%lu\n",signal.mode,static_cast<unsigned long>(signal.code));}
#if defined(DUSK_PICA_GAME_DRAW)
    // A reset can leave fapGm_Execute before the ordinary shared FrameEnd.
    // Close both owners before normal GPU/worker/service shutdown; incomplete
    // command/target state remains a fatal failure, never a successful reset.
    if(dusk::pica::game::frameActive()){
        if(!stream.finish()||!endGxGeometryFrame()||!endFramebufferFrame())
            fatal("Native reset left incomplete shared render state");
        if(!dusk::pica::game::endFrame())fatal(dusk::pica::game::failure());
        if(!platformEndGpuFrame(GX_CMDLIST_FLUSH))fatal("Native reset GPU completion unavailable");
    }
#endif
    if(drawContext.diagnosticRasterCarrier)
        OSReport("Native raster carrier totals: attempts=%llu submitted=%llu rejected=%llu first=%s vertex=%lu\n",
            static_cast<unsigned long long>(drawContext.rasterCarrierAttempts),
            static_cast<unsigned long long>(drawContext.rasterCarrierDraws),
            static_cast<unsigned long long>(drawContext.rasterCarrierRejects),
            drawContext.rasterCarrierFirstFailure?drawContext.rasterCarrierFirstFailure:"none",
            static_cast<unsigned long>(drawContext.rasterCarrierFirstVertex));
    if(drawContext.allowDirectSignedQDiagnostic)
        OSReport("Native signed-q totals: submitted_negative=%llu submitted_mixed=%llu reported_attempts=%llu\n",
            static_cast<unsigned long long>(drawContext.signedQNegativeDraws),
            static_cast<unsigned long long>(drawContext.signedQMixedDraws),
            static_cast<unsigned long long>(drawContext.signedQReports));
    reportPositiveQGpu(drawContext);
    reportCameraAlignedProof(drawContext);
    const auto samplerJoin=stopBenchmarkInputSampler();
    // Stop input at the last picture. Even if its join fails, join the audio
    // mixer before a kernel process exit can meet a live worker on core 3.
    OSReport("Native shutdown phase=audio stop\n");
    dusk::audio::Shutdown();
    if(samplerJoin!=0)containShutdownFailure("benchmark input sampler wait result=%08lx",static_cast<unsigned long>(samplerJoin));
    FrameTelemetryInput incompleteInputPicture;
    incompleteInputPicture.frame=frame+1;
    incompleteInputPicture.stage=dComIfGp_getStartStageName();
    incompleteInputPicture.room=dComIfGp_roomControl_getStayNo();
    incompleteInputPicture.ticks=0;
    drainBenchmarkInput(incompleteInputPicture,false);
    reportGxWorker(true);stopGxWorker();
    OSReport("Native shutdown phase=GPU completion\n");
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)fatal("Original game shutdown GPU timeout");
    finishGpuListCaptures();
    finishTourVisualPicture(true);
    finishBenchmarkPicture(true);
    shutdownDevConsole();
    // Console telemetry: per-stage summary plus the most recent frames.
    OSReport("Native frame telemetry: %s\n",writeFrameTelemetry(true)?"written to sdmc:/3ds/DuskLight":"not written");
    flushNativeLog(); // ordinary play keeps the log in RAM until here
    finishDeveloperRetainedDepth(false); // Explicitly incomplete if exit/reset skipped next FrameBegin.
    // Every game worker is cancelled at once and unwinds with ThreadExit at
    // the same time as the others (the DVD thread on core 2, the JKR, memory
    // card and JAS threads on core 0): runtime/eh_globals.cpp keeps each
    // one's exception state its own (Alpha 13/15: abort in unwind_phase2).
    OSReport("Native shutdown phase=worker barrier\n");
    const auto stopped=shutdownWorkers();
    OSReport("Native worker barrier: status=%s joined=%u workers=%u stackBytes=%lu waitingQueues=%u joining=%u\n",
        workerShutdownStatusName(stopped.status),stopped.joined,stopped.remaining.workers,
        static_cast<unsigned long>(stopped.remaining.stackBytes),stopped.remaining.waitingQueues,stopped.joining);
    if(stopped.status!=WorkerShutdownStatus::complete||stopped.remaining.workers||
       stopped.remaining.stackBytes||stopped.remaining.waitingQueues||stopped.joining){
        // Timeout preserves all unjoined storage. Never return through libc's
        // heap unmapping, or use the normal fatal UI, with workers still live.
        containShutdownFailure("worker barrier incomplete: status=%s joined=%u workers=%u stackBytes=%lu waitingQueues=%u joining=%u",
            workerShutdownStatusName(stopped.status),stopped.joined,stopped.remaining.workers,
            static_cast<unsigned long>(stopped.remaining.stackBytes),stopped.remaining.waitingQueues,stopped.joining);
    }
    OSReport("Native shutdown phase=APT unhook\n");
    aptUnhook(&hook);
    // Main owns the remaining graphics/GSP teardown. Keep original arenas and
    // resources alive until that teardown completes and the process returns.
    OSReport("Native shutdown phase=return to graphics owner\n");
    return 0;
}
}
