#if defined(DUSK_TARGET_3DS)
#include "../../platforms/3ds/runtime/gx_profile.hpp"
// Provided by the native game loop; absent in link-only probe configurations.
extern "C" void duskNativePainterDone() __attribute__((weak));
// Real-speed scheduling (game_startup.cpp): one picture can carry several
// original logic ticks. Only the first tick paints (the lists of the previous
// picture) and only the last one enters the draw lists; each phase runs with
// the number of ticks its picture stands for, so frame-scaled draw animations
// keep real time. A link-only probe without the native loop paints and draws
// every tick.
extern "C" bool duskNativeTickPaint __attribute__((weak));
extern "C" bool duskNativeTickDraw __attribute__((weak));
extern "C" float duskNativePaintTicks __attribute__((weak));
extern "C" float duskNativeDrawTicks __attribute__((weak));
// A tick that enters no draw lists still runs the logic of the play scene's
// draw phase (collision pass, moving backgrounds, particles, frame counter:
// d_s_play.cpp). The SD switch tick-logic-off.txt restores Alpha 7, where it
// ran once per picture.
extern "C" bool duskNativeTickLogic __attribute__((weak));
// Second-core worker overlap (platforms/3ds runtime/gx_worker.hpp): the worker
// may still draw the painter's shapes during this tick. Deleting a process
// frees what they read, creating one builds heaps and may set up shared model
// data: both first have them drawn (2 deletion, 3 creation; only when one is
// pending, a no-op when nothing is left to draw).
#include "../../platforms/3ds/runtime/gx_worker_sync.hpp"
namespace {
// Every 30th drawn picture (census=1): the state the logic carries from tick
// to tick that a replay must reproduce. Two runs with the same input that
// print the same line at the script's first picture and different lines
// later diverged in logic; different lines already at the first picture mean
// they started from different states (the curtain's warm-up length depends on
// picture times: scene_transition.cpp).
void nativeLogicStateProbe();
bool nativeTickLogic() { return !&duskNativeTickLogic || duskNativeTickLogic; }
bool nativeTickPaints() { return !&duskNativeTickPaint || duskNativeTickPaint; }
bool nativeTickDraws() { return !&duskNativeTickDraw || duskNativeTickDraw; }
float nativePaintTicks() { return &duskNativePaintTicks ? duskNativePaintTicks : 1.0f; }
float nativeDrawTicks() { return &duskNativeDrawTicks ? duskNativeDrawTicks : 1.0f; }
}
#endif
#if DUSK_GX_TRACE
#include <aurora/gx_trace.h>
#include <chrono>
#endif
/**
 * f_pc_manager.cpp
 * Framework - Process Manager
 */

#include "f_pc/f_pc_manager.h"
#include "SSystem/SComponent/c_API_graphic.h"
#include "SSystem/SComponent/c_lib.h"
#include "Z2AudioLib/Z2SoundMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_error_msg.h"
#include "d/d_lib.h"
#include "d/d_particle.h"
#if defined(DUSK_TARGET_3DS)
#include "d/d_s_play.h"
#endif
#include "f_ap/f_ap_game.h"
#include "f_pc/f_pc_creator.h"
#include "f_pc/f_pc_deletor.h"
#include "f_pc/f_pc_delete_tag.h"
#include "f_pc/f_pc_create_tag.h"
#include "f_pc/f_pc_draw.h"
#include "f_pc/f_pc_fstcreate_req.h"
#include "f_pc/f_pc_line.h"
#include "f_pc/f_pc_pause.h"
#include "f_pc/f_pc_priority.h"
#include "m_Do/m_Do_controller_pad.h"

#if defined(DUSK_TARGET_3DS)
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/JUtility/JUTFader.h"
#include "d/d_native_lod.h"
#include "d/d_native_logic.h"
#include "d/d_tick_profile.h"
#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_math.h"
#include "dusk/game_clock.h"
#include "m_Do/m_Do_graphic.h"
#endif

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
#include "dusk/game_clock.h"

#include "m_Do/m_Do_graphic.h"

#include <tracy/Tracy.hpp>
#endif

void fpcM_Draw(void* i_proc) {
    fpcDw_Execute((base_process_class*)i_proc);
}

int fpcM_DrawIterater(fpcM_DrawIteraterFunc i_drawIterFunc) {
    return fpcLyIt_OnlyHere(fpcLy_RootLayer(), (fpcLyIt_OnlyHereFunc)i_drawIterFunc, NULL);
}

int fpcM_Execute(void* i_proc) {
    return fpcEx_Execute((base_process_class*)i_proc);
}

int fpcM_Delete(void* i_proc) {
    return fpcDt_Delete((base_process_class*)i_proc);
}

BOOL fpcM_IsCreating(fpc_ProcID i_id) {
    return fpcCt_IsCreatingByID(i_id);
}

