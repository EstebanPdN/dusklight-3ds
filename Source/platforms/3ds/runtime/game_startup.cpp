#include <cstdlib>
#if DUSK_BG_BUDGET_EXPERIMENT_76
#include "bg_budget_experiment.hpp"
#endif
#include "copy_lifecycle_probe.hpp"
#if DUSK_GROUND_CREATE_PROBE
#include "ground_create_probe.hpp"
#endif
#if DUSK_SCENE_CREATE_PROBE
#include "scene_create_probe.hpp"
#endif
#include "cache_maintenance_profile.hpp"
#include "game_startup.hpp"
#include "application_dump_ctru.hpp"
#include "bootstrap.hpp"
#include "clock.hpp"
#include "os_arena.hpp"
#include "os_system.hpp"
#include "os_thread.hpp"
#include "pad_bootstrap.hpp"
#include "dvd_provider.hpp"
#include "gx_framebuffer.hpp"
#include "alpha_interval_target.hpp"
#include "signed_q_diagnostic.hpp"
#include "gx_draw_capture94.hpp"
#include "feedback_draw_capture76.hpp"
#include "strict_material74.hpp"
#include "../gx_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "gpu_sync.hpp"
#include "gx_stream.hpp"
#include "gx_material.hpp"
#include "gx_vertex_transform.hpp"
#include "gx_profile.hpp"
#include "fused_color_config.hpp"
#include "gx_compact_cache.hpp"
#include "gx_rigid_vertex.hpp"
#include "../gx_command_draw.hpp"
#include "gx_i4_bake_cache68.hpp"
#include "../gx_geometry_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "video_mode.hpp"
#include "texture_resources.hpp"
#include "texture_half_policy.hpp"
#include "audio_control.hpp"
#include "developer_script.hpp"
#include "font_pages.hpp"
#include "gx_copy_images.hpp"
#include "dusk/game_clock.h"
#include "dusk/version.hpp"
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_player.h"
#include "m_Do/m_Do_Reset.h"
#include "dusk/audio/DuskAudioSystem.h"
#include "helpers/gx_helper.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "c/c_dylink.h"
#include "m_Do/m_Do_machine.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_dvd_thread.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_main.h"
#include "f_ap/f_ap_game.h"
#include "f_op/f_op_actor_mng.h"
#include "DynamicLink.h"
#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <cstdio>
#include <cerrno>
#include <bit>
extern s32 LOAD_COPYDATE(void*);
extern "C" { extern u8 __start__[],__end__[]; }
namespace dusk { bool IsShuttingDown=false; }
extern std::uint32_t duskNativeDynamicReasons[4];
namespace dusk::native { void optionalCopyStatistics(std::uint64_t&,std::uint64_t&,std::uint64_t&); }
namespace dusk::native {
namespace {
bool liveGameArray(std::uint64_t address,u32 size){
    GxProfileScope profile(GxProfilePart::arrayValidate,gxSparseVertexProfiling);
    if(!address||!size||address>UINTPTR_MAX||size>UINTPTR_MAX-address)return false;
    auto* pointer=reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    auto* heap=JKRHeap::findFromRoot(pointer);
    s32 remaining=-1;
    if(heap&&heap->getHeapType()=='EXPH')remaining=static_cast<JKRExpHeap*>(heap)->getRemainingAllocationSize(pointer);
    else if(heap&&heap->getHeapType()=='SLID')remaining=static_cast<JKRSolidHeap*>(heap)->getRemainingUsedSize(pointer);
    return remaining>=0&&size<=static_cast<u32>(remaining);
}
BootStage bootStage;bool hasBootStage{};
gx::capture94::Capture drawCapture94;
gx::feedback76::Observer feedbackDrawCapture76;
void feedbackDrawLog76(const char* text){OSReport("%s",text);}
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
bool readVitalConfig80(){
    errno=0;auto* file=std::fopen("sdmc:/3ds/dusklight/vitals80.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native vital80 config unreadable; diagnostic disabled\n");
        return false;
    }
    char text[12];const auto bytes=std::fread(text,1,sizeof(text),file);bool enabled=false;
    bool valid=!std::ferror(file);if(std::fclose(file))valid=false;
    if(!valid||!parseSignedQDiagnostic({text,bytes},enabled)){
        OSReport("Native vital80 config invalid/unreadable; diagnostic disabled\n");return false;
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
    auto* file=std::fopen("sdmc:/3ds/dusklight/camera-cpu-clip.txt","rb");
    if(!file){
        if(errno!=ENOENT)OSReport("Native camera CPU clipping config unreadable; experiment disabled\n");
        return false;
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
    if(!file)return; // An ordinary boot retains the original opening scene.
    char text[96];const auto count=std::fread(text,1,sizeof(text),file);
    const bool valid=count<sizeof(text)&&!std::ferror(file)&&parseBootStage({text,count},bootStage);
    std::fclose(file);
    if(!valid)fatal("Invalid boot-stage.txt; expected STAGE ROOM POINT LAYER");
    hasBootStage=true;
    OSReport("Native developer stage requested: %s room=%d point=%d layer=%d\n",bootStage.name,bootStage.room,bootStage.point,bootStage.layer);
}
bool gameSuspended{},gameSleeping{},resetTiming{};
void onGameApt(APT_HookType type,void*){
    if(type==APTHOOK_ONSUSPEND)gameSuspended=true;
    if(type==APTHOOK_ONSLEEP)gameSleeping=true;
    if(type==APTHOOK_ONRESTORE)gameSuspended=false;
    if(type==APTHOOK_ONWAKEUP)gameSleeping=false;
    if(type==APTHOOK_ONRESTORE||type==APTHOOK_ONWAKEUP){resetTiming=true;osSetSpeedupEnable(true);}
    const bool paused=gameSuspended||gameSleeping||applicationDumpActive();
    setClockPaused(paused);dusk::audio::SetPaused(paused);
}
}
const BootStage* requestedBootStage(){return hasBootStage?&bootStage:nullptr;}
int runOriginalGame(){
    logText("Original native startup: GX stream\n");
    bool rgb565Reference=false;
    if(auto* file=std::fopen("sdmc:/3ds/dusklight/rgb565-rgba8-reference.txt","rb")){
        std::fclose(file);rgb565Reference=true;
    }
    std::array<NativeTextureFingerprint,64> halfPolicy{};const auto halfCount=readTextureHalfPolicy(halfPolicy);
    const bool fusedColorCopy=readFusedColorCopyConfigFromSd();
    if(!configureGxTextureCache(10*1024*1024,128,rgb565Reference,{halfPolicy.data(),halfCount},fusedColorCopy))fatal(gxTextureFailure());
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
    drawCapture94.initialize();
    const auto& captureSelection=drawCapture94.selection;
    OSReport("Native complete draw capture94: %s selection=%s class=%u target=%.8s,%d,%d,%d; first selected failure only\n",
        drawCapture94.armed()?"armed":"disabled",gx::capture94::selectionStatusName(captureSelection.status),
        unsigned(captureSelection.failureClass),captureSelection.target.name,captureSelection.target.room,
        captureSelection.target.point,captureSelection.target.layer);
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
    const auto strictMaterial74=strictmaterial74::readConfig();
    drawContext.allowVisualFallback=strictMaterial74==strictmaterial74::Config::enabled?
        strictmaterial74::allowFallback:nullptr;
    OSReport("Native strict material74: %s config=%s; TEV fallback refused only when enabled; perspective depth fallback unchanged\n",
        strictMaterial74==strictmaterial74::Config::enabled?"enabled":"disabled",
        strictmaterial74::configName(strictMaterial74));
    OSReport("Native original GX policy: classified perspective TEV/clipping approximations counted; resource/protocol/submission failures remain fatal; UI strict\n");
    OSReport("Native original GX policy: vertex depth/range fog plus final RGB TEV; Tier D counted\n");
    OSReport("Native original GX policy: nominal TEV fractions use nearest UNORM8; Tier D counted\n");
    OSReport("Native original GX policy: RGBA6 uses actual RGBA8 storage; 6-bit quantization pending\n");
    drawContext.reportFailure=[](const gx::CommandDraw& draw,const char* why){
        if(std::string_view(why).starts_with("GX visual fallback: ")){
            OSReport("Native degraded draw: %s\n",why);return;
        }
        if(drawCapture94.armed()){
            const auto receipt=drawCapture94.observe(draw,why,dComIfGp_getStartStageName(),dComIfGp_getStartStageRoomNo(),requestedBootStage());
            if(drawCapture94.attempted){
                char text[256];gx::capture94::formatReceipt(receipt,text,sizeof(text));
                // Post-close returned receipt is mandatory for capture admission.
                OSReport("Native capture94 returned: %s",text);
            }
        }
        reportCameraAlignedProof(drawContext);
        auto* player=dComIfGp_getPlayer(0);
        OSReport("Native rejected draw world: stage=%.8s room=%d player=%p\n",dComIfGp_getStartStageName(),dComIfGp_getStartStageRoomNo(),player);
        OSReport("Native draw rejected: %s primitive=%u vertices=%u\n",why,unsigned(draw.primitive),unsigned(draw.vertexCount));
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
    feedbackDrawCapture76.config=gx::feedback76::readConfig();
    OSReport("FRAME76 startup enabled=%u frame=%u limit=%u phase=pre-submit\n",
        unsigned(feedbackDrawCapture76.enabled()),feedbackDrawCapture76.config.frame,feedbackDrawCapture76.config.limit);
    const gx::CommandDrawSink feedbackSink76=+[](const gx::CommandDraw& draw,void* user){
        return feedbackDrawCapture76.invoke(draw,user,drawGxCommand,
            mDoGph_gInf_c::getFrameBufferTex(),feedbackDrawLog76);
    };
    processor.setSink(gx::feedback76::chooseSink(feedbackDrawCapture76.enabled(),drawGxCommand,feedbackSink76),&drawContext);
    const bool vital80=readVitalConfig80();
    OSReport("VITAL80 startup enabled=%u; read-only end-frame fields\n",unsigned(vital80));
    const bool automaticSu77=readAutomaticSuConfig77();
    if(!stream.configureAutomaticSu(automaticSu77))fatal("Original GX automatic SU77 configuration failed");
    OSReport("Native automatic SU77: %s; typed SDK updates, raw display-list writes retained\n",
        automaticSu77?"enabled":"disabled");
    if(!gx::setCommandStream(&stream))fatal("Original game GX stream installation failed");
    constexpr unsigned arenaBytes=48*1024*1024;
    void* arena=memalign(32,arenaBytes);
    if(!arena||!initializeClock()||!configureGameArena(arena,arenaBytes))fatal("Original game arena initialization failed");
    logText("Original native startup: disc validation\n");
    if(!initializeOriginalDisc())fatal("Original startup needs validated sdmc:/3ds/DuskLight/zelda.twilight");
    logText("Original native startup: disc ready; game state\n");
    readBootStage();loadDeveloperScripts();runDeveloperCalibration();setDeveloperDumpRegion(arena,arenaBytes);
    dusk::version::init();OSInit();
    mDoMain::sPowerOnTime=OSGetSystemTime();
    static mDoRstData resetData{};
    mDoRst::setResetData(&resetData);mDoRst::offReset();mDoRst::setLogoScnFlag(0);
    dComIfG_ct();mDoDvdThd::SyncWidthSound=false;
    aptHookCookie hook{};aptHook(&hook,onGameApt,nullptr);
    mDoMain::developmentMode=0;
    OSReport("Original native startup: machine\n");
    if(!mDoMch_Create())fatal("Original machine initialization failed");
    OSReport("Original native startup: graphics\n");
    if(!mDoGph_Create())fatal("Original graphics initialization failed");
    // JFWSystem owns PAD initialization and mDoCPd owns the gameplay JUT pad.
    // The diagnostic pad requires an existing JKR heap and must not be created
    // before mDoMch_Create or sampled in parallel with the original controller.
    if(!PADInit())fatal("Original PAD services are unavailable");
    mDoCPd_c::create();
    mDoDvdThd_callback_c::create((mDoDvdThd_callback_func)LOAD_COPYDATE,nullptr);
#if DUSK_SCENE_CREATE_PROBE
    initializeSceneProbeFromSd();
#endif
#if DUSK_GROUND_CREATE_PROBE
    initializeGroundProbeFromSd();
#endif
#if DUSK_BG_BUDGET_EXPERIMENT_76
    dusk::bg76::initializeFromSd();
#endif
    fapGm_Create();fopAcM_initManager();cDyl_InitAsync();
    g_mDoAud_audioHeap=JKRCreateSolidHeap(0x14D800*2,JKRGetCurrentHeap(),false);
    if(!g_mDoAud_audioHeap)fatal("Original audio heap allocation failed");
    dusk::game_clock::initialize();
    std::uint64_t frame=0;
    std::uint64_t ordinaryFrames=0,ordinaryExecutionTicks=0,ordinaryLoopTicks=0;
    try{
        while(aptMainLoop()){
            const auto loopStart=gxProfileTick();
            gxSparseVertexProfiling=frame%30==29;
            hidScanInput();
            updateApplicationDumpInput(hidKeysHeld());
            // START remains a game button; only this explicit chord exits.
            if(!applicationDumpInputSuppressed()&&(hidKeysHeld()&(KEY_START|KEY_SELECT))==(KEY_START|KEY_SELECT))break;
            if(developerExitRequested(frame)){OSReport("Native developer exit after frame %llu\n",static_cast<unsigned long long>(frame));break;}
            // Reported frame numbers start at 1 after the first completed frame.
            setDeveloperFrame(frame+1);
            if(feedbackDrawCapture76.enabled())feedbackDrawCapture76.setFrame(frame+1,feedbackDrawLog76);
            if(copyprobe::enabled)copyprobe::setFrame(frame+1);
            VIWaitForRetrace();
            if(resetTiming){dusk::game_clock::reset();resetTiming=false;}
            const auto timing=dusk::game_clock::advance();
            if(!platformBeginGpuFrame(C3D_FRAME_SYNCDRAW))fatal("Original game GPU frame admission failed");
            finishDeveloperRetainedDepth(true); // Before any new pre/depth capture.
            if(!collectTextureResources())fatal(textureResourceFailure());
            if(!beginGxGeometryFrame())fatal(gxGeometryFailure());
            if(!beginFramebufferFrame())fatal("Original game framebuffer begin failed");
            if(developerDepthAuditRequested(frame+1))captureDeveloperDepthBoundary(false);
            mDoGph_gInf_c::updateRenderSize();
            dusk::game_clock::begin_sim_tick();mDoCPd_c::read();
            if(mDoCPd_c::getTrig(PAD_1))OSReport("Original native PAD trigger=%04lx frame=%llu\n",
                static_cast<unsigned long>(mDoCPd_c::getTrig(PAD_1)),static_cast<unsigned long long>(frame));
            const auto workerBefore=nativeAudioStatus();
            const auto profileBefore=gxProfileTotals;const auto executionStart=gxProfileTick();
            gxSparseVertexProfiling=frame%30==29;resetGxNamedProfiles();
            cacheMaintenanceProfile={};cacheMaintenanceProfile.active=gxSparseVertexProfiling;
            // One diagnostic frame only; exclude this traced frame from timing
            // comparisons. All ordinary frames keep concise aggregate profiles.
            gxVertexLayoutTracing=frame==359;
            drawContext.cpuVertexReasons={};
#if DUSK_SCENE_CREATE_PROBE
            sceneProbeBegin(static_cast<std::uint32_t>(frame+1));
#endif
#if DUSK_GROUND_CREATE_PROBE
            groundProbeBegin(static_cast<std::uint32_t>(frame+1));
#endif
#if DUSK_BG_BUDGET_EXPERIMENT_76
            dusk::bg76::setFrame(static_cast<std::uint32_t>(frame+1));
#endif
            fapGm_Execute();
#if DUSK_GROUND_CREATE_PROBE
            groundProbeEnd();
#endif
#if DUSK_SCENE_CREATE_PROBE
            sceneProbeEnd();
#endif
#if DUSK_BG_BUDGET_EXPERIMENT_76
            dusk::bg76::flush();
#endif
            const auto audioStart=gxProfileTick();mDoAud_Execute();const auto audioTicks=gxProfileTick()-audioStart;dusk::game_clock::commit_sim_tick();
            const auto executionTicks=gxProfileTick()-executionStart;
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
            if(!platformEndGpuFrame(0))fatal("Native GPU frame completion unavailable");
            observeDeveloperRetainedDepth(RetainedDepthStage::afterFrameEnd);
            ++frame;
            // Main owner, completed GPU frame, before bottom progress UI.
            // No game/filesystem lock held and no worker suspension.
            if(applicationDumpRequested()){
                setClockPaused(true);dusk::audio::SetPaused(true);
                const bool captured=serviceApplicationDump(frame,dComIfGp_getStartStageName(),dComIfGp_roomControl_getStayNo());
                setClockPaused(gameSuspended||gameSleeping);
                dusk::audio::SetPaused(gameSuspended||gameSleeping);
                if(captured){dusk::game_clock::reset();resetTiming=true;continue;}
            }
            if(developerDumpRequested(frame))dumpDeveloperMemory(frame);
            if(frame==1||frame%30==0){
                const double perfLoopMs=ordinaryFrames?ordinaryLoopTicks*1000.0/(nativeTickFrequency*ordinaryFrames):0.0;
                const double perfExecutionMs=ordinaryFrames?ordinaryExecutionTicks*1000.0/(nativeTickFrequency*ordinaryFrames):0.0;
                auto* gameHeap=mDoExt_getGameHeap();
                const auto audio=nativeAudioStatus();
                const char* audioText=audio.output==AudioOutputState::ndsp?"NDSP active":
                    audio.output==AudioOutputState::componentUnavailable?"MUTED: DSP component missing":"not initialized";
                observeDeveloperRetainedDepth(RetainedDepthStage::beforeConsole);
                std::printf("\x1b[2J\x1b[1;1HDUSKLIGHT original runtime\nNew3DS native | 400x240 2D\nFrame %llu | dt %.4f\nStage: %.8s\n\nGX drawn / rejected\n%llu / %llu (degraded %llu)\nGPU textures: %lu KiB\nOwned BTI: %lu KiB\nGeometry: %lu / 524288 B\nJKR game free: %lu KiB\nLinear free: %lu KiB\n\nLoop %.1f ms = %.1f FPS\nExecution %.1f ms\n\nSTART+SELECT: exit\n",
                    static_cast<unsigned long long>(frame),timing.dt,dComIfGp_getStartStageName(),
                    static_cast<unsigned long long>(drawContext.submitted),static_cast<unsigned long long>(drawContext.rejected),
                    static_cast<unsigned long long>(drawContext.simplifiedDraws+drawContext.skippedDraws),
                    static_cast<unsigned long>(gxTextureBytes()/1024),static_cast<unsigned long>(textureResourceBytes()/1024),
                    static_cast<unsigned long>(gxGeometryBytes()),static_cast<unsigned long>(gameHeap?gameHeap->getTotalFreeSize()/1024:0),
                    static_cast<unsigned long>(linearSpaceFree()/1024),
                    perfLoopMs,perfLoopMs>0?1000.0/perfLoopMs:0.0,perfExecutionMs);
                flushNativeLog();
                if(auto* perfLog=std::fopen("sdmc:/3ds/DuskLight/perf.log","ab")){
                    // Real hardware timing, one line per 30 frames, for remote diagnosis.
                    std::fprintf(perfLog,"frame=%llu stage=%.8s loop_ms=%.2f fps=%.1f exec_ms=%.2f drawn=%llu rejected=%llu linear_free_kib=%lu\n",
                        static_cast<unsigned long long>(frame),dComIfGp_getStartStageName(),perfLoopMs,perfLoopMs>0?1000.0/perfLoopMs:0.0,perfExecutionMs,
                        static_cast<unsigned long long>(drawContext.submitted),static_cast<unsigned long long>(drawContext.rejected),
                        static_cast<unsigned long>(linearSpaceFree()/1024));
                    std::fclose(perfLog);
                }
                std::printf("\x1b[21;1HAudio: %.32s\nBlocks %lu | underruns %lu\nResult %08lx\n",audioText,
                    static_cast<unsigned long>(audio.blocks),static_cast<unsigned long>(audio.underruns),static_cast<unsigned long>(audio.result));
                observeDeveloperRetainedDepth(RetainedDepthStage::afterConsoleWrites);
                GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr),320*240*2);
                observeDeveloperRetainedDepth(RetainedDepthStage::afterConsoleFlush);
            }
            if(frame==1||frame%30==0){
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
                OSReport("Native GX immediate scratch peak=%u capacity=%u\n",unsigned(stream.peakBytes()),unsigned(stream.scratchCapacity()));
                OSReport("Native GX primitives grouped=%llu submissions=%llu vertices-reused=%llu\n",static_cast<unsigned long long>(processor.statistics().mergedPrimitives),static_cast<unsigned long long>(processor.statistics().draws),static_cast<unsigned long long>(processor.statistics().reusedVertices));
                auto milliseconds=[&](GxProfilePart part){const auto n=unsigned(part);return 1000.0*(gxProfileTotals.ticks[n]-profileBefore.ticks[n])/nativeTickFrequency;};
                OSReport("Native sparse array validation=%.3f ms calls=%llu\n",milliseconds(GxProfilePart::arrayValidate),static_cast<unsigned long long>(gxProfileTotals.calls[unsigned(GxProfilePart::arrayValidate)]-profileBefore.calls[unsigned(GxProfilePart::arrayValidate)]));
                for(const auto& e:gxNamedProfiles)if(e.calls)OSReport("Native painter %s %.3f ms calls=%llu\n",e.name,e.ticks*1000.0/nativeTickFrequency,static_cast<unsigned long long>(e.calls));
                {OSReport("Native rigid plan rejects:");for(unsigned c=0;c<gx::gxRigidPlanRejects.size();++c)if(gx::gxRigidPlanRejects[c])OSReport(" %u=%lu",c,static_cast<unsigned long>(gx::gxRigidPlanRejects[c]));
                 OSReport("\n");gx::gxRigidPlanRejects={};
                 OSReport("Native compact decode rejects:");for(unsigned c=0;c<gx::gxCompactDecodeRejects.size();++c)if(gx::gxCompactDecodeRejects[c])OSReport(" %u=%lu",c,static_cast<unsigned long>(gx::gxCompactDecodeRejects[c]));
                 OSReport("\n");gx::gxCompactDecodeRejects={};}
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
                {std::uint64_t nh,nm,nr,nk;nativeShapeStatistics(nh,nm,nr,nk);OSReport("Native direct shapes hits=%llu misses=%llu records=%llu keyMisses=%llu\n",
                    static_cast<unsigned long long>(nh),static_cast<unsigned long long>(nm),static_cast<unsigned long long>(nr),static_cast<unsigned long long>(nk));}
                {std::uint64_t ph,pm,ps,pf;preparedDrawStatistics(ph,pm,ps,pf);
                 OSReport("Native prepared draws hits=%llu misses=%llu stores=%llu fallbacks=%llu\n",static_cast<unsigned long long>(ph),
                    static_cast<unsigned long long>(pm),static_cast<unsigned long long>(ps),static_cast<unsigned long long>(pf));}
                if(auto* sc=gx::staticCompactCache())OSReport("Native static geometry hits=%llu misses=%llu stores=%llu resets=%llu bytes=%u/%u\n",
                    static_cast<unsigned long long>(sc->hits),static_cast<unsigned long long>(sc->misses),static_cast<unsigned long long>(sc->stores),
                    static_cast<unsigned long long>(sc->resets),unsigned(sc->bytesUsed()),unsigned(sc->capacity()));
                OSReport("Native shared arrays hits=%llu stores=%llu evictions=%llu copied=%llu compared=%llu\n",compactCache.arrayHits,compactCache.arrayStores,compactCache.arrayEvictions,compactCache.arrayBytes,compactCache.comparedBytes);
                OSReport("Native camera compatibility culled=%llu\n",static_cast<unsigned long long>(drawContext.cameraCulled));
                {const auto& v=drawContext.cpuVertexReasons;
                OSReport("Native CPU vertex reasons union=%llu trifog=%llu fogred=%llu linepoint=%llu noclip=%llu indirect=%llu untextured=%llu multitex=%llu coord=%llu plan=%llu\n",
                    v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);}
                OSReport("Native degraded draws simplified=%llu skipped=%llu\n",
                    static_cast<unsigned long long>(drawContext.simplifiedDraws),static_cast<unsigned long long>(drawContext.skippedDraws));
                OSReport("Native vertex paths direct=%.3f cpu=%.3f position=%.3f ms position-only=%llu\n",milliseconds(GxProfilePart::vertexRigid),milliseconds(GxProfilePart::vertexCpu),milliseconds(GxProfilePart::vertexPosition),static_cast<unsigned long long>(gxProfileTotals.positionOnlyVertices-profileBefore.positionOnlyVertices));
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
                if(vital80)OSReport("VITAL80 f=%u life=%u oxy=%d/%d/%d delta=%d/%d show=%u\n",
                    unsigned(frame),unsigned(dComIfGs_getLife()),dComIfGp_getOxygen(),dComIfGp_getNowOxygen(),dComIfGp_getMaxOxygen(),
                    dComIfGp_getOxygenCount(),dComIfGp_getMaxOxygenCount(),unsigned(dComIfGp_getOxygenShowFlag()));
            }
            observeDeveloperRetainedDepth(RetainedDepthStage::afterReporting);
            // Measure complete ordinary loops separately from detailed profiler,
            // console/log refresh, vertex tracing, screenshot and SD dump frames.
            if(frame!=1&&frame%30!=0&&!gxVertexLayoutTracing&&!developerCaptureRequested(frame)&&!developerDumpRequested(frame)&&!developerDepthAuditRequested(frame)&&!developerRetainedDepthDiagnosticFrame(frame)){
                ++ordinaryFrames;ordinaryExecutionTicks+=executionTicks;ordinaryLoopTicks+=gxProfileTick()-loopStart;
            }
        }
    }catch(const ResetSignal& signal){OSReport("Original reset requested mode=%d code=%lu\n",signal.mode,static_cast<unsigned long>(signal.code));}
#if DUSK_SCENE_CREATE_PROBE
    sceneProbeClose();
#endif
#if DUSK_GROUND_CREATE_PROBE
    groundProbeClose();
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
    feedbackDrawCapture76.finish(feedbackDrawLog76);
    reportPositiveQGpu(drawContext);
    reportCameraAlignedProof(drawContext);
    OSReport("Native shutdown phase=GPU completion\n");
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)fatal("Original game shutdown GPU timeout");
    finishDeveloperRetainedDepth(false); // Explicitly incomplete if exit/reset skipped next FrameBegin.
    OSReport("Native shutdown phase=audio stop\n");
    dusk::audio::Shutdown();
    OSReport("Native shutdown phase=worker barrier\n");
    const auto stopped=shutdownWorkers();
    OSReport("Native worker barrier: status=%s joined=%u workers=%u stackBytes=%lu waitingQueues=%u joining=%u\n",
        workerShutdownStatusName(stopped.status),stopped.joined,stopped.remaining.workers,
        static_cast<unsigned long>(stopped.remaining.stackBytes),stopped.remaining.waitingQueues,stopped.joining);
    if(stopped.status!=WorkerShutdownStatus::complete||stopped.remaining.workers||
       stopped.remaining.stackBytes||stopped.remaining.waitingQueues||stopped.joining){
        // Timeout preserves all unjoined storage. Never return through libc's
        // heap unmapping, or use the normal fatal UI, with workers still live.
        OSReport("Native shutdown failure: worker barrier incomplete; kernel process exit\n");
        svcExitProcess();
    }
    OSReport("Native shutdown phase=APT unhook\n");
    aptUnhook(&hook);
    // Main owns the remaining graphics/GSP teardown. Keep original arenas and
    // resources alive until that teardown completes and the process returns.
    OSReport("Native shutdown phase=return to graphics owner\n");
    return 0;
}
}
