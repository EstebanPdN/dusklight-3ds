#pragma once

#include "story_chapter_proof.hpp"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_b_gg.h"
#include "d/actor/d_a_b_gnd.h"
#include "d/actor/d_a_b_tn.h"
#include "d/actor/d_a_e_pz.h"
#include "d/actor/d_a_e_wb.h"
#include "d/actor/d_a_e_rd.h"
#include "d/actor/d_a_obj_fan.h"
#include "d/actor/d_a_obj_swhang.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_demo.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"

namespace dusk::native::story {

inline bool usingChapterAbility(Ability ability, const daAlink_c& link) {
    switch (ability) {
    case Ability::horse: return link.checkHorseRide() && std::abs(link.speedF) > 1.0f;
    case Ability::wolfSense: return link.checkWolf() && link.checkWolfEyeUp();
    case Ability::boomerang: return link.checkBoomerangThrowAnime();
    case Ability::ironBoots: return link.checkEquipHeavyBoots();
    case Ability::bow: return link.checkBowShootAnime();
    case Ability::zoraSwim:
        return link.checkZoraWearAbility() && link.checkSwimMoveProc() &&
            (std::abs(link.speedF) > 1.0f || std::abs(link.speed.y) > 1.0f);
    case Ability::clawshot:
        return dComIfGp_getSelectItem(SELECT_ITEM_X) == dItemNo_HOOKSHOT_e &&
            link.checkHookshotShootReturnMode();
    case Ability::spinner: return link.checkSpinnerRide();
    case Ability::ironBall: return link.checkIronBallThrowMode();
    case Ability::dominionRod: return link.checkCopyRodThrowAfter();
    case Ability::doubleClawshots:
        return dComIfGp_getSelectItem(SELECT_ITEM_X) == dItemNo_W_HOOKSHOT_e &&
            link.checkHookshotShootReturnMode();
    case Ability::lightSword:
        return link.checkLightMasterSwordEquip() && link.checkItemSwordEquip() &&
            link.getCutType() != daPy_py_c::CUT_TYPE_NONE;
    default: return false;
    }
}

inline DarknutStance darknutStance(daB_TN_c& darknut) {
    using Darknut = daB_TN_c;
    if (darknut.mActionMode1 <= Darknut::ACT_OPENING) return DarknutStance::entering;
    if (darknut.mActionMode1 == Darknut::ACT_ATTACKSHIELDH) return DarknutStance::swinging;
    if (darknut.mActionMode1 == Darknut::ACT_ATTACKH) {
        const float frame = darknut.mpModelMorf2->getFrame();
        // executeAttackH drops the body guard on entry. Its sword first goes
        // live at frame 18 (short swing) or 22 (long swing).
        if (darknut.mActionMode2 == Darknut::ACTION2_0_e || (frame >= 14.0f && frame < 30.0f))
            return DarknutStance::swinging;
    }
    if (darknut.mActionMode1 == Darknut::ACT_CHANGEDEMO || darknut.mActionMode1 == Darknut::ACT_ENDING ||
        darknut.mInvincibilityTimer != 0 || darknut.mSphA[1].ChkTgShield())
        return DarknutStance::guarded;
    return DarknutStance::exposed;
}

inline AeralfosControl aeralfosControl(const Step& step, daAlink_c& link) {
    auto* handle = static_cast<daObjSwHang_c*>(fopAcM_SearchByName(fpcNm_Obj_SwHang_e));
    if (!handle || fopAcM_GetRoomNo(handle) != step.start.room ||
        fpcM_IsCreating(fopAcM_GetID(handle))) return {};

    if (link.getHookshotRoofWaitActor() == handle && fopAcM_checkHookCarryNow(handle) &&
        (link.mProcID == daAlink_c::PROC_HOOKSHOT_ROOF_WAIT ||
         link.mProcID == daAlink_c::PROC_HOOKSHOT_ROOF_BOOTS)) {
        if (!link.checkEquipHeavyBoots()) {
            if (link.mProcID == daAlink_c::PROC_HOOKSHOT_ROOF_WAIT) return {AeralfosInput::boots};
        } else if (fopAcM_isSwitch(handle, handle->getSwNo())) {
            // The handle retracts the wind column. The fan also needs time
            // to drop its fast-rotation collision before Link lets go.
            auto* fan = static_cast<daObjFan_c*>(fopAcM_SearchByName(fpcNm_Obj_Fan_e));
            if (fan && fopAcM_GetRoomNo(fan) == step.start.room && fan->field_0xacc == 0)
                return {AeralfosInput::release};
        }
        return {};
    }

    if (link.mProcID == daAlink_c::PROC_HOOKSHOT_FLY || link.checkHookshotShootReturnMode()) return {};
    if (link.mProcID == daAlink_c::PROC_HOOKSHOT_SUBJECT) {
        if (!link.checkHookshotWait()) return {};
        if (!dComIfGp_checkCameraAttentionStatus(0, 0x10) || !link.checkHookshotReadyAnime())
            return {AeralfosInput::draw};
        // bura7C (type 4) registers its Clawshot sphere 150 units above mHangPos.
        cXyz aim = handle->getHangPos() - link.getHsChainRootPos();
        aim.y += 150.0f;
        return {AeralfosInput::aim,
            s16(aim.atan2sX_Z() - link.shape_angle.y - link.getBodyAngleY()),
            s16(aim.atan2sY_XZ() - link.getBodyAngleX())};
    }

    // Room 51's upper KCL floor ends at z=447 on the entrance axis.
    // Stop on the approach with a clear line up to the handle.
    const auto& pos = link.current.pos;
    if (pos.y < 2490.0f || !link.mLinkAcch.ChkGroundHit()) return {};
    if (pos.z > step.z + 35.0f) return {AeralfosInput::approach};
    return {AeralfosInput::draw};
}

struct ChapterWatch {
    const Step* step{};
    unsigned scene{};
    ChapterProof proof{};
    ChapterControl control{};
    int subjectProfile{-1};
    fpc_ProcID observedActor{fpcM_ERROR_PROCESS_ID_e};
    fpc_ProcID aeralfosFinishingActor{fpcM_ERROR_PROCESS_ID_e};

