#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DShapeTable.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/JKernel/JKRHeap.h"

#if defined(DUSK_TARGET_3DS)
// Per-tick model timers on profiled pictures (include/d/d_tick_profile.h).
#include "../../../../platforms/3ds/runtime/gx_profile.hpp"
#define J3D_TICK_PROFILE(var, name)                                                                \
    static const unsigned var##_profile = dusk::native::registerGxNamedProfile(name);              \
    dusk::native::GxTickProfileScope var(var##_profile)
#include "d/d_tick_profile.h"
#else
#define J3D_TICK_PROFILE(var, name)
#endif

#if TARGET_PC
#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/material.h"
#include "dusk/interp/vertex.h"
#endif

#define J3D_ASSERTMSG(LINE, COND, MSG) JUT_ASSERT_MSG(LINE, (COND) != 0, MSG)
#define J3D_WARN1(LINE, MSG, ARG1) JUT_WARN(LINE, MSG, ARG1)

#if defined(DUSK_TARGET_3DS)
// The second-core worker's overlap hook (null in host builds and until set).
#include "../../../../platforms/3ds/runtime/gx_worker_sync.hpp"
#endif

#if defined(DUSK_PICA_GAME_DRAW)
bool J3DModel::copyNativePalette(const dusk::pica::UniformBinding& binding, dusk::pica::Palette& out) {
    out = {}; out.count = 0;
    if (!mModelData || !mMtxBuffer || mMtxBuffer->mCurrentViewNo >= mMtxBuffer->mMtxNum ||
        !mMtxBuffer->mpDrawMtxArr[1] || !mMtxBuffer->mpNrmMtxArr[1] ||
        mModelData->checkBBoardFlag() || mModelData->checkFlag(0x110) ||
        (getMtxCalcMode() != 0 && getMtxCalcMode() != J3DMdlFlag_UseDefaultJ3D) ||
        !getDrawMtxPtr() || !getNrmMtxPtr() ||
        mDeformData || mSkinDeform || mVtxColorCalc || mUnkCalc1 || mUnkCalc2 ||
        checkFlag(J3DMdlFlag_SkinPosCpu | J3DMdlFlag_SkinNrmCpu)) return false;
    return dusk::pica::copyGamePalette(binding, mModelData->getDrawMtxNum(),
                                     getDrawMtxPtr(), getNrmMtxPtr(), out);
}
namespace {
bool nativeGameParameters(const void* instance, const dusk::pica::UniformBinding& binding,
                          dusk::pica::DrawUniforms& out, void*) noexcept {
    auto* model = const_cast<J3DModel*>(static_cast<const J3DModel*>(instance));
    if (!model || !model->copyNativePalette(binding, out.palette)) return false;
    return dusk::pica::game::applyCamera(out);
}
}
bool J3DModel::drawNativeShape(unsigned shape) {
    const dusk::pica::SourceIdentity empty;
    return dusk::pica::game::drawSourceShape(this, shape,
        mModelData ? mModelData->getNativeSource() : empty,
        nativeGameParameters, nullptr, dusk::pica::poseInput | dusk::pica::orderingInput);
}
#endif

void J3DModel::initialize() {
    mModelData = NULL;
    mFlags = 0;
    mDiffFlag = 0;
    mCalcCallBack = NULL;
    mUserArea = 0;
#if defined(DUSK_TARGET_3DS)
    mNativeShadowSlot = 0;
    mNativeEnvelopePending = 0;
    mNativeOwner = 0;
    mNativeShadowUnitPixels = 0.0f;
#endif

    mBaseScale.x = 1.0f;
    mBaseScale.y = 1.0f;
    mBaseScale.z = 1.0f;

    MTXIdentity(mBaseTransformMtx);
    MTXIdentity(mInternalView);
    IF_DUSK(MTXIdentity(mPresentationBase));

    mMtxBuffer = NULL;
    mMatPacket = NULL;
    mShapePacket = NULL;
    mDeformData = NULL;
    mSkinDeform = NULL;
    mVtxColorCalc = NULL;
    mUnkCalc1 = NULL;
    mUnkCalc2 = NULL;
}

s32 J3DModel::entryModelData(J3DModelData* pModelData, u32 mdlFlags, u32 mtxNum) {
    J3D_ASSERTMSG(83, pModelData != NULL, "Error : null pointer.");
    J3D_ASSERTMSG(84, mtxNum != 0, "Error : non-zero argument is specified 0.");
    int ret = kJ3DError_Success;

    mModelData = pModelData;
    mMtxBuffer = JKR_NEW J3DMtxBuffer();

    if (mMtxBuffer == NULL) {
        return kJ3DError_Alloc;
    }

    ret = mMtxBuffer->create(pModelData, mtxNum);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    ret = createShapePacket(pModelData);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    ret = createMatPacket(pModelData, mdlFlags);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    mVertexBuffer.setVertexData(&pModelData->getVertexData());
    prepareShapePackets();

    if (mdlFlags & J3DMdlFlag_Unk1) {
        onFlag(J3DMdlFlag_Unk1);
    }
    if (mdlFlags & J3DMdlFlag_UseDefaultJ3D) {
        onFlag(J3DMdlFlag_UseDefaultJ3D);
    }
    if (mdlFlags & J3DMdlFlag_EnableLOD) {
        onFlag(J3DMdlFlag_EnableLOD);
    }
    if (mdlFlags & J3DMdlFlag_UseSharedDL) {
        onFlag(J3DMdlFlag_UseSharedDL);
    }
    if (mdlFlags & J3DMdlFlag_UseSingleDL) {
        onFlag(J3DMdlFlag_UseSingleDL);
    }
    if (mdlFlags & J3DMdlFlag_DifferedDLBuffer) {
        onFlag(J3DMdlFlag_DifferedDLBuffer);
    }
    if (mdlFlags & J3DMdlFlag_SkinPosCpu) {
        J3D_WARN1(149, "%s", "Don't forget to call J3DModel::setSkinDeform\n if you set J3DMdlFlag_SkinPosCpu.");
        onFlag(J3DMdlFlag_SkinPosCpu);
    }
    if (mdlFlags & J3DMdlFlag_SkinNrmCpu) {
        J3D_WARN1(154, "%s", "Don't forget to call J3DModel::setSkinDeform\n if you set J3DMdlFlag_SkinNrmCpu.");
        onFlag(J3DMdlFlag_SkinNrmCpu);
    }

    return kJ3DError_Success;
}

#if TARGET_PC
void J3DModel::calc_presentation_base_mtx() {
    Mtx identity;
    MTXIdentity(identity);
    J3DCalcViewBaseMtx(identity, mBaseScale, mBaseTransformMtx, mPresentationBase);
    dusk::interp::record_final_mtx(mPresentationBase);
    prepare_presentation_view();
}

void J3DModel::prepare_presentation_view() {
    Mtx replacement;
    MtxP presentationBase = mPresentationBase;
    if (dusk::interp::lookup_replacement(mPresentationBase, replacement)) {
        presentationBase = replacement;
    }
    MTXConcat(j3dSys.getViewMtx(), presentationBase, mInternalView);
}

void J3DModel::setAnmMtx(int jointNo, Mtx m) {
    mMtxBuffer->setAnmMtx(jointNo, m);
    dusk::interp::record_final_mtx(mMtxBuffer->getAnmMtx(jointNo));
}

void J3DModel::forgetMtx() {
    dusk::interp::vertex::reset(&mVertexBuffer);
    dusk::interp::forget_mtx(mPresentationBase);
    for (u16 i = 0; i < mModelData->getJointNum(); ++i) {
        dusk::interp::forget_mtx(getAnmMtx(i));
    }
    for (u16 i = 0; i < mModelData->getWEvlpMtxNum(); ++i) {
        dusk::interp::forget_mtx(getWeightAnmMtx(i));
    }
}
#endif

s32 J3DModel::createShapePacket(J3DModelData* pModelData) {
    J3D_ASSERTMSG(173, pModelData != NULL, "Error : null pointer.");

    if (pModelData->getShapeNum() != 0) {
        mShapePacket = JKR_NEW_ARRAY(J3DShapePacket, pModelData->getShapeNum());

        if (mShapePacket == NULL) {
            return kJ3DError_Alloc;
        }

        for (int i = 0; i < pModelData->getShapeNum(); i++) {
            mShapePacket[i].setShape(pModelData->getShapeNodePointer(i));
            mShapePacket[i].setModel(this);
        }
    }

    return kJ3DError_Success;
}

s32 J3DModel::createMatPacket(J3DModelData* pModelData, u32 mdlFlags) {
    J3D_ASSERTMSG(207, pModelData != NULL, "Error : null pointer.");
    s32 ret = 0;

    if (pModelData->getMaterialNum() != 0) {
        mMatPacket = JKR_NEW_ARRAY(J3DMatPacket, pModelData->getMaterialNum());

        if (mMatPacket == NULL) {
            return kJ3DError_Alloc;
        }
    }
    
    u16 matNum = pModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* materialNode = pModelData->getMaterialNodePointer(i);
        J3DMatPacket* matPacket = mMatPacket + i;
        J3DShapePacket* shapePacket = mShapePacket + materialNode->getShape()->getIndex();

        matPacket->setMaterial(materialNode);
        matPacket->setInitShapePacket(shapePacket);
        matPacket->addShapePacket(shapePacket);
        matPacket->setTexture(pModelData->getTexture());
        matPacket->setMaterialID(materialNode->mMaterialID);

        if (pModelData->getModelDataType() == 1) {
            matPacket->lock();
        }

        if (mdlFlags & J3DMdlFlag_DifferedDLBuffer) {
            J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
            J3D_ASSERTMSG(253, dlobj != NULL, "Error : User need to call J3DModelData::newSharedDisplayList()  before using DifferedDLBuffer.");
            matPacket->setDisplayListObj(dlobj);
        } else {
            if (pModelData->getModelDataType() == 1) {
                if (mdlFlags & J3DMdlFlag_UseSingleDL) {
                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    matPacket->setDisplayListObj(dlobj);
                } else {
                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    ret = dlobj->single_To_Double();
                    if (ret != kJ3DError_Success)
                        return ret;

                    matPacket->setDisplayListObj(dlobj);
                }
            } else if (mdlFlags & J3DMdlFlag_UseSharedDL) {
                if (mdlFlags & J3DMdlFlag_UseSingleDL) {
                    ret = materialNode->newSingleSharedDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;

                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    matPacket->setDisplayListObj(dlobj);
                } else {
                    ret = materialNode->newSharedDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;

                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    ret = dlobj->single_To_Double();
                    if (ret != kJ3DError_Success)
                        return ret;

                    matPacket->setDisplayListObj(dlobj);
                }
            } else {
                if (mdlFlags & J3DMdlFlag_UseSingleDL) {
                    ret = matPacket->newSingleDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;
                } else {
                    ret = matPacket->newDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;
                }
            }
        }
    }

    return kJ3DError_Success;
}

