#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DVertex.h"
#include "JSystem/J3DGraphBase/J3DFifo.h"
#include <gd.h>
#if defined(DUSK_TARGET_3DS)
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "../../../../platforms/3ds/runtime/gx_profile.hpp"
#endif

#if TARGET_PC
#include "JSystem/JKernel/JKRHeap.h"

#include <dolphin/gx.h>
#if !defined(DUSK_TARGET_3DS)
#include <tracy/Tracy.hpp>
#endif
#endif

void J3DGDSetVtxAttrFmtv(GXVtxFmt, GXVtxAttrFmtList const*, bool);
void J3DFifoLoadPosMtxImm(Mtx, u32);
void J3DFifoLoadNrmMtxImm(Mtx, u32);

void J3DShape::initialize() {
    mMaterial = NULL;
    mIndex = -1;
    mMtxGroupNum = 0;
    mFlags = 0;
    mRadius = 0.0f;
    mMin.x = 0.0f;
    mMin.y = 0.0f;
    mMin.z = 0.0f;
    mMax.x = 0.0f;
    mMax.y = 0.0f;
    mMax.z = 0.0f;
    mVtxDesc = NULL;
    mShapeMtx = NULL;
    mShapeDraw = NULL;
    mVertexData = NULL;
    mDrawMtxData = NULL;
    mScaleFlagArray = NULL;
    mDrawMtx = NULL;
    mNrmMtx = NULL;
    mCurrentViewNo = &j3dDefaultViewNo;
    mHasNBT = false;
    mHasPNMTXIdx = false;
}

void J3DShape::addTexMtxIndexInDL(GXAttr attr, u32 valueBase) {
    u32 kSize[] = {0, 1, 1, 2};

    s32 pnmtxidxOffs = -1;
    s32 attrOffs = -1;
    s32 stride = 0;
    bool found = false;

    for (GXVtxDescList* vtxDesc = getVtxDesc(); vtxDesc->attr != GX_VA_NULL; vtxDesc++) {
        if (vtxDesc->attr == GX_VA_PNMTXIDX)
            pnmtxidxOffs = stride;

        if (attr < vtxDesc->attr && !found) {
            attrOffs = stride;
            found = true;
        }

        stride = stride + kSize[vtxDesc->type];
    }

    if (pnmtxidxOffs == -1)
        return;

    for (u16 i = 0; i < (u16)getMtxGroupNum(); i++)
        getShapeDraw(i)->addTexMtxIndexInDL(stride, attrOffs, (s32)valueBase);
}

void J3DShape::addTexMtxIndexInVcd(GXAttr attr) {
    u32 kSize[] = {0, 1, 1, 2};  // stripped data

    s32 attrIdx = -1;
    s32 attrOffs = -1;
    s32 stride = 0;

    GXVtxDescList* vtxDesc = getVtxDesc();
    s32 attrCount = 0;

    for (; vtxDesc->attr != GX_VA_NULL; vtxDesc++) {
        if (vtxDesc->attr == GX_VA_PNMTXIDX) {
            attrIdx = stride;
        }
        attrCount++;
    }

    if (attrIdx == -1)
        return;

    GXVtxDescList* newVtxDesc = JKR_NEW_ARRAY(GXVtxDescList, attrCount + 2);
    bool inserted = false;

    vtxDesc = getVtxDesc();
    GXVtxDescList* dst = newVtxDesc;
    for (; vtxDesc->attr != GX_VA_NULL; vtxDesc++) {
        if ((attr < vtxDesc->attr) && !inserted) {
            dst->attr = attr;
            dst->type = GX_DIRECT;
            attrOffs = stride;
            dst++;

            inserted = true;
        }

        dst->attr = vtxDesc->attr;
        dst->type = vtxDesc->type;
        stride = stride + kSize[vtxDesc->type];
        dst++;
    }

    dst->attr = GX_VA_NULL;
    dst->type = GX_NONE;
    mVtxDesc = newVtxDesc;
    makeVcdVatCmd();
}

void J3DShape::calcNBTScale(const Vec& param_0, f32 (*param_1)[3][3], f32 (*param_2)[3][3]) {
    for (u16 i = 0; i < mMtxGroupNum; i++)
        mShapeMtx[i]->calcNBTScale(param_0, param_1, param_2);
}

u16 J3DShape::countBumpMtxNum() const {
    u16 num = 0;
    for (u16 i = 0; i < mMtxGroupNum; i++)
        num += mShapeMtx[i]->getUseMtxNum();

    return num;
}

void J3DLoadCPCmd(u8 addr, u32 val) {
    GXCmd1u8(GX_LOAD_CP_REG);
    GXCmd1u8(addr);
    GXCmd1u32(val);
}

#if TARGET_PC
static void J3DLoadArrayBasePtr(GXAttr attr, void* data, u32 size, bool le) {
    u32 idx = (attr == GX_VA_NBT) ? 1 : (attr - GX_VA_POS);
    GXCmd1u8(GX_AURORA);
    GXCmd1u16(GX_AURORA_LOAD_ARRAYBASE | idx);
    GXCmd1u64((u64)data);
    GXCmd1u32(size);
    GXCmd1u8(le ? 1 : 0);
}
#else
static void J3DLoadArrayBasePtr(GXAttr attr, void* data) {
    u32 idx = (attr == GX_VA_NBT) ? 1 : (attr - GX_VA_POS);
    J3DLoadCPCmd(0xA0 + idx, ((uintptr_t)data & 0x7FFFFFFF));
}
#endif

