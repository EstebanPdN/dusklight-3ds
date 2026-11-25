#include "helpers/platform_features.h"
/**
 * m_Do_ext.cpp
 * Model, Animation, and Heap Functions
 */

#include "d/dolzel.h" // IWYU pragma: keep

#include <gf/GFPixel.h>
#include <gx.h>
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphLoader/J3DMaterialFactory.h"
#include "JSystem/JKernel/JKRAssertHeap.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JUtility/JUTCacheFont.h"
#include "JSystem/JUtility/JUTResFont.h"
#include "Z2AudioLib/Z2Creature.h"
#include "d/d_com_inf_game.h"
#include "global.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_main.h"
#include "m_Do/m_Do_mtx.h"
#include <cstdio>
#include <cstring>
#if defined(DUSK_TARGET_3DS)
#include "../../platforms/3ds/runtime/texture_resources.hpp"
#include "../../platforms/3ds/runtime/gx_leaf_wire.hpp"
#include "../../platforms/3ds/runtime/gx_stream.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "d/d_native_lod.h"
#include "../../platforms/3ds/runtime/model_lod.hpp"
#include "../../platforms/3ds/runtime/perf_features.hpp"
#include "JSystem/J3DGraphBase/J3DNativeInstances.h"
#include "d/d_kankyo.h"
#include "m_Do/m_Do_lib.h"
#endif

#if TARGET_PC
#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/line.h"
#include "dusk/interp/material.h"
#if !defined(DUSK_TARGET_3DS)
#include "dusk/logging.h"
#endif
#include "dusk/version.hpp"
#endif

DUSK_GAME_DATA u8 mDoExt::CurrentHeapAdjustVerbose;
DUSK_GAME_DATA u8 mDoExt::HeapAdjustVerbose;
DUSK_GAME_DATA u8 mDoExt::HeapAdjustQuiet;

#if TARGET_PC
namespace {
enum InterpKind : u8 {
    LINE_NONE,
    LINE_UNIFORM_WIDTH,
    LINE_PER_POINT_WIDTH,
};
}  // namespace
#endif

static void mDoExt_setJ3DData(Mtx mtx, const J3DTransformInfo* transformInfo, u16 param_2) {
    bool local_28;
    if (cM3d_IsZero(transformInfo->mScale.x - 1.0f) && cM3d_IsZero(transformInfo->mScale.y - 1.0f) && cM3d_IsZero(transformInfo->mScale.z - 1.0f)) {
        j3dSys.getModel()->setScaleFlag(param_2, 1);
        local_28 = true;
    } else {
        j3dSys.getModel()->setScaleFlag(param_2, 0);
        local_28 = false;
    }
    mtx[0][3] = transformInfo->mTranslate.x;
    mtx[1][3] = transformInfo->mTranslate.y;
    mtx[2][3] = transformInfo->mTranslate.z;

    if (!local_28) {
        f32* mtx_p = (f32*)mtx;
        *mtx_p *= transformInfo->mScale.x;
        mtx_p++;
        *mtx_p *= transformInfo->mScale.y;
        mtx_p++;
        *mtx_p *= transformInfo->mScale.z;
        mtx_p += 2;
        *mtx_p *= transformInfo->mScale.x;
        mtx_p++;
        *mtx_p *= transformInfo->mScale.y;
        mtx_p++;
        *mtx_p *= transformInfo->mScale.z;
        mtx_p += 2;
        *mtx_p *= transformInfo->mScale.x;
        mtx_p++;
        *mtx_p *= transformInfo->mScale.y;
        mtx_p++;
        *mtx_p *= transformInfo->mScale.z;
    }
    if (j3dSys.getModel()->getModelData()->getJointNodePointer(param_2)->getScaleCompensate() == 1) {
        f32* mtx_p = (f32*)mtx;
        Vec sp0C;
        sp0C.x = 1.0f / J3DSys::mParentS.x;
        sp0C.y = 1.0f / J3DSys::mParentS.y;
        sp0C.z = 1.0f / J3DSys::mParentS.z;
        *mtx_p++ *= sp0C.x;
        *mtx_p++ *= sp0C.x;
        *mtx_p++ *= sp0C.x;
        UNUSED(*mtx_p++);
        *mtx_p++ *= sp0C.y;
        *mtx_p++ *= sp0C.y;
        *mtx_p++ *= sp0C.y;
        UNUSED(*mtx_p++);
        *mtx_p++ *= sp0C.z;
        *mtx_p++ *= sp0C.z;
        *mtx_p++ *= sp0C.z;
    }
    mDoMtx_concat(J3DSys::mCurrentMtx, mtx, J3DSys::mCurrentMtx);
    j3dSys.getModel()->setAnmMtx(param_2, J3DSys::mCurrentMtx);
    J3DSys::mParentS.x = transformInfo->mScale.x;
    J3DSys::mParentS.y = transformInfo->mScale.y;
    J3DSys::mParentS.z = transformInfo->mScale.z;
}

static BOOL isCurrentSolidHeap() {
    JKRHeap* heap = JKRGetCurrentHeap();
    if (heap->getHeapType() != 'SLID') {
        OS_REPORT_ERROR("ソリッドヒープちゃうがな！\n");
        return FALSE;
    }
    return TRUE;
}

int mDoExt_baseAnm::initPlay(s16 i_frameMax, int i_attribute, f32 i_rate, s16 i_startF,
                             s16 i_endF) {
    mFrameCtrl.setStart(i_startF);

    if (i_endF < 0) {
        mFrameCtrl.init(i_frameMax);
    } else {
        mFrameCtrl.init(i_endF);
    }

    mFrameCtrl.setAttribute(i_attribute);
    mFrameCtrl.setRate(i_rate);

    if (i_rate >= 0.0f) {
        mFrameCtrl.setFrame(i_startF);
    } else {
        mFrameCtrl.setFrame(mFrameCtrl.getEnd());
    }

    mFrameCtrl.setLoop(mFrameCtrl.getFrame());

    return 1;
}

int mDoExt_baseAnm::play() {
    mFrameCtrl.update();
    return isStop();
}

int mDoExt_bpkAnm::init(J3DMaterialTable* i_matTable, J3DAnmColor* i_bpk, int i_anmPlay,
                        int i_attribute, f32 i_rate, s16 i_startF, s16 i_endF) {
    JUT_ASSERT(371, (i_anmPlay == FALSE || getFrameCtrl() != NULL || isCurrentSolidHeap()) && i_matTable != NULL && i_bpk != NULL);

    DUSK_DESKTOP_ONLY(dusk::interp::material::reset(getFrameCtrl()));
    mpAnm = i_bpk;
    mpAnm->searchUpdateMaterialID(i_matTable);

    if (i_anmPlay) {
        return initPlay(mpAnm->getFrameMax(), i_attribute < 0 ? mpAnm->getAttribute() : i_attribute,
                        i_rate, i_startF, i_endF);
    }

    return 1;
}

void mDoExt_bpkAnm::entry(J3DMaterialTable* i_matTable, f32 i_frame) {
    mpAnm->setFrame(i_frame);
    DUSK_DESKTOP_ONLY(dusk::interp::material::bind(mpAnm, getFrameCtrl()));
    i_matTable->entryMatColorAnimator(mpAnm);
}

int mDoExt_btpAnm::init(J3DMaterialTable* i_matTable, J3DAnmTexPattern* i_btp, int i_anmPlay,
                        int i_attribute, f32 i_rate, s16 i_startF, s16 i_endF) {
    JUT_ASSERT(420, (i_anmPlay == FALSE || getFrameCtrl() != NULL || isCurrentSolidHeap()) && i_matTable != NULL && i_btp != NULL);

    mpAnm = i_btp;
    mpAnm->searchUpdateMaterialID(i_matTable);

    if (i_anmPlay) {
        return initPlay(mpAnm->getFrameMax(), i_attribute < 0 ? mpAnm->getAttribute() : i_attribute, i_rate, i_startF, i_endF);
    }

    return 1;
}

void mDoExt_btpAnm::entry(J3DMaterialTable* i_matTable, s16 i_frame) {
    mpAnm->setFrame(i_frame);
    i_matTable->entryTexNoAnimator(mpAnm);
}

int mDoExt_btkAnm::init(J3DMaterialTable* i_matTable, J3DAnmTextureSRTKey* i_btk, int i_anmPlay,
                        int i_attribute, f32 i_rate, s16 i_startF, s16 i_endF) {
    JUT_ASSERT(468, (i_anmPlay == FALSE || getFrameCtrl() != NULL || isCurrentSolidHeap()) && i_matTable != NULL && i_btk != NULL);

    DUSK_DESKTOP_ONLY(dusk::interp::material::reset(getFrameCtrl()));
    mpAnm = i_btk;
    mpAnm->searchUpdateMaterialID(i_matTable);

    if (i_anmPlay) {
        return initPlay(mpAnm->getFrameMax(), i_attribute < 0 ? mpAnm->getAttribute() : i_attribute, i_rate, i_startF, i_endF);
    }

    return 1;
}

void mDoExt_btkAnm::entry(J3DMaterialTable* i_matTable, f32 i_frame) {
    mpAnm->setFrame(i_frame);
    DUSK_DESKTOP_ONLY(dusk::interp::material::bind(mpAnm, getFrameCtrl()));
    i_matTable->entryTexMtxAnimator(mpAnm);
}

int mDoExt_brkAnm::init(J3DMaterialTable* i_matTable, J3DAnmTevRegKey* i_brk, int i_anmPlay,
                        int i_attribute, f32 i_rate, s16 i_startF, s16 i_endF) {
    JUT_ASSERT(516, (i_anmPlay == FALSE || getFrameCtrl() != NULL || isCurrentSolidHeap()) && i_matTable != NULL && i_brk != NULL);

    DUSK_DESKTOP_ONLY(dusk::interp::material::reset(getFrameCtrl()));
    mpAnm = i_brk;
    mpAnm->searchUpdateMaterialID(i_matTable);

    if (i_anmPlay) {
        return initPlay(mpAnm->getFrameMax(), i_attribute < 0 ? mpAnm->getAttribute() : i_attribute, i_rate, i_startF, i_endF);
    }

    return 1;
}

void mDoExt_brkAnm::entry(J3DMaterialTable* i_matTable, f32 i_frame) {
    mpAnm->setFrame(i_frame);
    DUSK_DESKTOP_ONLY(dusk::interp::material::bind(mpAnm, getFrameCtrl()));
    i_matTable->entryTevRegAnimator(mpAnm);
}

void dummy3(J3DModel* i_model, void* i_bva, bool i_modify) {
    JUT_ASSERT(0, (i_modify || isCurrentSolidHeap()) && i_model != NULL && i_bva != NULL);
}

int mDoExt_bckAnm::init(J3DAnmTransform* i_bck, int i_play, int i_attr, f32 i_rate,
                        s16 i_startF, s16 i_endF, bool i_modify) {
    JUT_ASSERT(614, (i_modify || isCurrentSolidHeap()) && i_bck != NULL);
    mAnmTransform = i_bck;
    if (!i_modify) {
        mAnm = JKR_NEW J3DMtxCalcAnimation<J3DMtxCalcAnimationAdaptorDefault<J3DMtxCalcCalcTransformMaya>, J3DMtxCalcJ3DSysInitMaya>(mAnmTransform);
        if (!mAnm) {
            return 0;
        }
    } else {
        mAnm->setAnmTransform(mAnmTransform);
    }
    if (i_play) {
        return initPlay(mAnmTransform->getFrameMax(), i_attr < 0 ? mAnmTransform->getAttribute() : i_attr, i_rate, i_startF, i_endF);
    }
    return 1;
}

void mDoExt_bckAnm::changeBckOnly(J3DAnmTransform* i_bck) {
    mAnmTransform = i_bck;
    JUT_ASSERT(646, mAnm != 0);
    mAnm->setAnmTransform(mAnmTransform);
}

void dummy4(J3DModel* i_model, void* i_bxa, bool i_modify) {
    JUT_ASSERT(0, (i_modify || isCurrentSolidHeap()) && i_model != NULL && i_bxa != NULL);
}

void mDoExt_bckAnm::entry(J3DModelData* i_modelData, f32 i_frame) {
    mAnmTransform->setFrame(i_frame);
    i_modelData->getJointNodePointer(0)->setMtxCalc(mAnm);
}

void mDoExt_bckAnm::entryJoint(J3DModelData* i_modelData, u16 i_jntNo, f32 i_frame) {
    mAnmTransform->setFrame(i_frame);
    i_modelData->getJointNodePointer(i_jntNo)->setMtxCalc(mAnm);
}

int mDoExt_blkAnm::init(J3DDeformData* i_deformData, J3DAnmCluster* i_blk, int i_anmPlay,
                        int i_attribute, f32 i_rate, s16 i_start, s16 param_6) {
    JUT_ASSERT(747, (i_anmPlay == FALSE || getFrameCtrl() != NULL || isCurrentSolidHeap()) && i_deformData != NULL && i_blk != NULL);

    mpAnm = i_blk;
    i_deformData->setAnm(mpAnm);

    if (i_anmPlay) {
        return initPlay(mpAnm->getFrameMax(), i_attribute < 0 ? mpAnm->getAttribute() : i_attribute, i_rate, i_start, param_6);
    }

    return 1;
}

static void mDoExt_changeMaterial(J3DModel* i_model) {
    J3DModelData* model_data = i_model->getModelData();

    for (u16 i = 0; i < model_data->getMaterialNum(); i++) {
        model_data->getMaterialNodePointer(i)->change();
    }
}

void mDoExt_modelTexturePatch(J3DModelData* i_modelData) {
    j3dSys.setTexture(i_modelData->getTexture());

    for (u16 i = 0; i < i_modelData->getMaterialNum(); i++) {
        J3DMaterial* mat = i_modelData->getMaterialNodePointer(i);
        J3DTevBlock* tev = mat->getTevBlock();
        J3DDisplayListObj* dlObj = mat->getSharedDisplayListObj();

        BOOL ret = OSDisableInterrupts();
        GDInitGDLObj(&J3DDisplayListObj::sGDLObj, dlObj->getDisplayList(0),
                     dlObj->getDisplayListSize());
        GDSetCurrent(&J3DDisplayListObj::sGDLObj);
        tev->patchTexNoAndTexCoordScale();
        OSRestoreInterrupts(ret);
        GDSetCurrent(NULL);
    }
}

static void dummy1(J3DModel* model, J3DMaterial* material) {
    J3DMatPacket* packet = model->getMatPacket(0);
    packet->beginPatch();
    material->getTevBlock()->patch();
    material->getColorBlock()->patchLight();
    material->getTexGenBlock();
    material->getPEBlock()->patch();
    packet->endPatch();
}

static inline void modelMtxErrorCheck(J3DModel* i_model) {
#if DEBUG
    if (i_model->getBaseTRMtx()[0][0] == FLT_MAX) {
        i_model->getBaseTRMtx()[0][0] = 1.0f;
        JUT_WARN(883, "%s", "Model Matrix Not Initialize !\n");
    }
#endif
}

#if defined(DUSK_TARGET_3DS)
// Native level of detail, rule "parts" (include/d/d_native_lod.h): the parts
// of an ordinary actor's model too small on the top screen are not entered.
// The game's own hidden shapes stay as they are; the shapes this rule hides
// are shown again right after the entry (the model data is shared by every
// instance of the model).
namespace {
struct LodPicture {
    unsigned picture = 0;
    float unitPixels = 208.0f;
    bool event = false;
    const void* player = nullptr;
    const void* horse = nullptr;
    const void* lockon = nullptr;
    const void* owner = nullptr;
    bool ownerEligible = false;
};
LodPicture l_lodPicture;

bool lodOwnerEligible() {
    LodPicture& p = l_lodPicture;
    const void* owner = dNativeLod::drawOwner;
    if (owner != p.owner) {
        p.owner = owner;
        p.ownerEligible = owner != NULL && !p.event && owner != p.player && owner != p.horse &&
                          owner != p.lockon && dNativeLod::drawOwnerName != fpcNm_BG_e &&
                          fopAc_IsActor(const_cast<void*>(owner));
    }
    return p.ownerEligible;
}

f32 lodColumnScale2(const Mtx m) {
    const f32 a = m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0];
    const f32 b = m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1];
    const f32 c = m[0][2] * m[0][2] + m[1][2] * m[1][2] + m[2][2] * m[2][2];
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

// Where a shape of i_model lies for the picture: the radius of its bound
// (world units), the camera depth of the bound's near edge and the scale
// that turns its model units into world units. Skinned shapes: their boxes
// are bind-pose extents in the model's space, which the base matrix and
// scale place in the world; the root joint's animated offset widens the
// bound. False for shapes whose size is not known (billboards face the
// camera, empty boxes).
struct LodModelPlace {
    Mtx base;
    f32 baseScale2;
    f32 rootOffset;
};
void lodModelPlace(J3DModel* i_model, LodModelPlace& o_place) {
    MTXCopy(i_model->getBaseTRMtx(), o_place.base);
    const Vec& baseScale = *i_model->getBaseScale();
    for (int r = 0; r < 3; r++) {
        o_place.base[r][0] *= baseScale.x;
        o_place.base[r][1] *= baseScale.y;
        o_place.base[r][2] *= baseScale.z;
    }
    o_place.baseScale2 = lodColumnScale2(o_place.base);
    MtxP root = i_model->getAnmMtx(0);
    const f32 rx = root[0][3] - o_place.base[0][3], ry = root[1][3] - o_place.base[1][3],
              rz = root[2][3] - o_place.base[2][3];
    o_place.rootOffset = std::sqrt(rx * rx + ry * ry + rz * rz);
}
bool lodShapeBound(J3DModel* i_model, J3DModelData* data, J3DShape* shape, LodModelPlace& place, MtxP view,
                   f32& o_radius, f32& o_depth, f32& o_scale) {
    const Vec& mn = *shape->getMin();
    const Vec& mx = *shape->getMax();
    const f32 dx = mx.x - mn.x, dy = mx.y - mn.y, dz = mx.z - mn.z;
    const f32 half = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(half > 0.0f) || !(half < 1.0e7f)) {
        return false;
    }
    const Vec center = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
    J3DShapeMtx* shapeMtx = shape->getShapeMtx(0);
    const u32 type = shapeMtx->getType();
    MtxP at;
    f32 slack = 0.0f;
    if (shape->getMtxGroupNum() == 1 && (type == 'SMTX' || type == 'SMCV')) {
        const u16 drw = shapeMtx->getUseMtxIndex(0);
        if (drw < data->getDrawMtxNum() && data->getDrawMtxFlag(drw) == 0 &&
            data->getDrawMtxIndex(drw) < data->getJointNum())
        {
            // One joint: the box is the part's extent in that joint's space.
            at = i_model->getAnmMtx(data->getDrawMtxIndex(drw));
            o_scale = std::sqrt(lodColumnScale2(at));
            o_radius = half * o_scale;
        } else {
            at = place.base;
            o_scale = std::sqrt(place.baseScale2);
            o_radius = 1.5f * half * o_scale;
            slack = place.rootOffset;
        }
    } else if (type == 'SMML' || type == 'SMMC' || type == 'SMTX' || type == 'SMCV') {
        at = place.base;
        o_scale = std::sqrt(place.baseScale2);
        o_radius = 1.5f * half * o_scale;
        slack = place.rootOffset;
    } else {
        return false;
    }
    const f32 wx = at[0][0] * center.x + at[0][1] * center.y + at[0][2] * center.z + at[0][3];
    const f32 wy = at[1][0] * center.x + at[1][1] * center.y + at[1][2] * center.z + at[1][3];
    const f32 wz = at[2][0] * center.x + at[2][1] * center.y + at[2][2] * center.z + at[2][3];
    o_depth = -(view[2][0] * wx + view[2][1] * wy + view[2][2] * wz + view[2][3]) - o_radius - slack;
    return true;
}

// Hides the visible shapes of i_model that project under the parts threshold;
// returns how many, their indices in o_hidden.
int lodHideParts(J3DModel* i_model, u16* o_hidden, int i_capacity) {
    const dNativeLod::Config& cfg = dNativeLod::config();
    if (cfg.partPixels <= 0.0f || i_model->getMtxCalcMode() != 0 || !lodOwnerEligible()) {
        return 0;
    }
    J3DModelData* data = i_model->getModelData();
    const u16 shapeNum = data->getShapeNum();
    if (shapeNum == 0 || shapeNum > i_capacity || data->getJointNum() == 0) {
        return 0;
    }
    MtxP view = j3dSys.getViewMtx();
    const f32 unitPixels = l_lodPicture.unitPixels;
    LodModelPlace place;
    lodModelPlace(i_model, place);
    int hidden = 0;
    for (u16 i = 0; i < shapeNum; i++) {
        J3DShape* shape = data->getShapeNodePointer(i);
        if (shape == NULL || shape->checkFlag(J3DShpFlag_Visible) || shape->checkFlag(J3DShpFlag_NoMtx) ||
            shape->getMtxGroupNum() == 0)
        {
            continue;
        }
        f32 radius, depth, scale;
        if (!lodShapeBound(i_model, data, shape, place, view, radius, depth, scale)) {
            continue;
        }
        if (dNativeLod::partTooSmall(radius, depth, unitPixels, cfg.partPixels)) {
            shape->hide();
            o_hidden[hidden++] = i;
            dNativeLod::census.partShapes++;
            dNativeLod::census.partGroups += shape->getMtxGroupNum();
        }
    }
    return hidden;
}