s32 J3DModel::newDifferedDisplayList(u32 diffFlags) {
    mDiffFlag = diffFlags;

    u16 shapeNum = getModelData()->getShapeNum();
    for (u16 i = 0; i < shapeNum; i++) {
        s32 ret = getShapePacket(i)->newDifferedDisplayList(diffFlags);
        if (ret != kJ3DError_Success)
            return ret;
    }

    return kJ3DError_Success;
}

void J3DModel::ptrToIndex() {
    j3dSys.setModel(this);
    j3dSys.setTexture(mModelData->getTexture());

    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMatPacket* matPacket = getMatPacket(i);

        GDLObj dlobj;
        GDInitGDLObj(&dlobj, matPacket->getDisplayList(0), matPacket->getDisplayListSize());
        GDSetCurrent(&dlobj);

        mModelData->getMaterialNodePointer(i)->getTevBlock()->ptrToIndex();

        GDSetCurrent(NULL);
    }
}

void J3DModel::lock() {
    int matNum = mModelData->getMaterialNum();
    for (int i = 0; i < matNum; i++) {
        mMatPacket[i].lock();
    }
}

void J3DModel::unlock() {
    int matNum = mModelData->getMaterialNum();
    for (int i = 0; i < matNum; i++) {
        mMatPacket[i].unlock();
    }
}