void J3DShape::loadVtxArray() const {
#if TARGET_PC
    // TODO: these can very easily overcount if the data isn't in F32 format
    if (j3dSys.getVtxPos() != mVertexData->getVtxPosArray()) {
        J3DLoadArrayBasePtr(GX_VA_POS, j3dSys.getVtxPos(), j3dSys.mVtxPosNum * sizeof(Vec), true);
    }

    if (!mHasNBT && j3dSys.getVtxNrm() != mVertexData->getVtxNrmArray()) {
        J3DLoadArrayBasePtr(GX_VA_NRM, j3dSys.getVtxNrm(), j3dSys.mVtxNrmNum * sizeof(Vec), true);
    }

    if (j3dSys.getVtxCol() != mVertexData->getVtxColorArray(0)) {
        J3DLoadArrayBasePtr(GX_VA_CLR0, j3dSys.getVtxCol(), j3dSys.mVtxColNum * sizeof(GXColor), true);
    }
#else
    J3DLoadArrayBasePtr(GX_VA_POS, j3dSys.getVtxPos());

    if (!mHasNBT) {
        J3DLoadArrayBasePtr(GX_VA_NRM, j3dSys.getVtxNrm());
    }

    J3DLoadArrayBasePtr(GX_VA_CLR0, j3dSys.getVtxCol());
#endif
}

bool J3DShape::isSameVcdVatCmd(J3DShape* other) {
    u8* a = (u8*)other->getVcdVatCmd();
    u8* b = mVcdVatCmd;
    for (u32 i = 0; i < kVcdVatDLSize; i++)
        if (a[i] != b[i])
            return false;

    return true;
}

void J3DShape::makeVtxArrayCmd() {
    GXVtxAttrFmtList* vtxAttr = mVertexData->getVtxAttrFmtList();

    u8 stride[12];
    void* array[12];
    for (u32 i = 0; i < 12; i++) {
        stride[i] = 0;
        array[i] = 0;
    }

    for (; vtxAttr->attr != GX_VA_NULL; vtxAttr++) {
        switch (vtxAttr->attr) {
        case GX_VA_POS: {
            if (vtxAttr->type == GX_F32)
                stride[vtxAttr->attr - GX_VA_POS] = 12;
            else
                stride[vtxAttr->attr - GX_VA_POS] = 6;

            array[vtxAttr->attr - GX_VA_POS] = mVertexData->getVtxPosArray();
            mVertexData->setVtxPosFrac((u8)vtxAttr->frac);
            mVertexData->setVtxPosType((GXCompType)vtxAttr->type);
        } break;
        case GX_VA_NRM: {
            if (vtxAttr->type == GX_F32)
                stride[vtxAttr->attr - GX_VA_POS] = 12;
            else
                stride[vtxAttr->attr - GX_VA_POS] = 6;

            array[vtxAttr->attr - GX_VA_POS] = mVertexData->getVtxNrmArray();
            mVertexData->setVtxNrmFrac((u8)vtxAttr->frac);
            mVertexData->setVtxNrmType((GXCompType)vtxAttr->type);
        } break;
        case GX_VA_CLR0:
        case GX_VA_CLR1: {
            stride[vtxAttr->attr - GX_VA_POS] = 4;
            array[vtxAttr->attr - GX_VA_POS] = mVertexData->getVtxColorArray(vtxAttr->attr - GX_VA_CLR0);
        } break;
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7: {
            if (vtxAttr->type == GX_F32)
                stride[vtxAttr->attr - GX_VA_POS] = 8;
            else
                stride[vtxAttr->attr - GX_VA_POS] = 4;

            array[vtxAttr->attr - GX_VA_POS] = mVertexData->getVtxTexCoordArray(vtxAttr->attr - GX_VA_TEX0);
        } break;
        default:
            break;
        }
    }

    GXVtxDescList* vtxDesc = mVtxDesc;
    mHasPNMTXIdx = false;
    for (; vtxDesc->attr != GX_VA_NULL; vtxDesc++) {
        if (vtxDesc->attr == GX_VA_NBT && vtxDesc->type != GX_NONE) {
            mHasNBT = true;
            stride[GX_VA_NRM - GX_VA_POS] *= 3;
            array[GX_VA_NRM - GX_VA_POS] = mVertexData->getVtxNBTArray();
            // TODO: How set array size here?
        } else if (vtxDesc->attr == GX_VA_PNMTXIDX && vtxDesc->type != GX_NONE) {
            mHasPNMTXIdx = true;
        }
    }

#if TARGET_PC
    for (u32 i = 0; i < 12; i++) {
        GXAttr attr = GXAttr(i + GX_VA_POS);
        if (array[i] != nullptr)
            GDSetArraySized(attr, array[i], mVertexData->getVtxArrByteSize(attr), mVertexData->getVtxArrStride(attr), true);
        else
            GDSetArraySized(attr, nullptr, 0, mVertexData->getVtxArrStride(attr), true);
    }
#else
    for (u32 i = 0; i < 12; i++) {
        if (array[i] != 0)
            GDSetArray((GXAttr)(i + GX_VA_POS), array[i], stride[i]);
        else
            GDSetArrayRaw((GXAttr)(i + GX_VA_POS), 0, stride[i]);
    }
#endif
}

void J3DShape::makeVcdVatCmd() {
#if !defined(DUSK_TARGET_3DS)
    static BOOL sInterruptFlag = OSDisableInterrupts();
    OSDisableScheduler();
#endif
    // Native GD builders are thread-local; this caller exclusively owns its list.

    GDLObj gdl_obj;
    GDInitGDLObj(&gdl_obj, mVcdVatCmd, kVcdVatDLSize);
    GDSetCurrent(&gdl_obj);
    GDSetVtxDescv(mVtxDesc);
    makeVtxArrayCmd();
    J3DGDSetVtxAttrFmtv(GX_VTXFMT0, mVertexData->getVtxAttrFmtList(), mHasNBT);
    GDPadCurr32();
    GDFlushCurrToMem();
    GDSetCurrent(NULL);
#if !defined(DUSK_TARGET_3DS)
    OSEnableScheduler();
    OSRestoreInterrupts(sInterruptFlag);
#endif
}

DUSK_GAME_DATA void* J3DShape::sOldVcdVatCmd;