// Rules "levels" and "shadowlevels" (d_native_lod.h): the shape packets of
// an entered model get the shape's levels and the level for the scene. The
// painter uses them for this draw phase only (J3DPacket.cpp), so a model
// entered another way, or not this time, is drawn whole.
void lodChooseLevels(J3DModel* i_model) {
    namespace features = dusk::native::features;
    namespace model_lod = dusk::native::model_lod;
    const bool scene = features::on(features::Id::modelLod), shadow = features::on(features::Id::shadowLod);
    if (!scene && !shadow) {
        return;
    }
    J3DModelData* data = i_model->getModelData();
    const model_lod::Model* levels = model_lod::find(data);
    if (levels == NULL || levels->shapes != data->getShapeNum() || data->getJointNum() == 0) {
        return;
    }
    const dNativeLod::Config& cfg = dNativeLod::config();
    const LodPicture& p = l_lodPicture;
    const void* owner = dNativeLod::drawOwner;
    // Never Link; an actor not during an event; room models always.
    const bool eligible = scene && cfg.levelPixels > 0.0f && i_model->getMtxCalcMode() == 0 && owner != p.player &&
                          (dNativeLod::drawOwnerName == fpcNm_BG_e ||
                           (owner != NULL && !p.event && fopAc_IsActor(const_cast<void*>(owner))));
    MtxP view = j3dSys.getViewMtx();
    LodModelPlace place;
    lodModelPlace(i_model, place);
    model_lod::Counters& counted = model_lod::counters();
    for (u16 i = 0; i < levels->shapes; i++) {
        J3DShapePacket* packet = i_model->getShapePacket(i);
        const model_lod::Shape& lod = levels->shape[i];
        J3DShape* shape = data->getShapeNodePointer(i);
        if (lod.levels == 0 || shape == NULL || shape->checkFlag(J3DShpFlag_Visible)) {
            continue;
        }
        packet->mNativeLodShape = &lod;
        packet->mNativeLodPhase = dNativeLod::drawPhase;
        unsigned level = 0;
        f32 radius, depth, scale;
        if (eligible && lodShapeBound(i_model, data, shape, place, view, radius, depth, scale) && depth > 1.0f) {
            level = dNativeLod::chooseLevel(lod.error.data(), lod.levels, scale * p.unitPixels / depth,
                                            cfg.levelPixels, packet->mNativeLod);
        }
        packet->mNativeLod = u8(level);
        counted.shapesSeen++;
        counted.shapesReduced += level != 0;
        counted.levelChoices[level]++;
    }
}
void lodCountEntered(J3DModel* i_model, bool i_tiny) {
    J3DModelData* data = i_model->getModelData();
    unsigned shapes = 0, groups = 0;
    for (u16 i = 0; i < data->getShapeNum(); i++) {
        J3DShape* shape = data->getShapeNodePointer(i);
        if (shape != NULL && !shape->checkFlag(J3DShpFlag_Visible)) {
            shapes++;
            groups += shape->getMtxGroupNum();
        }
    }
    if (i_tiny) {
        dNativeLod::census.actorGroups += groups;
        return;
    }
    dNativeLod::census.shapes += shapes;
    dNativeLod::census.groups += groups;
    const short name = dNativeLod::drawOwnerName;
    if (name >= 0 && name < 800) {
        dNativeLod::census.groupsByProc[name] += groups;
    }
}
}  // namespace

static bool mDoExt_entry(J3DModel* i_model) {
    if (dNativeLod::drawOwnerTiny) {
        // Rule "actor": the draw method of a tiny actor runs, its models
        // are not entered.
        if (dNativeLod::config().census) {
            lodCountEntered(i_model, true);
        }
        return false;
    }
    u16 hidden[128];
    const int count = lodHideParts(i_model, hidden, 128);
    lodChooseLevels(i_model);
    if (dNativeLod::config().census) {
        lodCountEntered(i_model, false);
    }
    i_model->entry();
    J3DModelData* data = i_model->getModelData();
    for (int i = 0; i < count; i++) {
        data->getShapeNodePointer(hidden[i])->show();
    }
    return true;
}

// Instance batches (m_Do_ext.h mDoExt_nativeInstancesUpdateDL and the 3D line
// runs of mDoExt_3DlineMat1_c::nativeDrawRun; the J3D side is
// JSystem/J3DGraphBase/J3DNativeInstances.h). Game thread only.
namespace {
// sdmc:/3ds/DuskLight/batch-instances.txt, read once: "0" off (the original
// per-model code and one strip per line material), "v" verify (every model of
// a batch also gets its light and differed list as the original code gives
// them, and those lists are compared with the carrier's), anything else or
// no file: on.
int nativeInstancesMode() {
    static const int mode = [] {
        int value = 1;
        if (FILE* file = std::fopen("sdmc:/3ds/DuskLight/batch-instances.txt", "rb")) {
            char text[8] = {};
            (void)std::fread(text, 1, sizeof(text) - 1, file);
            std::fclose(file);
            value = text[0] == '0' ? 0 : (text[0] == 'v' || text[0] == 'V') ? 2 : 1;
        }
        OSReport("Native instance batches: %s (SD switch batch-instances.txt: 0 off, v verify)\n",
                 value == 0 ? "off" : value == 2 ? "verify" : "on");
        return value;
    }();
    return mode;
}

struct NativeInstanceCounts {
    unsigned pictures, batches, models, refused, verified, differed, lineRuns, lineMaterials;
    const char* refusal;
};
NativeInstanceCounts l_nativeInstances;

bool nativeMaterialAnimated(const J3DMaterialAnm* anm) {
    if (anm == NULL) {
        return false;
    }
    if (anm->hasMaterialAnimation()) {
        return true;
    }
    for (int i = 0; i < 8; i++) {
        if (anm->getTexNoAnm(i).getAnmFlag()) {
            return true;
        }
    }
    return false;
}

const char* nativeInstancesRefusal(J3DModel* const* models, int count, dKy_tevstr_c* tevStr) {
    if (count < 2 || count > J3DNativeInstances::maxModels) {
        return "count";
    }
    // dKy_setLight_nowroom_actor scales the room lights by the move ratio on
    // every call for these types: per-model calls compound, one call would not.
    if (tevStr == NULL || tevStr->Type == 9 || tevStr->Type == 10) {
        return "light type";
    }
    // The parts rule decides model by model.
    if (dNativeLod::config().partPixels > 0.0f) {
        return "lod parts";
    }
    if (models[0] == NULL) {
        return "model";
    }
    J3DModelData* data = models[0]->getModelData();
    if (data == NULL || data->getShapeNum() == 0 || data->getMaterialNum() == 0) {
        return "model";
    }
    for (int i = 0; i < count; i++) {
        J3DModel* model = models[i];
        if (model == NULL || model->getModelData() != data) {
            return "model";
        }
        if (model->checkFlag(J3DMdlFlag_SkinPosCpu | J3DMdlFlag_SkinNrmCpu | J3DMdlFlag_EnableLOD) ||
            model->getMtxCalcMode() == 2) {
            return "model flags";
        }
        // Arrays the CPU rewrites per model, or a calc callback (it could
        // read what the other models' code changed in between).
        if (model->mDeformData != NULL || model->mSkinDeform != NULL || model->mVtxColorCalc != NULL ||
            model->mUnkCalc1 != NULL || model->mUnkCalc2 != NULL || model->mCalcCallBack != NULL) {
            return "model calc";
        }
        for (int k = 0; k < i; k++) {
            if (models[k] == model) {
                return "model twice";
            }
        }
    }
    for (u16 i = 0; i < data->getMaterialNum(); i++) {
        J3DMaterial* material = data->getMaterialNodePointer(i);
        // dRes_info_c gives every material of a loaded model an empty
        // J3DMaterialAnm (all tracks off): its calc changes nothing, so only a
        // track that is on refuses the batch.
        if (nativeMaterialAnimated(material->getMaterialAnm())) {
            return "material animation";
        }
        // The batch is drawn where the last model's entry is, which is where
        // the original draws them all only in lists that keep a model's
        // entries together in reverse order (by material, or unsorted).
        const int sort = j3dSys.getDrawBuffer(material->isDrawModeOpaTexEdge() == FALSE)->getSortMode();
        if (sort != J3DDrawBufSortMode_Mat && sort != J3DDrawBufSortMode_MatAnm && sort != J3DDrawBufSortMode_Non) {
            return "draw list";
        }
    }
    for (u16 i = 0; i < data->getShapeNum(); i++) {
        if (const char* why = J3DNativeInstancesShapeRefusal(data->getShapeNodePointer(i))) {
            return why;
        }
    }
    return NULL;
}

// Verify: the differed lists of `model` (made as the original code makes
// them) are the bytes the carrier's give every model of the batch.
bool nativeSameDifferedLists(J3DModel* model, J3DModel* carrier) {
    J3DModelData* data = carrier->getModelData();
    for (u16 i = 0; i < data->getShapeNum(); i++) {
        J3DDisplayListObj* a = model->getShapePacket(i)->getDisplayListObj();
        J3DDisplayListObj* b = carrier->getShapePacket(i)->getDisplayListObj();
        if ((a == NULL) != (b == NULL)) {
            return false;
        }
        if (a != NULL && (a->getDisplayListSize() != b->getDisplayListSize() ||
                          std::memcmp(a->getDisplayList(0), b->getDisplayList(0), a->getDisplayListSize()) != 0)) {
            return false;
        }
    }
    return true;
}

// Every draw phase: a new picture for the J3D side; every 300 a report.
void nativeInstancesPicture() {
    J3DNativeInstancesBeginPicture();
    NativeInstanceCounts& c = l_nativeInstances;
    if (++c.pictures < 300) {
        return;
    }
    if (nativeInstancesMode() != 0) {
        J3DNativeInstanceStats& s = J3DNativeInstancesStats();
        u32 lists = 0, bytes = 0, capacity = 0;
        J3DNativeInstancesStore(lists, bytes, capacity);
        const f32 n = 1.0f / c.pictures;
        OSReport("Native instance batches, last %u pictures, per picture: batches %.1f of %.1f models, drawn %.1f "
                 "models in %.1f draws (%.1f draws saved), one by one %.1f (last: %s); refused %.1f (last: %s); "
                 "lists made %u kept %u, %u/%u bytes, store emptied %u\n",
                 c.pictures, c.batches * n, c.models * n, s.instances * n, s.draws * n,
                 (s.instances - s.draws) * n, s.alone * n, s.why != NULL ? s.why : "-", c.refused * n,
                 c.refusal != NULL ? c.refusal : "-", s.listsMade, lists, bytes, capacity, s.recycled);
        // A second line: OSReport cuts one line at its buffer size.
        OSReport("Native instance batches, per picture: 3D line runs %.1f of %.1f materials (%.1f draws saved); "
                 "verify models %u differed %u\n",
                 c.lineRuns * n, c.lineMaterials * n, (c.lineMaterials - c.lineRuns) * n, c.verified,
                 c.differed);
    }
    J3DNativeInstanceStats& s = J3DNativeInstancesStats();
    s.draws = s.instances = s.alone = s.listsMade = s.recycled = 0;
    c = NativeInstanceCounts{};
}
}  // namespace

bool mDoExt_nativeInstancesUpdateDL(J3DModel* const* models, int count, dKy_tevstr_c* tevStr,
                                    J3DNativeInstances* batch) {
    const int mode = nativeInstancesMode();
    const char* why = mode == 0 ? "off" : nativeInstancesRefusal(models, count, tevStr);
    if (why == NULL) {
        batch->models = models;
        batch->count = u16(count);
        if (!J3DNativeInstancesRegister(models[count - 1], batch)) {
            why = "carriers";
        }
    }
    if (why != NULL) {
        if (mode != 0) {
            l_nativeInstances.refused++;
            l_nativeInstances.refusal = why;
        }
        return false;
    }
    J3DModel* carrier = models[count - 1];
    J3DModelData* data = carrier->getModelData();
    // mDoExt_modelUpdateDL's branch: diff() writes the differed lists.
    const bool differed = data->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL && !data->isLocked();
    for (int i = 0; i < count - 1; i++) {
        J3DModel* model = models[i];
        if (mode == 2) {
            // The original code for this model but its entry (and viewCalc,
            // which only the entered packets read).
            g_env_light.setLightTevColorType_MAJI(model, tevStr);
            model->calc();
            if (differed) {
                model->calcMaterial();
                model->diff();
            }
        } else {
            model->calc();
        }
    }
    g_env_light.setLightTevColorType_MAJI(carrier, tevStr);
    mDoExt_modelUpdateDL(carrier);
    if (mode == 2 && differed) {
        for (int i = 0; i < count - 1; i++) {
            l_nativeInstances.verified++;
            if (!nativeSameDifferedLists(models[i], carrier)) {
                l_nativeInstances.differed++;
            }
        }
    }
    l_nativeInstances.batches++;
    l_nativeInstances.models += count;
    return true;
}

void dNativeLod::beginDrawPhase() {
    nativeInstancesPicture();
    LodPicture& p = l_lodPicture;
    p.picture++;
    p.unitPixels = dNativeLod::pixelsPerUnit(mDoLib_clipper::getFovyRate());
    p.event = dComIfGp_event_runCheck() != FALSE;
    p.player = dComIfGp_getPlayer(0);
    p.horse = dComIfGp_getHorseActor();
    p.lockon = dComIfGp_getAttention() != NULL ? dComIfGp_getAttention()->LockonTarget(0) : NULL;
    p.owner = reinterpret_cast<const void*>(1);  // no process has this address
    p.ownerEligible = false;
    dNativeLod::drawPhase++;
    dNativeLod::unitPixels = p.unitPixels;

    const dNativeLod::Config& cfg = dNativeLod::config();
    if (!cfg.census) {
        return;
    }
    dNativeLod::Census& c = dNativeLod::census;
    c.eventPictures += p.event;
    if (++c.pictures < 300) {
        return;
    }
    const f32 n = 1.0f / c.pictures;
    const unsigned skinDone = J3DModel::sNativeEnvelopeCount[0];
    const unsigned skinDeferred = J3DModel::sNativeEnvelopeCount[1];
    const unsigned skinLate = J3DModel::sNativeEnvelopeCount[2];
    OSReport("Native LOD, last %u pictures, per picture: entered shapes %.1f groups %.1f; parts rule "
             "left out shapes %.1f groups %.1f; actor rule %.1f actors groups %.1f; other casters' model "
             "shadows %.1f, %.1f made blobs; grass/flowers tested %.1f thinned %.1f; skin matrices made "
             "%.1f deferred %.1f (%.1f of them at draw); event pictures %u\n",
             c.pictures, c.shapes * n, c.groups * n, c.partShapes * n, c.partGroups * n, c.actors * n,
             c.actorGroups * n,
             (c.shadowKept + c.shadowBlobs) * n, c.shadowBlobs * n, c.foliageTested * n,
             c.foliageHidden * n, skinDone * n, skinDeferred * n, skinLate * n, c.eventPictures);
    char line[256];
    int at = std::snprintf(line, sizeof(line), "Native LOD groups by process (f_pc_name.h ids):");
    for (int rank = 0; rank < 14 && at < int(sizeof(line)) - 16; rank++) {
        int best = 0;
        for (int i = 1; i < 800; i++) {
            if (c.groupsByProc[i] > c.groupsByProc[best]) {
                best = i;
            }
        }
        if (c.groupsByProc[best] == 0) {
            break;
        }
        at += std::snprintf(line + at, sizeof(line) - at, " %d=%.1f", best, c.groupsByProc[best] * n);
        c.groupsByProc[best] = 0;
    }
    OSReport("%s\n", line);
    c = dNativeLod::Census{};
    dusk::native::model_lod::report();
    J3DModel::sNativeEnvelopeCount[0] = J3DModel::sNativeEnvelopeCount[1] =
        J3DModel::sNativeEnvelopeCount[2] = 0;
}
#else
static bool mDoExt_entry(J3DModel* i_model) {
    i_model->entry();
    return true;
}
#endif

static bool mDoExt_modelDiff(J3DModel* i_model) {
    modelMtxErrorCheck(i_model);
    i_model->calcMaterial();
    i_model->diff();
#if TARGET_PC
    if (!dusk::game_clock::is_sim_frame()) {
        return false;
    }
#endif
    return mDoExt_entry(i_model);
}

void mDoExt_modelUpdate(J3DModel* i_model) {
    modelMtxErrorCheck(i_model);

    J3DModelData* model_data = i_model->getModelData();

    if (model_data->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL &&
        !model_data->isLocked())
    {
        i_model->calc();
        mDoExt_modelDiff(i_model);
    } else {
        // J3DModel::update(): calc() and entry().
        i_model->calc();
        mDoExt_entry(i_model);
        i_model->lock();
    }

    i_model->viewCalc();
}

void mDoExt_modelUpdateDL(J3DModel* i_model) {
    modelMtxErrorCheck(i_model);

    J3DModelData* model_data = i_model->getModelData();

    if (model_data->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL &&
        !model_data->isLocked())
    {
        i_model->calc();
        mDoExt_modelDiff(i_model);
    } else {
        i_model->unlock();
        // J3DModel::update(): calc() and entry().
        i_model->calc();
        mDoExt_entry(i_model);
        i_model->lock();
    }

    i_model->viewCalc();
}

static bool enterModelDisplayList(J3DModel* i_model) {
#if TARGET_PC
    if (!dusk::game_clock::is_sim_frame()) {
        // FRAME INTERP NOTE: This fixes issue #355 where some lights would flicker.
        // This is likely better solved by updating J3DMaterial::needsInterpCallBack,
        // but it's unclear what exactly needs to be added.
        i_model->calcMaterial();
        i_model->diff();
        return false;
    }
#endif

    modelMtxErrorCheck(i_model);

    J3DModelData* model_data = i_model->getModelData();

    bool entered;
    if (model_data->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL &&
        !model_data->isLocked())
    {
        entered = mDoExt_modelDiff(i_model);
    } else {
        i_model->unlock();
        entered = mDoExt_entry(i_model);
        i_model->lock();
    }

    i_model->viewCalc();
    return entered;
}

void mDoExt_modelEntryDL(J3DModel* i_model) {
    enterModelDisplayList(i_model);
}

void mDoExt_btkAnmRemove(J3DModelData* i_modelData) {
    for (u16 i = 0; i < i_modelData->getMaterialNum(); i++) {
        J3DMaterialAnm* matAnm = i_modelData->getMaterialNodePointer(i)->getMaterialAnm();

        if (matAnm != NULL) {
            for (u16 j = 0; (int)j < 8; j++) {
                matAnm->setTexMtxAnm(j, NULL);
            }
        }
    }
}

void mDoExt_brkAnmRemove(J3DModelData* i_modelData) {
    for (u16 i = 0; i < i_modelData->getMaterialNum(); i++) {
        J3DMaterialAnm* matAnm = i_modelData->getMaterialNodePointer(i)->getMaterialAnm();

        if (matAnm != NULL) {
            for (u16 j = 0; (int)j < 4; j++) {
                matAnm->setTevColorAnm(j, NULL);
                matAnm->setTevKColorAnm(j, NULL);
            }
        }
    }
}

// this needs a lot of work still
static void dummy2() {
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;
    FORCE_DONT_INLINE;

    ((J3DMaterial*)NULL)->getMaterialMode();

    J3DGXColor gxColor;
    J3DColorChan colorChan;
    J3DTexCoord texCoord;
    J3DGXColorS10 gxColorS10;
    J3DTevStageInfo tevStageInfo;
    J3DTevStage* stage_p = NULL;
    stage_p->setTevStageInfo(tevStageInfo);
    J3DTevStage tevStage;
    J3DIndTevStage indStage;
    J3DIndTevStage indTevStage;
    J3DTevOrder tevOrder;
    J3DTevSwapModeTable tevSwapModeTable;
    J3DIndTexOrder indTexOrder;

    J3DIndTexMtx* indTexMtx = JKR_NEW J3DIndTexMtx();
    J3DIndTexCoordScale* indTexCoordScale = JKR_NEW J3DIndTexCoordScale();
    J3DColorBlock* colorBlock = NULL;
    colorBlock->setColorChanNum((u8)NULL);
    colorBlock->setMatColor(0, gxColor);
    colorBlock->setColorChan(0, colorChan);
    colorBlock->setAmbColor(0, gxColor);

    J3DTexGenBlock* texGenBlock = NULL;
    texGenBlock->setTexGenNum((u32)NULL);
    texGenBlock->setTexCoord(0, NULL);

    J3DTevBlock* tevBlock = NULL;
    tevBlock->setTevStageNum((u8)0);
    tevBlock->setTevColor(0, J3DGXColorS10());
    tevBlock->setTevKColor(0, gxColor);
    tevBlock->setTevOrder(0, tevOrder);
    tevBlock->setTevKColorSel(0, (u8)0);
    tevBlock->setTevKAlphaSel(0, (u8)0);
    tevBlock->setTevSwapModeTable(0, tevSwapModeTable);
    tevBlock->setTexNo(0, (u16)0);
    tevBlock->setTevStage(0, tevStage);
    tevBlock->setIndTevStage(0, indTevStage);

    J3DIndBlock* indBlock = NULL;
    indBlock->setIndTexStageNum(0);
    indBlock->setIndTexMtx(0, *indTexMtx);
    indBlock->setIndTexCoordScale(0, *indTexCoordScale);

    J3DPEBlock* peBlock = NULL;
    J3DAlphaComp alphaComp;
    peBlock->setAlphaComp(alphaComp);
    J3DBlend blend;
    peBlock->setBlend(blend);
    J3DZMode zMode;
    peBlock->setZMode(zMode);
    u8 compLoc = 0;
    peBlock->setZCompLoc(compLoc);

    colorBlock->getColorChanNum();
    colorBlock->getMatColor(0);
    colorBlock->getColorChan(0);
    colorBlock->getAmbColor(0);

    texGenBlock->getTexGenNum();
    texGenBlock->getTexCoord(0);
    texGenBlock->getTexMtx(0);

    tevBlock->getTevStageNum();
    tevBlock->getTevColor(0);
    tevBlock->getTevKColor(0);
    tevBlock->getTevOrder(0);
    tevBlock->getTevKColorSel(0);
    tevBlock->getTevKAlphaSel(0);
    tevBlock->getTevSwapModeTable(0);
    tevBlock->getTexNo(0);
    tevBlock->getTevStage(0);
    tevBlock->getIndTevStage(0);

    indBlock = NULL;
    indBlock->getIndTexStageNum();
    indBlock->getIndTexOrder(0);
    indBlock->getIndTexMtx(0);
    indBlock->getIndTexCoordScale(0);

    indTexOrder.getMap();
    indTexOrder.getCoord();

    peBlock = NULL;
    peBlock->getFog();
    peBlock->getAlphaComp();
    peBlock->getBlend();
    peBlock->getZMode();
    peBlock->getZCompLoc();

    ((J3DMaterial*)NULL)->setMaterialAnm(NULL);
}

int mDoExt_invisibleModel::create(J3DModel* i_model, u8 param_1) {
    J3DModelData* model_data = i_model->getModelData();

    mpPackets = JKR_NEW_ARRAY(mDoExt_invJntPacket, model_data->getJointNum());
    if (mpPackets == NULL) {
        return 0;
    }

    mModel = i_model;
    mDoExt_invJntPacket* packet = mpPackets;

    for (u16 i = 0; i < model_data->getJointNum(); i++) {
        packet->setJoint(mModel, i, param_1);
        packet++;
    }

    return 1;
}

void mDoExt_invisibleModel::entryJoint(cXyz* param_0) {
    J3DModelData* modelData = mModel->getModelData();
    mDoExt_invJntPacket* packet = mpPackets;

    if (param_0 == NULL) {
        cXyz tmp;
        for (u16 i = 0; i < modelData->getJointNum(); i++) {
            MtxP anmMtx = mModel->getAnmMtx(i);
            tmp.set(anmMtx[0][3], anmMtx[1][3], anmMtx[2][3]);
            dComIfGd_entryZSortListZxlu(packet, tmp);
            packet++;
        }
    } else {
        for (u16 i = 0; i < modelData->getJointNum(); i++) {
            dComIfGd_entryZSortListZxlu(packet, *param_0);
            packet++;
        }
    }
}