void J3DModel::makeDL() {
    j3dSys.setModel(this);
    j3dSys.setTexture(mModelData->getTexture());

    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);
        mModelData->getMaterialNodePointer(i)->makeDisplayList();
    }
}

void J3DModel::calcMaterial() {
    J3D_TICK_PROFILE(tick_profile, "j3d.calcMaterial");
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        j3dSys.onFlag(4);
    } else {
        j3dSys.offFlag(4);
    }

    if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        j3dSys.onFlag(8);
    } else {
        j3dSys.offFlag(8);
    }

    mModelData->syncJ3DSysFlags();
    j3dSys.setTexture(mModelData->getTexture());

    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);

        J3DMaterial* material = mModelData->getMaterialNodePointer(i);
        if (material->getMaterialAnm() != NULL) {
            material->getMaterialAnm()->calc(material);
        }

        MtxP jointMtx = getAnmMtx(material->getJoint()->getJntNo());
#if TARGET_PC
        Mtx presentedJoint;
        if (dusk::interp::is_presentation_active() &&
            dusk::interp::lookup_replacement(jointMtx, presentedJoint))
        {
            jointMtx = presentedJoint;
        }
#endif
        material->calc(jointMtx);
    }
}

void J3DModel::calcDiffTexMtx() {
    j3dSys.setModel(this);

    u16 matNum =  mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);
        J3DMaterial* materialNode = mModelData->getMaterialNodePointer(i);
        materialNode->calcDiffTexMtx(getAnmMtx(materialNode->getJoint()->getJntNo()));
    }

    u16 shapeNum = getModelData()->getShapeNum();
    for (u16 i = 0; i < shapeNum; i++) {
        J3DShapePacket* shapePacket = getShapePacket(i);
        J3DTexGenBlock* texGenBlock = mModelData->getShapeNodePointer(i)->getMaterial()->getTexGenBlock();

        for (u16 j = 0; (int)j < 8; j++) {
            J3DTexMtx* texMtxNode = texGenBlock->getTexMtx(j);
            J3DTexMtxObj* texMtxObj = shapePacket->getTexMtxObj();
            if (texMtxNode != NULL && texMtxObj != NULL) {
                texMtxObj->setMtx(j, texMtxNode->getMtx());
            }
        }
    }
}