#if defined(DUSK_TARGET_3DS)
extern "C" void duskNativeCallVertexSetup(const void* data, u32 size);
#define J3D_CALL_VERTEX_SETUP(list, size) duskNativeCallVertexSetup(list, size)
// packetPaint (gx_command_api.cpp): J3DCurrentMtx::load's three commands as
// typed register writes instead of FIFO bytes the stream parses again; false
// when the switch is off or the stream is not idle.
extern "C" bool duskNativeLoadCurrentMtx(u32 a, u32 b);
#define J3D_LOAD_CURRENT_MTX(mtx) \
    do { if (!duskNativeLoadCurrentMtx((mtx).getMtxIdxRegA(), (mtx).getMtxIdxRegB())) (mtx).load(); } while (0)
#else
#define J3D_CALL_VERTEX_SETUP(list, size) GXCallDisplayList(list, size)
#define J3D_LOAD_CURRENT_MTX(mtx) (mtx).load()
#endif

void J3DShape::loadCurrentMtx() const {
    J3D_LOAD_CURRENT_MTX(mCurrentMtx);
}

void J3DShape::loadPreDrawSetting() const {
    if (sOldVcdVatCmd != mVcdVatCmd) {
        J3D_CALL_VERTEX_SETUP(mVcdVatCmd, kVcdVatDLSize);
        sOldVcdVatCmd = mVcdVatCmd;
    }

    J3D_LOAD_CURRENT_MTX(mCurrentMtx);
}

DUSK_GAME_DATA bool J3DShape::sEnvelopeFlag;

void J3DShape::setArrayAndBindPipeline() const {
    J3DShapeMtx::setCurrentPipeline((mFlags & 0x1C) >> 2);
    loadVtxArray();
#if defined(DUSK_TARGET_3DS)
    bindMtxArrays();
}

// The rest of setArrayAndBindPipeline (also the direct record's way back).
void J3DShape::bindMtxArrays() const {
    static const auto bindIndex = dusk::native::registerGxNamedProfile("j3d.bindMtx");
    dusk::native::GxNamedProfileScope bindScope(bindIndex);
    const auto count = j3dSys.getModel()->getModelData()->getDrawMtxNum();
    j3dSys.setModelDrawMtx(mDrawMtx[*mCurrentViewNo], count);
    j3dSys.setModelNrmMtx(mNrmMtx[*mCurrentViewNo], count);
#else
    j3dSys.setModelDrawMtx(mDrawMtx[*mCurrentViewNo]);
    j3dSys.setModelNrmMtx(mNrmMtx[*mCurrentViewNo]);
#endif
    J3DShapeMtx::sCurrentScaleFlag = mScaleFlagArray;
    J3DShapeMtx::sNBTFlag = mHasNBT;
    sEnvelopeFlag = mHasPNMTXIdx;
    J3DShapeMtx::sTexMtxLoadType = getTexMtxLoadType();
}

#if defined(DUSK_TARGET_3DS)
extern "C" void duskNativeStaticGeometry(u32 generation, const void* list);
extern "C" bool duskNativeDrawShape(u32 generation, const void* list, u32 size);
u32 duskNativeDynamicReasons[4];
#include "../../../../platforms/3ds/runtime/model_lod.hpp"
J3DShape::NativeLod J3DShape::sNativeLod;
#endif

#if defined(DUSK_TARGET_3DS)
// Native static geometry: the model draws its original archive arrays (no CPU
// skin deformation or vertex color animation), so decoded vertices for each
// display list can be reused by generation. 0: arrays the CPU rewrites.
u32 J3DShape::nativeGeneration() const {
    const u32 generation = mVertexData != NULL &&
        j3dSys.getVtxPos() == mVertexData->getVtxPosArray() &&
        j3dSys.getVtxNrm() == mVertexData->getVtxNrmArray() &&
        j3dSys.getVtxCol() == mVertexData->getVtxColorArray(0) ?
        mVertexData->mNativeGeneration : 0;
    if (!generation) {
        // Diagnostic: which array made this shape dynamic (pos, nrm, col, no data).
        if (mVertexData == NULL) ++duskNativeDynamicReasons[3];
        else {
            if (j3dSys.getVtxPos() != mVertexData->getVtxPosArray()) ++duskNativeDynamicReasons[0];
            if (j3dSys.getVtxNrm() != mVertexData->getVtxNrmArray()) ++duskNativeDynamicReasons[1];
            if (j3dSys.getVtxCol() != mVertexData->getVtxColorArray(0)) ++duskNativeDynamicReasons[2];
        }
    }
    return generation;
}

namespace {
bool J3DNativeSkinDraw(J3DShape* shape, u32 generation);  // J3DNativeSkinPalette.h
}

// drawFast's matrix groups from `first` (`firstLoaded`: its matrices are
// loaded already). A skinned shape whose groups merge into wider palettes
// (features::Id::skinPalette) draws them as runs instead, unless it takes a
// level of detail this picture: the levels were cut for the shape's own
// groups and a merged run has none. So a far shape sends fewer indices and a
// near one (drawn whole anyway) fewer draws.
void J3DShape::drawMtxGroups(u16 first, bool firstLoaded, u32 generation) const {
    const bool reducedScene = sNativeLod.shape != NULL && sNativeLod.scene != 0;
    if (first == 0 && !firstLoaded && generation != 0 && !reducedScene &&
        J3DNativeSkinDraw(const_cast<J3DShape*>(this), generation))
        return;
    for (u16 n = mMtxGroupNum, i = first; i < n; i++) {
        if (mShapeMtx[i] != NULL && !(firstLoaded && i == first)) {
            static const auto mtxIndex = dusk::native::registerGxNamedProfile("j3d.shapeMtxLoad");
            dusk::native::GxNamedProfileScope mtxScope(mtxIndex);
            mShapeMtx[i]->load();
        }
        if (mShapeDraw[i] != NULL) {
            static const auto queueIndex = dusk::native::registerGxNamedProfile("j3d.queueShape");
            dusk::native::GxNamedProfileScope queueScope(queueIndex);
            // Levels of detail replace the index lists of a recorded list's
            // batches (generation 0 lists are never recorded).
            const auto* lod = static_cast<const dusk::native::model_lod::Shape*>(sNativeLod.shape);
            const bool reduced = lod != NULL && generation != 0 && i < lod->groups && (sNativeLod.scene | sNativeLod.shadow) != 0;
            if (reduced) {
                duskNativeLodSelect(&lod->group[i], sNativeLod.scene, sNativeLod.shadow);
            }
            // Native engine: recorded static shapes draw directly on the
            // PICA, and with the second-core worker on every shape is
            // queued for it (generation 0: arrays the CPU rewrites).
            if (!duskNativeDrawShape(generation, mShapeDraw[i]->getDisplayList(), mShapeDraw[i]->getDisplayListSize())) {
                duskNativeStaticGeometry(generation, mShapeDraw[i]->getDisplayList());
                mShapeDraw[i]->draw();
                duskNativeStaticGeometry(0, NULL);
            }
            if (reduced) {
                duskNativeLodSelect(NULL, 0, 0);
            }
        }
    }
}
#endif