void mDoExt_invisibleModel::entryDL(cXyz* param_0) {
    J3DDrawBuffer* buffer0 = j3dSys.getDrawBuffer(0);
    J3DDrawBuffer* buffer1 = j3dSys.getDrawBuffer(1);
    dComIfGd_setListZxlu();
#if defined(DUSK_TARGET_3DS)
    // The joint depth pass replays the material packet built by model entry.
    // An omitted model has no matching color pass and may have an empty list.
    if (enterModelDisplayList(mModel)) {
        entryJoint(param_0);
    }
#else
    mDoExt_modelEntryDL(mModel);
    entryJoint(param_0);
#endif
    j3dSys.setDrawBuffer(buffer0, 0);
    j3dSys.setDrawBuffer(buffer1, 1);
}

void mDoExt_setupShareTexture(J3DModelData* i_modelData, J3DModelData* i_shareModelData) {
    JUT_ASSERT(1547, i_modelData != NULL && i_shareModelData != NULL);
    J3DTexture* texture = i_modelData->getTexture();
    JUT_ASSERT(1549, texture != NULL);
    JUTNameTab* textureName = i_modelData->getTextureName();
    JUT_ASSERT(1551, textureName != NULL);
    J3DTexture* shareTexture = i_shareModelData->getTexture();
    JUT_ASSERT(1553, shareTexture != NULL);
    JUTNameTab* shareTextureName = i_shareModelData->getTextureName();
    JUT_ASSERT(1555, shareTextureName != NULL)

    bool bvar = false;
    for (u16 i = 0; i < texture->getNum(); i++) {
        if (texture->getResTIMG(i)->imageOffset == 0) {
            for (u16 j = 0; j < shareTexture->getNum(); j++) {
                if (!strcmp(textureName->getName(i), shareTextureName->getName(j))) {
                    JUT_ASSERT(1564, shareTexture->getResTIMG(j)->imageOffset != 0);
                    texture->setResTIMG(i, *shareTexture->getResTIMG(j));
                    bvar = true;
                }
            }
        }
    }

    if (bvar) {
        j3dSys.setTexture(texture);

        for (u16 i = 0; i < i_modelData->getMaterialNum(); i++) {
            J3DMaterial* mat = i_modelData->getMaterialNodePointer(i);
            J3DTevBlock* tevBlock = mat->getTevBlock();
            JUT_ASSERT(1577, tevBlock != NULL);
            J3DDisplayListObj* dlObj = mat->getSharedDisplayListObj();

            BOOL ret = OSDisableInterrupts();
            GDInitGDLObj(&J3DDisplayListObj::sGDLObj, dlObj->getDisplayList(0),
                         dlObj->getDisplayListSize());
            GDSetCurrent(&J3DDisplayListObj::sGDLObj);
            tevBlock->patchTexNoAndTexCoordScale();
            OSRestoreInterrupts(ret);
            GDSetCurrent(NULL);
        }
    }
}

static const char* dummy3() {
    // MWCC ignores mapping of some japanese characters using the
    // byte 0x5C (ASCII '\'). This is why this string is hex-encoded.

    // "ソリッドヒープちゃうがな！\n"
    // "This isn't a solid heap! \n"
    return "\x83\x5C\x83\x8A\x83\x62\x83\x68\x83\x71\x81\x5B\x83\x76\x82\xBF\x82\xE1\x82\xA4\x82"
        "\xAA\x82\xC8\x81\x49\x0A";
}

void mDoExt_setupStageTexture(J3DModelData* i_modelData) {
    J3DTexture* texture_p = i_modelData->getTexture();

    if (texture_p != NULL) {
        JUTNameTab* tex_name_p = i_modelData->getTextureName();
        if (tex_name_p != NULL) {
            bool set_timg = false;

            for (u16 i = 0; i < texture_p->getNum(); i++) {
                if (texture_p->getResTIMG(i)->imageOffset == 0) {
                    char res_name[64];
                    snprintf(res_name, sizeof(res_name), "%s.bti", tex_name_p->getName(i));
                    OS_REPORT("%d:設定テクスチャー名<%s>\n", i, res_name);

                    ResTIMG* timg = (ResTIMG*)dComIfG_getStageRes(res_name);
                    JUT_ASSERT(1623, timg != NULL);
                    texture_p->setResTIMG(i, *timg);
                    set_timg = true;
                }
            }

            if (set_timg) {
                j3dSys.setTexture(texture_p);

                for (u16 i = 0; i < i_modelData->getMaterialNum(); i++) {
                    J3DMaterial* mat_p = i_modelData->getMaterialNodePointer(i);
                    J3DTevBlock* block_p = mat_p->getTevBlock();

                    if (block_p != NULL) {
                        J3DDisplayListObj* dlObj_p = mat_p->getSharedDisplayListObj();

                        if (dlObj_p != NULL) {
                            BOOL state = OSDisableInterrupts();

                            GDInitGDLObj(&J3DDisplayListObj::sGDLObj, dlObj_p->getDisplayList(0),
                                         dlObj_p->getDisplayListSize());
                            GDSetCurrent(&J3DDisplayListObj::sGDLObj);
                            block_p->patchTexNoAndTexCoordScale();

                            OSRestoreInterrupts(state);
                            GDSetCurrent(NULL);
                        }
                    }
                }
            }
        }
    }
}

static JKRAssertHeap* AssertHeap;

JKRAssertHeap* mDoExt_createAssertHeap(JKRHeap* parentHeap) {
    JUT_ASSERT(1666, AssertHeap == NULL);
    AssertHeap = JKRAssertHeap::create(parentHeap);
    JKRHEAP_NAME(AssertHeap, "Assert");
    return AssertHeap;
}

JKRAssertHeap* mDoExt_getAssertHeap() {
    return AssertHeap;
}

static JKRExpHeap* DbPrintHeap;

JKRExpHeap* mDoExt_createDbPrintHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(1693, DbPrintHeap == NULL || heapSize == 0);
    DbPrintHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(DbPrintHeap, "DbPrintHeap");
    return DbPrintHeap;
}

JKRExpHeap* mDoExt_getDbPrintHeap() {
    return DbPrintHeap;
}

DUSK_GAME_DATA JKRExpHeap* gameHeap;
static intptr_t safeGameHeapSize = -1;

JKRExpHeap* mDoExt_createGameHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(1739, gameHeap == NULL || heapSize == 0);
    gameHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(gameHeap, "gameHeap");
    gameHeap->setAllocationMode(JKRExpHeap::ALLOC_MODE_1);
    return gameHeap;
}

JKRExpHeap* mDoExt_getGameHeap() {
    return gameHeap;
}

void mDoExt_setSafeGameHeapSize() {
    if (safeGameHeapSize < 0) {
        safeGameHeapSize = mDoExt_getGameHeap()->getTotalUsedSize();
        OS_REPORT("++++++++++++++++++++++++++++++ GameHeap Rock !!\n");
        mDoExt_getGameHeap()->dump();
        mDoExt_getGameHeap()->changeGroupID(7);
    }
}

void dummy5() {
    JUT_ASSERT(0, safeGameHeapSize >= 0);
    OS_REPORT("++++++++++++++++++++++++++++++ safeGameHeapSize <%x>\n", safeGameHeapSize);
}

size_t mDoExt_getSafeGameHeapSize() {
    return safeGameHeapSize;
}

DUSK_GAME_DATA JKRExpHeap* zeldaHeap;
DUSK_GAME_DATA intptr_t safeZeldaHeapSize = -1;

JKRExpHeap* mDoExt_createZeldaHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(1815, zeldaHeap == NULL || heapSize == 0);
    zeldaHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(zeldaHeap, "zeldaHeap");
    return zeldaHeap;
}

JKRExpHeap* mDoExt_getZeldaHeap() {
    return zeldaHeap;
}

void mDoExt_setSafeZeldaHeapSize() {
    if (safeGameHeapSize < 0) {
        safeGameHeapSize = mDoExt_getGameHeap()->getTotalUsedSize();
        OS_REPORT("++++++++++++++++++++++++++++++ ZeldaHeap Rock !!\n");
        mDoExt_getGameHeap()->dump();
        mDoExt_getGameHeap()->changeGroupID(7);
    }
}

void mDoExt_addSafeZeldaHeapSize(intptr_t size) {
    JUT_ASSERT(1869, safeZeldaHeapSize >= 0);
    safeZeldaHeapSize -= size;
    OS_REPORT("++++++++++++++++++++++++++++++ addSafeZeldaHeapSize <%x>\n", -size);
    mDoExt_getZeldaHeap()->changeGroupID(mDoExt_getZeldaHeap()->getCurrentGroupId() + 1);
}

intptr_t mDoExt_getSafeZeldaHeapSize() {
    return safeZeldaHeapSize;
}

DUSK_GAME_DATA JKRExpHeap* commandHeap;
DUSK_GAME_DATA intptr_t safeCommandHeapSize = -1;

JKRExpHeap* mDoExt_createCommandHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(1894, commandHeap == 0 || heapSize == 0);
    commandHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(commandHeap, "commandHeap");
    return commandHeap;
}

JKRExpHeap* mDoExt_getCommandHeap() {
    return commandHeap;
}

void mDoExt_setSafeCommandHeapSize() {
    if (safeCommandHeapSize < 0) {
        safeCommandHeapSize = mDoExt_getGameHeap()->getTotalUsedSize();
        OS_REPORT("++++++++++++++++++++++++++++++ CommandHeap Rock !!\n");
        mDoExt_getGameHeap()->dump();
        mDoExt_getGameHeap()->changeGroupID(7);
    }
}

intptr_t mDoExt_getSafeCommandHeapSize() {
    return safeCommandHeapSize;
}

DUSK_GAME_DATA JKRExpHeap* archiveHeap;
DUSK_GAME_DATA intptr_t safeArchiveHeapSize = -1;

JKRExpHeap* mDoExt_createArchiveHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(1966, archiveHeap == 0 || heapSize == 0);
    archiveHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(archiveHeap, "archiveHeap");
    archiveHeap->setAllocationMode(JKRExpHeap::ALLOC_MODE_1);
    return archiveHeap;
}

void mDoExt_setSafeArchiveHeapSize() {
    if (safeArchiveHeapSize < 0) {
        safeArchiveHeapSize = mDoExt_getArchiveHeap()->getTotalUsedSize();
        OS_REPORT("++++++++++++++++++++++++++++++ ArchiveHeap Rock !!\n");
        mDoExt_getArchiveHeap()->dump();
        mDoExt_getArchiveHeap()->changeGroupID(7);
    }
}

void dummy6() {
    JUT_ASSERT(0, safeArchiveHeapSize >= 0);
    OS_REPORT("++++++++++++++++++++++++++++++ safeArchiveHeapSize <%x>\n", safeArchiveHeapSize);
}

intptr_t mDoExt_getSafeArchiveHeapSize() {
    return safeArchiveHeapSize;
}

JKRExpHeap* mDoExt_getArchiveHeap() {
    JKRExpHeap* heap = archiveHeap;
    return heap;
}

JKRExpHeap* mDoExt_getArchiveHeapPtr() {
    return archiveHeap;
}

static JKRExpHeap* j2dHeap;
DUSK_GAME_DATA intptr_t safeJ2dHeapSize = -1;

JKRExpHeap* mDoExt_createJ2dHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(2059, j2dHeap == 0 || heapSize == 0);
    j2dHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(j2dHeap, "j2dHeap");
    j2dHeap->setAllocationMode(JKRExpHeap::ALLOC_MODE_1);
    return j2dHeap;
}

void mDoExt_setSafeJ2dHeapSize() {
    if (safeJ2dHeapSize < 0) {
        safeJ2dHeapSize = mDoExt_getJ2dHeap()->getTotalUsedSize();
        OS_REPORT("++++++++++++++++++++++++++++++ j2dHeap Rock !!\n");
        mDoExt_getJ2dHeap()->dump();
        mDoExt_getJ2dHeap()->changeGroupID(7);
    }
}

void dummy7() {
    JUT_ASSERT(0, safeJ2dHeapSize >= 0);
    OS_REPORT("++++++++++++++++++++++++++++++ safeJ2dHeapSize <%x>\n", safeJ2dHeapSize);
}

intptr_t mDoExt_getSafeJ2dHeapSize() {
    return safeJ2dHeapSize;
}

JKRExpHeap* mDoExt_getJ2dHeap() {
    JKRExpHeap* heap = j2dHeap;
    return heap;
}

static JKRExpHeap* HostIOHeap;

JKRHeap* mDoExt_createHostIOHeap(u32 heapSize, JKRHeap* parentHeap) {
    JUT_ASSERT(2142, HostIOHeap == 0 || heapSize == 0);
    HostIOHeap = JKRExpHeap::create(heapSize, parentHeap, true);
    JKRHEAP_NAME(HostIOHeap, "HostIOHeap");
    return HostIOHeap;
}

JKRExpHeap* mDoExt_getHostIOHeap() {
    return HostIOHeap;
}

static JKRHeap* situationHeap;

void dummy8(u32 heapSize) {
    int situationHeapLocked = 0;
    JUT_ASSERT(0, situationHeap == 0 || heapSize == 0);
    JUT_ASSERT(0, situationHeapLocked == 0);
    JUT_ASSERT(0, situationHeap);
    JUT_ASSERT(0, situationHeapLocked);
    JKRHeap* heap = NULL;
    JUT_ASSERT(0, heap == situationHeap);
}

static JKRHeap* mDoExt_SaveCurrentHeap;

JKRSolidHeap* mDoExt_createSolidHeap(u32 i_size, JKRHeap* i_parent, u32 i_alignment) {
    if (i_parent == NULL) {
        i_parent = JKRGetCurrentHeap();
    }

    JKRSolidHeap* createdHeap;
    if (i_size == 0 || i_size == -1) {
        #if DEBUG
        if (mDoExt::HeapAdjustVerbose) {
            // "mDoExt_createSolidHeap size not set"
            OS_REPORT("\x1b[44mmDoExt_createSolidHeap サイズ未設定\n\x1b[m");
            // "Ensure that the maximum available space is allocated %08x"
            OS_REPORT("最大空き容量確保します %08x\n\x1b[m", i_parent->getFreeSize());
        }
        #endif
        createdHeap = JKRCreateSolidHeap(-1, i_parent, false);
    } else {
        u32 solidHeapSize = ALIGN_NEXT(sizeof(JKRSolidHeap), 0x10);
        i_size = ALIGN_NEXT(i_size, 0x10);
        i_size += solidHeapSize;

        if (0x10 < i_alignment) {
            i_size = (i_alignment - 0x10 + i_size);
        }
        createdHeap = JKRCreateSolidHeap(i_size, i_parent, false);
    }

    if (createdHeap != NULL) {
        JKRSetErrorFlag(createdHeap, true);
        #if DEBUG
        if (mDoExt::HeapAdjustVerbose != 0) {
            u32 solidHeapSize = createdHeap->getHeapSize();
            OS_REPORT(
                "JKRCreateSolidHeap %08x i_size=%08x solidHeapSize=%08x\n",
                createdHeap,
                i_size,
                solidHeapSize
            );
        }
        #endif
    }

#if DEBUG
    if (createdHeap == NULL) {
        OS_WARNING(
            "mDoExt_createMaxSolidHeap : ソリッドヒープ%fKの確保に失敗 連続空き容量=%fK 残り空き容量=%f\n",
            i_size / 1024.0f,
            i_parent->getFreeSize() / 1024.0f,
            i_parent->getTotalFreeSize() / 1024.0f
        );
        static BOOL displayed = FALSE;
        if (displayed == FALSE) {
            displayed = TRUE;
            i_parent->dump_sort();
        }
        return createdHeap;
    }
#endif

    return createdHeap;
}

JKRSolidHeap* mDoExt_createSolidHeapFromGame(u32 i_size, u32 i_alignment) {
    return mDoExt_createSolidHeap(i_size, mDoExt_getGameHeap(), i_alignment);
}

JKRSolidHeap* mDoExt_createSolidHeapFromSystem(u32 i_size, u32 i_alignment) {
    return mDoExt_createSolidHeap(i_size, mDoExt_getZeldaHeap(), i_alignment);
}

JKRSolidHeap* mDoExt_createSolidHeapToCurrent(JKRHeap** o_heap, u32 i_size, JKRHeap* i_parent,
                                              u32 i_alignment) {
    if (o_heap != NULL) {
        *o_heap = JKRGetCurrentHeap();
    }

    JKRSolidHeap* resultHeap = mDoExt_createSolidHeap(i_size, i_parent, i_alignment);
    if (resultHeap != NULL) {
        mDoExt_setCurrentHeap(resultHeap);
    }

    return resultHeap;
}

JKRSolidHeap* mDoExt_createSolidHeapToCurrent(u32 i_size, JKRHeap* i_parent, u32 i_alignment) {
    JUT_ASSERT(2427, OSGetCurrentThread() == &mainThread);
    JUT_ASSERT(2428, mDoExt_SaveCurrentHeap == NULL);
    JKRSolidHeap* heap =
        mDoExt_createSolidHeapToCurrent(&mDoExt_SaveCurrentHeap, i_size, i_parent, i_alignment);
    if (mDoExt::CurrentHeapAdjustVerbose) {
        // "Saving the current heap %08x"
        OS_REPORT("カレントヒープを保存 %08x\n", mDoExt_SaveCurrentHeap);
    }
    return heap;
}

JKRSolidHeap* mDoExt_createSolidHeapFromGameToCurrent(JKRHeap** o_heap, u32 i_size,
                                                      u32 i_alignment) {
    JKRExpHeap* gameHeap = mDoExt_getGameHeap();
    JKRSolidHeap* solidHeap =
        mDoExt_createSolidHeapToCurrent(o_heap, i_size, gameHeap, i_alignment);
    return solidHeap;
}

JKRSolidHeap* mDoExt_createSolidHeapFromGameToCurrent(u32 i_size, u32 i_alignment) {
    JKRExpHeap* gameHeap = mDoExt_getGameHeap();
    JKRSolidHeap* solidHeap = mDoExt_createSolidHeapToCurrent(i_size, gameHeap, i_alignment);
    return solidHeap;
}

u32 mDoExt_adjustSolidHeap(JKRSolidHeap* i_heap) {
    if (i_heap == NULL) {
        return -1;
    }

    JKRHeap* parentHeap = i_heap->getParent();
    if (parentHeap == NULL || parentHeap->getHeapType() != 'EXPH') {
        // "Can't adjust because parent heap isn't Exp %08x %08x\n"
        OSReport_Error("親ヒープがExpでないのでアジャストできません %08x %08x\n", i_heap,
                       parentHeap);
        return -1;
    }

    u32 estimatedSize = i_heap->getHeapSize();
    s32 result = i_heap->adjustSize();
    if (result < 0) {
        // "adjustSize failure %08x\n"
        OSReport_Error("adjustSize失敗 %08x\n", i_heap);
        return -1;
    }
    u32 actualSize = i_heap->getHeapSize();

    #if DEBUG
    if (mDoExt::HeapAdjustVerbose) {
        // "\x1B[33mSolid heap estimate: %08x actual: %08x\n\x1B[m"
        OS_REPORT("\x1B[33mソリッドヒープの見積もり %08x 実際 %08x\n\x1B[m", estimatedSize, actualSize);

        if (estimatedSize > 0x400 && estimatedSize > (actualSize + 0x400)) {
            // "\x1B[33mThe estimate is %x bytes too large (%5.2f times). This could lead to degraded memory efficiency.\n\x1B[m"
            OS_REPORT("\x1B[33m見積もりが %x バイト大きい(%5.2f倍)です。メモリ効率悪化が懸念されます。\n\x1B[m", estimatedSize - actualSize, (f32)estimatedSize / (f32)actualSize);
        } else if (estimatedSize >= (actualSize + 0x20)) {
            // "\x1B[36mThe estimate is %x bytes too large. That's pretty good.\n\x1B[m"
            OS_REPORT("\x1B[36m見積もりが %x バイト大きいです。だいぶいい感じです。\n\x1B[m", estimatedSize - actualSize);
        } else {
            // "\x1B[32mThe estimate is spot on! Perfect.\n\x1B[m"
            OS_REPORT("\x1B[32m見積もりが、ジャストミート！完璧です。\n\x1B[m");
        }
    }

    if (mDoExt::HeapAdjustVerbose) {
        OS_REPORT("JKRSolidHeap::adjustSize %08x (%08x bytes)\n", (u32)i_heap, result);
    }
    #endif

    // this probably indicates that 0x80 is some constant, maybe from a sizeof(JKRSolidHeap)
    // with alignment?
    u32 r25 = 0x80;
    if (result >= r25) {
        return result - r25;
    }

    return result;
}

u32 mDoExt_adjustSolidHeapToSystem(JKRSolidHeap* i_heap) {
    u32 result = mDoExt_adjustSolidHeap(i_heap);
    mDoExt_restoreCurrentHeap();
    return result;
}

void mDoExt_destroySolidHeap(JKRSolidHeap* i_heap) {
    JKRDestroySolidHeap(i_heap);
    if (g_printOtherHeapDebug) {
        // "Solid heap destruction"
        OS_REPORT("mDoExt_destroySolidHeap ソリッドヒープ破壊 %08x\n", i_heap);
    }
}

void mDoExt_destroyExpHeap(JKRExpHeap* i_heap) {
    ASSERTLINE(2576, i_heap != mDoExt_getCurrentHeap());
    JKRDestroyExpHeap(i_heap);
#if DEBUG
    if (mDoExt::HeapAdjustVerbose) {
        OS_REPORT("mDoExt_destroyExpHeap Expヒープ破壊 %08x\n", i_heap);
    }
#endif
}

JKRHeap* mDoExt_setCurrentHeap(JKRHeap* heap) {
    JUT_ASSERT(2594, heap != NULL);
    JKRHeap* result = JKRSetCurrentHeap(heap);
    if (mDoExt::CurrentHeapAdjustVerbose) {
        // "Changing the current heap to the specified heap %08x->%08x"
        OS_REPORT("カレントヒープを指定ヒープに変更 %08x->%08x\n", result, heap);
    }
    return result;
}

JKRHeap* mDoExt_getCurrentHeap() {
    return JKRGetCurrentHeap();
}