void J3DModel::diff() {
    J3D_TICK_PROFILE(tick_profile, "j3d.diff");
    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);
        mModelData->getMaterialNodePointer(i)->diff(mDiffFlag);
    }
}

s32 J3DModel::setDeformData(J3DDeformData* p_deformData, u32 param_1) {
    mDeformData = p_deformData;

    if (p_deformData == NULL) {
        return kJ3DError_Success;
    }

    s32 ret = mVertexBuffer.copyLocalVtxArray(param_1);
    if (ret) {
        return ret;
    }

    if (param_1 & 4) {
        p_deformData->offAllFlag(1);
    }

    return kJ3DError_Success;
}

s32 J3DModel::setSkinDeform(J3DSkinDeform* p_skinDeform, u32 flags) {
    mSkinDeform = p_skinDeform;

    if (p_skinDeform == NULL) {
        offFlag(J3DMdlFlag_SkinPosCpu);
        offFlag(J3DMdlFlag_SkinNrmCpu);
        return 5;
    }

    mSkinDeform->initMtxIndexArray(mModelData);

    if (mModelData->checkFlag(0x100)) {
        mSkinDeform->changeFastSkinDL(mModelData);
        flags &= ~2;
        flags &= ~4;
        mSkinDeform->transformVtxPosNrm(mModelData);
        mSkinDeform->initSkinInfo(mModelData);
    }

    s32 ret = kJ3DError_Success;
    if ((~flags & 2)) {
        ret = mVertexBuffer.allocTransformedVtxPosArray();
        if (ret != kJ3DError_Success) {
            offFlag(J3DMdlFlag_SkinPosCpu);
            return ret;
        }
        onFlag(J3DMdlFlag_SkinPosCpu);
    } else {
        offFlag(J3DMdlFlag_SkinPosCpu);
    }

    if ((~flags & 4)) {
        ret = mVertexBuffer.allocTransformedVtxNrmArray();
        if (ret != kJ3DError_Success) {
            offFlag(J3DMdlFlag_SkinNrmCpu);
            return ret;
        }
        onFlag(J3DMdlFlag_SkinNrmCpu);
    } else {
        offFlag(J3DMdlFlag_SkinNrmCpu);
    }

    return ret;
}