void J3DShape::drawFast() const {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#else
    static const auto setupIndex = dusk::native::registerGxNamedProfile("j3d.vtxSetup");
    dusk::native::GxNamedProfileSpan setupSpan(setupIndex);
#endif
    if (sOldVcdVatCmd != mVcdVatCmd) {
        J3D_CALL_VERTEX_SETUP(mVcdVatCmd, kVcdVatDLSize);
        sOldVcdVatCmd = mVcdVatCmd;
    }

    if (sEnvelopeFlag != 0 && !mHasPNMTXIdx)
        J3D_LOAD_CURRENT_MTX(mCurrentMtx);

    setArrayAndBindPipeline();
    if (!checkFlag(J3DShpFlag_NoMtx)) {
        if (J3DShapeMtx::getLODFlag())
            J3DShapeMtx::resetMtxLoadCache();

#if defined(DUSK_TARGET_3DS)
        const u32 generation = nativeGeneration();
        setupSpan.stop();
        drawMtxGroups(0, false, generation);
#else
        for (u16 n = mMtxGroupNum, i = 0; i < n; i++) {
            if (mShapeMtx[i] != NULL)
                mShapeMtx[i]->load();
            if (mShapeDraw[i] != NULL)
                mShapeDraw[i]->draw();
        }
#endif
    } else {
        J3DFifoLoadPosMtxImm(*j3dSys.getShapePacket()->getBaseMtxPtr(), GX_PNMTX0);
        J3DFifoLoadNrmMtxImm(*j3dSys.getShapePacket()->getBaseMtxPtr(), GX_PNMTX0);
        for (u16 n = mMtxGroupNum, i = 0; i < n; i++)
            if (mShapeDraw[i] != NULL)
                mShapeDraw[i]->draw();
    }
}

#if defined(DUSK_TARGET_3DS)
extern "C" bool duskNativeDirectList(const void* list, u32 size);
extern "C" bool duskNativeDirectCurrentMtx(u32 a, u32 b);
extern "C" bool duskNativeDirectBind(unsigned slot, const void* data, u32 size, u32 stride);
extern "C" bool duskNativeDirectIndexed(unsigned code, unsigned index, unsigned packed);
extern "C" bool duskNativeDirectShape(u32 generation, const void* list, u32 size);
extern "C" bool duskNativeDirectOpen(void);

// Every shape matrix type is taken: the plain J3DShapeMtx on pipeline 0
// (indexed loads, entered directly) and every other (J3DShapeMtxConcatView,
// the type all models of this game get from their load flags 0x...10, its
// Multi/BBoard/YBBoard kinds, J3DShapeMtxMulti, the CPU pipelines), whose own
// load() runs unchanged and whose GX calls (GXNativeLoadXf, GXSETARRAY,
// GXLoadTexMtxImm, GXNativeLoadIndx) the open capture records.
const char* J3DShape::nativeDirectEligible() const {
    if (checkFlag(J3DShpFlag_NoMtx))
        return "direct: shape without matrix";
    if (mVcdVatCmd == NULL)
        return "direct: shape without vertex setup";
    if (mVertexData == NULL)
        return "direct: shape without vertex data";
    for (u16 i = 0; i < mMtxGroupNum; i++) {
        if (mShapeDraw[i] == NULL || mShapeDraw[i]->getDisplayList() == NULL || mShapeDraw[i]->getDisplayListSize() == 0)
            return "direct: shape group without list";
    }
    return NULL;
}

// loadPreDrawSetting: the vertex setup list (the memo runs it in a capture,
// see native_packets.hpp) and J3DCurrentMtx::load's three commands.
bool J3DShape::nativeDirectPreDraw() const {
    if (sOldVcdVatCmd != mVcdVatCmd) {
        if (!duskNativeDirectList(mVcdVatCmd, kVcdVatDLSize))
            return false;
        sOldVcdVatCmd = mVcdVatCmd;
    }
    return duskNativeDirectCurrentMtx(mCurrentMtx.getMtxIdxRegA(), mCurrentMtx.getMtxIdxRegB());
}