void fpcM_Management(fpcM_ManagementFunc i_preExecuteFn, fpcM_ManagementFunc i_postExecuteFn) {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    MtxInit();
#if defined(DUSK_TARGET_3DS)
    // Depth peeks (the sun/lens-flare visibility points) read the frame's
    // depth buffer. Only the tick that paints starts with no picture on the
    // GPU; on the later ticks of a picture the peek waited for the whole
    // picture just submitted (console Alpha 11, F_SP104: 104-147 waits per
    // 300 pictures, 8-16 ms each). The requests stay queued (64 slots, the
    // sun asks for 5) and are read on the next painting tick: the flare
    // follows one picture later, as it already did between ticks.
    if (!fapGm_HIO_c::isCaptureScreen() && nativeTickPaints()) {
        dComIfGd_peekZdata();
    }
#else
    if (!fapGm_HIO_c::isCaptureScreen()) {
        dComIfGd_peekZdata();
    }
#endif
    fapGm_HIO_c::executeCaptureScreen();

    bool shutdownRet = dShutdownErrorMsg_c::execute();
    if (!shutdownRet) {
        static bool l_dvdError = false;

        bool dvdErrRet = dDvdErrorMsg_c::execute();
        if (!dvdErrRet) {
            if (l_dvdError) {
                dLib_time_c::startTime();
                Z2GetSoundMgr()->pauseAllGameSound(false);
                l_dvdError = false;
            }

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
            if (dusk::game_clock::g_frameTiming.separatePresentation) {
                if (JUTFader* fader = mDoGph_gInf_c::getFader()) {
                    fader->advance();
                }
            }
#endif

            // The main loop manages painting when simulation and presentation are separated.
#if !defined(DUSK_TARGET_3DS)
            IF_DUSK_BLOCK(!dusk::game_clock::g_frameTiming.separatePresentation)
#endif
            #if defined(DUSK_TARGET_3DS)
            if (nativeTickPaints()) {
                dusk::game_clock::set_presentation_frames(nativePaintTicks());
                { dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::gamePaint, true);
#endif
                cAPIGph_Painter();
#if defined(DUSK_TARGET_3DS)
                }
                dusk::game_clock::set_presentation_frames(1.0f);
                // The frame's draws are assembled: let the PICA render them
                // while the actors update and enter the next frame's lists.
                if (duskNativePainterDone) duskNativePainterDone();
            } else if (JUTFader* fader = mDoGph_gInf_c::getFader()) {
                // The skipped paint would have advanced the screen fader once.
                fader->advance();
            }
#endif
#if !defined(DUSK_TARGET_3DS)
            IF_DUSK_BLOCK_END
#endif

            if (!dPa_control_c::isStatus(1)) {
#if defined(DUSK_TARGET_3DS)
                if (g_fpcDtTg_Queue.mSize != 0) dusk::native::syncGxOverlap(2);
#endif
                fpcDt_Handler();
            } else {
                dPa_control_c::offStatus(1);
            }

            if (!fpcPi_Handler()) {
                JUT_ASSERT(353, FALSE);
            }

#if defined(DUSK_TARGET_3DS)
            if (g_fpcCtTg_Queue.mSize != 0) dusk::native::syncGxOverlap(3);
#endif
            if (!fpcCt_Handler()) {
                JUT_ASSERT(357, FALSE);
            }

            if (i_preExecuteFn != NULL) {
                i_preExecuteFn();
            }

            if (!fapGm_HIO_c::isCaptureScreen()) {
#if DUSK_GX_TRACE
                const auto traceUpdateStart = std::chrono::steady_clock::now();
#endif
                #if defined(DUSK_TARGET_3DS)
                { dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::gameUpdate, true);
                // Level of detail "skin": a tick whose picture is not drawn
                // leaves the skin matrices to the draw (J3DModel::calc).
                J3DModel::sNativeDeferEnvelope = dNativeLod::config().skin && !nativeTickDraws();
                if (dNativeLogic::g_split) {
                    // The cost of one section timer on this machine: 32 empty
                    // pairs per tick ("profile.emptyPair", ms / calls).
                    for (int i = 0; i < 32; i++) {
                        DUSK_SPLIT_SPAN(split_empty, "profile.emptyPair");
                        split_empty.stop();
                    }
                }
#endif
                fpcEx_Handler((fpcLnIt_QueueFunc)fpcM_Execute);
#if defined(DUSK_TARGET_3DS)
                J3DModel::sNativeDeferEnvelope = false;
                }
#endif
#if DUSK_GX_TRACE
                dusk_gx_trace_cpu(0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceUpdateStart).count());
#endif
            }