void J3DModel::calcAnmMtx() {
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_UseDefaultJ3D)) {
        getModelData()->getJointTree().calc(mMtxBuffer, j3dDefaultScale, j3dDefaultMtx);
    } else {
        getModelData()->getJointTree().calc(mMtxBuffer, mBaseScale, mBaseTransformMtx);
    }
}

#if defined(DUSK_TARGET_3DS)
bool J3DModel::sNativeDeferEnvelope;
u32 J3DModel::sNativeEnvelopeCount[3];
#endif

void J3DModel::calcWeightEnvelopeMtx() {
#if defined(DUSK_TARGET_3DS)
    mNativeEnvelopePending = 0;
#endif
    if (getModelData()->getWEvlpMtxNum() != 0 && !checkFlag(J3DMdlFlag_EnableLOD)) {
        if (!getModelData()->checkFlag(0x100)) {
            mMtxBuffer->calcWeightEnvelopeMtx();
#if defined(DUSK_TARGET_3DS)
            sNativeEnvelopeCount[0]++;
#endif
        }
    }
}

void J3DModel::update() {
    calc();
    entry();
}

void J3DModel::calc() {
    J3D_TICK_PROFILE(tick_profile, "j3d.calc");
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinPosCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
    }

    if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinNrmCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
    }

    mModelData->syncJ3DSysFlags();
#if defined(DUSK_TARGET_3DS)
    // The second-core worker may still draw this model's last picture from the
    // arrays rewritten below (CPU skinning, deformers, vertex colours; their
    // double buffers cover one calc, not two in one picture): it draws them
    // first (4, only while it still draws the last picture).
    if (mDeformData || mSkinDeform || mVtxColorCalc || mUnkCalc1 || mUnkCalc2)
        dusk::native::syncGxOverlap(4);
#endif
    mVertexBuffer.frameInit();

    if (mUnkCalc2 != NULL) {
        mUnkCalc2->calc(mModelData);
    }

    if (mDeformData != NULL) {
        mDeformData->deform(this);
    }

    if (mVtxColorCalc != NULL) {
        mVtxColorCalc->calc(this);
    }

    if (mUnkCalc1 != NULL) {
        mUnkCalc1->calc(this);
    }

    {
        J3D_TICK_PROFILE(anm_profile, "j3d.calc.anmMtx");
        calcAnmMtx();
    }
    {
        J3D_TICK_PROFILE(envelope_profile, "j3d.calc.envelope");
#if defined(DUSK_TARGET_3DS)
        // A logic tick whose picture is not drawn: nothing reads the skin
        // matrices before the next calc(), which makes them again. A model
        // calculated on such a tick and drawn without a calc on the drawing
        // tick gets them from viewCalc() instead, from its joint matrices as
        // they are then. Made here as before for models whose calc reads them
        // (CPU skinning, deformers, a calc callback).
        if (sNativeDeferEnvelope && mSkinDeform == NULL && mUnkCalc1 == NULL && mUnkCalc2 == NULL &&
            mCalcCallBack == NULL && getModelData()->getWEvlpMtxNum() != 0)
        {
            mNativeEnvelopePending = 1;
            sNativeEnvelopeCount[1]++;
        } else
#endif
        calcWeightEnvelopeMtx();
    }

    if (mSkinDeform != NULL) {
        mSkinDeform->deform(this);
    }

    if (mCalcCallBack != NULL) {
        mCalcCallBack(this, 0);
    }

#if TARGET_PC
    dusk::interp::vertex::capture(&mVertexBuffer, mDeformData);

    for (u16 i = 0; i < mModelData->getJointNum(); ++i) {
        dusk::interp::record_final_mtx(getAnmMtx(i));
    }

    for (u16 i = 0; i < mModelData->getWEvlpMtxNum(); ++i) {
        dusk::interp::record_final_mtx(getWeightAnmMtx(i));
    }
#endif
}