// drawFast for an eligible shape, statement by statement. Where the original
// code runs (loadVtxArray, the shape matrix loads) its GX calls are recorded
// by the open capture; one that cannot be (a full record, an indexed load
// through an unknown binding) ends the capture: what it held runs on the
// original path and the rest of the shape follows it there.
J3DShape::NativeDirect J3DShape::nativeDirectDrawFast() const {
    if (sOldVcdVatCmd != mVcdVatCmd) {
        if (!duskNativeDirectList(mVcdVatCmd, kVcdVatDLSize))
            return nativeRefused;
        sOldVcdVatCmd = mVcdVatCmd;
    }
    if (sEnvelopeFlag != 0 && !mHasPNMTXIdx &&
        !duskNativeDirectCurrentMtx(mCurrentMtx.getMtxIdxRegA(), mCurrentMtx.getMtxIdxRegB()))
        return nativeRefused;

    // setArrayAndBindPipeline: loadVtxArray itself (it binds nothing for the
    // model's own arrays; CPU-deformed arrays through GX, recorded); the two
    // matrix arrays as GXSETARRAY binds them.
    const u32 pipeline = (mFlags & 0x1C) >> 2;
    J3DShapeMtx::setCurrentPipeline(pipeline);
    loadVtxArray();
    if (!duskNativeDirectOpen()) {
        bindMtxArrays();
        if (J3DShapeMtx::getLODFlag())
            J3DShapeMtx::resetMtxLoadCache();
        drawMtxGroups(0, false, nativeGeneration());
        return nativeEnded;
    }
    const auto count = j3dSys.getModel()->getModelData()->getDrawMtxNum();
    Mtx* drawMtx = mDrawMtx[*mCurrentViewNo];
    Mtx33* nrmMtx = mNrmMtx[*mCurrentViewNo];
    j3dSys.mModelDrawMtx = drawMtx;
    if (!duskNativeDirectBind(GX_POS_MTX_ARRAY - GX_VA_POS, drawMtx, count * sizeof(Mtx), sizeof(Mtx)))
        return nativeRefused;
    j3dSys.mModelNrmMtx = nrmMtx;
    if (!duskNativeDirectBind(GX_NRM_MTX_ARRAY - GX_VA_POS, nrmMtx, count * sizeof(Mtx33), sizeof(Mtx33)))
        return nativeRefused;
    J3DShapeMtx::sCurrentScaleFlag = mScaleFlagArray;
    J3DShapeMtx::sNBTFlag = mHasNBT;
    sEnvelopeFlag = mHasPNMTXIdx;
    J3DShapeMtx::sTexMtxLoadType = getTexMtxLoadType();

    if (J3DShapeMtx::getLODFlag())
        J3DShapeMtx::resetMtxLoadCache();
    const u32 generation = nativeGeneration();
    for (u16 n = mMtxGroupNum, i = 0; i < n; i++) {
        if (mShapeMtx[i] != NULL) {
            if (pipeline == 0 && mShapeMtx[i]->getType() == 'SMTX') {
                // J3DShapeMtx::load, pipeline 0 (loadMtxIndx_PNGP, slot 0): the
                // position and normal matrices of the shape's draw matrix index.
                const u16 index = mShapeMtx[i]->getUseMtxIndex(0);
                if (!duskNativeDirectIndexed(GX_LOAD_INDX_A, index, 0xB000) ||
                    !duskNativeDirectIndexed(GX_LOAD_INDX_B, index, ((9 - 1) << 12) | 0x400))
                    return nativeRefused;
            } else {
                // The original load: its matrix math on this thread, its GX
                // calls into the capture (typed calls are recorded at once,
                // in order with the direct operations).
                mShapeMtx[i]->load();
                if (!duskNativeDirectOpen()) {
                    drawMtxGroups(i, true, generation);
                    return nativeEnded;
                }
            }
        }
        if (!duskNativeDirectShape(generation, mShapeDraw[i]->getDisplayList(), mShapeDraw[i]->getDisplayListSize()))
            return nativeRefused;
    }
    return nativeDirect;
}
#endif

void J3DShape::draw() const {
    resetVcdVatCache();
    loadPreDrawSetting();
    drawFast();
}

void J3DShape::simpleDraw() const {
    resetVcdVatCache();
    loadPreDrawSetting();
    J3DShapeMtx::setCurrentPipeline((mFlags & 0x1C) >> 2);
    loadVtxArray();
    for (u16 n = mMtxGroupNum, i = 0; i < n; i++) {
        if (mShapeDraw[i] != NULL) {
            mShapeDraw[i]->draw();
        }
    }
}

void J3DShape::simpleDrawCache() const {
    if (sOldVcdVatCmd != mVcdVatCmd) {
        J3D_CALL_VERTEX_SETUP(mVcdVatCmd, kVcdVatDLSize);
        sOldVcdVatCmd = mVcdVatCmd;
    }

    if (sEnvelopeFlag && !mHasPNMTXIdx)
        mCurrentMtx.load();

    loadVtxArray();
#if defined(DUSK_TARGET_3DS)
    // Native static geometry: same record/replay as drawFast when the shape
    // draws its original archive arrays.
    const u32 generation = mVertexData != NULL &&
        j3dSys.getVtxPos() == mVertexData->getVtxPosArray() &&
        j3dSys.getVtxNrm() == mVertexData->getVtxNrmArray() &&
        j3dSys.getVtxCol() == mVertexData->getVtxColorArray(0) ?
        mVertexData->mNativeGeneration : 0;
    for (u16 n = mMtxGroupNum, i = 0; i < n; i++) {
        if (mShapeDraw[i] == NULL)
            continue;
        if (!duskNativeDrawShape(generation, mShapeDraw[i]->getDisplayList(), mShapeDraw[i]->getDisplayListSize())) {
            duskNativeStaticGeometry(generation, mShapeDraw[i]->getDisplayList());
            mShapeDraw[i]->draw();
            duskNativeStaticGeometry(0, NULL);
        }
    }
#else
    for (u16 n = mMtxGroupNum, i = 0; i < n; i++)
        if (mShapeDraw[i] != NULL)
            mShapeDraw[i]->draw();
#endif
}

#if defined(DUSK_TARGET_3DS)
// ---------------------------------------------------------------------------
// Instance batches (JSystem/J3DGraphBase/J3DNativeInstances.h). Game thread
// only: the draw phase registers, the painter draws. The worker on core 2
// reads a merged list through the queued shape, one picture late: a list is
// never written again once built (a rebuilt or recycled one gets a new
// generation, so no static-geometry entry or shape record of the old one is
// ever found for it), and the store empties only when none of its lists was
// drawn in the last three pictures.
// ---------------------------------------------------------------------------
#include "JSystem/J3DGraphBase/J3DNativeInstances.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/J3DGraphBase/J3DTransform.h"
#include <cmath>
#include <cstring>

extern "C" void duskNativeShadowCaster(unsigned slot);