#if defined(DUSK_TARGET_3DS)
            const bool drawTick = nativeTickDraws();
            if (drawTick && (!fapGm_HIO_c::isCaptureScreen() || fapGm_HIO_c::getCaptureScreenDivH() != 1)) {
#else
            if (!fapGm_HIO_c::isCaptureScreen() || fapGm_HIO_c::getCaptureScreenDivH() != 1) {
#endif
#if DUSK_GX_TRACE
                const auto traceDrawStart = std::chrono::steady_clock::now();
#endif
                #if defined(DUSK_TARGET_3DS)
                dusk::game_clock::set_presentation_frames(nativeDrawTicks());
                dNativeLod::beginDrawPhase();
                { dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::gameDraw, true);
#endif
                fpcDw_Handler((fpcDw_HandlerFuncFunc)fpcM_DrawIterater, (fpcDw_HandlerFunc)fpcM_Draw);
#if defined(DUSK_TARGET_3DS)
                }
                dusk::game_clock::set_presentation_frames(1.0f);
                dNativeLogic::endPicture();
                nativeLogicStateProbe();
#endif
#if DUSK_GX_TRACE
                dusk_gx_trace_cpu(1, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceDrawStart).count());
#endif
            }

#if defined(DUSK_TARGET_3DS)
            if (!drawTick && nativeTickLogic()) {
                static const unsigned logicProfile = dusk::native::registerGxNamedProfile("tick.sceneLogic");
                dusk::native::GxNamedProfileScope named(logicProfile);
                dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::gameUpdate, true);
                fpcM_DrawIterater(dScnPly_LogicOnlyDraw);
            }
            if (drawTick) dComIfGp_drawSimpleModel();
#else
            IF_DUSK(dComIfGp_drawSimpleModel());
#endif

            if (i_postExecuteFn != NULL) {
                i_postExecuteFn();
            }

            IF_NOT_DUSK(dComIfGp_drawSimpleModel());
        } else if (!l_dvdError) {
            dLib_time_c::stopTime();
            Z2GetSoundMgr()->pauseAllGameSound(true);
#if PLATFORM_GCN
#define FPCM_MANAGEMENT_GAMEPAD_COUNT 1
#elif PLATFORM_SHIELD && !DEBUG
#define FPCM_MANAGEMENT_GAMEPAD_COUNT 0
#else
#define FPCM_MANAGEMENT_GAMEPAD_COUNT 4
#endif
            for (u32 i = 0; i < FPCM_MANAGEMENT_GAMEPAD_COUNT; i++) {
                mDoCPd_c::stopMotorWaveHard(i);
            }
            l_dvdError = true;
        }
    }
}

#if defined(DUSK_TARGET_3DS)
namespace {
void nativeLogicStateProbe() {
    static unsigned picture;
    if (++picture % 30 != 0 || !dNativeLogic::config().census) {
        return;
    }
    int rnd[6];
    cM_rndState(rnd);
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player != NULL) {
        OSReport("Native logic state picture=%u counter=%lu rnd=%d,%d,%d rnd2=%d,%d,%d link=%.3f,%.3f,%.3f "
                 "angle=%d speed=%.3f\n",
                 picture, (unsigned long)g_Counter.mCounter0, rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5],
                 player->current.pos.x, player->current.pos.y, player->current.pos.z,
                 player->shape_angle.y, player->speedF);
    } else {
        OSReport("Native logic state picture=%u counter=%lu rnd=%d,%d,%d rnd2=%d,%d,%d no player\n", picture,
                 (unsigned long)g_Counter.mCounter0, rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    }
}
}  // namespace
#endif

void fpcM_Init() {
    static layer_class rootlayer;
    static node_list_class queue[10];

    fpcLy_Create(&rootlayer, NULL, queue, 10);
    fpcLn_Create();
}

base_process_class* fpcM_FastCreate(s16 i_procname, FastCreateReqFunc i_createReqFunc,
                                    void* i_createData, void* i_append) {
    return fpcFCtRq_Request(fpcLy_CurrentLayer(), i_procname, (fstCreateFunc)i_createReqFunc,
                            i_createData, i_append);
}

int fpcM_IsPause(void* i_proc, u8 i_flag) {
    return fpcPause_IsEnable((base_process_class*)i_proc, i_flag & 0xFF);
}

void fpcM_PauseEnable(void* i_proc, u8 i_flag) {
    fpcPause_Enable((process_node_class*)i_proc, i_flag & 0xFF);
}

void fpcM_PauseDisable(void* i_proc, u8 i_flag) {
    fpcPause_Disable((process_node_class*)i_proc, i_flag & 0xFF);
}

void* fpcM_JudgeInLayer(fpc_ProcID i_layerID, fpcCtIt_JudgeFunc i_judgeFunc, void* i_data) {
    layer_class* layer = fpcLy_Layer(i_layerID);
    if (layer != NULL) {
        void* ret = fpcCtIt_JudgeInLayer(i_layerID, i_judgeFunc, i_data);
        if (ret == NULL) {
            return fpcLyIt_Judge(layer, i_judgeFunc, i_data);
        }
        return ret;
    }

    return NULL;
}
