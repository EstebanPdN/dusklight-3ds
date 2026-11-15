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
#if defined(DUSK_PICA_GAME_DRAW)
#include "../native/game_draw_api.hpp"
#endif
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
#include "../gx_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "gpu_sync.hpp"
#include "gpu_canary.hpp"
#include "gx_stream.hpp"
#include "gx_material.hpp"
#include "gx_transform_commands.hpp"
#include "gx_vertex_transform.hpp"
#include "gx_profile.hpp"
#include "fused_color_config.hpp"
#include "gx_compact_cache.hpp"
#include "gx_rigid_vertex.hpp"
#include "../gx_command_draw.hpp"
#include "helpers/native_effects.h"
#include "shadow_casters.hpp"
#include "gx_i4_bake_cache68.hpp"
#include "../gx_geometry_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "video_mode.hpp"
#include "texture_resources.hpp"
#include "texture_half_policy.hpp"
#include "audio_control.hpp"
#include "developer_script.hpp"
#include "frame_telemetry.hpp"
#include "dev_console.hpp"
#include "helpers/native_frame_wait.h"
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
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <bit>
extern s32 LOAD_COPYDATE(void*);
extern "C" { extern u8 __start__[],__end__[]; }
namespace dusk { bool IsShuttingDown=false; }
extern std::uint32_t duskNativeDynamicReasons[4];
namespace dusk::native { void optionalCopyStatistics(std::uint64_t&,std::uint64_t&,std::uint64_t&); }
extern "C" std::uint32_t duskNativeHeapEpoch;
namespace dusk::native {
namespace {
bool liveGameArray(std::uint64_t address,u32 size){
    GxProfileScope profile(GxProfilePart::arrayValidate,gxSparseVertexProfiling);
    if(!address||!size||address>UINTPTR_MAX||size>UINTPTR_MAX-address)return false;
    // The same model arrays are bound for every shape. A proof stays valid
    // until some heap frees, shrinks or destroys an allocation.
    struct Proof {std::uint32_t address{},size{},epoch{};};
    static std::array<Proof,64> proofs{};
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
namespace {
// Test session state: the save in memory was replaced by an empty one with the
// test equipment, so travelling may change it freely. Never set for a save the
// player loaded, unless the automatic test starts (it must not be saved after).
bool developerSession;
CommandDrawContext* devDrawContext;
void developerLoadout(int form){
    // Sword, shield, lantern and boomerang: the scripted B, R, X and Y inputs
    // then exercise the original attack, guard, light and projectile code.
    execItemGet(dItemNo_SWORD_e);execItemGet(dItemNo_HYLIA_SHIELD_e);
    execItemGet(dItemNo_KANTERA_e);execItemGet(dItemNo_BOOMERANG_e);
    dComIfGs_setSelectItemIndex(SELECT_ITEM_X,SLOT_1);dComIfGs_setSelectItemIndex(SELECT_ITEM_Y,SLOT_0);
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
bool developerWarp(const char* stage,int room,int point,int layer,int form,bool test){
    auto* play=fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    scene_class* other{};
    if(!play){
        for(const s16 name:{s16(fpcNm_NAME_SCENE_e),s16(fpcNm_OPENING_SCENE_e),s16(fpcNm_MENU_SCENE_e)})
            if(auto* found=fpcM_SearchByName(name)){other=reinterpret_cast<scene_class*>(found);break;}
        if(!other)return false; // logo and warnings: startup resources may still be loading
    }else if(dComIfGp_isEnableNextStage())return false;
    const bool noSave=!play||!std::strncmp(dComIfGp_getStartStageName(),"F_SP102",8);
    if(noSave||(test&&!developerSession)){dComIfGs_init();developerSession=true;}
    if(developerSession)developerLoadout(form);
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
// The game state written into a dump's info.txt: what would otherwise have to
// be dug out of the RAM image by hand (camera, cutscene, player, heaps, actors).
void dumpStateReport(std::FILE* file){
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
        std::fprintf(file,"camera_eye=%.1f %.1f %.1f\ncamera_center=%.1f %.1f %.1f\ncamera_fovy=%.2f bank=%d\n",
            eye.x,eye.y,eye.z,center.x,center.y,center.z,fopCamM_GetFovy(view),int(fopCamM_GetBank(view)));
    }else std::fprintf(file,"camera=none\n");
    std::fprintf(file,"cutscene_camera=%d cutscene_frame=%lu cutscene_mode=%ld event_running=%d\n",int(dDemo_c::getCamera()!=nullptr),
        static_cast<unsigned long>(dDemo_c::getFrame()),static_cast<long>(dDemo_c::getMode()),int(dComIfGp_event_runCheck()));
    if(auto* player=dComIfGp_getPlayer(0)){
        std::fprintf(file,"player_position=%.1f %.1f %.1f angle_y=%d speed=%.2f room=%d life=%u\n",player->current.pos.x,player->current.pos.y,
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
    std::fprintf(file,"game_heap_free=%lu\nzelda_heap_free=%lu\narchive_heap_free=%lu\ngpu_texture_bytes=%lu\nowned_bti_bytes=%lu\n"
        "font_page_bytes=%lu\ncopy_image_bytes=%lu\noffscreen_bytes=%lu\ntransient_geometry_bytes=%lu\n",
        static_cast<unsigned long>(gameHeap?gameHeap->getTotalFreeSize():0),static_cast<unsigned long>(zeldaHeap?zeldaHeap->getTotalFreeSize():0),
        static_cast<unsigned long>(archiveHeap?archiveHeap->getTotalFreeSize():0),static_cast<unsigned long>(gxTextureBytes()),
        static_cast<unsigned long>(textureResourceBytes()),static_cast<unsigned long>(fontPageBytes()),
        static_cast<unsigned long>(copyImageBytes()),static_cast<unsigned long>(offscreenFramebufferBytes()),
        static_cast<unsigned long>(gxGeometryBytes()));
    if(auto* sc=gx::staticCompactCache())std::fprintf(file,"static_geometry_hits=%llu\nstatic_geometry_misses=%llu\nstatic_geometry_stores=%llu\n"
        "static_geometry_resets=%llu\nstatic_geometry_bytes=%u\nstatic_geometry_capacity=%u\n",
        static_cast<unsigned long long>(sc->hits),static_cast<unsigned long long>(sc->misses),static_cast<unsigned long long>(sc->stores),
        static_cast<unsigned long long>(sc->resets),unsigned(sc->bytesUsed()),unsigned(sc->capacity()));
    {std::uint64_t nh,nm,nr,nk;nativeShapeStatistics(nh,nm,nr,nk);
     std::fprintf(file,"direct_shape_hits=%llu\ndirect_shape_misses=%llu\ndirect_shape_records=%llu\ndirect_shape_key_misses=%llu\n",
        static_cast<unsigned long long>(nh),static_cast<unsigned long long>(nm),static_cast<unsigned long long>(nr),static_cast<unsigned long long>(nk));}
    const auto audio=nativeAudioStatus();
    std::fprintf(file,"audio_output=%s\naudio_blocks=%lu\naudio_underruns=%lu\n",audio.output==AudioOutputState::ndsp?"ndsp":"muted",
        static_cast<unsigned long>(audio.blocks),static_cast<unsigned long>(audio.underruns));
}
}
// d_s_logo, after the direct stage entry has initialized an empty save.
void applyBootStageLoadout(){
    if(!bootStageFromTest)return;
    developerSession=true;developerLoadout(0);
}
}
// Real-speed scheduling, read by fpcM_Management (f_pc_manager.cpp): which
// tick of the picture paints, which one enters the draw lists, and how many
// original ticks each of those phases stands for.
extern "C" {
bool duskNativeTickPaint=true,duskNativeTickDraw=true;
float duskNativePaintTicks=1,duskNativeDrawTicks=1;
// Developer log of the cutscene camera, one line per logic tick (SD switch camera-log.txt).
bool duskNativeCameraLog=false;
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
    auto* stream=gx::commandStream();
    if(stream&&stream->idle())(void)platformSubmitGpu();
}
namespace dusk::native {
int runOriginalGame(){
    // Before the log file is first opened: a resumed test keeps the previous log.
    {DevGameHooks hooks;
     hooks.warp=developerWarp;
     hooks.report=developerReport;hooks.stuck=developerStuckReport;hooks.fullDump=requestApplicationDump;
     hooks.fullDumpBusy=+[]{return applicationDumpRequested()||applicationDumpActive();};
     setApplicationDumpReport(dumpStateReport);
     initializeDevConsole(hooks);}
    logText("Original native startup: GX stream\n");
    bool rgb565Reference=false;
    if(auto* file=std::fopen("sdmc:/3ds/dusklight/rgb565-rgba8-reference.txt","rb")){
        std::fclose(file);rgb565Reference=true;
    }
    std::array<NativeTextureFingerprint,64> halfPolicy{};const auto halfCount=readTextureHalfPolicy(halfPolicy);
    const bool fusedColorCopy=readFusedColorCopyConfigFromSd();
    // 12 MiB: the title shots need about 10 MiB of images at once (they
    // cycled through a 10 MiB cache); the linear heap grew by 4 MiB for it.
    if(!configureGxTextureCache(12*1024*1024,128,rgb565Reference,{halfPolicy.data(),halfCount},fusedColorCopy))fatal(gxTextureFailure());
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
    // Strict rejection (the game ends on the first unsupported draw) only on request.
    {auto* strict=std::fopen("sdmc:/3ds/DuskLight/strict-draws.txt","rb");
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
    // Ordinary play keeps detailed profiling, the periodic report and SD writes
    // out of the frame loop; developer runs retain them (see developer_script).
    const bool detailedDiagnostics=developerDiagnosticsRequested();
    auto sdSwitch=[](const char* path){if(auto* file=std::fopen(path,"rb")){std::fclose(file);return true;}return false;};
    // Console A/B switches. Azahar models neither CPU caches nor GPU time.
    // The whole-heap frame-end flush is Citro3D's default and the only one
    // proven on a console; flush-ranges-only.txt keeps the cheaper A/B path.
    const bool flushAllLinear=!sdSwitch("sdmc:/3ds/DuskLight/flush-ranges-only.txt");
    const bool gpuChunksOff=sdSwitch("sdmc:/3ds/DuskLight/gpu-chunks-off.txt");
    const bool stackedWaits=sdSwitch("sdmc:/3ds/DuskLight/stacked-waits.txt");
    platformSetGpuChunkDraws(gpuChunksOff?0:24);
    const bool kernelFlush=sdSwitch("sdmc:/3ds/DuskLight/svc-flush.txt");
    setGxGeometryKernelFlush(kernelFlush);
    OSReport("Native geometry publication: %s\n",kernelFlush?"kernel cache flush (SD switch)":"GSP cache flush request");
    const bool gameFlush=sdSwitch("sdmc:/3ds/DuskLight/game-cache-flush.txt");
    setGameCacheFlush(gameFlush);
    OSReport("Native game cache flushes: %s\n",gameFlush?"forwarded to the kernel (SD switch)":"not forwarded");
    const bool nativeChainOff=sdSwitch("sdmc:/3ds/DuskLight/native-chain-off.txt");
    setNativeShapeChain(!nativeChainOff);
    OSReport("Native shape replay continuation: %s\n",nativeChainOff?"off (SD switch)":"on");
    OSReport("Native frame pacing: %s; GPU submission: %s; frame-end flush: %s\n",
        stackedWaits?"legacy stacked retrace waits (SD switch)":"single display deadline",
        gpuChunksOff?"whole frame at frame end (SD switch)":"incremental, every 24 draws and after the painter",
        flushAllLinear?"complete linear heap":"command list and written ranges (SD switch)");
    // Game speed: 30 logic ticks per second of real time (see LogicPace).
    // real-speed-off.txt restores one tick per picture (Alpha 0, slow motion);
    // draw-every-tick.txt also enters the draw lists on the extra ticks.
    const bool realSpeed=!sdSwitch("sdmc:/3ds/DuskLight/real-speed-off.txt");
    const bool drawEveryTick=sdSwitch("sdmc:/3ds/DuskLight/draw-every-tick.txt");
    duskNativeCameraLog=sdSwitch("sdmc:/3ds/DuskLight/camera-log.txt");
    OSReport("Native game speed: %s%s\n",realSpeed?"30 logic ticks per second of real time, up to 4 per picture":
        "one logic tick per picture (SD switch real-speed-off.txt)",drawEveryTick?"; draw lists on every tick (SD switch)":"");
    OSReport("Native GX syncs: %s\n",nativeGxSyncWaits()?"wait for the whole GPU (SD switch gx-sync-wait.txt)":
        "ordered on the GPU; only readbacks, eviction and texture re-decode wait");
    unsigned previousTicks=1;
    initializeFrameTelemetry();
    initializeGpuCanary();
    // Rigid routes. The multi-texture shader has completed on a console in
    // every run and draws everything by default, its projected unit included
    // (new on 2026-11-14: the guard records a stop of that unit).
    drawContext.allowMultiProjected=!gpuCanaryMultiProjectedRefused();
    if(auto* off=std::fopen("sdmc:/3ds/DuskLight/mt-projected-off.txt","rb")){std::fclose(off);drawContext.allowMultiProjected=false;}
    // The single-texture rigid shader stopped the console PICA in every build
    // that used it (2026-11-13). Its outputs were completed on 2026-11-14,
    // unproven on a console: it is used only when rigid-single-on.txt asks for
    // it, or as the second choice once the projected unit above was refused
    // (the CPU route is the third). A stop of it is learned and refuses it.
    gx::multiTextureWidePalette=!sdSwitch("sdmc:/3ds/DuskLight/mt-wide-palette-off.txt");
    drawContext.allowProjectionMode=!sdSwitch("sdmc:/3ds/DuskLight/projection-mode-off.txt");
    OSReport("Native projected textures on the CPU route: %s\n",drawContext.allowProjectionMode?
        "PICA projection mode (per-pixel divide)":"per-vertex divide (SD switch)");
    const bool singleAsked=sdSwitch("sdmc:/3ds/DuskLight/rigid-single-on.txt");
    drawContext.allowSingleRigidShader=(singleAsked||!drawContext.allowMultiProjected)&&!gpuCanaryRigidSingleRefused()&&
        !sdSwitch("sdmc:/3ds/DuskLight/rigid-single-off.txt");
    OSReport("Native rigid shaders: multi-texture palette %s; projected unit %s; single-texture shader %s\n",
        gx::multiTextureWidePalette?"up to the rows of unused units":"below row 81 (SD switch)",drawContext.allowMultiProjected?"on":"off",
        drawContext.allowSingleRigidShader?(singleAsked?"on (SD switch rigid-single-on.txt)":"on (second choice after the projected unit was refused)"):
        "off (the multi-texture shader or the CPU route draws its share)");
    std::uint64_t telemetryPaint=0,telemetryUpdate=0,telemetryDraw=0,telemetryDrawn=0;
    u32 telemetryAudio=nativeAudioStatus().mixTicks;
    std::uint64_t consolePaintTicks=0,consoleUpdateTicks=0,consoleGpuWaitTicks=0;
    std::uint32_t consoleDraws=0,consoleGameFree=0,consoleLinearFree=0;
    try{
        while(aptMainLoop()){
            const auto loopStart=gxProfileTick();
            // Developer runs and the test's profile phase take the sparse detailed profile.
            const bool detailFrames=detailedDiagnostics||devConsoleProfiling();
            gxSparseVertexProfiling=detailFrames&&frame%30==29;
            hidScanInput();
            // Scripted developer input reaches the dump chord and the test chord
            // too (emulator tests): held keys only, so a scripted game button
            // never operates the console pages.
            u32 heldKeys=hidKeysHeld();{std::int16_t sx{},sy{};applyScriptedInput(heldKeys,sx,sy);}
            updateApplicationDumpInput(heldKeys);
            // START and SELECT stay with the game and the console: START+SELECT
            // toggles the automatic test; the EXIT tab leaves the game.
            if(!applicationDumpInputSuppressed())updateDevConsoleInput(heldKeys,hidKeysDown());
            if(devConsoleExitRequested())break;
            if(developerExitRequested(frame)){OSReport("Native developer exit after frame %llu\n",static_cast<unsigned long long>(frame));break;}
            // Reported frame numbers start at 1 after the first completed frame.
            setDeveloperFrame(frame+1);
            if(gpuCanaryAutoAdvance()&&dComIfGp_getStartStageName()[0])gpuCanaryWorldEntered();
            if(copyprobe::enabled)copyprobe::setFrame(frame+1);
            // The game's display wait (JFWDisplay::beginRender) is the single
            // pacing point of a frame. Wait here only if the previous iteration
            // never reached it, so an unpaced state cannot spin.
            if(stackedWaits||!consumeFramePaced())VIWaitForRetrace();
            if(resetTiming){dusk::game_clock::reset();logicPace.reset();resetTiming=false;}
            // Logic ticks this picture carries; the first paints the previous
            // picture's lists, the last enters this picture's lists.
            const unsigned ticks=realSpeed?logicPace.due(loopStart):1;
            duskNativePaintTicks=float(previousTicks);duskNativeDrawTicks=drawEveryTick?1.0f:float(ticks);
            const auto timing=dusk::game_clock::advance();
            if(!platformBeginGpuFrame(stackedWaits?C3D_FRAME_SYNCDRAW:0))fatal("Original game GPU frame admission failed");
            finishDeveloperRetainedDepth(true); // Before any new pre/depth capture.
            if(!collectTextureResources())fatal(textureResourceFailure());
            if(!beginGxGeometryFrame())fatal(gxGeometryFailure());
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
            gxSparseVertexProfiling=detailFrames&&frame%30==29;resetGxNamedProfiles();
            native_effects::emitterReport=gxSparseVertexProfiling;native_effects::emitterRowCount=0;
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
            std::uint64_t audioTicks=0;
            for(unsigned tick=0;tick<ticks;++tick){
                duskNativeTickPaint=tick==0;duskNativeTickDraw=drawEveryTick||tick+1==ticks;
                // The pad is read once per tick: its edges reach only the first.
                dusk::game_clock::begin_sim_tick();mDoCPd_c::read();
                if(mDoCPd_c::getTrig(PAD_1))OSReport("Original native PAD trigger=%04lx frame=%llu\n",
                    static_cast<unsigned long>(mDoCPd_c::getTrig(PAD_1)),static_cast<unsigned long long>(frame));
                fapGm_Execute();
                const auto audioStart=gxProfileTick();mDoAud_Execute();audioTicks+=gxProfileTick()-audioStart;
                dusk::game_clock::commit_sim_tick();
            }
            duskNativeTickPaint=duskNativeTickDraw=true;
            // The next paint shows this picture: it follows the previous paint by these ticks.
            previousTicks=ticks;
#if DUSK_GROUND_CREATE_PROBE
            groundProbeEnd();
#endif
#if DUSK_SCENE_CREATE_PROBE
            sceneProbeEnd();
#endif
#if DUSK_BG_BUDGET_EXPERIMENT_76
            dusk::bg76::flush();
#endif
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
            ++frame;
            {
                // One RAM sample per frame; SD output only at a stage change or exit.
                FrameTelemetryInput sample;
                sample.frame=frame;sample.loopTicks=gxProfileTick()-loopStart;sample.executionTicks=executionTicks;
                const auto paint=gxProfileTotals.ticks[unsigned(GxProfilePart::gamePaint)];
                const auto update=gxProfileTotals.ticks[unsigned(GxProfilePart::gameUpdate)];
                const auto draw=gxProfileTotals.ticks[unsigned(GxProfilePart::gameDraw)];
                sample.paintTicks=paint-telemetryPaint;sample.updateTicks=update-telemetryUpdate;sample.drawTicks=draw-telemetryDraw;
                telemetryPaint=paint;telemetryUpdate=update;telemetryDraw=draw;
                sample.paceTicks=takeFramePaceTicks();
                const u32 audioNow=nativeAudioStatus().mixTicks;sample.audioMixTicks=u32(audioNow-telemetryAudio);telemetryAudio=audioNow;
                sample.gpu=platformTakeGpuTelemetry();sample.gpuTailMilliseconds=C3D_GetDrawingTime();
                sample.draws=std::uint32_t(drawContext.submitted-telemetryDrawn);telemetryDrawn=drawContext.submitted;
                sample.stage=dComIfGp_getStartStageName();sample.room=dComIfGp_roomControl_getStayNo();
                sample.ticks=ticks;
                recordFrameTelemetry(sample);
                consolePaintTicks=sample.paintTicks;consoleUpdateTicks=sample.updateTicks;consoleDraws=sample.draws;
                consoleGpuWaitTicks=sample.gpu.beginWaitTicks+sample.gpu.endWaitTicks+sample.gpu.completeWaitTicks+sample.gpu.presentWaitTicks;
            }
            // Main owner, completed GPU frame, before bottom progress UI.
            // No game/filesystem lock held and no worker suspension.
            if(applicationDumpRequested()){
                {char note[256];float fps{};int speed{};devConsoleRates(fps,speed);auto* player=dComIfGp_getPlayer(0);
                 std::snprintf(note,sizeof(note),"fps=%.1f\ngame_speed_percent=%d\nlogic_ticks_last_picture=%u\nplayer_position=%.0f %.0f %.0f\n",
                    fps,speed,ticks,player?player->current.pos.x:0.0f,player?player->current.pos.y:0.0f,player?player->current.pos.z:0.0f);
                 setApplicationDumpNote(note);}
                setClockPaused(true);dusk::audio::SetPaused(true);
                const bool captured=serviceApplicationDump(frame,dComIfGp_getStartStageName(),dComIfGp_roomControl_getStayNo());
                setClockPaused(gameSuspended||gameSleeping);
                dusk::audio::SetPaused(gameSuspended||gameSleeping);
                devConsoleRedraw();
                if(captured){dusk::game_clock::reset();resetTiming=true;continue;}
            }
            if(developerDumpRequested(frame))dumpDeveloperMemory(frame);
            if(!detailedDiagnostics&&frame%60==0)ordinaryFrames=ordinaryExecutionTicks=ordinaryLoopTicks=0;
            // Console evidence: without this a freeze left no log at all (the
            // log stayed in RAM until a clean exit). Durable, main thread only.
            if(!detailedDiagnostics&&frame%30==0)flushNativeLog();
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
                 unsigned remembered,dropped,passes;gxShadowCasterStatistics(remembered,dropped,passes);
                 OSReport("Native shadow casters since last report: passes=%u remembered-draws=%u dropped=%u\n",passes,remembered,dropped);}
                {unsigned tu,tc,tr;gx::textureObjectUsage(tu,tc,&tr);
                 OSReport("Native GX texture objects used=%u/%u reclaimed=%u\n",tu,tc,tr);}
                {std::uint64_t ch,cc,ci,cm;gx::compiledListStatistics(ch,cc,ci,cm);
                 OSReport("Native compiled lists: replayed=%llu compiled=%llu interpreted=%llu mismatches=%llu\n",ch,cc,ci,cm);}
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
            }
            observeDeveloperRetainedDepth(RetainedDepthStage::afterReporting);
            {
                // Bottom-screen console and automatic test, between frames.
                DevGameStatus status;
                status.frame=frame;status.stage=dComIfGp_getStartStageName();status.room=dComIfGp_roomControl_getStayNo();
                status.point=dComIfGp_getStartStagePoint();status.layer=dComIfGp_getStartStageLayer();
                auto* player=dComIfGp_getPlayer(0);
                status.playerReady=player!=nullptr;status.stageChanging=dComIfGp_isEnableNextStage();
                status.playScene=status.playerReady&&fpcM_SearchByName(fpcNm_PLAY_SCENE_e)!=nullptr;
                if(player){status.position[0]=player->current.pos.x;status.position[1]=player->current.pos.y;status.position[2]=player->current.pos.z;}
                const double scale=1000.0/nativeTickFrequency;
                status.loopMs=float((gxProfileTick()-loopStart)*scale);status.execMs=float(executionTicks*scale);
                status.paintMs=float(consolePaintTicks*scale);status.updateMs=float(consoleUpdateTicks*scale);status.gpuWaitMs=float(consoleGpuWaitTicks*scale);
                status.ticks=ticks;
                status.draws=consoleDraws;status.rejected=drawContext.rejected;status.degraded=drawContext.simplifiedDraws;
                status.textureBytes=std::uint32_t(gxTextureBytes());status.geometryBytes=std::uint32_t(gxGeometryBytes());
                if(frame%30==0){auto* heap=mDoExt_getGameHeap();consoleGameFree=heap?std::uint32_t(heap->getTotalFreeSize()):0;consoleLinearFree=linearSpaceFree();}
                status.gameHeapFree=consoleGameFree;status.linearFree=consoleLinearFree;
                if(serviceDevConsole(status)){dusk::game_clock::reset();resetTiming=true;}
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
    reportPositiveQGpu(drawContext);
    reportCameraAlignedProof(drawContext);
    gpuCanarySessionEnd();
    OSReport("Native shutdown phase=GPU completion\n");
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)fatal("Original game shutdown GPU timeout");
    shutdownDevConsole();
    // Console telemetry: per-stage summary plus the most recent frames.
    OSReport("Native frame telemetry: %s\n",writeFrameTelemetry(true)?"written to sdmc:/3ds/DuskLight":"not written");
    flushNativeLog(); // ordinary play keeps the log in RAM until here
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