namespace {
struct NativeCarrier {
    const J3DModel* model;
    const J3DNativeInstances* instances;
};
const u32 kNativeCarriers = 64;
NativeCarrier sNativeCarriers[kNativeCarriers];
u32 sNativeCarrierCount;
u32 sNativePicture;
J3DNativeInstanceStats sNativeStats;

// A merged list: `slots` copies of a shape's list, copy k with matrix index
// 3k (GX_PNMTX0 + k) in front of every vertex, and that shape's vertex setup
// list with the matrix index switched on.
struct NativeInstanceList {
    u32 generation;  // this list's own, never reused; 0: free
    u32 sourceGeneration;
    const u8* sourceList;
    const u8* sourceSetup;
    u32 list, listBytes, setup;  // offsets in the arena
    u32 lastUse;                 // picture
    u16 slots;
};
const u32 kNativeListArena = 64 * 1024;
const u32 kNativeLists = 48;
alignas(32) u8 sNativeListArena[kNativeListArena];
NativeInstanceList sNativeLists[kNativeLists];
u32 sNativeListArenaUsed;
// Vertex-data generations count up from 1 (J3DVertex.cpp); these have the
// top bit, so neither space ever meets the other.
u32 sNativeListGeneration;

// One model's matrices, as J3DShapeMtxConcatView::loadMtxConcatView_PNGP
// computes them.
struct NativeInstanceMatrix {
    Mtx position;
    Mtx33 normal;
    bool scaled;
};
NativeInstanceMatrix sNativeMatrices[J3DNativeInstances::maxModels];

// Bytes of one vertex of a list whose attributes are all indexed (none
// direct, no matrix index): 0 for any other descriptor.
u32 nativeIndexedVertexBytes(const GXVtxDescList* desc) {
    if (desc == NULL)
        return 0;
    u32 bytes = 0;
    for (int n = 0; desc[n].attr != GX_VA_NULL; n++) {
        if (n >= 32)
            return 0;
        if (desc[n].type == GX_NONE)
            continue;
        if (desc[n].attr < GX_VA_POS || desc[n].attr > GX_VA_TEX7)
            return 0;
        if (desc[n].type == GX_INDEX8)
            bytes += 1;
        else if (desc[n].type == GX_INDEX16)
            bytes += 2;
        else
            return 0;
    }
    return bytes;
}

bool nativePrimitive(u8 op) {
    switch (op & 0xF8) {
    case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: case 0xA8: case 0xB0: case 0xB8:
        return true;
    }
    return false;
}

// Bytes of one copy of `list` with a matrix index before every vertex; 0 when
// the list holds anything but primitives and NOP padding.
u32 nativeCopyBytes(const u8* list, u32 size, u32 vertexBytes) {
    u32 at = 0, bytes = 0;
    while (at < size) {
        const u8 op = list[at];
        if (op == 0) {
            at++;
            continue;
        }
        if (!nativePrimitive(op) || at + 3 > size)
            return 0;
        const u32 count = (u32(list[at + 1]) << 8) | list[at + 2];
        if (count == 0 || at + 3 + count * vertexBytes > size)
            return 0;
        bytes += 3 + count * (vertexBytes + 1);
        at += 3 + count * vertexBytes;
    }
    return bytes;
}

u8* nativeCopy(const u8* list, u32 size, u32 vertexBytes, u8 index, u8* out) {
    u32 at = 0;
    while (at < size) {
        const u8 op = list[at];
        if (op == 0) {
            at++;
            continue;
        }
        const u32 count = (u32(list[at + 1]) << 8) | list[at + 2];
        *out++ = op;
        *out++ = list[at + 1];
        *out++ = list[at + 2];
        at += 3;
        for (u32 v = 0; v < count; v++) {
            *out++ = index;
            std::memcpy(out, list + at, vertexBytes);
            out += vertexBytes;
            at += vertexBytes;
        }
    }
    return out;
}

// A shape's vertex setup list (J3DShape::makeVcdVatCmd) starts with the CP
// write of VCD_LO (08 50, the value big-endian); its bit 0 puts a direct
// matrix index in front of every vertex.
bool nativeSetupPatchable(const u8* setup) {
    return setup != NULL && setup[0] == 0x08 && setup[1] == 0x50 && (setup[5] & 1) == 0;
}

bool nativeSetupMatches(const u8* copy, const u8* source) {
    return std::memcmp(copy, source, 5) == 0 && copy[5] == (source[5] | 1) &&
           std::memcmp(copy + 6, source + 6, J3DShape::kVcdVatDLSize - 6) == 0;
}

const NativeInstanceList* nativeInstanceList(const u8* list, u32 size, const u8* setup, u32 sourceGeneration,
                                             u32 vertexBytes, u16 slots) {
    NativeInstanceList* free = NULL;
    for (u32 i = 0; i < kNativeLists; i++) {
        NativeInstanceList& e = sNativeLists[i];
        if (e.generation == 0) {
            if (free == NULL)
                free = &e;
            continue;
        }
        if (e.sourceGeneration == sourceGeneration && e.sourceList == list && e.sourceSetup == setup &&
            e.slots == slots)
        {
            if (nativeSetupMatches(sNativeListArena + e.setup, setup)) {
                e.lastUse = sNativePicture;
                return &e;
            }
            // The shape's setup list was made again: a new list (its arena
            // bytes stay as they were until the store empties).
            e.generation = 0;
            if (free == NULL)
                free = &e;
            break;
        }
    }
    const u32 copy = nativeCopyBytes(list, size, vertexBytes);
    if (copy == 0 || !nativeSetupPatchable(setup))
        return NULL;
    const u32 listBytes = (copy * slots + 31) & ~31u;
    const u32 need = listBytes + J3DShape::kVcdVatDLSize;
    if (need > kNativeListArena)
        return NULL;
    if (free == NULL || sNativeListArenaUsed + need > kNativeListArena) {
        for (u32 i = 0; i < kNativeLists; i++) {
            if (sNativeLists[i].generation != 0 && sNativeLists[i].lastUse + 3 > sNativePicture)
                return NULL;
        }
        std::memset(sNativeLists, 0, sizeof(sNativeLists));
        sNativeListArenaUsed = 0;
        free = &sNativeLists[0];
        sNativeStats.recycled++;
    }
    u8* out = sNativeListArena + sNativeListArenaUsed;
    u8* at = out;
    for (u16 k = 0; k < slots; k++)
        at = nativeCopy(list, size, vertexBytes, u8(3 * k), at);
    while (at < out + listBytes)
        *at++ = 0;  // GX_NOP
    std::memcpy(out + listBytes, setup, J3DShape::kVcdVatDLSize);
    out[listBytes + 5] |= 1;
    free->generation = 0x80000000u | ++sNativeListGeneration;
    free->sourceGeneration = sourceGeneration;
    free->sourceList = list;
    free->sourceSetup = setup;
    free->list = sNativeListArenaUsed;
    free->listBytes = listBytes;
    free->setup = sNativeListArenaUsed + listBytes;
    free->lastUse = sNativePicture;
    free->slots = slots;
    sNativeListArenaUsed += need;
    sNativeStats.listsMade++;
    return free;
}

// Models per draw the PICA's rigid shaders take (runtime/gx_rigid_vertex.hpp,
// addRigidMatrix and paletteLimit): rows 0-5 hold the first matrix, the
// lights end at row 19 + 4 per light, then six rows per further matrix up to
// row 81 + 4 per texture unit left unused (89 with one texture).
u16 nativeInstancesPerDraw(J3DMaterial* material) {
    unsigned lights = 1;
    if (J3DColorChan* chan = material != NULL ? material->getColorChan(0) : NULL) {
        lights = __builtin_popcount(chan->getLightMask());
        if (lights == 0)
            lights = 1;
    }
    unsigned units = material != NULL ? material->getTexGenNum() : 3;
    units = units < 1 ? 1 : units > 3 ? 3 : units;
    const unsigned limit = 81 + 4 * (3 - units), base = 19 + 4 * lights;
    if (base + 6 > limit)
        return 1;
    const unsigned slots = 2 + (limit - base - 6) / 6;
    return u16(slots > 10 ? 10 : slots);
}

bool nativeFinite(const f32* values, int count) {
    for (int i = 0; i < count; i++) {
        if (!(std::fabs(values[i]) <= 1.0e8f))
            return false;  // NaN too: the PICA route refuses such rows
    }
    return true;
}

bool nativeInstanceMatrix(J3DModel* model, MtxP base, u16 useMtxIndex, NativeInstanceMatrix& out) {
    J3DModelData* data = model->getModelData();
    if (data->getDrawMtxFlag(useMtxIndex) != 0)
        return false;
    const u16 drw = data->getDrawMtxIndex(useMtxIndex);
    Mtx* table = (Mtx*)model->getMtxBuffer()->getUserAnmMtx(0);
    MTXConcat(base, table[drw], out.position);
    out.scaled = model->getMtxBuffer()->getScaleFlagArray()[drw] != 1;
    if (out.scaled) {
        std::memset(out.normal, 0, sizeof(out.normal));
        J3DPSCalcInverseTranspose(out.position, out.normal);
    }
    return nativeFinite(&out.position[0][0], 12) && (!out.scaled || nativeFinite(&out.normal[0][0], 9));
}

void nativeLoadInstanceMatrix(NativeInstanceMatrix& m, u32 slot) {
    J3DFifoLoadPosMtxImm(m.position, slot * 3);
    if (m.scaled)
        J3DFifoLoadNrmMtxImm3x3(m.normal, slot * 3);
    else
        J3DFifoLoadNrmMtxImm(m.position, slot * 3);
}
}  // namespace