void mDoExt_restoreCurrentHeap() {
    JUT_ASSERT(2711, OSGetCurrentThread() == &mainThread);
    JUT_ASSERT(2712, mDoExt_SaveCurrentHeap != FALSE);
    JKRHeap* result = JKRSetCurrentHeap(mDoExt_SaveCurrentHeap);
    if (mDoExt::CurrentHeapAdjustVerbose) {
        // "The current heap has been restored %08x->%08x"
        OS_REPORT("カレントヒープを復帰しました %08x->%08x\n", result, mDoExt_SaveCurrentHeap);
    }
    mDoExt_SaveCurrentHeap = NULL;
}

int mDoExt_resIDToIndex(JKRArchive* p_archive, u16 id) {
    JKRArchive::SDIFileEntry* res = p_archive->findIdResource(id);

    if (res == NULL) {
        return -1;
    }

    return res - p_archive->mFiles;
}

void mDoExt_MtxCalcAnmBlendTbl::calc() {
    u16 jntNo = getJoint()->getJntNo();
    j3dSys.setCurrentMtxCalc(this);
    if (mNum == 1) {
        J3DTransformInfo info1;
        mAnmRatio->getAnmTransform()->getTransform(jntNo, &info1);
        J3DMtxCalcCalcTransformMaya::calcTransform(info1);
        return;
    }
    J3DTransformInfo info2;
    Quaternion quat1;
    Quaternion quat2;
    Quaternion quat3;
    mAnmRatio->getAnmTransform()->getTransform(jntNo, &info2);
    JMAEulerToQuat(info2.mRotation.x, info2.mRotation.y, info2.mRotation.z, &quat1);
    f32 f30 = mAnmRatio->getRatio();
    quat3 = quat1;
    for (int i = 1; i < mNum; i++) {
        if (mAnmRatio[i].getAnmTransform() != NULL) {
            J3DTransformInfo info3;
            mAnmRatio[i].getAnmTransform()->getTransform(jntNo, &info3);
            f32 ratio = mAnmRatio[i].getRatio();
            f30 = 1.0f - ratio;
            JMAEulerToQuat(info3.mRotation.x, info3.mRotation.y, info3.mRotation.z, &quat2);
            JMAQuatLerp(&quat1, &quat2, ratio, &quat3);
            quat1 = quat3;
            info2.mTranslate.x = info2.mTranslate.x * f30 + info3.mTranslate.x * ratio;
            info2.mTranslate.y = info2.mTranslate.y * f30 + info3.mTranslate.y * ratio;
            info2.mTranslate.z = info2.mTranslate.z * f30 + info3.mTranslate.z * ratio;
            info2.mScale.x = info2.mScale.x * f30 + info3.mScale.x * ratio;
            info2.mScale.y = info2.mScale.y * f30 + info3.mScale.y * ratio;
            info2.mScale.z = info2.mScale.z * f30 + info3.mScale.z * ratio;
        }
    }
    Mtx mtx;
    mDoMtx_quat(mtx, &quat3);
    mDoExt_setJ3DData(mtx, &info2, jntNo);
}


void mDoExt_MtxCalcAnmBlendTblOld::calc() {
    u16 jntNo = getJoint()->getJntNo();
    j3dSys.setCurrentMtxCalc(this);
    J3DModelData* modelData = j3dSys.getModel()->getModelData();
    J3DTransformInfo info1;
    Quaternion quat1;
    Quaternion quat2;
    Quaternion quat3;
    mAnmRatio->getAnmTransform()->getTransform(jntNo, &info1);
    JMAEulerToQuat(info1.mRotation.x, info1.mRotation.y, info1.mRotation.z, &quat1);
    f32 var_f31;
    f32 var_f30 = mAnmRatio[0].getRatio();
    quat3 = quat1;
    for (int i = 1; i < mNum; i++) {
        if (mAnmRatio[i].getAnmTransform()) {
            J3DTransformInfo info2;
            mAnmRatio[i].getAnmTransform()->getTransform(jntNo, &info2);
            var_f31 = mAnmRatio[i].getRatio();
            var_f30 = 1.0f - var_f31;
            JMAEulerToQuat(info2.mRotation.x, info2.mRotation.y, info2.mRotation.z, &quat2);
            JMAQuatLerp(&quat1, &quat2, var_f31, &quat3);
            quat1 = quat3;
            info1.mTranslate.x = info1.mTranslate.x * var_f30 + info2.mTranslate.x * var_f31;
            info1.mTranslate.y = info1.mTranslate.y * var_f30 + info2.mTranslate.y * var_f31;
            info1.mTranslate.z = info1.mTranslate.z * var_f30 + info2.mTranslate.z * var_f31;
            info1.mScale.x = info1.mScale.x * var_f30 + info2.mScale.x * var_f31;
            info1.mScale.y = info1.mScale.y * var_f30 + info2.mScale.y * var_f31;
            info1.mScale.z = info1.mScale.z * var_f30 + info2.mScale.z * var_f31;
        }
    }

    J3DTransformInfo* oldTransInfo = mOldFrame->getOldFrameTransInfo(jntNo);
    Quaternion* oldQuat = mOldFrame->getOldFrameQuaternion(jntNo);
    if (mOldFrame->getOldFrameFlg()) {
        if (mOldFrame->getOldFrameRate() > 0.0f && mOldFrame->getOldFrameStartJoint() <= jntNo && mOldFrame->getOldFrameEndJoint() > jntNo) {
            var_f30 = mOldFrame->getOldFrameRate();
            var_f31 = 1.0f - var_f30;
            JMAQuatLerp(oldQuat, &quat1, var_f31, &quat3);
            info1.mTranslate.x = info1.mTranslate.x * var_f31 + oldTransInfo->mTranslate.x * var_f30;
            info1.mTranslate.y = info1.mTranslate.y * var_f31 + oldTransInfo->mTranslate.y * var_f30;
            info1.mTranslate.z = info1.mTranslate.z * var_f31 + oldTransInfo->mTranslate.z * var_f30;
            info1.mScale.x = info1.mScale.x * var_f31 + oldTransInfo->mScale.x * var_f30;
            info1.mScale.y = info1.mScale.y * var_f31 + oldTransInfo->mScale.y * var_f30;
            info1.mScale.z = info1.mScale.z * var_f31 + oldTransInfo->mScale.z * var_f30;
        }
    } else if (jntNo == modelData->getJointNum() - 1) {
        mOldFrame->onOldFrameFlg();
    }
    Mtx mtx;
    mDoMtx_quat(mtx, &quat3);
    mDoExt_setJ3DData(mtx, &info1, jntNo);
    *oldQuat = quat3;
    *oldTransInfo = info1;
    if (jntNo == modelData->getJointNum() - 1) {
        mOldFrame->decOldFrameMorfCounter();
    }
}

void mDoExt_MtxCalcOldFrame::initOldFrameMorf(f32 param_0, u16 frameStartJoint, u16 frameEndJoint) {
    if (param_0 > 0.0f) {
        mOldFrameMorfCounter = param_0;
        field_0x8 = 1.0f / param_0;
        mOldFrameRate = 1.0f;
        field_0x10 = 1.0f;
        field_0x14 = 1.0f;
        decOldFrameMorfCounter();
    } else {
        mOldFrameMorfCounter = 0.0f;
        field_0x8 = 0.0f;
        mOldFrameRate = 0.0f;
        field_0x10 = 0.0f;
        field_0x14 = 0.0f;
    }

    mOldFrameStartJoint = frameStartJoint;
    mOldFrameEndJoint = frameEndJoint;
}

void mDoExt_MtxCalcOldFrame::decOldFrameMorfCounter() {
    if (mOldFrameMorfCounter > 0.0f) {

        mOldFrameMorfCounter -= 1.0f;
        if (mOldFrameMorfCounter <= 0.0f) {
            mOldFrameMorfCounter = 0.0f;
            field_0x8 = 0.0f;
            mOldFrameRate = 0.0f;
        }

        field_0x14 = field_0x10;
        field_0x10 = mOldFrameMorfCounter * field_0x8;

        if (field_0x14 > 0.0f) {
            mOldFrameRate = 1.0f - (field_0x14 - field_0x10) / field_0x14;
        } else {
            mOldFrameRate = 0.0f;
        }
    }
}

mDoExt_morf_c::mDoExt_morf_c() {
    mpModel = NULL;
    mpAnm = NULL;
    mpTransformInfo = NULL;
    mpQuat = NULL;
}

mDoExt_morf_c::~mDoExt_morf_c() {
    /* empty function */
}

void mDoExt_morf_c::setMorf(f32 i_morf) {
    if (mPrevMorf < 0.0f || i_morf == 0.0f) {
        mCurMorf = 1.0f;
    } else {
        mCurMorf = 0.0f;
        mMorfStep = 0.0f;
        field_0x34 = 1.0f / i_morf;
    }

    mPrevMorf = mCurMorf;
}

void mDoExt_morf_c::frameUpdate() {
    if (mCurMorf < 1.0f) {
        mPrevMorf = mCurMorf;

        if (field_0x34 < 0.0f) {
            cLib_chaseF(&mMorfStep, 1.0f, -field_0x34);
            mCurMorf += mMorfStep * ((1.0f - mCurMorf) * mMorfStep);
        } else {
            cLib_chaseF(&mCurMorf, 1.0f, field_0x34);
        }
    }

    mFrameCtrl.update();
}

mDoExt_McaMorf::mDoExt_McaMorf(J3DModelData* modelData, mDoExt_McaMorfCallBack1_c* callback1,
                                   mDoExt_McaMorfCallBack2_c* callback2, J3DAnmTransform* anmTransform,
                                   int param_4, f32 param_5, int param_6, int param_7, int param_8,
                                   void* param_9, u32 param_10, u32 param_11) {
    field_0x50 = false;
    field_0x51 = false;
    field_0x52 = false;
    create(modelData, callback1, callback2, anmTransform, param_4, param_5, param_6, param_7, param_8, param_9, param_10, param_11);
}

mDoExt_McaMorf::~mDoExt_McaMorf() {
    if (field_0x50 && mpSound != NULL) {
        mpSound->stopAnime();
    }
}

int mDoExt_McaMorf::create(J3DModelData* modelData, mDoExt_McaMorfCallBack1_c* callback1,
                           mDoExt_McaMorfCallBack2_c* callback2, J3DAnmTransform* anmTransform,
                           int param_4, f32 param_5, int param_6, int param_7, int param_8,
                           void* param_9, u32 param_10, u32 param_11) {
    mpModel = NULL;
    mpSound = NULL;
    mpTransformInfo = NULL;
    mpQuat = NULL;
    if (!modelData) {
        return 0;
    }
    if (modelData->getMaterialNodePointer(0)->getSharedDisplayListObj() && param_10 == 0) {
        if (modelData->isLocked()) {
            param_10 = 0x20000;
        } else {
            param_10 = 0x80000;
        }
    }
    mpModel = mDoExt_J3DModel__create(modelData, param_10, param_11);
    if (!mpModel) {
        return 0;
    }
    if (param_10 != 0x80000) {
        mDoExt_changeMaterial(mpModel);
    }
    if (param_9 == NULL && anmTransform) {
        param_9 = ((mDoExt_transAnmBas*)anmTransform)->getBas();
        if (param_9) {
            param_8 = 1;
        }
    }
    if (param_8) {
        mpSound = JKR_NEW mDoExt_zelAnime();
        if (!mpSound) {
            goto cleanup;
        }
    }
    setAnm(anmTransform, param_4, 0.0f, param_5, param_6, param_7, param_9);
    mPrevMorf = -1.0f;
    mpTransformInfo = JKR_NEW_ARRAY(J3DTransformInfo, modelData->getJointNum());
    if (!mpTransformInfo) {
        goto cleanup;
    }
    mpQuat = JKR_NEW_ARRAY(Quaternion, modelData->getJointNum());
    if (mpQuat) {
        J3DTransformInfo* info = mpTransformInfo;
        Quaternion* quat = mpQuat;
        J3DModelData* r23 = mpModel->getModelData();
        int jointNum = r23->getJointNum();
        for (int i = 0; i < jointNum; i++) {
            J3DJoint* joint = r23->getJointNodePointer(i);
            J3DTransformInfo& transInfo = joint->getTransformInfo();
            *info = transInfo;
            JMAEulerToQuat(info->mRotation.x, info->mRotation.y, info->mRotation.z, quat);
            info++;
            quat++;
        }
        mpCallback1 = callback1;
        mpCallback2 = callback2;
        return 1;
    }
    cleanup:
    if (mpSound) {
        mpSound->stopAnime();
        mpSound = NULL;
    }
    if (mpTransformInfo) {
        mpTransformInfo = NULL;
    }
    if (mpQuat) {
        mpQuat = NULL;
    }
    if (mpModel) {
        mpModel = NULL;
    }
    return 0;
}

void mDoExt_McaMorf::calc() {
    if (mpModel == NULL) {
        return;
    }

    u16 jntNo = getJoint()->getJntNo();
    j3dSys.setCurrentMtxCalc(this);

    J3DTransformInfo sp48;
    J3DTransformInfo sp28;
    J3DTransformInfo* var_r29;
    if (mpTransformInfo == NULL) {
        var_r29 = &sp48;
    } else {
        var_r29 = &mpTransformInfo[jntNo];
    }

    Quaternion sp18;
    Quaternion* var_r27;
    if (mpQuat == NULL) {
        var_r27 = &sp18;
    } else {
        var_r27 = &mpQuat[jntNo];
    }

    if (mpAnm == NULL) {
        *var_r29 = mpModel->getModelData()->getJointNodePointer(jntNo)->getTransformInfo();
        if (mpCallback1 != NULL) {
            mpCallback1->execute(jntNo, var_r29);
        }

        JMAEulerToQuat(var_r29->mRotation.x, var_r29->mRotation.y, var_r29->mRotation.z, var_r27);
        J3DMtxCalcCalcTransformMaya::calcTransform(*var_r29);
    } else if (mCurMorf >= 1.0f || mpTransformInfo == NULL || mpQuat == NULL) {
        getTransform(jntNo, var_r29);
        if (mpCallback1 != NULL) {
            mpCallback1->execute(jntNo, var_r29);
        }
        JMAEulerToQuat(var_r29->mRotation.x, var_r29->mRotation.y, var_r29->mRotation.z, var_r27);
        J3DMtxCalcCalcTransformMaya::calcTransform(*var_r29);
    } else {
        f32 var_f31;
        if (field_0x52) {
            var_f31 = 1.0f;
        } else {
            var_f31 = (mCurMorf - mPrevMorf) / (1.0f - mPrevMorf);
        }
        f32 var_f30 = 1.0f - var_f31;

        Quaternion sp8;
        getTransform(jntNo, &sp28);

        if (mpCallback1 != NULL) {
            mpCallback1->execute(jntNo, &sp28);
        }

        JMAEulerToQuat(sp28.mRotation.x, sp28.mRotation.y, sp28.mRotation.z, &sp8);
        JMAQuatLerp(var_r27, &sp8, var_f31, var_r27);

        Mtx sp68;
        mDoMtx_quat(sp68, var_r27);

        var_r29->mTranslate.x = var_r29->mTranslate.x * var_f30 + sp28.mTranslate.x * var_f31;
        var_r29->mTranslate.y = var_r29->mTranslate.y * var_f30 + sp28.mTranslate.y * var_f31;
        var_r29->mTranslate.z = var_r29->mTranslate.z * var_f30 + sp28.mTranslate.z * var_f31;
        var_r29->mScale.x = var_r29->mScale.x * var_f30 + sp28.mScale.x * var_f31;
        var_r29->mScale.y = var_r29->mScale.y * var_f30 + sp28.mScale.y * var_f31;
        var_r29->mScale.z = var_r29->mScale.z * var_f30 + sp28.mScale.z * var_f31;
        mDoExt_setJ3DData(sp68, var_r29, jntNo);
    }

    if (mpCallback2 != NULL) {
        mpCallback2->execute(jntNo);
    }
}

void mDoExt_McaMorf::setAnm(J3DAnmTransform* param_0, int param_1, f32 param_2, f32 param_3,
                                f32 param_4, f32 param_5, void* param_6) {
    mpAnm = param_0;
    setStartFrame(param_4);
    if (param_5 < 0.0f) {
        if (!mpAnm) {
            mFrameCtrl.init(0);
        } else {
            mFrameCtrl.init(mpAnm->getFrameMax());
        }
    } else {
        mFrameCtrl.init((s16)param_5);
    }
    if (param_0 && param_1 < 0) {
        param_1 = param_0->getAttribute();
    }
    setPlayMode(param_1);
    setPlaySpeed(param_3);
    if (param_3 >= 0.0f) {
        setFrame(param_4);
    } else {
        setFrame(getEndFrame());
    }
    setLoopFrame(getFrame());
    setMorf(param_2);
    if (mpSound) {
        if (param_6 == NULL && param_0) {
            param_6 = ((mDoExt_transAnmBas*)param_0)->getBas();
        }
        mpSound->field_0x48 = param_6;
        if (mpSound->field_0x48) {
            mpSound->initAnime(param_6, getPlaySpeed() >= 0.0f, getLoopFrame(), 0.0f);
        } else {
            mpSound->stopAnime();
        }
    }
}

u32 mDoExt_McaMorf::play(Vec* param_0, u32 param_1, s8 param_2) {
    frameUpdate();
    if (mpSound != NULL && mpSound->field_0x48 != NULL && param_0 != NULL) {
        mpSound->updateAnime(getFrame(), getPlaySpeed());
        mpSound->framework(param_1, param_2);
        field_0x50 = true;
    }
    return isStop();
}

void mDoExt_McaMorf::entryDL() {
    if (mpModel != NULL) {
        mDoExt_modelEntryDL(mpModel);
    }
}

void mDoExt_McaMorf::modelCalc() {
    if (mpModel != NULL) {
        if (mpAnm != NULL) {
            mpAnm->setFrame(mFrameCtrl.getFrame());
        }

        mpModel->getModelData()->getJointNodePointer(0)->setMtxCalc(this);
        mpModel->calc();
    }
}

void mDoExt_McaMorf::getTransform(u16 param_0, J3DTransformInfo* param_1) {
    mpAnm->getTransform(param_0, param_1);
    if (field_0x51) {
        if (param_0 == 0) {
            param_1->mTranslate.x *= mTranslateScale.x;
            param_1->mTranslate.y *= mTranslateScale.y;
            param_1->mTranslate.z *= mTranslateScale.z;
        } else {
            J3DTransformInfo& info = mpModel->getModelData()->getJointNodePointer(param_0)->getTransformInfo();
            param_1->mTranslate.x = info.mTranslate.x;
            param_1->mTranslate.y = info.mTranslate.y;
            param_1->mTranslate.z = info.mTranslate.z;
        }
    }
}

mDoExt_McaMorfSO::mDoExt_McaMorfSO(J3DModelData* param_0, mDoExt_McaMorfCallBack1_c* param_1,
                                   mDoExt_McaMorfCallBack2_c* param_2, J3DAnmTransform* param_3,
                                   int param_4, f32 param_5, int param_6, int param_7,
                                   Z2Creature* param_8, u32 i_modelFlag, u32 i_differedDlistFlag) {
    mTranslate = false;
    mMorfNone = false;
    create(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8,
           i_modelFlag, i_differedDlistFlag);
}

mDoExt_McaMorfSO::~mDoExt_McaMorfSO() {
    stopZelAnime();
}

int mDoExt_McaMorfSO::create(J3DModelData* i_modelData, mDoExt_McaMorfCallBack1_c* param_1,
                             mDoExt_McaMorfCallBack2_c* param_2, J3DAnmTransform* param_3,
                             int param_4, f32 param_5, int param_6, int param_7,
                             Z2Creature* i_sound, u32 i_modelFlag, u32 i_differedDlistFlag) {
    mpModel = NULL;
    mpTransformInfo = NULL;
    mpQuat = NULL;
    mpSound = NULL;

    if (i_modelData == NULL) {
        return 0;
    }

    if (i_modelData->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL &&
        i_modelFlag == 0)
    {
        if (i_modelData->isLocked()) {
            i_modelFlag = 0x20000;
        } else {
            i_modelFlag = 0x80000;
        }
    }

    mpModel = mDoExt_J3DModel__create(i_modelData, i_modelFlag, i_differedDlistFlag);
    if (mpModel == NULL) {
        return 0;
    }

    if (i_modelFlag != 0x80000) {
        mDoExt_changeMaterial(mpModel);
    }

    mpSound = i_sound;

    if (param_3 != NULL) {
        mpBas = ((mDoExt_transAnmBas*)param_3)->getBas();
    }

    setAnm(param_3, param_4, 0.0f, param_5, param_6, param_7);
    mPrevMorf = -1.0f;

    mpTransformInfo = JKR_NEW_ARRAY(J3DTransformInfo, i_modelData->getJointNum());
    if (mpTransformInfo != NULL) {
        mpQuat = JKR_NEW_ARRAY(Quaternion, i_modelData->getJointNum());

        if (mpQuat != NULL) {
            J3DTransformInfo* transInfo_p = mpTransformInfo;
            Quaternion* quat = mpQuat;
            J3DModelData* modelData = mpModel->getModelData();
            int jointNum = modelData->getJointNum();

            for (int i = 0; i < jointNum; i++) {
                J3DJoint* joint = modelData->getJointNodePointer(i);
                J3DTransformInfo& transInfo = joint->getTransformInfo();;
                *transInfo_p = transInfo;
                JMAEulerToQuat(transInfo_p->mRotation.x, transInfo_p->mRotation.y,
                               transInfo_p->mRotation.z, quat);

                transInfo_p++;
                quat++;
            }

            mpCallback1 = param_1;
            mpCallback2 = param_2;
            return 1;
        }
    }

    if (mpTransformInfo != NULL) {
        mpTransformInfo = NULL;
    }

    if (mpQuat != NULL) {
        mpQuat = NULL;
    }

    if (mpModel != NULL) {
        mpModel = NULL;
    }

    return 0;
}

