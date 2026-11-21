#pragma once

#include "story_benchmark.hpp"
#include "story_portal.hpp"
#include "d/actor/d_a_alink.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_fmap2D.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_scrn_explain.h"

namespace dusk::native::story {

// Read the target from the map already loaded by the original menu. The
// caller can only move the cursor and press buttons through the virtual pad.
inline PortalWarpSample observePortalWarp(const Step& step, daAlink_c& link) {
    PortalWarpSample sample;
    sample.departing = link.mProcID == daAlink_c::PROC_WARP &&
        std::string_view(dComIfGp_getEventManager().getRunEventName()) == "POTAL_WARPIN";
    // The original menu retains this decision while it slides out and until
    // Link consumes it. The map may already be partially or completely closed.
    sample.confirmed = dMeter2Info_getWarpStatus() == WARP_STATUS_DECIDED_e &&
        dComIfGp_SelectWarpPt_get() == step.warpPortal &&
        dMeter2Info_getWarpPlayerNo() == step.warpSpawn &&
        dMeter2Info_getWarpRoomNo() == step.destination.room &&
        std::string_view(dMeter2Info_getWarpStageName()) == step.destination.stage;
    if (sample.confirmed) return sample;
    const auto* map = dMenu_Fmap_c::MyClass;
    if (!map) {
        if (!dComIfGp_event_runCheck() && !dComIfGp_isPauseFlag() &&
            daPy_py_c::getMidnaActor() && link.checkAcceptDungeonWarpAlink(0))
            sample.input = WarpInput::openMap;
        return sample;
    }
    auto* back = map->mpDraw2DBack;
    const auto* portalData = map->mpPortalDat;
    if (map->mAlphaRatio != 1.0f || !back || !portalData) return sample;

    const auto* target = portalData->mData;
    const auto* end = target + portalData->mCount;
    while (target != end && target->mSelectWarpPt != step.warpPortal) ++target;
    if (target == end || !target->mRegionNo || target->mRegionNo > 8 ||
        !dComIfGs_isStageSwitch(target->mStageNo, target->mSwitchNo)) return sample;

    const bool selected = map->mPortalNo == target - portalData->mData;
    const auto region = target->mRegionNo - 1;
    const auto moveTo = [&](float x, float y) {
        sample.input = WarpInput::moveCursor;
        sample.cursorDx = x - back->getArrowPos2DX();
        sample.cursorDy = y - back->getArrowPos2DY();
    };
    switch (map->mProcess) {
    case dMenu_Fmap_c::PROC_ALL_MAP:
        if (!map->mIsWarpMap) sample.input = WarpInput::showPortals;
        else if (back->getSelectRegion() == region) sample.input = WarpInput::select;
        else moveTo(back->mRegionMinMapX[region] + back->field_0xf0c[region] +
                        back->mRegionMapSizeX[region] * back->mZoom * 0.5f,
                    back->mRegionMinMapY[region] + back->field_0xf2c[region] +
                        back->mRegionMapSizeY[region] * back->mZoom * 0.5f);
        break;
    case dMenu_Fmap_c::PROC_SPOT_MAP:
    case dMenu_Fmap_c::PROC_REGION_MAP:
        sample.input = WarpInput::showPortals;
        break;
    case dMenu_Fmap_c::PROC_PORTAL_WARP_MAP:
        if (back->getRegionCursor() != region) break;
        if (selected) sample.input = WarpInput::select;
        else {
            float x, y;
            back->calcAllMapPos2D(target->mPosition.x, target->mPosition.z, &x, &y);
            moveTo(x, y);
        }
        break;
    case dMenu_Fmap_c::PROC_PORTAL_WARP_SELECT:
        if (selected) {
            auto* prompt = map->mpDraw2DTop->getScrnExplainPtr();
            if (prompt && prompt->getStatus() == dMsgScrnExplain_c::STATUS_MOVE_SELECT_e)
                sample.input = prompt->getSelectCursor() == 0 ? WarpInput::confirm : WarpInput::chooseYes;
        }
        break;
    default: break;
    }
    return sample;
}

}