void J3DModel::entry() {
    DUSK_TICK_PROFILE(tick_profile, "j3d.entry");
    j3dSys.setModel(this);
#if defined(DUSK_TARGET_3DS)
    mNativeOwner = dusk::native::gxEntryOwner;
#endif

    if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinPosCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
    }

    if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinNrmCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
    }

    mModelData->syncJ3DSysFlags();
    j3dSys.setTexture(mModelData->getTexture());

    for (u16 i = 0; i < mModelData->getJointNum(); i++) {
        J3DJoint* joint = mModelData->getJointNodePointer(i);
        if (joint->getMesh() != NULL) {
            joint->entryIn();
        }
    }

#if TARGET_PC
    if (mModelData->needsInterpCallBack()) {
        dusk::interp::material::record_model(this);
    }
#endif
}

void J3DModel::viewCalc() {
    J3D_TICK_PROFILE(tick_profile, "j3d.viewCalc");
#if defined(DUSK_TARGET_3DS)
    if (mNativeEnvelopePending) {
        sNativeEnvelopeCount[2]++;
        calcWeightEnvelopeMtx();
    }
#endif
    mMtxBuffer->swapDrawMtx();
    mMtxBuffer->swapNrmMtx();

    if (getModelData()->checkFlag(0x10)) {
        if (getMtxCalcMode() == 2) {
#if TARGET_PC
            calc_presentation_base_mtx();
#else
            J3DCalcViewBaseMtx(j3dSys.getViewMtx(), mBaseScale, mBaseTransformMtx,
                               (MtxP)&mInternalView);
#endif
        }
    } else if (isCpuSkinningOn()) {
        if (getMtxCalcMode() == 2) {
#if TARGET_PC
            calc_presentation_base_mtx();
#else
            J3DCalcViewBaseMtx(j3dSys.getViewMtx(), mBaseScale, mBaseTransformMtx,
                               (MtxP)&mInternalView);
#endif
        }
    } else if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        mMtxBuffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
        calcNrmMtx();
        calcBumpMtx();
        DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
        DCStoreRange(getNrmMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx33));
    } else if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        mMtxBuffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
        calcBBoardMtx();
        DCStoreRange(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
    } else {
        {
            J3D_TICK_PROFILE(draw_profile, "j3d.viewCalc.drawMtx");
            mMtxBuffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
        }
        {
            J3D_TICK_PROFILE(nrm_profile, "j3d.viewCalc.nrmMtx");
            calcNrmMtx();
        }
        calcBBoardMtx();
        calcBumpMtx();
        DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
        DCStoreRange(getNrmMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx33));
    }

    prepareShapePackets();
}

void J3DModel::calcNrmMtx() {
    mMtxBuffer->calcNrmMtx();
}

void J3DModel::calcBumpMtx() {
    if (getModelData()->checkBumpFlag()) {
        u32 bumpMtxIdx = 0;
        u16 materialNum = getModelData()->getMaterialNum();

        for (u16 i = 0; i < materialNum; i++) {
            J3DMaterial* material = getModelData()->getMaterialNodePointer(i);
            if (material->getNBTScale()->mbHasScale == TRUE) {
                material->getShape()->calcNBTScale(*material->getNBTScale()->getScale(),
                                                   getNrmMtxPtr(), getBumpMtxPtr(bumpMtxIdx));
                DCStoreRange(getBumpMtxPtr(bumpMtxIdx), mModelData->getDrawMtxNum() * sizeof(Mtx33));
                bumpMtxIdx++;
            }
        }
    }
}

void J3DModel::calcBBoardMtx() {
    if (getModelData()->checkBBoardFlag()) {
        mMtxBuffer->calcBBoardMtx();
    }
}

void J3DModel::prepareShapePackets() {
    u16 shapeNum = mModelData->getShapeNum();

    for (u16 i = 0; i < shapeNum; i++) {
        J3DShape* shapeNode = mModelData->getShapeNodePointer(i);
        J3DShapePacket* shapePacket = &mShapePacket[i];
        shapePacket->setMtxBuffer(mMtxBuffer);

        if (getMtxCalcMode() == 2) {
            shapePacket->setBaseMtxPtr(&mInternalView);
        } else {
            shapePacket->setBaseMtxPtr((Mtx*)j3dSys.getViewMtx());
        }
    }
}