void mDoExt_McaMorfSO::calc() {
    if (mpModel != NULL) {
        u16 jnt_no = getJoint()->getJntNo();
        j3dSys.setCurrentMtxCalc(this);

        J3DTransformInfo trans;
        J3DTransformInfo* trans_p;
        if (mpTransformInfo == NULL) {
            trans_p = &trans;
        } else {
            trans_p = &mpTransformInfo[jnt_no];
        }

        Quaternion quat;
        Quaternion* quat_p;
        if (mpQuat == NULL) {
            quat_p = &quat;
        } else {
            quat_p = &mpQuat[jnt_no];
        }

        if (mpAnm == NULL) {
            *trans_p = mpModel->getModelData()->getJointNodePointer(jnt_no)->getTransformInfo();

            if (mpCallback1 != NULL) {
                mpCallback1->execute(jnt_no, trans_p);
            }

            JMAEulerToQuat(trans_p->mRotation.x, trans_p->mRotation.y, trans_p->mRotation.z,
                           quat_p);
            J3DMtxCalcCalcTransformMaya::calcTransform(*trans_p);
        } else if (mCurMorf >= 1.0f || mpTransformInfo == NULL || mpQuat == NULL) {
            getTransform(jnt_no, trans_p);

            if (mpCallback1 != NULL) {
                mpCallback1->execute(jnt_no, trans_p);
            }

            JMAEulerToQuat(trans_p->mRotation.x, trans_p->mRotation.y, trans_p->mRotation.z,
                           quat_p);
            J3DMtxCalcCalcTransformMaya::calcTransform(*trans_p);
        } else {
            f32 lerp_factor;
            if (mMorfNone) {
                lerp_factor = 1.0f;
            } else {
                lerp_factor = (mCurMorf - mPrevMorf) / (1.0f - mPrevMorf);
            }
            f32 inv_lerp_factor = 1.0f - lerp_factor;

            J3DTransformInfo trans2;
            getTransform(jnt_no, &trans2);

            if (mpCallback1 != NULL) {
                mpCallback1->execute(jnt_no, &trans2);
            }

            Quaternion quat2;
            JMAEulerToQuat(trans2.mRotation.x, trans2.mRotation.y, trans2.mRotation.z, &quat2);
            JMAQuatLerp(quat_p, &quat2, lerp_factor, quat_p);

            Mtx mtx;
            mDoMtx_quat(mtx, quat_p);

            trans_p->mTranslate.x = trans_p->mTranslate.x * inv_lerp_factor
                                    + trans2.mTranslate.x * lerp_factor;
            trans_p->mTranslate.y = trans_p->mTranslate.y * inv_lerp_factor
                                    + trans2.mTranslate.y * lerp_factor;
            trans_p->mTranslate.z = trans_p->mTranslate.z * inv_lerp_factor
                                    + trans2.mTranslate.z * lerp_factor;
            trans_p->mScale.x = trans_p->mScale.x * inv_lerp_factor
                                + trans2.mScale.x * lerp_factor;
            trans_p->mScale.y = trans_p->mScale.y * inv_lerp_factor
                                + trans2.mScale.y * lerp_factor;
            trans_p->mScale.z = trans_p->mScale.z * inv_lerp_factor
                                + trans2.mScale.z * lerp_factor;

            mDoExt_setJ3DData(mtx, trans_p, jnt_no);
        }

        if (mpCallback2 != NULL) {
            mpCallback2->execute(jnt_no);
        }
    }
}

void mDoExt_McaMorfSO::setAnm(J3DAnmTransform* i_anm, int i_attr, f32 i_morf, f32 i_rate,
                              f32 i_start, f32 i_end) {
    mpAnm = i_anm;
    setStartFrame(i_start);

    if (i_end < 0.0f) {
        if (mpAnm == NULL) {
            mFrameCtrl.init(0);
        } else {
            mFrameCtrl.init(mpAnm->getFrameMax());
        }
    } else {
        mFrameCtrl.init((s16)i_end);
    }

    if (i_anm != NULL && i_attr < 0) {
        i_attr = i_anm->getAttribute();
    }

    setPlayMode(i_attr);
    setPlaySpeed(i_rate);

    if (i_rate >= 0.0f) {
        setFrame(i_start);
    } else {
        setFrame(getEndFrame());
    }

    setLoopFrame(getFrame());
    setMorf(i_morf);

    if (mpSound != NULL) {
        if (i_anm != NULL) {
            mpBas = static_cast<mDoExt_transAnmBas*>(i_anm)->getBas();
        } else {
            mpBas = NULL;
        }

        if (mpBas != NULL) {
            mpSound->initAnime(mpBas, getPlaySpeed() >= 0.0f, getLoopFrame(), getFrame());
        }
    }
}

int mDoExt_McaMorfSO::play(u32 param_0, s8 param_1) {
    frameUpdate();

    if (mpSound != NULL) {
        mpSound->framework(param_0, param_1);

        if (mpBas != NULL) {
            mpSound->updateAnime(getFrame(), getPlaySpeed());
        }
    }

    return isStop();
}

void mDoExt_McaMorfSO::updateDL() {
    if (mpModel != NULL) {
        if (mpAnm != NULL) {
            mpAnm->setFrame(mFrameCtrl.getFrame());
        }

        mpModel->getModelData()->getJointNodePointer(0)->setMtxCalc(this);
        mDoExt_modelUpdateDL(mpModel);
        mPrevMorf = mCurMorf;
    }
}

void mDoExt_McaMorfSO::entryDL() {
    if (mpModel != NULL) {
        mDoExt_modelEntryDL(mpModel);
    }
}

void mDoExt_McaMorfSO::modelCalc() {
    if (mpModel != NULL) {
        if (mpAnm != NULL) {
            mpAnm->setFrame(mFrameCtrl.getFrame());
        }

        mpModel->getModelData()->getJointNodePointer(0)->setMtxCalc(this);
        mpModel->calc();
    }
}

void mDoExt_McaMorfSO::getTransform(u16 param_0, J3DTransformInfo* param_1) {
    mpAnm->getTransform(param_0, param_1);
    if (mTranslate) {
        if (param_0 == 0) {
            param_1->mTranslate.x *= mTranslateScale.x;
            param_1->mTranslate.y *= mTranslateScale.y;
            param_1->mTranslate.z *= mTranslateScale.z;
        } else {
            J3DTransformInfo& info = mpModel->getModelData()->getJointNodePointer(param_0)->getTransformInfo();
            param_1->mTranslate.x = info.mTranslate.x;
            param_1->mTranslate.y = info.mTranslate.y;
            param_1->mTranslate.z = info.mTranslate.z;
        }
    }
}

void mDoExt_McaMorfSO::stopZelAnime() {
    if (mpSound != NULL) {
        mpSound->deleteObject();
    }
}

mDoExt_McaMorf2::mDoExt_McaMorf2(J3DModelData* param_0, mDoExt_McaMorfCallBack1_c* param_1,
                                 mDoExt_McaMorfCallBack2_c* param_2, J3DAnmTransform* param_3,
                                 J3DAnmTransform* param_4, int param_5, f32 param_6, int param_7,
                                 int param_8, Z2Creature* param_9, u32 param_10, u32 param_11) {
    create(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9,
           param_10, param_11);
}

mDoExt_McaMorf2::~mDoExt_McaMorf2() {
    stopZelAnime();
}

 int mDoExt_McaMorf2::create(J3DModelData* param_0, mDoExt_McaMorfCallBack1_c* param_1,
                                 mDoExt_McaMorfCallBack2_c* param_2, J3DAnmTransform* param_3,
                                 J3DAnmTransform* param_4, int param_5, f32 param_6, int param_7,
                                 int param_8, Z2Creature* param_9, u32 param_10, u32 param_11) {
    mpModel = NULL;
    mpTransformInfo = NULL;
    mpQuat = NULL;
    mpSound = NULL;

    if (param_0 == NULL) {
        return 0;
    }

    if (param_0->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL && param_10 == 0) {
        if (param_0->isLocked()) {
            param_10 = 0x20000;
        } else {
            param_10 = 0x80000;
        }
    }

    mpModel = mDoExt_J3DModel__create(param_0, param_10, param_11);
    if (mpModel == NULL) {
        return 0;
    }

    if (param_10 != 0x80000) {
        mDoExt_changeMaterial(mpModel);
    }

    mpSound = param_9;

    setAnm(param_3, param_4, 0.0f, param_5, 0.0f, param_6, param_7, param_8);
    mPrevMorf = -1.0f;

    mpTransformInfo = JKR_NEW_ARRAY(J3DTransformInfo, param_0->getJointNum());
    if (mpTransformInfo == NULL) {
        ERROR_EXIT();
        return 0;
    }

    mpQuat = JKR_NEW_ARRAY(Quaternion, param_0->getJointNum());
    if (mpQuat == NULL) {
        ERROR_EXIT();
        return 0;
    }

    J3DTransformInfo* var_r29 = mpTransformInfo;
    Quaternion* var_r26 = mpQuat;
    J3DModelData* model_data = mpModel->getModelData();
    int sp3C = model_data->getJointNum();

    for (int i = 0; i < sp3C; i++) {
        J3DJoint* sp30 = model_data->getJointNodePointer(i);
        J3DTransformInfo* sp2C = &sp30->getTransformInfo();

        *var_r29 = *sp2C;
        JMAEulerToQuat(var_r29->mRotation.x, var_r29->mRotation.y, var_r29->mRotation.z, var_r26);

        var_r29++;
        var_r26++;
    }

    mpCallback1 = param_1;
    mpCallback2 = param_2;
    return 1;
}

void mDoExt_McaMorf2::ERROR_EXIT() {
    if (mpTransformInfo != NULL) {
        mpTransformInfo = NULL;
    }

    if (mpQuat != NULL) {
        mpQuat = NULL;
    }

    if (mpModel != NULL) {
        mpModel = NULL;
    }
}

void mDoExt_McaMorf2::calc() {
    if (mpModel != NULL) {
        u16 jnt_no = J3DMtxCalc::getJoint()->getJntNo();
        j3dSys.setCurrentMtxCalc(this);

        J3DTransformInfo spF0[2];
        Mtx spC0;

        J3DTransformInfo spA0;
        J3DTransformInfo sp80;
        J3DTransformInfo* var_r30;
        if (mpTransformInfo == NULL) {
            var_r30 = &spA0;
        } else {
            var_r30 = &mpTransformInfo[jnt_no];
        }

        Quaternion sp60[2];
        Quaternion sp40[2];

        Quaternion sp30;
        Quaternion sp20;
        Quaternion* var_r27;
        f32 var_f31;
        f32 var_f30;
        f32 var_f29;
        f32 sp18[2];
        f32 sp10[2];
        if (mpQuat == NULL) {
            var_r27 = &sp30;
        } else {
            var_r27 = &mpQuat[jnt_no];
        }

        if (mpAnm == NULL) {
            *var_r30 = mpModel->getModelData()->getJointNodePointer(jnt_no)->getTransformInfo();

            if (mpCallback1 != NULL) {
                mpCallback1->execute(jnt_no, var_r30);
            }

            JMAEulerToQuat(var_r30->mRotation.x, var_r30->mRotation.y, var_r30->mRotation.z,
                           var_r27);
            J3DMtxCalcCalcTransformMaya::calcTransform(*var_r30);
        } else if (mCurMorf >= 1.0f || mpTransformInfo == NULL || mpQuat == NULL) {
            mpAnm->getTransform(jnt_no, &spF0[0]);
            if (field_0x40 == NULL) {
                if (mpCallback1 != NULL) {
                    mpCallback1->execute(jnt_no, &spF0[0]);
                }

                JMAEulerToQuat(spF0[0].mRotation.x, spF0[0].mRotation.y, spF0[0].mRotation.z,
                               var_r27);
                J3DMtxCalcCalcTransformMaya::calcTransform(spF0[0]);
                *var_r30 = spF0[0];
            } else {
                field_0x40->getTransform(jnt_no, &spF0[1]);

                sp18[0] = 1.0f - mAnmRate;
                sp18[1] = mAnmRate;

                var_r30->mScale.x = spF0[0].mScale.x * sp18[0]
                                    + spF0[1].mScale.x * sp18[1];
                var_r30->mScale.y = spF0[0].mScale.y * sp18[0]
                                    + spF0[1].mScale.y * sp18[1];
                var_r30->mScale.z = spF0[0].mScale.z * sp18[0]
                                    + spF0[1].mScale.z * sp18[1];
                var_r30->mTranslate.x = spF0[0].mTranslate.x * sp18[0]
                                    + spF0[1].mTranslate.x * sp18[1];
                var_r30->mTranslate.y = spF0[0].mTranslate.y * sp18[0]
                                        + spF0[1].mTranslate.y * sp18[1];
                var_r30->mTranslate.z = spF0[0].mTranslate.z * sp18[0]
                                        + spF0[1].mTranslate.z * sp18[1];

                for (int i = 0; i < 2; i++) {
                    JMAEulerToQuat(spF0[i].mRotation.x, spF0[i].mRotation.y, spF0[i].mRotation.z, &sp60[i]);
                }

                var_f29 = sp18[1] / (sp18[0] + sp18[1]);
                JMAQuatLerp(&sp60[0], &sp60[1], var_f29, var_r27);

                mDoMtx_quat(spC0, var_r27);
                mDoExt_setJ3DData(spC0, var_r30, jnt_no);
            }
        } else if (field_0x40 == NULL) {
            var_f31 = (mCurMorf - mPrevMorf) / (1.0f - mPrevMorf);
            var_f30 = 1.0f - var_f31;

            mpAnm->getTransform(jnt_no, &sp80);
            if (mpCallback1 != NULL) {
                mpCallback1->execute(jnt_no, &sp80);
            }

            JMAEulerToQuat(sp80.mRotation.x, sp80.mRotation.y, sp80.mRotation.z, &sp20);
            JMAQuatLerp(var_r27, &sp20, var_f31, var_r27);
            mDoMtx_quat(spC0, var_r27);

            var_r30->mTranslate.x = var_r30->mTranslate.x * var_f30
                                    + sp80.mTranslate.x * var_f31;
            var_r30->mTranslate.y = var_r30->mTranslate.y * var_f30
                                    + sp80.mTranslate.y * var_f31;
            var_r30->mTranslate.z = var_r30->mTranslate.z * var_f30
                                    + sp80.mTranslate.z * var_f31;
            var_r30->mScale.x = var_r30->mScale.x * var_f30
                                + sp80.mScale.x * var_f31;
            var_r30->mScale.y = var_r30->mScale.y * var_f30
                                + sp80.mScale.y * var_f31;
            var_r30->mScale.z = var_r30->mScale.z * var_f30
                                + sp80.mScale.z * var_f31;

            mDoExt_setJ3DData(spC0, var_r30, jnt_no);
        } else {
            mpAnm->getTransform(jnt_no, &spF0[0]);
            field_0x40->getTransform(jnt_no, &spF0[1]);

            sp10[0] = 1.0f - mAnmRate;
            sp10[1] = mAnmRate;

            sp80.mScale.x = spF0[0].mScale.x * sp10[0]
                                + spF0[1].mScale.x * sp10[1];
            sp80.mScale.y = spF0[0].mScale.y * sp10[0]
                                + spF0[1].mScale.y * sp10[1];
            sp80.mScale.z = spF0[0].mScale.z * sp10[0]
                                + spF0[1].mScale.z * sp10[1];
            sp80.mTranslate.x = spF0[0].mTranslate.x * sp10[0]
                                + spF0[1].mTranslate.x * sp10[1];
            sp80.mTranslate.y = spF0[0].mTranslate.y * sp10[0]
                                    + spF0[1].mTranslate.y * sp10[1];
            sp80.mTranslate.z = spF0[0].mTranslate.z * sp10[0]
                                    + spF0[1].mTranslate.z * sp10[1];

            for (int i = 0; i < 2; i++) {
                JMAEulerToQuat(spF0[i].mRotation.x, spF0[i].mRotation.y, spF0[i].mRotation.z, &sp40[i]);
            }

            var_f31 = sp10[1] / (sp10[0] + sp10[1]);
            JMAQuatLerp(&sp40[0], &sp40[1], var_f31, &sp20);

            var_f31 = (mCurMorf - mPrevMorf) / (1.0f - mPrevMorf);
            var_f30 = 1.0f - var_f31;
            JMAQuatLerp(var_r27, &sp20, var_f31, var_r27);

            var_r30->mTranslate.x = var_r30->mTranslate.x * var_f30
                                    + sp80.mTranslate.x * var_f31;
            var_r30->mTranslate.y = var_r30->mTranslate.y * var_f30
                                    + sp80.mTranslate.y * var_f31;
            var_r30->mTranslate.z = var_r30->mTranslate.z * var_f30
                                    + sp80.mTranslate.z * var_f31;
            var_r30->mScale.x = var_r30->mScale.x * var_f30
                                + sp80.mScale.x * var_f31;
            var_r30->mScale.y = var_r30->mScale.y * var_f30
                                + sp80.mScale.y * var_f31;
            var_r30->mScale.z = var_r30->mScale.z * var_f30
                                + sp80.mScale.z * var_f31;

            mDoMtx_quat(spC0, var_r27);
            mDoExt_setJ3DData(spC0, var_r30, jnt_no);
        }

        if (mpCallback2 != NULL) {
            mpCallback2->execute(jnt_no);
        }
    }
}

void mDoExt_McaMorf2::setAnm(J3DAnmTransform* param_0, J3DAnmTransform* param_1, f32 param_2,
                             int i_attr, f32 i_morf, f32 i_speed, f32 i_start, f32 i_end) {
    mpAnm = param_0;
    field_0x40 = param_1;
    mAnmRate = param_2;

    setStartFrame(i_start);

    if (i_end < 0.0f) {
        if (mpAnm == NULL) {
            mFrameCtrl.init(0);
        } else {
            mFrameCtrl.init(mpAnm->getFrameMax());
        }
    } else {
        mFrameCtrl.init((s16)i_end);
    }

    if (i_attr < 0) {
        i_attr = param_0->getAttribute();
    }

    setPlayMode(i_attr);
    setPlaySpeed(i_speed);

    if (i_speed >= 0.0f) {
        setFrame(i_start);
    } else {
        setFrame(getEndFrame());
    }

    setLoopFrame(getFrame());
    setMorf(i_morf);

    if (mpSound != NULL) {
        if (param_2 < 0.5f) {
            mpBas = ((mDoExt_transAnmBas*)param_0)->getBas();
        } else {
            mpBas = ((mDoExt_transAnmBas*)param_1)->getBas();
        }

        if (mpBas != NULL) {
            mpSound->initAnime(mpBas, getPlaySpeed() >= 0.0f, getLoopFrame(), getFrame());
        }
    }
}

void mDoExt_McaMorf2::setAnmRate(f32 param_0) {
    void* pBas = NULL;
    mAnmRate = param_0;
    if (mpSound != NULL) {
        if (mAnmRate < 0.5f) {
            if (mpAnm != NULL) {
                pBas = ((mDoExt_transAnmBas*)mpAnm)->getBas();
            }
        } else if (field_0x40 != NULL) {
            pBas = ((mDoExt_transAnmBas*)field_0x40)->getBas();
        }
        if (pBas != mpBas) {
            if (pBas != NULL) {
                mpBas = pBas;
                mpSound->initAnime(mpBas, getPlaySpeed() >= 0.0f, getLoopFrame(), getFrame());
            } else {
                mpBas = NULL;
            }
        }
    }
}

int mDoExt_McaMorf2::play(u32 param_0, s8 param_1) {
    frameUpdate();

    if (mpSound != NULL) {
        mpSound->framework(param_0, param_1);

        if (mpBas != NULL) {
            mpSound->updateAnime(getFrame(), getPlaySpeed());
        }
    }

    return isStop();
}

void mDoExt_McaMorf2::entryDL() {
    if (mpModel != NULL) {
        mDoExt_modelEntryDL(mpModel);
    }
}

void mDoExt_McaMorf2::modelCalc() {
    if (mpModel != NULL) {
        if (mpAnm != NULL) {
            mpAnm->setFrame(mFrameCtrl.getFrame());
        }

        if (field_0x40 != NULL) {
            field_0x40->setFrame(mFrameCtrl.getFrame());
        }

        mpModel->getModelData()->getJointNodePointer(0)->setMtxCalc(this);
        mpModel->calc();
    }
}

void mDoExt_McaMorf2::stopZelAnime() {
    if (mpSound != NULL) {
        mpSound->deleteObject();
    }
}

void mDoExt_offCupOnAupPacket::draw() {
    GFSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR, 0, 1, 1);
}

void mDoExt_onCupOffAupPacket::draw() {
    GFSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR, 1, 0, 1);
}

void mDoExt_invJntPacket::draw() {
    J3DShape::resetVcdVatCache();

    if (field_0x16) {
        J3DModelData* sp20 = field_0x10->getModelData();
#if TARGET_PC
        j3dSys.setTexture(sp20->getTexture());
#endif
        J3DJoint* sp1C = sp20->getJointNodePointer(field_0x14);

        for (J3DMaterial* mesh = sp1C->getMesh(); mesh != NULL; mesh = mesh->getNext()) {
            mesh->load();
            J3DMatPacket* sp18 = field_0x10->getMatPacket(mesh->getIndex());
            sp18->callDL();
            GFSetZMode(1, GX_LEQUAL, 1);
            GFSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR, 0, 0, 1);

            J3DShapePacket* shapePkt = sp18->getShapePacket();
            JUT_ASSERT(5011, shapePkt != NULL);
            shapePkt->getShape()->loadPreDrawSetting();

            do {
                if (!shapePkt->getShape()->checkFlag(1)) {
                    if (shapePkt->getDisplayListObj() != NULL) {
                        shapePkt->getDisplayListObj()->callDL();
                    }

                    shapePkt->drawFast();
                }

                shapePkt = (J3DShapePacket*)shapePkt->getNextPacket();
            } while (shapePkt != NULL);
        }
    } else {
        ATTRIBUTE_ALIGN(32) static u8 l_invisibleMat[] = {
            0x10, 0x00, 0x00, 0x10, 0x0E, 0x00, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00,
            0x00, 0x04, 0x00, 0x61, 0x28, 0x38, 0x00, 0x00, 0x61, 0xC0, 0x08, 0xFF, 0xFC, 0x61, 0xC1,
            0x08, 0xFF, 0xF0, 0x61, 0xF3, 0x7F, 0x00, 0x00, 0x61, 0x43, 0x00, 0x00, 0x41, 0x61, 0x40,
            0x00, 0x00, 0x17, 0x61, 0xEE, 0x00, 0x00, 0x00, 0x61, 0xEF, 0x00, 0x00, 0x00, 0x61, 0xF0,
            0x00, 0x00, 0x00, 0x61, 0xF1, 0x00, 0x00, 0x00, 0x61, 0xF2, 0x00, 0x00, 0x00, 0x61, 0x41,
            0x00, 0x00, 0x04, 0x10, 0x00, 0x00, 0x10, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
            0x10, 0x09, 0x00, 0x00, 0x00, 0x01, 0x61, 0x00, 0x00, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        GXCallDisplayList(l_invisibleMat, 0x80);

        J3DModelData* sp14 = field_0x10->getModelData();
        J3DJoint* sp10 = sp14->getJointNodePointer(field_0x14);
        J3DMaterial* mesh = sp10->getMesh();

        sp14->getShapeNodePointer(0)->loadPreDrawSetting();

        for (; mesh != NULL; mesh = mesh->getNext()) {
            J3DShape* spC = mesh->getShape();
            if (!spC->checkFlag(1)) {
                J3DShapePacket* shapePkt = field_0x10->getShapePacket(spC->getIndex());
                shapePkt->drawFast();
            }
        }
    }

    J3DShape::resetVcdVatCache();
    GFSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR, 1, 0, 1);
}