#include "J3DNativeSkinPalette.h"

void J3DNativeInstancesBeginPicture() {
    sNativeCarrierCount = 0;
    sNativePicture++;
    skinPicture();
}

bool J3DNativeInstancesRegister(J3DModel* carrier, const J3DNativeInstances* instances) {
    for (u32 i = 0; i < sNativeCarrierCount; i++) {
        if (sNativeCarriers[i].model == carrier) {
            sNativeCarriers[i].instances = instances;
            return true;
        }
    }
    if (sNativeCarrierCount >= kNativeCarriers)
        return false;
    sNativeCarriers[sNativeCarrierCount].model = carrier;
    sNativeCarriers[sNativeCarrierCount].instances = instances;
    sNativeCarrierCount++;
    return true;
}

const J3DNativeInstances* J3DNativeInstancesOf(const J3DModel* model) {
    for (u32 i = 0; i < sNativeCarrierCount; i++) {
        if (sNativeCarriers[i].model == model)
            return sNativeCarriers[i].instances;
    }
    return NULL;
}

const char* J3DNativeInstancesShapeRefusal(J3DShape* shape) {
    if (shape == NULL || shape->getMtxGroupNum() != 1 || shape->getShapeMtx(0) == NULL ||
        shape->getShapeDraw(0) == NULL || shape->getShapeDraw(0)->getDisplayList() == NULL)
        return "shape groups";
    if (shape->getShapeMtx(0)->getType() != 'SMCV')
        return "shape matrix";
    // 0x10: J3DShape::drawFast's pipeline bits beyond the CPU-skin ones (those
    // two follow the model, refused separately).
    if (shape->checkFlag(J3DShpFlag_NoMtx) || shape->checkFlag(0x10) || shape->getNBTFlag() ||
        shape->getTexMtxLoadType() == 0x2000)
        return "shape pipeline";
    const u32 vertexBytes = nativeIndexedVertexBytes(shape->getVtxDesc());
    if (vertexBytes == 0)
        return "shape attributes";
    if (nativeCopyBytes(shape->getShapeDraw(0)->getDisplayList(), shape->getShapeDraw(0)->getDisplayListSize(),
                        vertexBytes) == 0)
        return "shape list";
    if (!nativeSetupPatchable((const u8*)shape->getVcdVatCmd()))
        return "shape setup";
    return NULL;
}