    void observe(const Step& expected) {
        const auto goal = expected.chapter.goal;
        if (goal <= ChapterGoal::exit) return;
        const Place here{dComIfGp_getStartStageName(), dComIfGp_roomControl_getStayNo(),
            dComIfGp_getStartStagePoint(), dComIfGp_getStartStageLayer()};
        auto* play = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
        auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
        if (!play || !link || bool(link->checkWolf()) != expected.wolf) return;
        const auto sceneId = fpcM_GetID(play);
        if (!matches(expected.start, here) &&
            !(inIntroEndRoom(expected, here) && step == &expected &&
              scene == sceneId && proof.evidence.demoTicks >= 7)) return;
        if (step != &expected || scene != sceneId) {
            *this = {};
            step = &expected;
            scene = sceneId;
            if (goal >= ChapterGoal::bossIntro) {
                if (const auto* name = dStage_searchName(expected.chapter.subject))
                    subjectProfile = name->procname;
            }
        }

        const bool eventRunning = dComIfGp_event_runCheck();
        const bool phantomZant = expected.chapter.introInput == IntroInput::phantomZant;
        const bool aeralfos = expected.chapter.introInput == IntroInput::aeralfos;
        ChapterSample sample{};
        sample.eventRunning = eventRunning;
        sample.demoCanEnd = !phantomZant && !aeralfos;
        const bool confirmingAeralfosControl = control.aeralfos.input == AeralfosInput::confirmControl;
        control = {};
        observedActor = fpcM_ERROR_PROCESS_ID_e;
        control.ironBootsOn = link->checkEquipHeavyBoots();
        if (aeralfos && !eventRunning && !link->checkPlayerDemoMode() && !proof.evidence.demoTicks)
            control.aeralfos = aeralfosControl(expected, *link);
        if (expected.chapter.introInput == IntroInput::dekuToad) {
            if (dComIfGp_getCamera(0))
                control.firstPerson = dCam_getBody()->Mode() == 4;
        }
        if (goal == ChapterGoal::event) {
            if (eventRunning) {
                const char* name = dComIfGp_getPEvtManager()->getRunEventName();
                sample.demoActive = name && std::string_view(expected.chapter.subject) == name;
                // The package clock also advances while a message suspends
                // JStudio. The retail director uses this clock for timeline waits.
                if (sample.demoActive && dDemo_c::m_object) sample.timelineFrame = dDemo_c::getFrameNoMsg();
            }
        } else if (goal == ChapterGoal::ability) {
            sample.abilityActive = !eventRunning && usingChapterAbility(expected.chapter.ability, *link);
        } else if (subjectProfile >= 0) {
            struct Search {
                int profile, room;
                const daAlink_c& link;
                bool phantomZant;
                bool streetArchery;
                fopAc_ac_c* found{};
                float distance{9000.0f};
            } search{subjectProfile, here.room, *link, phantomZant,
                goal == ChapterGoal::hit && expected.chapter.ability == Ability::bow};
            fopAcIt_Judge(+[](void* candidate, void* opaque) -> void* {
                auto* actor = static_cast<fopAc_ac_c*>(candidate);
                auto& target = *static_cast<Search*>(opaque);
                if (fopAcM_GetName(actor) != target.profile || fpcM_IsCreating(fopAcM_GetID(actor)))
                    return nullptr;
                // Zant's single ACTR belongs to the stage and follows the
                // player between arenas. His opening starts in room 50.
                const int room = fopAcM_GetRoomNo(actor);
                if (room != target.room &&
                    !(room == -1 && target.profile == fpcNm_B_ZANT_e))
                    return nullptr;
                // E_pz also names teleport markers and spawned projectiles.
                if (target.phantomZant && static_cast<daE_PZ_c*>(actor)->arg0 > 1)
                    return nullptr;
                // Walk down the street toward a guard at Link's level. A
                // roof guard can be closer in XZ even under an overhang.
                if (target.streetArchery && std::abs(actor->current.pos.y - target.link.current.pos.y) > 180.0f)
                    return nullptr;
                const float distance = std::hypot(actor->current.pos.x - target.link.current.pos.x,
                    actor->current.pos.z - target.link.current.pos.z);
                if (fopAcM_GetID(actor) == target.link.getAtnActorID()) {
                    target.found = actor;
                    target.distance = distance;
                    return actor;
                }
                if (distance < target.distance) {
                    target.found = actor;
                    target.distance = distance;
                }
                return nullptr;
            }, &search);
            if (auto* actor = search.found) {
                sample.actor = fopAcM_GetID(actor);
                observedActor = sample.actor;
                sample.actorPresent = true;
                sample.health = actor->health;
                if (goal == ChapterGoal::hit && expected.chapter.ability == Ability::bow &&
                    subjectProfile == fpcNm_E_RD_e) {
                    const auto& bulblin = *reinterpret_cast<const e_rd_class*>(actor);
                    // at_power_check distinguishes Link's arrow from enemy arrows.
                    sample.playerArrowDamage = bulblin.damage_timer > 0 && bulblin.at_info.mHitType == HIT_TYPE_ARROW;
                }
                sample.targeted = link->getAtnActorID() == sample.actor && search.distance < 750.0f;
                control.bowTargeted = sample.targeted && link->checkAttentionLock();
                sample.attack = !eventRunning && link->getCutType() != daPy_py_c::CUT_TYPE_NONE;
                sample.demoActive = eventRunning && actor->eventInfo.checkCommandDemoAccrpt();
                if (phantomZant) {
                    const auto& boss = *static_cast<const daE_PZ_c*>(actor);
                    const bool opening = boss.mActionMode == daE_PZ_c::ACTION_OPENING_DEMO_e;
                    sample.demoActive &= opening;
                    // The opening resets and reorders its event at the camera handoff.
                    sample.demoCanEnd = !opening && !link->checkPlayerDemoMode();
                }
                if (aeralfos) {
                    const auto& boss = *static_cast<const daB_GG_c*>(actor);
                    sample.demoActive &= boss.mCamMode != 0;
                    // St_DemoAction releases the camera when its final
                    // two-shot (mode 8) runs out on the following tick.
                    if (sample.demoActive && boss.mCamMode == 8 && boss.mTimers[1] == 1) {
                        aeralfosFinishingActor = sample.actor;
                        OSReport("Native Aeralfos opening: actor=%u final camera mode=8 timer=1\n",
                            unsigned(sample.actor));
                    }
                    if (sample.actor == aeralfosFinishingActor && boss.mCamMode == 0 &&
                        !eventRunning && !link->checkPlayerDemoMode() && !proof.evidence.demoEnded) {
                        control.aeralfos = {AeralfosInput::confirmControl};
                        // Verify an ordinary player movement procedure after the camera handoff.
                        sample.demoCanEnd = confirmingAeralfosControl && link->mProcID == daAlink_c::PROC_MOVE &&
                            std::abs(link->speedF) > 1.0f;
                        if (sample.demoCanEnd)
                            OSReport("Native Aeralfos opening: actor=%u camera released, Link moving at %.1f\n",
                                unsigned(sample.actor), double(link->speedF));
                    }
                }
                if (eventRunning && expected.chapter.introInput == IntroInput::argorok) {
                    // B_dr stores the scene director it creates in parentActorID.
                    auto* director = fopAcM_SearchByID(actor->parentActorID);
                    sample.demoActive = director && fopAcM_GetName(director) == fpcNm_L7ODR_e &&
                        director->eventInfo.checkCommandDemoAccrpt();
                }
                if (sample.demoActive && expected.chapter.introInput == IntroInput::kingBulblin) {
                    // Both camp events run on the king's eventInfo. E_wb runs
                    // the gate camera in mode 71 and his battle intro in 51–52.
                    auto* boar = reinterpret_cast<e_wb_class*>(fopAcM_SearchByName(fpcNm_E_WB_e));
                    sample.demoActive = boar && fopAcM_GetRoomNo(&boar->enemy) == expected.start.room &&
                        (boar->demo_mode == 51 || boar->demo_mode == 52);
                }
                control.targetPresent = true;
                control.targetX = actor->current.pos.x;
                control.targetZ = actor->current.pos.z;
                control.targetDistance = search.distance;
                if (goal == ChapterGoal::hit && subjectProfile == fpcNm_B_TN_e) {
                    auto& darknut = *static_cast<daB_TN_c*>(actor);
                    sample.acceptedSwordHits = darknut.mAcceptedPlayerSwordHits;
                    const bool locked = sample.targeted && link->checkAttentionLock();
                    control.darknut = {darknutStance(darknut), locked, bool(link->checkItemSwordEquip())};
                    control.darknutJumpAttackPrompt = locked &&
                        dComIfGp_getDoStatus() == BUTTON_STATUS_ATTACK;
                }
                if (goal == ChapterGoal::duel) {
                    auto& ganondorf = *static_cast<b_gnd_class*>(actor);
                    sample.finishingBlow = link->getCutType() == daPy_py_c::CUT_TYPE_DOWN;
                    // B_gnd enters modes 61–65 after accepting its finishing
                    // demo. Mode 65 requests room exit 0 into demo29.
                    sample.duelEnding = sample.demoActive &&
                        ganondorf.mDemoCamMode >= 61 && ganondorf.mDemoCamMode <= 65;
                    control.finishingPrompt = sample.targeted && dComIfGp_getDoStatus() == BUTTON_STATUS_FINISH;
                    control.swordLock = ganondorf.mDemoCamMode == 50 || ganondorf.mDemoCamMode == 51;
                    control.duelTargeted = sample.targeted;
                    control.duelSwordDrawn = link->checkItemSwordEquip();
                    // B_gnd's on-foot WAIT starts in walking mode 1.
                    control.duelBossWalking = ganondorf.mActionMode == 10 && ganondorf.mMoveMode == 1;
                    control.duelClashPrompt = sample.targeted && ganondorf.checkAttackChance();
                    // Winning the clash sets this until the stagger ends or Ganondorf falls.
                    control.duelClashOpening = ganondorf.field_0xc7c != 0;
                    control.duelDown = ganondorf.checkDownFlg();
                }
            }
        }
        proof.record(sample);
    }
};

}