int mDoExt_3Dline_c::init(u16 param_0, int param_1, BOOL param_2) {
    field_0x0 = JKR_NEW_ARRAY(cXyz, param_0);
    if (field_0x0 == NULL) {
        return 0;
    }

    if (param_1 != 0) {
        field_0x4 = JKR_NEW_ARRAY(f32, param_0);
        if (field_0x4 == NULL) {
            return 0;
        }
    } else {
        field_0x4 = NULL;
    }

    int sp20 = param_0 * 2;

    field_0x8[0] = JKR_NEW_ARRAY(cXyz, sp20);
    if (field_0x8[0] == NULL) {
        return 0;
    }

    field_0x8[1] = JKR_NEW_ARRAY(cXyz, sp20);
    if (field_0x8[1] == NULL) {
        return 0;
    }

    field_0x10[0] = JKR_NEW_ARRAY(mDoExt_3Dline_field_0x10_c, sp20);
    if (field_0x10[0] == NULL) {
        return 0;
    }

    field_0x10[1] = JKR_NEW_ARRAY(mDoExt_3Dline_field_0x10_c, sp20);
    if (field_0x10[1] == NULL) {
        return 0;
    }

    if (param_2) {
        field_0x18[0] = JKR_NEW_ARRAY(cXy, sp20);
        if (field_0x18[0] == NULL) {
            return 0;
        }

        field_0x18[1] = JKR_NEW_ARRAY(cXy, sp20);
        if (field_0x18[1] == NULL) {
            return 0;
        }

        cXy* tex0 = field_0x18[0];
        cXy* tex1 = field_0x18[1];
        for (s32 i = 0; i < param_0; i++) {
            tex0[0].x = 0.0f;
            tex1[0].x = 0.0f;

            tex0[1].x = 1.0f;
            tex1[1].x = 1.0f;

            tex0 += 2;
            tex1 += 2;
        }
    }

    return 1;
}

int mDoExt_3DlineMat0_c::init(u16 param_0, u16 param_1, int param_2) {
    field_0x10 = param_0;
    field_0x12 = param_1;

    field_0x18 = JKR_NEW_ARRAY(mDoExt_3Dline_c, param_0);
    if (field_0x18 == NULL) {
        return 0;
    }

    for (int i = 0; i < param_0; i++) {
        if (!field_0x18[i].init(param_1, param_2, FALSE)) {
            return 0;
        }
    }

    field_0x4 = NULL;
    field_0x16 = 0;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
    mInterpWidth = 0.0f;
    mInterpTaper = 0;
    mInterpKind = LINE_NONE;
    dusk::interp::line::reset(this);
#endif
    return 1;
}

ATTRIBUTE_ALIGN(32) static u8 l_matDL[132] = {
    0x08, 0x30, 0x3C, 0xF3, 0xCF, 0x00, 0x10, 0x00, 0x00, 0x10, 0x18, 0x3C, 0xF3, 0xCF, 0x00,
    0x10, 0x00, 0x00, 0x10, 0x0E, 0x00, 0x00, 0x7F, 0x32, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00,
    0x00, 0x05, 0x00, 0x10, 0x00, 0x00, 0x10, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0x61, 0x28, 0x38,
    0x00, 0x00, 0x61, 0xC0, 0x28, 0xF6, 0xAF, 0x61, 0xC1, 0x08, 0xFF, 0xE0, 0x61, 0x43, 0x00,
    0x00, 0x41, 0x61, 0x40, 0x00, 0x00, 0x17, 0x61, 0x41, 0x00, 0x00, 0x0C, 0x61, 0xF3, 0x7F,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10,
    0x09, 0x00, 0x00, 0x00, 0x01, 0x61, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#if defined(DUSK_TARGET_3DS)
// One line as direct vertices inside an open triangle strip: position, normal
// and, with `tex`, texture coordinate. `joinBefore` repeats the first vertex
// and `joinAfter` the last one: with both, two lines are tied by four
// triangles without area, and since a line has an even number of vertices the
// next one starts on an even place of the strip and keeps its winding.
// Written in blocks: the GX vertex calls cost a call chain per value (2,100
// vertices of lines took 8 ms of a picture in Ordon Spring through them).
static void nativeLineStrip(const cXyz* pos, const mDoExt_3Dline_field_0x10_c* nrm, const cXy* tex, int vertices,
                            bool joinBefore, bool joinAfter) {
    auto* stream = dusk::native::gx::commandStream();
    if (stream == NULL) {
        OSPanic(__FILE__, __LINE__, "Native 3D line stream unavailable");
    }
    u8 bytes[23 * 48];
    unsigned used = 0;
    const int last = vertices + (joinAfter ? 1 : 0);
    for (int k = joinBefore ? -1 : 0; k < last; k++) {
        const int j = k < 0 ? 0 : k >= vertices ? vertices - 1 : k;
        const float position[3] = {pos[j].x, pos[j].y, pos[j].z};
        const s8 normal[3] = {nrm[j].x, nrm[j].y, nrm[j].z};
        const float st[2] = {tex != NULL ? tex[j].x : 0.0f, tex != NULL ? tex[j].y : 0.0f};
        used += dusk::native::gx::lineVertexWire(bytes + used, position, normal, tex != NULL ? st : NULL);
        if (used + 23 > sizeof(bytes) || k + 1 == last) {
            if (!stream->write({bytes, used})) {
                OSPanic(__FILE__, __LINE__, "Native 3D line stream failed");
            }
            used = 0;
        }
    }
}
// Lines of `vertices` vertices each that fit one strip: the count of a GXBegin
// is 16 bits wide, and a strip of 21,000 vertices is 62,994 triangle corners.
static int nativeLinesPerStrip(int vertices) {
    return vertices + 2 < 21000 ? 21000 / (vertices + 2) : 1;
}
#endif

void mDoExt_3DlineMat0_c::setMaterial() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    j3dSys.reinitGX();
    GXSetNumIndStages(0);
    dKy_setLight_again();
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_CLR_RGB, GX_RGB8, 6);
    dKy_GxFog_set();
    GXCallDisplayList(l_matDL, 0x80);
    GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
    GXLoadNrmMtxImm(cMtx_getIdentity(), GX_PNMTX0);
}

void mDoExt_3DlineMat0_c::draw() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    GXSetTevColor(GX_TEVREG2, field_0x8);

    if (field_0xc != NULL) {
        dKy_Global_amb_set(field_0xc);
    }

    mDoExt_3Dline_c* var_r28 = field_0x18;
    int var_r26 = (field_0x14 << 1) & 0xFFFF;

#if defined(DUSK_TARGET_3DS)
    // One strip for all the lines of this material (see
    // mDoExt_3DlineMat1_c::draw).
    if (field_0x10 > 0 && var_r26 >= 3) {
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
        const int linesPerStrip = nativeLinesPerStrip(var_r26);
        for (int first = 0; first < field_0x10; first += linesPerStrip) {
            const int count = field_0x10 - first < linesPerStrip ? field_0x10 - first : linesPerStrip;
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, u16(count * var_r26 + (count - 1) * 2));
            for (int i = 0; i < count; i++) {
                nativeLineStrip(var_r28[first + i].field_0x8[field_0x16], var_r28[first + i].field_0x10[field_0x16], NULL, var_r26,
                                i > 0, i + 1 < count);
            }
            GXEnd();
        }
    }
#else
    for (int i = 0; i < field_0x10; i++) {
        GXSETARRAY(GX_VA_POS, var_r28->field_0x8[field_0x16], sizeof(cXyz) * var_r26, sizeof(cXyz), true);
        GXSETARRAY(GX_VA_NRM, var_r28->field_0x10[field_0x16], 3 * var_r26, 3, true);

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, var_r26);
        for (u16 j = 0; j < (u16)var_r26; j++) {
            GXPosition1x16(j);
            GXNormal1x16(j);

            GXPosition1x16(++j);
            GXNormal1x16(j);
        }
        GXEnd();
        var_r28++;
    }
#endif

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
    if (!dusk::interp::is_enabled())
#endif
    {
        field_0x16 ^= (u8)1;
    }
}

void mDoExt_3DlineMat0_c::update(int param_0, f32 param_1, GXColor& param_2, u16 param_3,
                                     dKy_tevstr_c* param_4) {
    field_0x8 = param_2;
    field_0xc = param_4;

    if (param_0 < 0) {
        field_0x14 = field_0x12;
    } else if (param_0 > field_0x12) {
        field_0x14 = field_0x12;
    } else {
        field_0x14 = (u16)param_0;
    }

#if TARGET_PC
    mInterpWidth = param_1;
    mInterpTaper = param_3;
    mInterpKind = LINE_UNIFORM_WIDTH;
#endif

    view_class* sp_2c = dComIfGd_getView();

    mDoExt_3Dline_c* sp_28 = field_0x18;

    f32 local_f30 = param_3 != 0 ? param_1 / param_3 : 0.0f;

    u32 sp_24 = (field_0x14 << 1) * 12;
    u32 sp_20 = (field_0x14 << 1) * 3;

    cXyz sp_12c;
    cXyz sp_120;
    cXyz sp_114;
    cXyz sp_108;

    cXyz* local_r27;
    cXyz* local_r26;
    cXyz* sp_1c;

    mDoExt_3Dline_field_0x10_c* local_r30;
    mDoExt_3Dline_field_0x10_c* local_r29;
    mDoExt_3Dline_field_0x10_c* sp_18;

    for (s32 sp_14 = 0; sp_14 < field_0x10; sp_14++) {
        local_r27 = sp_28->field_0x0;

        sp_1c = sp_28->field_0x8[field_0x16];
        local_r26 = sp_1c;
        sp_18 = sp_28->field_0x10[field_0x16];
        local_r30 = sp_18;

        local_r29 = local_r30 + 1;

        f32 local_f31 = param_1;

        sp_120 = local_r27[1] - local_r27[0];
        sp_12c = local_r27[0] - sp_2c->lookat.eye;
        sp_120 = sp_120.outprod(sp_12c);
        sp_120.normalizeZP();

        local_r30->x = sp_120.x * 64.0f;
        local_r30->y = sp_120.y * 64.0f;
        local_r30->z = sp_120.z * 64.0f;
        local_r29->x = -local_r30->x;
        local_r29->y = -local_r30->y;
        local_r29->z = -local_r30->z;

        sp_120 *= local_f31;
        *local_r26++ = *local_r27 + sp_120;
        *local_r26++ = *local_r27 - sp_120;
        local_r27++;
        sp_114 = *local_r27 + sp_120;
        sp_108 = *local_r27 - sp_120;

        for (s32 local_r19 = field_0x14 - 2; local_r19 > 0; local_r19--) {
            if (local_r19 < param_3) {
                local_f31 -= local_f30;
            }

            sp_120 = local_r27[1] - local_r27[0];
            sp_12c = local_r27[0] - sp_2c->lookat.eye;
            sp_120 = sp_120.outprod(sp_12c);
            sp_120.normalizeZP();

            local_r30 += 2;
            local_r29 += 2;

            local_r30->x = sp_120.x * 64.0f;
            local_r30->y = sp_120.y * 64.0f;
            local_r30->z = sp_120.z * 64.0f;
            local_r29->x = -local_r30->x;
            local_r29->y = -local_r30->y;
            local_r29->z = -local_r30->z;

            sp_120 *= local_f31;
            sp_114 += *local_r27 + sp_120;
            sp_108 += *local_r27 - sp_120;
            *local_r26++ = sp_114 * 0.5f;
            *local_r26++ = sp_108 * 0.5f;

            local_r27++;

            sp_114 = *local_r27 + sp_120;
            sp_108 = *local_r27 - sp_120;
        }
        local_r29 += 1;
        local_r29->x = local_r30->x;
        local_r29->y = local_r30->y;
        local_r29->z = local_r30->z;
        local_r30 += 1;
        local_r29 += 1;
        local_r29->x = local_r30->x;
        local_r29->y = local_r30->y;
        local_r29->z = local_r30->z;
        if (param_3 != 0) {
            *local_r26++ = *local_r27;
            *local_r26 = *local_r27;
        } else {
            *local_r26++ = sp_114;
            *local_r26 = sp_108;
        }
        DCStoreRangeNoSync(sp_1c, sp_24);
        DCStoreRangeNoSync(sp_18, sp_20);
        sp_28++;
    }
}

void mDoExt_3DlineMat0_c::update(int param_0, GXColor& param_2, dKy_tevstr_c* param_4) {
    field_0x8 = param_2;
    field_0xc = param_4;

    if (param_0 < 0) {
        field_0x14 = field_0x12;
    } else if (param_0 > field_0x12) {
        field_0x14 = field_0x12;
    } else {
        field_0x14 = (u16)param_0;
    }

    IF_DUSK(mInterpKind = LINE_PER_POINT_WIDTH);

    view_class* sp_34 = dComIfGd_getView();

    mDoExt_3Dline_c* sp_30 = field_0x18;

    u32 sp_2c = (field_0x14 << 1) * 12;
    u32 sp_28 = (field_0x14 << 1) * 3;

    cXyz sp_12c;
    cXyz sp_120;
    cXyz sp_114;
    cXyz sp_108;

    cXyz* local_r27;
    cXyz* sp_24;
    cXyz* sp_20;

    mDoExt_3Dline_field_0x10_c* local_r30;
    mDoExt_3Dline_field_0x10_c* local_r29;
    mDoExt_3Dline_field_0x10_c* sp_1c;
    f32* size_p;

    for (s32 sp_14 = 0; sp_14 < field_0x10; sp_14++) {
        local_r27 = sp_30->field_0x0;

        size_p = sp_30->field_0x4;

        JUT_ASSERT(5445, size_p != NULL);

        sp_20 = sp_30->field_0x8[field_0x16];
        sp_24 = sp_20;
        sp_1c = sp_30->field_0x10[field_0x16];
        local_r30 = sp_1c;

        local_r29 = local_r30 + 1;

        sp_120 = local_r27[1] - local_r27[0];
        sp_12c = local_r27[0] - sp_34->lookat.eye;
        sp_120 = sp_120.outprod(sp_12c);
        sp_120.normalizeZP();

        local_r30->x = sp_120.x * 64.0f;
        local_r30->y = sp_120.y * 64.0f;
        local_r30->z = sp_120.z * 64.0f;
        local_r29->x = -local_r30->x;
        local_r29->y = -local_r30->y;
        local_r29->z = -local_r30->z;

        sp_120 *= *size_p;
        *sp_24++ = *local_r27 + sp_120;
        *sp_24++ = *local_r27 - sp_120;
        local_r27++;
        size_p++;
        sp_114 = *local_r27 + sp_120;
        sp_108 = *local_r27 - sp_120;

        for (s32 local_r19 = field_0x14 - 2; local_r19 > 0; local_r19--) {
            sp_120 = local_r27[1] - local_r27[0];
            sp_12c = local_r27[0] - sp_34->lookat.eye;
            sp_120 = sp_120.outprod(sp_12c);
            sp_120.normalizeZP();

            local_r30 += 2;
            local_r29 += 2;

            local_r30->x = sp_120.x * 64.0f;
            local_r30->y = sp_120.y * 64.0f;
            local_r30->z = sp_120.z * 64.0f;
            local_r29->x = -local_r30->x;
            local_r29->y = -local_r30->y;
            local_r29->z = -local_r30->z;

            sp_120 *= *size_p;
            sp_114 += *local_r27 + sp_120;
            sp_108 += *local_r27 - sp_120;
            *sp_24++ = sp_114 * 0.5f;
            *sp_24++ = sp_108 * 0.5f;

            local_r27++;
            size_p++;

            sp_114 = *local_r27 + sp_120;
            sp_108 = *local_r27 - sp_120;
        }
        local_r29 += 1;
        local_r29->x = local_r30->x;
        local_r29->y = local_r30->y;
        local_r29->z = local_r30->z;
        local_r30 += 1;
        local_r29 += 1;
        local_r29->x = local_r30->x;
        local_r29->y = local_r30->y;
        local_r29->z = local_r30->z;
        *sp_24++ = sp_114;
        *sp_24 = sp_108;
        DCStoreRangeNoSync(sp_20, sp_2c);
        DCStoreRangeNoSync(sp_1c, sp_28);
        sp_30++;
    }
}

int mDoExt_3DlineMat1_c::init(u16 param_0, u16 param_1, ResTIMG* param_2, int param_3) {
#if defined(DUSK_TARGET_3DS)
    mNativeTextureResource=param_2;
#endif
    mNumLines = param_0;
    field_0x32 = param_1;
    mpLines = JKR_NEW_ARRAY(mDoExt_3Dline_c, param_0);
    if (mpLines == NULL) {
        return 0;
    }

    for (s32 iVar2 = 0; iVar2 < param_0; iVar2++) {
        if (mpLines[iVar2].init(param_1, param_3, TRUE) == 0) {
            return 0;
        }
    }

    field_0x4 = 0;
    mIsDrawn = 0;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
    mInterpWidth = 0.0f;
    mInterpTaper = 0;
    mInterpKind = LINE_NONE;
    dusk::interp::line::reset(this);
#endif

    GXInitTexObj(&mTextureObject, (void*)((intptr_t)param_2 + param_2->imageOffset), param_2->width,
                 param_2->height, (GXTexFmt)param_2->format, (GXTexWrapMode)param_2->wrapS,
                 (GXTexWrapMode)param_2->wrapT, param_2->mipmapCount > 1 ? GX_TRUE : GX_FALSE);
    GXInitTexObjLOD(&mTextureObject, (GXTexFilter)param_2->minFilter,
                    (GXTexFilter)param_2->magFilter, param_2->minLOD * 0.125f,
                    param_2->maxLOD * 0.125f, param_2->LODBias * 0.01f, (u8)param_2->biasClamp,
                    (u8)param_2->doEdgeLOD, (GXAnisotropy)param_2->maxAnisotropy);

    return 1;
}

ATTRIBUTE_ALIGN(32) static u8 l_mat1DL[141] = {
    0x10, 0x00, 0x00, 0x10, 0x40, 0xFF, 0xFF, 0x42, 0x80, 0x08, 0x30, 0x3C, 0xF3, 0xCF, 0x00, 0x10,
    0x00, 0x00, 0x10, 0x18, 0x3C, 0xF3, 0xCF, 0x00, 0x10, 0x00, 0x00, 0x10, 0x0E, 0x00, 0x00, 0x7F,
    0x32, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00, 0x10, 0x0C, 0xFF,
    0xFF, 0xFF, 0xFF, 0x61, 0x28, 0x38, 0x00, 0x40, 0x61, 0xC0, 0x28, 0xFA, 0x8F, 0x61, 0xC1, 0x08,
    0xFF, 0xF0, 0x61, 0x43, 0x00, 0x00, 0x41, 0x61, 0x40, 0x00, 0x00, 0x17, 0x61, 0x41, 0x00, 0x00,
    0x0C, 0x61, 0xF3, 0x7F, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x3F, 0x00, 0x00, 0x00, 0x01, 0x10,
    0x00, 0x00, 0x10, 0x09, 0x00, 0x00, 0x00, 0x01, 0x61, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void mDoExt_3DlineMat1_c::setMaterial() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    j3dSys.reinitGX();
    GXSetNumIndStages(0);
    dKy_setLight_again();
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_CLR_RGB, GX_RGB8, 6);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_CLR_RGBA, GX_F32, 0);
    dKy_GxFog_set();
    GXCallDisplayList(l_mat1DL, 0x80);
    GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
    GXLoadNrmMtxImm(cMtx_getIdentity(), GX_PNMTX0);
}

void mDoExt_3DlineMat1_c::draw() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
#if defined(DUSK_TARGET_3DS)
    if(!dusk::native::retainArchiveTextureForLifetime(mNativeTextureResource))
        OSPanic(__FILE__,__LINE__,"Native 3D-line texture: %s",dusk::native::textureResourceFailure());
#endif
    GXLoadTexObj(&mTextureObject, GX_TEXMAP0);
    GXSetTexCoordScaleManually(GX_TEXCOORD0, 1, GXGetTexObjWidth(&mTextureObject), GXGetTexObjHeight(&mTextureObject));
    GXSetTevColor(GX_TEVREG2, mColor);
    if (mpTevStr != NULL) {
        dKy_Global_amb_set(mpTevStr);
    }
    mDoExt_3Dline_c* lines = mpLines;
    u16 vert_num = field_0x34 << 1;
#if defined(DUSK_TARGET_3DS)
    // One strip with direct attributes for all the lines of this material,
    // instead of one indexed strip per line. A strip is a complete draw on
    // the 3DS (93 of them a picture in Ordon Spring, 0.3 ms each in Azahar),
    // and every line has arrays of its own, so indexed vertices cannot share
    // a draw. Two lines are tied by repeating the last vertex of one and the
    // first of the next (nativeLineStrip). A triangle list did the same in
    // one draw with three times the vertices, and one long rope then cost
    // 1.2 ms more than its strip (Death Mountain, 100 vertices).
    if (mNumLines > 0 && vert_num >= 3) {
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        const int linesPerStrip = nativeLinesPerStrip(vert_num);
        for (int first = 0; first < mNumLines; first += linesPerStrip) {
            const int count = mNumLines - first < linesPerStrip ? mNumLines - first : linesPerStrip;
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, u16(count * vert_num + (count - 1) * 2));
            for (int i = 0; i < count; i++) {
                nativeLineStrip(lines[first + i].field_0x8[mIsDrawn], lines[first + i].field_0x10[mIsDrawn],
                                lines[first + i].field_0x18[mIsDrawn], vert_num, i > 0, i + 1 < count);
            }
            GXEnd();
        }
    }
#else
    for (s32 i = 0; i < mNumLines; i++) {
        GXSETARRAY(GX_VA_POS, lines->field_0x8[mIsDrawn], vert_num * sizeof(cXyz), sizeof(cXyz), true);
        GXSETARRAY(GX_VA_NRM, lines->field_0x10[mIsDrawn],
                   vert_num * sizeof(mDoExt_3Dline_field_0x10_c),
                   sizeof(mDoExt_3Dline_field_0x10_c), true);
        GXSETARRAY(GX_VA_TEX0, lines->field_0x18[mIsDrawn], vert_num * sizeof(cXy), sizeof(cXy), true);
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, vert_num);

        u16 j = 0;
        while (j < vert_num) {
            GXPosition1x16(j);
            GXNormal1x16(j);
            GXTexCoord1x16(j);
            GXPosition1x16(++j);
            GXNormal1x16(j);
            GXTexCoord1x16(j);
            j++;
        }
        GXEnd();
        lines++;
    }