void J3DNativeInstancesDraw(J3DShape* shape, const J3DShapePacket& carrier, const J3DNativeInstances& batch) {
    static const auto profileIndex = dusk::native::registerGxNamedProfile("j3d.instances");
    dusk::native::GxNamedProfileScope profile(profileIndex);
    J3DModel* const* models = batch.models;
    const int count = batch.count;
    J3DModel* carrierModel = carrier.getModel();
    const bool viewBase = carrier.getBaseMtxPtr() == (Mtx*)j3dSys.getViewMtx();
    const char* why = NULL;
    u32 generation = 0;
    if (count < 1 || count > J3DNativeInstances::maxModels || models[count - 1] != carrierModel) {
        why = "batch";
    } else if ((why = J3DNativeInstancesShapeRefusal(shape)) != NULL) {
    } else if (shape->checkFlag(0x1C) || J3DShapeMtx::getLODFlag() || J3DDifferedTexMtx::sTexGenBlock != NULL) {
        why = "draw state";
    } else if ((generation = shape->nativeGeneration()) == 0) {
        why = "arrays";
    }
    if (why == NULL) {
        J3DModelData* data = carrierModel->getModelData();
        MtxP base = *carrier.getBaseMtxPtr();
        const u16 use = shape->getShapeMtx(0)->getUseMtxIndex(0);
        for (int i = 0; i < count && why == NULL; i++) {
            J3DModel* model = models[i];
            if (model == NULL || model->getModelData() != data ||
                model->checkFlag(J3DMdlFlag_SkinPosCpu | J3DMdlFlag_SkinNrmCpu | J3DMdlFlag_EnableLOD) ||
                model->getMtxCalcMode() == 2)
                why = "model";
            else if (viewBase && model->mNativeShadowSlot != 0)
                why = "shadow caster";
            else if (!nativeInstanceMatrix(model, base, use, sNativeMatrices[i]))
                why = "matrix";
        }
    }
    const u16 perDraw = why == NULL ? nativeInstancesPerDraw(shape->getMaterial()) : 0;
    if (why == NULL && perDraw < 2)
        why = "palette";
    const NativeInstanceList* full = NULL;
    const NativeInstanceList* rest = NULL;
    if (why == NULL) {
        const u8* list = shape->getShapeDraw(0)->getDisplayList();
        const u32 size = shape->getShapeDraw(0)->getDisplayListSize();
        const u8* setup = (const u8*)shape->getVcdVatCmd();
        const u32 vertexBytes = nativeIndexedVertexBytes(shape->getVtxDesc());
        const int remainder = count % perDraw;
        if (count >= perDraw && (full = nativeInstanceList(list, size, setup, generation, vertexBytes, perDraw)) == NULL)
            why = "list store";
        else if (remainder != 0 &&
                 (rest = nativeInstanceList(list, size, setup, generation, vertexBytes, u16(remainder))) == NULL)
            why = "list store";
    }

    if (why != NULL) {
        // Each model by the original drawFast, in the original order: the
        // shape reads the current model's matrices (J3DShapeMtxConcatView)
        // and scale flags; the carrier's differed list served them all.
        sNativeStats.why = why;
        for (int i = count - 1; i >= 0; i--) {
            J3DModel* model = models[i];
            if (model == NULL)
                continue;
            j3dSys.setModel(model);
            shape->setScaleFlagArray(model->getMtxBuffer()->getScaleFlagArray());
            const unsigned slot = viewBase ? model->mNativeShadowSlot : 0;
            if (slot != 0)
                duskNativeShadowCaster(slot);
            shape->drawFast();
            if (slot != 0)
                duskNativeShadowCaster(0);
            sNativeStats.alone++;
        }
        j3dSys.setModel(carrierModel);
        shape->setScaleFlagArray(carrierModel->getMtxBuffer()->getScaleFlagArray());
        return;
    }

    // J3DShape::drawFast, with the merged lists' vertex setup: the setup list
    // first, the current matrix after a shape with matrix indices (these
    // shapes have none of their own), the arrays and pipeline, then per draw
    // the matrices of its models and the merged list.
    const NativeInstanceList* first = count >= perDraw ? full : rest;
    void* setup = sNativeListArena + first->setup;
    if (J3DShape::sOldVcdVatCmd != setup) {
        J3D_CALL_VERTEX_SETUP(setup, J3DShape::kVcdVatDLSize);
        J3DShape::sOldVcdVatCmd = setup;
    }
    if (J3DShape::sEnvelopeFlag)
        shape->loadCurrentMtx();
    shape->setArrayAndBindPipeline();
    for (int i = count - 1; i >= 0;) {
        const int n = i + 1 >= perDraw ? perDraw : i + 1;
        const NativeInstanceList& entry = n == perDraw ? *full : *rest;
        setup = sNativeListArena + entry.setup;
        if (J3DShape::sOldVcdVatCmd != setup) {
            J3D_CALL_VERTEX_SETUP(setup, J3DShape::kVcdVatDLSize);
            J3DShape::sOldVcdVatCmd = setup;
        }
        for (int k = 0; k < n; k++, i--)
            nativeLoadInstanceMatrix(sNativeMatrices[i], u32(k));
        const void* merged = sNativeListArena + entry.list;
        if (!duskNativeDrawShape(entry.generation, merged, entry.listBytes)) {
            duskNativeStaticGeometry(entry.generation, merged);
            GXCallDisplayList(merged, entry.listBytes);
            duskNativeStaticGeometry(0, NULL);
        }
        sNativeStats.draws++;
    }
    sNativeStats.instances += count;
}

J3DNativeInstanceStats& J3DNativeInstancesStats() {
    return sNativeStats;
}

void J3DNativeInstancesStore(u32& lists, u32& bytes, u32& capacity) {
    lists = 0;
    for (u32 i = 0; i < kNativeLists; i++)
        lists += sNativeLists[i].generation != 0;
    bytes = sNativeListArenaUsed;
    capacity = kNativeListArena;
}
#endif