#endif
    GXSetTexCoordScaleManually(GX_TEXCOORD0, 0, 0, 0);
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
    if (!dusk::interp::is_enabled())
#endif
    {
        mIsDrawn ^= (u8)1;
    }
}

#if defined(DUSK_TARGET_3DS)
// The texture object is a handle of its own in each material (the port tags
// its storage), made by init() from the BTI alone: the same BTI is the same
// image, format, wrap and filter.
bool mDoExt_3DlineMat1_c::nativeSameDraw(const mDoExt_3DlineMat1_c& other) const {
    return mNativeTextureResource != NULL && other.mNativeTextureResource == mNativeTextureResource &&
           other.mpTevStr == mpTevStr && other.mColor.r == mColor.r && other.mColor.g == mColor.g &&
           other.mColor.b == mColor.b && other.mColor.a == mColor.a;
}

// The materials of a run set the same texture, texture scale, colour and
// ambient (what draw() sets before its lines and after them), so their lines
// are one strip, tied as nativeLineStrip ties the lines of one material:
// every line has an even number of vertices, the joins are triangles without
// area and each line keeps its winding. The bridge's rope materials (one per
// plank and the side ropes) were one draw each. Lines in the original order;
// each material's double-buffer index flips as its draw() flips it.
mDoExt_3DlineMat_c* mDoExt_3DlineMat1_c::nativeDrawRun() {
    struct LineRef {
        const cXyz* pos;
        const mDoExt_3Dline_field_0x10_c* nrm;
        const cXy* tex;
        int vertices;
    };
    static LineRef lines[512];
    const int id = getMaterialID();
    mDoExt_3DlineMat_c* end = field_0x4;
    int materials = 1;
    int count = 0;
    bool fits = true;
    for (mDoExt_3DlineMat1_c* mat = this;;) {
        const int vertices = mat->field_0x34 << 1;
        if (mat->mNumLines > 0 && vertices >= 3) {
            for (int i = 0; i < mat->mNumLines; i++) {
                if (count == 512) {
                    fits = false;
                    break;
                }
                const mDoExt_3Dline_c& line = mat->mpLines[i];
                lines[count].pos = line.field_0x8[mat->mIsDrawn];
                lines[count].nrm = line.field_0x10[mat->mIsDrawn];
                lines[count].tex = line.field_0x18[mat->mIsDrawn];
                lines[count].vertices = vertices;
                count++;
            }
        }
        if (!fits || end == NULL || end->getMaterialID() != id ||
            !nativeSameDraw(*static_cast<mDoExt_3DlineMat1_c*>(end))) {
            break;
        }
        mat = static_cast<mDoExt_3DlineMat1_c*>(end);
        end = end->field_0x4;
        materials++;
    }
    if (materials == 1 || !fits) {
        draw();
        return field_0x4;
    }
    l_nativeInstances.lineRuns++;
    l_nativeInstances.lineMaterials += materials;

    // draw() of the first material, for the lines of all of them.
    if(!dusk::native::retainArchiveTextureForLifetime(mNativeTextureResource))
        OSPanic(__FILE__,__LINE__,"Native 3D-line texture: %s",dusk::native::textureResourceFailure());
    GXLoadTexObj(&mTextureObject, GX_TEXMAP0);
    GXSetTexCoordScaleManually(GX_TEXCOORD0, 1, GXGetTexObjWidth(&mTextureObject), GXGetTexObjHeight(&mTextureObject));
    GXSetTevColor(GX_TEVREG2, mColor);
    if (mpTevStr != NULL) {
        dKy_Global_amb_set(mpTevStr);
    }
    if (count > 0) {
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    }
    // Strips of at most 21,000 vertices (nativeLinesPerStrip).
    for (int first = 0; first < count;) {
        int total = lines[first].vertices;
        int last = first + 1;
        while (last < count && total + 2 + lines[last].vertices <= 21000) {
            total += 2 + lines[last].vertices;
            last++;
        }
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, u16(total));
        for (int i = first; i < last; i++) {
            nativeLineStrip(lines[i].pos, lines[i].nrm, lines[i].tex, lines[i].vertices, i > first, i + 1 < last);
        }
        GXEnd();
        first = last;
    }
    GXSetTexCoordScaleManually(GX_TEXCOORD0, 0, 0, 0);
    mDoExt_3DlineMat_c* mat = this;
    for (int i = 0; i < materials; i++, mat = mat->field_0x4) {
        static_cast<mDoExt_3DlineMat1_c*>(mat)->mIsDrawn ^= (u8)1;
    }
    return end;
}
#endif

void mDoExt_3DlineMat1_c::update(int param_0, f32 param_1, GXColor& param_2, u16 param_3, dKy_tevstr_c* param_4) {
    mColor = param_2;
    this->mpTevStr = param_4;
    if (param_0 < 0) {
        field_0x34 = field_0x32;
    } else if (param_0 > field_0x32) {
        field_0x34 = field_0x32;
    } else {
        field_0x34 = param_0;
    }

#if TARGET_PC
    mInterpWidth = param_1;
    mInterpTaper = param_3;
    mInterpKind = LINE_UNIFORM_WIDTH;
#endif

    view_class* sp_3c = dComIfGd_getView();
    mDoExt_3Dline_c* sp_38 = mpLines;
    f32 local_f27 = param_3 != 0 ? param_1 / param_3 : 0.0f;

    u32 sp_34 = (field_0x34 << 1) * 12;
    u32 sp_30 = (field_0x34 << 1) * 3;
    u32 sp_2c = (field_0x34 << 1) * 8;

    cXyz sp_13c;
    cXyz sp_130;
    cXyz sp_124;
    cXyz sp_118;

    cXyz* local_r27;
    cXyz* sp_28;
    cXyz* sp_24;

    mDoExt_3Dline_field_0x10_c* local_r30;
    mDoExt_3Dline_field_0x10_c* local_r28;
    mDoExt_3Dline_field_0x10_c* sp_20;

    cXy* sp_1c;
    cXy* sp_18;

    f32 local_f29;
    f32 local_f31 = 0.0f;

    for (s32 sp_14 = 0; sp_14 < mNumLines; sp_14++) {
        local_r27 = sp_38[0].field_0x0;
        sp_24 = sp_38->field_0x8[mIsDrawn];
        sp_28 = sp_24;

        sp_20 = sp_38->field_0x10[mIsDrawn];
        local_r30 = sp_20;
        local_r28 = local_r30 + 1;

        sp_18 = sp_38->field_0x18[mIsDrawn];
        sp_1c = sp_18;

        local_f29 = param_1;

        sp_1c++->y = local_f31;
        sp_1c++->y = local_f31;

        sp_130 = local_r27[1] - local_r27[0];
        f32 local_f30 = sp_130.abs();

        if (param_1 < 8.0f) {
            local_f31 += local_f30 * 0.1f;
        } else {
            local_f31 += local_f30 * 0.02f * (8.0f / param_1);
        }

        sp_13c = *local_r27 - sp_3c->lookat.eye;
        sp_130 = sp_130.outprod(sp_13c);
        sp_130.normalizeZP();

        local_r30->x = sp_130.x * 64.0f;
        local_r30->y = sp_130.y * 64.0f;
        local_r30->z = sp_130.z * 64.0f;
        local_r28->x = -local_r30->x;
        local_r28->y = -local_r30->y;
        local_r28->z = -local_r30->z;

        sp_130 *= local_f29;
        *sp_28++ = *local_r27 + sp_130;
        *sp_28++ = *local_r27 - sp_130;
        local_r27++;
        sp_124 = *local_r27 + sp_130;
        sp_118 = *local_r27 - sp_130;

        for (s32 sp_10 = field_0x34 - 2; sp_10 > 0; sp_10--) {
            if (sp_10 < param_3) {
                local_f29 -= local_f27;
            }

            sp_1c++->y = local_f31;
            sp_1c++->y = local_f31;

            sp_130 = local_r27[1] - local_r27[0];
            local_f30 = sp_130.abs();

            if (param_1 < 8.0f) {
                local_f31 += local_f30 * 0.1f;
            } else {
                local_f31 += local_f30 * 0.02f * (8.0f / param_1);
            }

            sp_13c = local_r27[0] - sp_3c->lookat.eye;
            sp_130 = sp_130.outprod(sp_13c);
            sp_130.normalizeZP();

            local_r30 += 2;
            local_r28 += 2;

            local_r30->x = sp_130.x * 64.0f;
            local_r30->y = sp_130.y * 64.0f;
            local_r30->z = sp_130.z * 64.0f;
            local_r28->x = -local_r30->x;
            local_r28->y = -local_r30->y;
            local_r28->z = -local_r30->z;

            sp_130 *= local_f29;
            sp_124 += *local_r27 + sp_130;
            sp_118 += *local_r27 - sp_130;
            *sp_28++ = sp_124 * 0.5f;
            *sp_28++ = sp_118 * 0.5f;

            local_r27++;
            sp_124 = local_r27[0] + sp_130;
            sp_118 = local_r27[0] - sp_130;
        }

        sp_1c++->y = local_f31;
        sp_1c->y = local_f31;

        local_r28 += 1;
        local_r28->x = local_r30->x;
        local_r28->y = local_r30->y;
        local_r28->z = local_r30->z;
        local_r30 += 1;
        local_r28 += 1;
        local_r28->x = local_r30->x;
        local_r28->y = local_r30->y;
        local_r28->z = local_r30->z;

        if (param_3 != 0) {
            *sp_28++ = *local_r27;
            *sp_28 = *local_r27;
        } else {
            *sp_28++ = sp_124;
            *sp_28 = sp_118;
        }
        DCStoreRangeNoSync(sp_24, sp_34);
        DCStoreRangeNoSync(sp_20, sp_30);
        DCStoreRangeNoSync(sp_18, sp_2c);
        sp_38++;
    }
}

#if !TARGET_PC
#include "assets/l_mat2DL__d_a_grass.h"
#endif

void mDoExt_3DlineMat2_c::setMaterial() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    j3dSys.reinitGX();
    GXSetNumIndStages(0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_RGB8, 6);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_CLR_RGBA, GX_F32, 0);
    dKy_GxFog_set();
    GXCallDisplayList(NULL, 0x80); // DEBUG NONMATCHING - this is supposed to reference l_mat2DL
    GXLoadPosMtxImm(j3dSys.getViewMtx(), 0);
    GXLoadNrmMtxImm(cMtx_getIdentity(), 0);
}

void mDoExt_3DlineMat1_c::update(int param_0, GXColor& param_2, dKy_tevstr_c* param_4) {
    mColor = param_2;
    this->mpTevStr = param_4;
    if (param_0 < 0) {
        field_0x34 = field_0x32;
    } else if (param_0 > field_0x32) {
        field_0x34 = field_0x32;
    } else {
        field_0x34 = param_0;
    }

    IF_DUSK(mInterpKind = LINE_PER_POINT_WIDTH);

    view_class* stack_3c = dComIfGd_getView();
    mDoExt_3Dline_c* sp_38 = mpLines;
    f32 local_f30;
    f32 local_f29;

    u32 sp_34 = (field_0x34 << 1) * 12;
    u32 sp_30 = (field_0x34 << 1) * 3;
    u32 sp_2c = (field_0x34 << 1) * 8;
    cXyz sp_13c;
    cXyz sp_130;
    cXyz sp_124;
    cXyz sp_118;
    cXyz* local_r27;
    cXyz* sp_28;
    cXyz* sp_24;
    mDoExt_3Dline_field_0x10_c* local_r30;
    mDoExt_3Dline_field_0x10_c* local_r28;
    mDoExt_3Dline_field_0x10_c* sp_20;
    cXy* sp_1c;
    cXy* sp_18;
    f32 local_f31 = 0.0f;
    f32* size_p;
    for (s32 sp_14 = 0; sp_14 < mNumLines; sp_14++) {
        local_r27 = sp_38[0].field_0x0;
        size_p = sp_38->field_0x4;
        JUT_ASSERT(5875, size_p != NULL);
        sp_24 = sp_38->field_0x8[mIsDrawn];
        sp_28 = sp_24;
        sp_20 = sp_38->field_0x10[mIsDrawn];
        local_r30 = sp_20;
        local_r28 = local_r30 + 1;
        sp_18 = sp_38->field_0x18[mIsDrawn];
        sp_1c = sp_18;
        sp_1c++->y = local_f31;
        sp_1c++->y = local_f31;
        sp_130 = local_r27[1] - local_r27[0];
        local_f30 = sp_130.abs();
        local_f31 += local_f30 * 0.1f;
        sp_13c = local_r27[0] - stack_3c->lookat.eye;
        sp_130 = sp_130.outprod(sp_13c);
        sp_130.normalizeZP();
        local_r30->x = sp_130.x * 64.0f;
        local_r30->y = sp_130.y * 64.0f;
        local_r30->z = sp_130.z * 64.0f;
        local_r28->x = -local_r30->x;
        local_r28->y = -local_r30->y;
        local_r28->z = -local_r30->z;
        sp_130 *= *size_p;
        *sp_28++ = local_r27[0] + sp_130;
        *sp_28++ = local_r27[0] - sp_130;
        local_r27++;
        size_p++;
        sp_124 = local_r27[0] + sp_130;
        sp_118 = local_r27[0] - sp_130;
        for (s32 sp_10 = field_0x34 - 2; sp_10 > 0; sp_10--) {
            sp_1c++->y = local_f31;
            sp_1c++->y = local_f31;
            sp_130 = local_r27[1] - local_r27[0];
            local_f30 = sp_130.abs();
            local_f31 += local_f30 * 0.1f;
            sp_13c = local_r27[0] - stack_3c->lookat.eye;
            sp_130 = sp_130.outprod(sp_13c);
            sp_130.normalizeZP();
            local_r30 += 2;
            local_r28 += 2;
            local_r30->x = sp_130.x * 64.0f;
            local_r30->y = sp_130.y * 64.0f;
            local_r30->z = sp_130.z * 64.0f;
            local_r28->x = -local_r30->x;
            local_r28->y = -local_r30->y;
            local_r28->z = -local_r30->z;
            sp_130 *= *size_p;
            sp_124 += local_r27[0] + sp_130;
            sp_118 += local_r27[0] - sp_130;
            *sp_28++ = sp_124 * 0.5f;
            *sp_28++ = sp_118 * 0.5f;
            local_r27++;
            size_p++;
            sp_124 = local_r27[0] + sp_130;
            sp_118 = local_r27[0] - sp_130;
        }

        sp_1c++->y = local_f31;
        sp_1c->y = local_f31;

        local_r28 += 1;
        local_r28->x = local_r30->x;
        local_r28->y = local_r30->y;
        local_r28->z = local_r30->z;
        local_r30 += 1;
        local_r28 += 1;
        local_r28->x = local_r30->x;
        local_r28->y = local_r30->y;
        local_r28->z = local_r30->z;

        *sp_28++ = sp_124;
        *sp_28 = sp_118;
        DCStoreRangeNoSync(sp_24, sp_34);
        DCStoreRangeNoSync(sp_20, sp_30);
        DCStoreRangeNoSync(sp_18, sp_2c);
        sp_38 += 1;
    }
}

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
namespace {

template <typename Material>
void refresh_3dline_geometry(Material* material, dusk::interp::line::Points points, u8 kind,
                             f32 width, u16 taper, GXColor& color, dKy_tevstr_c* tevStr) {
    if (!dusk::interp::is_enabled()) {
        return;
    }

    dusk::interp::line::write(material, points);
    if (kind == LINE_UNIFORM_WIDTH) {
        material->update(points.point_count, width, color, taper, tevStr);
    } else if (kind == LINE_PER_POINT_WIDTH) {
        material->update(points.point_count, color, tevStr);
    }
}

}  // namespace

void mDoExt_3DlineMat0_c::captureInterpPoints() {
    dusk::interp::line::capture(this, {field_0x18, field_0x10, field_0x14});
}

void mDoExt_3DlineMat0_c::refreshGeometryForPresentation() {
    refresh_3dline_geometry(this, {field_0x18, field_0x10, field_0x14}, mInterpKind,
                            mInterpWidth, mInterpTaper, field_0x8, field_0xc);
}

void mDoExt_3DlineMat1_c::captureInterpPoints() {
    dusk::interp::line::capture(this, {mpLines, mNumLines, field_0x34});
}

void mDoExt_3DlineMat1_c::refreshGeometryForPresentation() {
    refresh_3dline_geometry(this, {mpLines, mNumLines, field_0x34}, mInterpKind,
                            mInterpWidth, mInterpTaper, mColor, mpTevStr);
}
#endif

void mDoExt_3DlineMatSortPacket::setMat(mDoExt_3DlineMat_c* i_3DlineMat) {
    if (mp3DlineMat == NULL) {
        dComIfGd_getListPacket()->entryImm(this, 0);
    }
    i_3DlineMat->field_0x4 = mp3DlineMat;
    mp3DlineMat = i_3DlineMat;
    DUSK_DESKTOP_ONLY(i_3DlineMat->captureInterpPoints());
}

void mDoExt_3DlineMatSortPacket::draw() {
    mp3DlineMat->setMaterial();
    mDoExt_3DlineMat_c* lineMat = mp3DlineMat;
#if defined(DUSK_TARGET_3DS)
    // Materials 1 and 2 (mDoExt_3DlineMat2_c draws as 1): runs that set the
    // same state are one strip (nativeDrawRun; batch-instances.txt 0: off).
    const int id = lineMat->getMaterialID();
    if ((id == 1 || id == 2) && nativeInstancesMode() != 0) {
        do {
            lineMat = static_cast<mDoExt_3DlineMat1_c*>(lineMat)->nativeDrawRun();
        } while (lineMat != NULL);
        J3DShape::resetVcdVatCache();
        return;
    }
#endif
    do {
        lineMat->draw();
        lineMat = lineMat->field_0x4;
    } while (lineMat != NULL);
    J3DShape::resetVcdVatCache();
}

#if DEBUG || !__MWERKS__
mDoExt_cube8pPacket::mDoExt_cube8pPacket(cXyz* i_points, const GXColor& i_color) {
    cXyz* pnt_array = mPoints;

    for (int i = 0; i < 8; i++) {
        *(pnt_array)++ = *(i_points)++;
    }

    DCStoreRangeNoSync(mPoints, sizeof(cXyz) * 8);
    mColor = i_color;
}

void drawCube(MtxP mtx, cXyz* pos, const GXColor& color) {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    GXSETARRAY(GX_VA_POS, pos, sizeof(cXyz) * 8, sizeof(cXyz), true);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP,
                  GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, color);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXLoadPosMtxImm(mtx, 0);
    GXSetCurrentMtx(0);

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 14);
    GXPosition1x8(4);
    GXPosition1x8(6);
    GXPosition1x8(5);
    GXPosition1x8(7);
    GXPosition1x8(3);
    GXPosition1x8(6);
    GXPosition1x8(2);
    GXPosition1x8(4);
    GXPosition1x8(0);
    GXPosition1x8(5);
    GXPosition1x8(1);
    GXPosition1x8(3);
    GXPosition1x8(0);
    GXPosition1x8(2);
    GXEnd();
}

void mDoExt_cube8pPacket::draw() {
    drawCube(j3dSys.getViewMtx(), mPoints, mColor);
}

mDoExt_cubePacket::mDoExt_cubePacket(cXyz& i_position, cXyz& i_size, csXyz& i_angle, const GXColor& i_color) {
    mPosition = i_position;
    mSize = i_size;
    mAngle = i_angle;
    mColor = i_color;
}

void mDoExt_cubePacket::draw() {
    static cXyz l_pos[8] = {
        cXyz(-1.0f, 1.0f, -1.0f), cXyz(1.0f, 1.0f, -1.0f),   cXyz(-1.0f, 1.0f, 1.0f),
        cXyz(1.0f, 1.0f, 1.0f),   cXyz(-1.0f, -1.0f, -1.0f), cXyz(1.0f, -1.0f, -1.0f),
        cXyz(-1.0f, -1.0f, 1.0f), cXyz(1.0f, -1.0f, 1.0f),
    };

    mDoMtx_stack_c::transS(mPosition.x, mPosition.y, mPosition.z);
    mDoMtx_stack_c::XYZrotM(mAngle.x, mAngle.y, mAngle.z);
    mDoMtx_stack_c::scaleM(mSize.x, mSize.y, mSize.z);
    mDoMtx_stack_c::revConcat(j3dSys.getViewMtx());
    drawCube(mDoMtx_stack_c::get(), l_pos, mColor);
}

mDoExt_quadPacket::mDoExt_quadPacket(cXyz* i_points, const GXColor& i_color, u8 i_clipZ) {
    cXyz* pnt_array = mPoints;

    for (int i = 0; i < 4; i++) {
        *(pnt_array)++ = *(i_points)++;
    }

    DCStoreRangeNoSync(mPoints, sizeof(cXyz) * 4);
    mColor = i_color;
    mClipZ = i_clipZ;
}

void mDoExt_quadPacket::draw() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    GXSETARRAY(GX_VA_POS, mPoints, sizeof(mPoints), sizeof(cXyz), true);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP,
                  GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXLoadPosMtxImm(j3dSys.getViewMtx(), 0);
    GXSetCurrentMtx(0);

    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition1x8(0);
    GXPosition1x8(1);
    GXPosition1x8(2);
    GXPosition1x8(3);
    GXEnd();
}

mDoExt_trianglePacket::mDoExt_trianglePacket(cXyz* i_points, const GXColor& i_color, u8 i_clipZ) {
    cXyz* pnt_array = mPoints;

    for (int i = 0; i < 3; i++) {
        *(pnt_array)++ = *(i_points)++;
    }

    DCStoreRangeNoSync(mPoints, sizeof(cXyz) * 3);
    mColor = i_color;
    mClipZ = i_clipZ;
}

void mDoExt_trianglePacket::draw() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    j3dSys.reinitGX();

    GXSETARRAY(GX_VA_POS, mPoints, sizeof(mPoints), sizeof(cXyz), true);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXLoadPosMtxImm(j3dSys.getViewMtx(), 0);
    GXSetCurrentMtx(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetZCompLoc(GX_ENABLE);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, g_clearColor);
    GXSetFogRangeAdj(GX_DISABLE, 0, NULL);
    GXSetCullMode(GX_CULL_NONE);
    GXSetDither(GX_ENABLE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetNumIndStages(0);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition1x8(0);
    GXPosition1x8(1);
    GXPosition1x8(2);
    GXEnd();

    J3DShape::resetVcdVatCache();
}

mDoExt_linePacket::mDoExt_linePacket(cXyz& i_start, cXyz& i_end, const GXColor& i_color, u8 i_clipZ, u8 i_width) {
    mStart = i_start;
    mEnd = i_end;
    mColor = i_color;
    mClipZ = i_clipZ;
    mWidth = i_width;
}

void mDoExt_linePacket::draw() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    j3dSys.reinitGX();

    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXLoadPosMtxImm(j3dSys.getViewMtx(), 0);
    GXSetCurrentMtx(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetZCompLoc(GX_ENABLE);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, g_clearColor);
    GXSetFogRangeAdj(GX_DISABLE, 0, NULL);
    GXSetCullMode(GX_CULL_NONE);
    GXSetDither(GX_ENABLE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetNumIndStages(0);
    GXSetLineWidth(mWidth, GX_TO_ZERO);

    GXBegin(GX_LINES, GX_VTXFMT0, 2);
    GXPosition3f32(mStart.x, mStart.y, mStart.z);
    GXPosition3f32(mEnd.x, mEnd.y, mEnd.z);
    GXEnd();

    J3DShape::resetVcdVatCache();
}

mDoExt_ArrowPacket::mDoExt_ArrowPacket(cXyz& i_position, cXyz& param_1, const GXColor& i_color, u8 i_clipZ, u8 i_lineWidth) {
    mStart = i_position;
    mEnd = param_1;
    mColor = i_color;
    mClipZ = i_clipZ;
    mLineWidth = i_lineWidth;
}

void mDoExt_ArrowPacket::draw() {
    Mtx sp28;
    cXyz sp18;

    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetLineWidth(mLineWidth, GX_TO_ZERO);

    sp18 = mEnd - mStart;
    MtxTrans(mStart.x, mStart.y, mStart.z, 0);
    cMtx_YrotM(*calc_mtx, sp18.atan2sX_Z());
    cMtx_XrotM(*calc_mtx, cM_atan2s(JMAFastSqrt(SQUARE(sp18.x) + SQUARE(sp18.z)), sp18.y));
    cMtx_concat(j3dSys.getViewMtx(), *calc_mtx, sp28);

    GXLoadPosMtxImm(sp28, 0);
    GXSetCurrentMtx(0);

    GXBegin(GX_LINES, GX_VTXFMT0, 2);
    GXPosition3f32(0.0f, 0.0f, 0.0f);
    GXPosition3f32(0.0f, sp18.abs(), 0.0f);
    GXEnd();

    f32 var_f29 = sp18.abs();
    f32 var_f31 = var_f29 * 0.1f;
    f32 var_f30 = var_f29 * 0.8f;

    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 6);
    GXPosition3f32(0.0f, var_f29, 0.0f);
    GXPosition3f32(0.0f, var_f30, var_f31);
    GXPosition3f32(var_f31, var_f30, 0.0f);
    GXPosition3f32(0.0f, var_f30, -var_f31);
    GXPosition3f32(-var_f31, var_f30, 0.0f);
    GXPosition3f32(0.0f, var_f30, var_f31);
    GXEnd();
}

mDoExt_pointPacket::mDoExt_pointPacket(cXyz& i_position, const GXColor& i_color, u8 i_clipZ, u8 i_lineWidth) {
    mPosition = i_position;
    mColor = i_color;
    mClipZ = i_clipZ;
    mLineWidth = i_lineWidth;
}

void mDoExt_pointPacket::draw() {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    j3dSys.reinitGX();

    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetPointSize(mLineWidth, GX_TO_ZERO);

    GXLoadPosMtxImm(j3dSys.getViewMtx(), 0);
    GXSetCurrentMtx(0);

    GXBegin(GX_POINTS, GX_VTXFMT0, 1);
    GXPosition3f32(mPosition.x, mPosition.y, mPosition.z);
    GXEnd();

    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
}

mDoExt_circlePacket::mDoExt_circlePacket(cXyz& i_position, f32 i_radius, const GXColor& i_color, u8 i_clipZ, u8 i_lineWidth) {
    mPosition = i_position;
    mRadius = i_radius;
    mColor = i_color;
    mClipZ = i_clipZ;
    mLineWidth = i_lineWidth;
}

void mDoExt_circlePacket::draw() {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetLineWidth(mLineWidth, GX_TO_ZERO);
    GXLoadPosMtxImm(j3dSys.getViewMtx(), 0);
    GXSetCurrentMtx(0);

    cXyz sp38;
    cXyz sp44;
    int numEdges = 36;
    sp38.y = sp44.y = mPosition.y;
    
    GXBegin(GX_LINES, GX_VTXFMT0, numEdges * 2);
    for (int i = 0; i < numEdges; i++) {
        sp38.x = cM_fcos((i * 6.2831855f) / numEdges) * mRadius;
        sp38.z = cM_fsin((i * 6.2831855f) / numEdges) * mRadius;

        sp44.x = cM_fcos(((i + 1) * 6.2831855f) / numEdges) * mRadius;
        sp44.z = cM_fsin(((i + 1) * 6.2831855f) / numEdges) * mRadius;

        sp38.x += mPosition.x;
        sp38.z += mPosition.z;
        sp44.x += mPosition.x;
        sp44.z += mPosition.z;
        GXPosition3f32(sp38.x, sp38.y, sp38.z);
        GXPosition3f32(sp44.x, sp44.y, sp44.z);
    }
    GXEnd();
}

mDoExt_spherePacket::mDoExt_spherePacket(cXyz& i_position, f32 i_size, const GXColor& i_color, u8 i_clipZ) {
    mPosition = i_position;
    mSize = i_size;
    mColor = i_color;
    mClipZ = i_clipZ;
}

void mDoExt_spherePacket::draw() {
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_C0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);

    mDoMtx_stack_c::copy(j3dSys.getViewMtx());
    mDoMtx_stack_c::transM(mPosition.x, mPosition.y, mPosition.z);
    mDoMtx_stack_c::scaleM(mSize, mSize, mSize);

    GXLoadPosMtxImm(mDoMtx_stack_c::get(), 0);
    mDoMtx_stack_c::inverseTranspose();

    GXLoadNrmMtxImm(mDoMtx_stack_c::get(), 0);
    GXSetCurrentMtx(0);

    GXDrawSphere(8, 8);
}

mDoExt_cylinderPacket::mDoExt_cylinderPacket(cXyz& i_position, f32 i_radius, f32 i_height, const GXColor& i_color, u8 i_clipZ) {
    mPosition = i_position;
    mRadius = i_radius;
    mHeight = i_height;
    mColor = i_color;
    mClipZ = i_clipZ;
}

void mDoExt_cylinderPacket::draw() {
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, 1, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_C0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);

    f32 var_f31 = mHeight * 0.5f;

    mDoMtx_stack_c::copy(j3dSys.getViewMtx());
    mDoMtx_stack_c::transM(mPosition.x, mPosition.y + var_f31, mPosition.z);
    mDoMtx_stack_c::scaleM(mRadius, var_f31, mRadius);
    mDoMtx_stack_c::XrotM(0x4000);

    GXLoadPosMtxImm(mDoMtx_stack_c::get(), 0);
    mDoMtx_stack_c::inverseTranspose();

    GXLoadNrmMtxImm(mDoMtx_stack_c::get(), 0);
    GXSetCurrentMtx(0);
    GXDrawCylinder(8);
}

mDoExt_cylinderMPacket::mDoExt_cylinderMPacket(Mtx i_mtx, const GXColor& i_color, u8 i_clipZ) {
    cMtx_copy(i_mtx, mMatrix);
    mColor = i_color;
    mClipZ = i_clipZ;
}

void mDoExt_cylinderMPacket::draw() {
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, mColor);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_C0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    if (mClipZ) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    } else {
        GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    }

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);

#if TARGET_PC
    Mtx modelViewMtx;
    cMtx_concat(j3dSys.getViewMtx(), mMatrix, modelViewMtx);
    GXLoadPosMtxImm(modelViewMtx, 0);

    Mtx normalMtx;
    cMtx_inverseTranspose(modelViewMtx, normalMtx);
    GXLoadNrmMtxImm(normalMtx, 0);
#else
    cMtx_concat(j3dSys.getViewMtx(), mMatrix, mMatrix);

    GXLoadPosMtxImm(mMatrix, 0);
    cMtx_inverseTranspose(mMatrix, mMatrix);

    GXLoadNrmMtxImm(mMatrix, 0);
#endif
    GXSetCurrentMtx(0);

    GXDrawCylinder(8);
}
#endif

static void mDoExt_initFontCommon(JUTFont** mDoExt_font_p, ResFONT** mDoExt_resfont_p, JKRHeap* param_2,
                                  char const* param_3, JKRArchive* param_4, u8 param_5,
                                  u32 param_6, u32 param_7) {
    JUTFont** mDoExt_font = mDoExt_font_p;
    ResFONT** mDoExt_resfont = mDoExt_resfont_p;
    // these assertion messages are fakematches, not quite sure what's going on here -
    // the assembly indicates the parameters are dereferenced inline into compiler temps,
    // but the messages don't show a dereference
    JUT_ASSERT_MSG(7141, *mDoExt_font == NULL, "mDoExt_font == 0");
    JUT_ASSERT_MSG(7142, *mDoExt_resfont == NULL, "mDoExt_resfont == 0");
    *mDoExt_resfont = (ResFONT*)JKRGetResource('ROOT', param_3, param_4);
    JUT_ASSERT_MSG(7144, *mDoExt_resfont != NULL, "mDoExt_resfont != 0");
    if (param_5 == 0) {
        u32 cacheSize = JUTCacheFont::calcCacheSize(param_7, param_6);
        JUTCacheFont* cacheFont = JKR_NEW_ARGS (param_2, 0) JUTCacheFont(*mDoExt_resfont, cacheSize, param_2);
        if (cacheFont->isValid()) {
            // "Allocated %d bytes for font cache buffer size"
            OS_REPORT("\nキャッシュフォントバッファサイズとして %d バイト確保しました\n", cacheSize);
            // "The size of the JUTCacheFont class is %d bytes."
            OS_REPORT("JUTCacheFontクラスサイズは %d バイトです。\n", sizeof(JUTCacheFont));
            // "The number of cached pages is %d."
            OS_REPORT("キャッシュページ数は %d です。\n\n", cacheFont->getCachePage());
            // "The maximum sheet size is %d."
            OS_REPORT("シートサイズ（最大）は %d です。\n\n", cacheFont->getMaxSheetSize());
            *mDoExt_font = cacheFont;
            cacheFont->setPagingType(JUTCacheFont::PAGE_TYPE_1);
        }
        JKRRemoveResource(*mDoExt_resfont, NULL);
        *mDoExt_resfont = NULL;
    } else {
        *mDoExt_font = JKR_NEW JUTResFont(*mDoExt_resfont, param_2);
        // "Allocated %d bytes for the font buffer size"
        OS_REPORT("\nフォントバッファサイズとして %d バイト確保しました\n",
                  JKRGetMemBlockSize(param_2, *mDoExt_font));
        // "The size of the JUTResFont class is %d bytes."
        OS_REPORT("JUTResFontクラスサイズは %d バイトです。\n", sizeof(JUTResFont));
    }
    if (*mDoExt_font != NULL && !(*mDoExt_font)->isValid()) {
        // "Failed to create cache font class"
        OSReport_FatalError("\nキャッシュフォントクラス作成に失敗しました\n");
        JKR_DELETE(*mDoExt_font);
        *mDoExt_font = NULL;
    }
    JUT_ASSERT_MSG(7183, *mDoExt_font != NULL, "mDoExt_font != 0");
}

static JUTFont* mDoExt_font0;

static int mDoExt_font0_getCount;

static ResFONT* mDoExt_resfont0;

static void mDoExt_initFont0() {
    static char const fontdata[] = "rodan_b_24_22.bfn";
#if TARGET_PC
    if (dusk::version::isRegionJpn()) {
        mDoExt_initFontCommon(&mDoExt_font0, &mDoExt_resfont0, mDoExt_getZeldaHeap(),
                              fontdata, dComIfGp_getFontArchive(), 0, 200, 512);
    } else {
        mDoExt_initFontCommon(&mDoExt_font0, &mDoExt_resfont0, mDoExt_getZeldaHeap(),
                              fontdata, dComIfGp_getFontArchive(), 1, 0, 0);
    }
#elif REGION_JPN
    mDoExt_initFontCommon(&mDoExt_font0, &mDoExt_resfont0, mDoExt_getZeldaHeap(),
                          fontdata, dComIfGp_getFontArchive(), 0, 200, 512);
#else
    mDoExt_initFontCommon(&mDoExt_font0, &mDoExt_resfont0, mDoExt_getZeldaHeap(),
                          fontdata, dComIfGp_getFontArchive(), 1, 0, 0);
#endif
}

JUTFont* mDoExt_getMesgFont() {
    if (mDoExt_font0 == NULL) {
        mDoExt_initFont0();
    }

    mDoExt_font0_getCount++;
    return mDoExt_font0;
}

void mDoExt_removeMesgFont() {
    JUT_ASSERT(7238, mDoExt_font0_getCount > 0);
    if (mDoExt_font0_getCount > 0) {
        mDoExt_font0_getCount--;
        JUT_ASSERT(7241, mDoExt_font0_getCount > 0);
        if (mDoExt_font0_getCount == 0) {
            JKR_DELETE(mDoExt_font0);
            mDoExt_font0 = NULL;
            if (mDoExt_resfont0 != NULL) {
#if TARGET_PC
                if (dusk::version::isRegionJpn()) {
                    JKRFileLoader::removeResource(mDoExt_resfont0, NULL);
                } else {
                    JKRFree(mDoExt_resfont0);
                }
#elif REGION_JPN
                JKRFileLoader::removeResource(mDoExt_resfont0, NULL);
#else
                JKRFree(mDoExt_resfont0);
#endif
                mDoExt_resfont0 = NULL;
            }
        }
    }
}

static JUTFont* mDoExt_font1;

static int mDoExt_font1_getCount;

static ResFONT* mDoExt_resfont1;

static void mDoExt_initFont1() {
    static char const fontdata[] = "reishotai_24_22.bfn";
    mDoExt_initFontCommon(&mDoExt_font1, &mDoExt_resfont1, mDoExt_getZeldaHeap(),
                          fontdata, dComIfGp_getRubyArchive(), 1, 1, 0x8000);
}

JUTFont* mDoExt_getRubyFont() {
    if (mDoExt_font1 == NULL) {
        mDoExt_initFont1();
    }

    mDoExt_font1_getCount++;
    return mDoExt_font1;
}

static JUTFont* mDoExt_font2;

static int mDoExt_font2_getCount;

static ResFONT* mDoExt_resfont2;

static void mDoExt_initFont2() {
    static char const fontdata[] = "reishotai_24_22.bfn";
    mDoExt_initFontCommon(&mDoExt_font2, &mDoExt_resfont2, mDoExt_getZeldaHeap(),
                          fontdata, dComIfGp_getRubyArchive(), 1, 1, 0x8000);
}

JUTFont* mDoExt_getSubFont() {
    if (mDoExt_font2 == NULL) {
        mDoExt_initFont2();
    }

    mDoExt_font2_getCount++;
    return mDoExt_font2;
}

void dummy9() {
    JUT_ASSERT(0, mDoExt_font1_getCount > 0);
}

void mDoExt_removeSubFont() {
    JUT_ASSERT(7354, mDoExt_font2_getCount > 0);
    if (mDoExt_font2_getCount > 0) {
        mDoExt_font2_getCount--;
        JUT_ASSERT(7357, mDoExt_font2_getCount > 0);
        if (mDoExt_font2_getCount == 0) {
            JKR_DELETE(mDoExt_font2);
            mDoExt_font2 = NULL;
            if (mDoExt_resfont2 != NULL) {
                JKRFree(mDoExt_resfont2);
                mDoExt_resfont2 = NULL;
            }
        }
    }
}

J3DModel* mDoExt_J3DModel__create(J3DModelData* i_modelData, u32 i_modelFlag, u32 i_differedDlistFlag) {
    if (i_modelData != NULL) {
        J3DModel* model = JKR_NEW J3DModel();

        if (model != NULL) {
#if DEBUG
            model->getBaseTRMtx()[0][0] = MAXFLOAT;
#endif
            bool hasSharedDlistObj =
                i_modelData->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL;
            // Update the modelFlag if the model data passed in has a shared dlist object
            if (hasSharedDlistObj != false) {
                if (i_modelData->isLocked()) {
                    i_modelFlag = J3DMdlFlag_UseSharedDL;
                } else if (i_modelFlag == J3DMdlFlag_UseSharedDL) {
                    i_modelFlag |= J3DMdlFlag_UseSingleDL;
                } else {
                    i_modelFlag = J3DMdlFlag_DifferedDLBuffer;
                }
            }

            // Set up the model
            s32 entryModelData = model->entryModelData(i_modelData, i_modelFlag, 1);
            if (entryModelData == kJ3DError_Success) {
                if (i_modelFlag == J3DMdlFlag_DifferedDLBuffer &&
                    model->newDifferedDisplayList(i_differedDlistFlag) != kJ3DError_Success)
                {
                    return NULL;
                }

                model->lock();
                return model;
            }
        }
    }

    return NULL;
}

DUSK_GAME_DATA DummyCheckHeap* dch;

DummyCheckHeap::DummyCheckHeap() {
    mAlloc = NULL;
    mSize = 0;
    mHeap = NULL;
}

DummyCheckHeap::~DummyCheckHeap() {
    if (mHeap != NULL) {
        mHeap->destroy();
    }
}

int DummyCheckHeap::isVirgin() {
    if (mAlloc != NULL && mSize > 0) {
        for (int i = 0; i < mSize; i++) {
            if (((u8*)mAlloc)[i] != 0xdd) {
                u8* off = (u8*)mAlloc + i;
                // "DummyCheckHeap: Heap corruption detected"
                OS_REPORT_ERROR("DummyCheckHeap 領域破壊発見 %08x+%08x=%08x\n", mAlloc, i, off);
                return false;
            }
        }
    }
    return true;
}

JKRHeap* DummyCheckHeap::getHeap() {
    if (mHeap != NULL) {
        JUT_ASSERT(7604, mHeap);
        JUT_ASSERT(7605, mAlloc);
        JKRHeap* heap = mHeap;
        JUT_ASSERT(7607, mSize > 0);
        isVirgin();
        JKRFreeToHeap(mHeap, mAlloc);
        mHeap = NULL;
        mAlloc = NULL;
        return heap;
    } else {
        return NULL;
    }
}

void DummyCheckHeap::setHeap(JKRHeap* heap) {
    JUT_ASSERT(7620, heap != NULL);
    JUT_ASSERT(7621, mHeap == NULL);
    JUT_ASSERT(7622, mAlloc == NULL);
    mHeap = heap;
    mSize = JKRGetFreeSize(mHeap);
    JUT_ASSERT(7625, mSize > 0);
    mAlloc = JKRAllocFromHeap(mHeap, mSize, 0);
    JUT_ASSERT(7627, mAlloc);
    memset(mAlloc, 0xdd, mSize);
}

int DummyCheckHeap_init() {
    JUT_ASSERT(7634, dch == NULL);
    dch = JKR_NEW_ARGS((JKRHeap*)mDoExt_getZeldaHeap(), 0) DummyCheckHeap();
    if (dch == NULL) {
        return false;
    }

    return true;
}

int DummyCheckHeap_create() {
    if (dch != NULL) {
        if (dch->mHeap == NULL) {
            JKRHeap* rootHeap = JKRGetRootHeap2();
            JKRHeap* heap = JKRCreateExpHeap(0x60000, rootHeap, false);
            if (heap == NULL) {
                // "DummyCheckHeap allocation failure"
                OS_REPORT_ERROR("DummyCheckHeap_create 領域確保失敗\n");
                return 0;
            }
            dch->setHeap(heap);
        }
        return 1;
    }
    return 0;
}

void DummyCheckHeap_destroy() {
    if (dch != NULL && dch->mHeap != NULL) {
        JKRHeap* heap = dch->getHeap();
        if (heap != NULL) {
            heap->destroy();
        }
    }
}

int DummyCheckHeap_isVirgin() {
    int retval;
    if (dch != NULL) {
        retval = dch->isVirgin();
    } else {
        retval = 1;
    }
    return retval;
}

void DummyCheckHeap_check() {
    if (dch != NULL) {
       DummyCheckHeap_isVirgin();
    }
}

DUSK_GAME_DATA u32 aram_cache_size;

u32 mDoExt_getAraCacheSize() {
    return aram_cache_size;
}

void mDoExt_setAraCacheSize(u32 size) {
    aram_cache_size = size;
}

OSThread* mDoExt_GetCurrentRunningThread() {
    OSThread* thread = OSGetCurrentThread();
    if (thread != NULL && thread->state != 2) {
        thread = NULL;
    }

    return thread;
}

// TODO: hack to get these inlines to appear for debug
static void dummy() {
    { J3DZMode temp; temp = temp; J3DZMode temp2(temp); }
    { J3DBlend temp; temp = temp; J3DBlend temp2(temp); }
    { J3DAlphaComp temp; temp = temp; J3DAlphaComp temp2(temp); }
    { J3DIndTexCoordScale temp; temp = temp; J3DIndTexCoordScale temp2(temp); }
    { J3DIndTexMtx temp; temp = temp; J3DIndTexMtx temp2(temp); }
    { J3DIndTevStage temp; temp = temp; J3DIndTevStage temp2(temp); }
    { J3DTevStage temp; temp = temp; J3DTevStage temp2(temp); }
    { J3DTevSwapModeTable temp; temp = temp; J3DTevSwapModeTable temp2(temp); }
    { J3DTevOrder temp; temp = temp; J3DTevOrder temp2(temp); }
    { J3DGXColorS10 temp; temp = temp; J3DGXColorS10 temp2(temp); }
    { J3DTexCoord temp; temp = temp; J3DTexCoord temp2(temp); }
    { J3DColorChan temp; temp = temp; J3DColorChan temp2(temp); }
    { J3DGXColor temp; temp = temp; J3DGXColor temp2(temp); }
    { J3DIndTexOrderInfo temp; temp = temp; J3DIndTexOrderInfo temp2(temp); }
    { J3DFog temp; temp = temp; J3DFog temp2(temp); }
    { J3DTexMtx temp; temp = temp; J3DTexMtx temp2(temp); }
}

#if TARGET_PC
void mDoExt_btkAnm::entryFrame(f32 frame) {
    mpAnm->setFrame(frame);
    DUSK_DESKTOP_ONLY(dusk::interp::material::bind(mpAnm, getFrameCtrl()));
}

void mDoExt_brkAnm::entryFrame(f32 frame) {
    mpAnm->setFrame(frame);
    DUSK_DESKTOP_ONLY(dusk::interp::material::bind(mpAnm, getFrameCtrl()));
}
#endif
