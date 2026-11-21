#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/JKernel/JKRHeap.h"
#include <os.h>
#include <cstring>
#include "global.h"
#if defined(DUSK_TARGET_3DS)
#include "../../../../platforms/3ds/runtime/gx_profile.hpp"
#include "../../../../platforms/3ds/runtime/model_lod.hpp"
#include "../../../../platforms/3ds/runtime/perf_features.hpp"
#include "../../../../include/d/d_native_lod.h"
#include "JSystem/J3DGraphBase/J3DNativeInstances.h"
#define NATIVE_J3D_PROFILE(name) static const auto nativeProfileIndex_##__LINE__=dusk::native::registerGxNamedProfile(name);\
    dusk::native::GxNamedProfileScope nativeProfileScope(nativeProfileIndex_##__LINE__)
// Native model shadows: PICA draws of a tagged model are remembered so the
// caster pass can replay them (platforms/3ds/runtime/shadow_casters.hpp).
extern "C" void duskNativeShadowCaster(unsigned slot);
// Native packets (platforms/3ds/runtime/native_packets.hpp): the register
// work of a material packet is captured instead of run here; the original
// code below runs unchanged in between.
extern "C" bool duskNativePacketBegin(int kind);
extern "C" void duskNativePacketEnd(void);
extern "C" bool duskNativeDirectEnabled(void);
extern "C" bool duskNativeDirectVerifying(void);
extern "C" bool duskNativeDirectBegin(int kind);
extern "C" bool duskNativeDirectOpen(void);
extern "C" bool duskNativeDirectList(const void* list, u32 size);
extern "C" void duskNativeDirectEnd(u32 stateHash);
extern "C" void duskNativeDirectAbort(const char* why);
extern "C" void duskNativeDirectFallback(const char* why);
extern "C" void duskNativeDirectRealState(u32 stateHash);
#else
#define NATIVE_J3D_PROFILE(name)
#endif

#if defined(DUSK_PICA_GAME_DRAW)
#include "../../../../platforms/3ds/native/game_draw_api.hpp"
// Both sync the second-core GX worker first (gx_command_draw.cpp): the
// worker draws with the PICA state caches the end call resets.
extern "C" bool duskNativeGameDrawBegin(void** target);
extern "C" void duskNativeGameDrawEnd(void);
namespace {
void drawNativeGameShape(const J3DShapePacket& packet){
    if(packet.checkFlag(J3DShpFlag_Hidden)||!packet.getShape())return;
    void* framebuffer=nullptr;
    if(!duskNativeGameDrawBegin(&framebuffer))OSPanic(__FILE__,__LINE__,"Unsafe native game draw boundary");
    j3dSys.setModel(packet.getModel()); // Preserve game-side current-model identity.
    const bool target=dusk::pica::game::target(framebuffer);
    const bool drawn=target&&packet.getModel()&&packet.getModel()->drawNativeShape(packet.getShape()->getIndex());
    // Transitional UI/effects still share the SDK context. Their cached PICA
    // state must not survive a native draw (or a failed native state change).
    duskNativeGameDrawEnd();
    if(!drawn)OSPanic(__FILE__,__LINE__,"%s (submitted parts=%u)",dusk::pica::game::failure(),dusk::pica::game::submittedParts());
}
}
#endif

#if TARGET_PC
#if !defined(DUSK_TARGET_3DS)
#include <tracy/Tracy.hpp>
#endif
#endif

#if defined(DUSK_TARGET_3DS) && !defined(DUSK_PICA_GAME_DRAW)
// Native packets: 1 a room model's packet, 2 another rigid packet, 0 one the
// capture would end at once (runtime/native_packets.hpp begin): a model whose
// vertex arrays the CPU rewrites (J3DShape::loadVtxArray then binds arrays
// through an Aurora command), or a shape without matrices (its display list
// draws directly).
static int nativePacketKind(const J3DMatPacket& packet) {
    if (packet.mNativeFlags & J3DDrawPacket::NATIVE_ROOM_MODEL)
        return 1;
    const J3DShapePacket* shapePacket = packet.getShapePacket();
    J3DModel* model = shapePacket != NULL ? shapePacket->getModel() : NULL;
    if (model == NULL || model->checkFlag(J3DMdlFlag_SkinPosCpu | J3DMdlFlag_SkinNrmCpu))
        return 0;
    // A shape that carries an instance batch draws it on the original path
    // (J3DShapePacket::drawFast); a capture or direct record would draw only
    // the carrier.
    for (const J3DShapePacket* carrier = shapePacket; carrier != NULL && carrier->getModel() != NULL;
         carrier = static_cast<const J3DShapePacket*>(carrier->getNextPacket())) {
        if (J3DNativeInstancesOf(carrier->getModel()) != NULL)
            return 0;
    }
    J3DVertexBuffer* buffer = model->getVertexBuffer();
    J3DVertexData* data = buffer->getVertexData();
    if (data == NULL || buffer->getCurrentVtxPos() != data->getVtxPosArray() ||
        buffer->getCurrentVtxNrm() != data->getVtxNrmArray())
        return 0;
    for (unsigned n = 0; shapePacket != NULL && n < 1024; ++n) {
        const J3DShape* shape = shapePacket->getShape();
        if (shape != NULL && shape->checkFlag(J3DShpFlag_NoMtx))
            return 0;
        shapePacket = static_cast<const J3DShapePacket*>(shapePacket->getNextPacket());
    }
    return 2;
}
#endif

#if defined(DUSK_TARGET_3DS) && !defined(DUSK_PICA_GAME_DRAW)
// The J3D state a material packet's draw leaves for what follows: j3dSys and
// the static draw state of J3DShape, J3DShapeMtx and J3DDifferedTexMtx. A
// direct record must leave the same (verify mode compares the hashes).
namespace {
struct NativeJ3DState {
    alignas(8) unsigned char sys[sizeof(J3DSys)];
    void* oldVcdVat;
    bool envelope, nbt, lod;
    u32 pipeline, texMtxLoadType;
    u8* scaleFlag;
    u16 mtxCache[10];
    J3DTexGenBlock* texGenBlock;
    J3DTexMtxObj* texMtxObj;
    MtxP concatTable[2];
    NativeJ3DState& save() {
        memset(static_cast<void*>(this), 0, sizeof(*this)); // padding too: the hash reads every byte
        memcpy(sys, static_cast<const void*>(&j3dSys), sizeof(J3DSys));
        oldVcdVat = J3DShape::sOldVcdVatCmd;
        envelope = J3DShape::sEnvelopeFlag;
        nbt = J3DShapeMtx::sNBTFlag;
        lod = J3DShapeMtx::sLODFlag;
        pipeline = J3DShapeMtx::sCurrentPipeline;
        texMtxLoadType = J3DShapeMtx::sTexMtxLoadType;
        scaleFlag = J3DShapeMtx::sCurrentScaleFlag;
        memcpy(mtxCache, J3DShapeMtx::sMtxLoadCache, sizeof(mtxCache));
        texGenBlock = J3DDifferedTexMtx::sTexGenBlock;
        texMtxObj = J3DDifferedTexMtx::sTexMtxObj;
        concatTable[0] = J3DShapeMtxConcatView::sMtxPtrTbl[0];
        concatTable[1] = J3DShapeMtxConcatView::sMtxPtrTbl[1];
        return *this;
    }
    void restore() const {
        memcpy(static_cast<void*>(&j3dSys), sys, sizeof(J3DSys));
        J3DShape::sOldVcdVatCmd = oldVcdVat;
        J3DShape::sEnvelopeFlag = envelope;
        J3DShapeMtx::sNBTFlag = nbt;
        J3DShapeMtx::sLODFlag = lod;
        J3DShapeMtx::sCurrentPipeline = pipeline;
        J3DShapeMtx::sTexMtxLoadType = texMtxLoadType;
        J3DShapeMtx::sCurrentScaleFlag = scaleFlag;
        memcpy(J3DShapeMtx::sMtxLoadCache, mtxCache, sizeof(mtxCache));
        J3DDifferedTexMtx::sTexGenBlock = texGenBlock;
        J3DDifferedTexMtx::sTexMtxObj = texMtxObj;
        J3DShapeMtxConcatView::sMtxPtrTbl[0] = concatTable[0];
        J3DShapeMtxConcatView::sMtxPtrTbl[1] = concatTable[1];
    }
    u32 hash() const {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(this);
        u32 h = 2166136261u;
        for (size_t i = 0; i < sizeof(*this); i++)
            h = (h ^ bytes[i]) * 16777619u;
        return h;
    }
};
}
#endif

J3DError J3DDisplayListObj::newDisplayList(u32 maxSize) {
    mMaxSize = ALIGN_NEXT(maxSize, 0x20);
    mpDisplayList[0] = JKR_NEW_ARRAY_ARGS(char, mMaxSize, 0x20);
    mpDisplayList[1] = JKR_NEW_ARRAY_ARGS(char, mMaxSize, 0x20);
    mSize = 0;

    if (mpDisplayList[0] == NULL || mpDisplayList[1] == NULL)
        return kJ3DError_Alloc;

    return kJ3DError_Success;
}

J3DError J3DDisplayListObj::newSingleDisplayList(u32 maxSize) {
    mMaxSize = ALIGN_NEXT(maxSize, 0x20);
    mpDisplayList[0] = JKR_NEW_ARRAY_ARGS(char, mMaxSize, 0x20);
    mpDisplayList[1] = mpDisplayList[0];
    mSize = 0;

    if (mpDisplayList[0] == NULL)
        return kJ3DError_Alloc;

    return kJ3DError_Success;
}

int J3DDisplayListObj::single_To_Double() {
    if (mpDisplayList[0] == mpDisplayList[1]) {
        mpDisplayList[1] = JKR_NEW_ARRAY_ARGS(char, mMaxSize, 0x20);

        if (mpDisplayList[1] == NULL)
            return kJ3DError_Alloc;

        memcpy(mpDisplayList[1], mpDisplayList[0], mMaxSize);
        DCStoreRange(mpDisplayList[1], mMaxSize);
    }

    return kJ3DError_Success;
}

void J3DDisplayListObj::setSingleDisplayList(void* pDLData, u32 size) {
    J3D_ASSERT_NULLPTR(148, pDLData != NULL);

    mMaxSize = ALIGN_NEXT(size, 0x20);
    mpDisplayList[0] = pDLData;
    mpDisplayList[1] = mpDisplayList[0];
    mSize = size;
}

void J3DDisplayListObj::swapBuffer() {
    void* pTmp = mpDisplayList[0];
    mpDisplayList[0] = mpDisplayList[1];
    mpDisplayList[1] = pTmp;
}

void J3DDisplayListObj::callDL() const {
#if defined(DUSK_PICA_GAME_DRAW)
    OSPanic(__FILE__,__LINE__,"J3D display-list execution is forbidden in the native model renderer");
#else
    GXCallDisplayList(mpDisplayList[0], mSize);

#endif
}

#if defined(DUSK_TARGET_3DS)
thread_local GDLObj J3DDisplayListObj::sGDLObj;
#else
DUSK_GAME_DATA GDLObj J3DDisplayListObj::sGDLObj;

DUSK_GAME_DATA s32 J3DDisplayListObj::sInterruptFlag;
#endif

void J3DDisplayListObj::beginDL() {
    swapBuffer();
#if !defined(DUSK_TARGET_3DS)
    sInterruptFlag = OSDisableInterrupts();
#endif
    GDInitGDLObj(&sGDLObj, (u8*)mpDisplayList[0], mMaxSize);
    GDSetCurrent(&sGDLObj);
}

u32 J3DDisplayListObj::endDL() {
    GDPadCurr32();
#if !defined(DUSK_TARGET_3DS)
    OSRestoreInterrupts(sInterruptFlag);
#endif
    mSize = GDGetGDLObjOffset(&sGDLObj);
    GDFlushCurrToMem();
    GDSetCurrent(NULL);
    return mSize;
}

void J3DDisplayListObj::beginPatch() {
    beginDL();
}

u32 J3DDisplayListObj::endPatch() {
#if !defined(DUSK_TARGET_3DS)
    OSRestoreInterrupts(sInterruptFlag);
#endif
    GDSetCurrent(NULL);
    return mSize;
}

int J3DPacket::entry(J3DDrawBuffer* pBuffer) {
    J3D_ASSERT_NULLPTR(290, pBuffer != NULL);
    return 1;
}

void J3DPacket::addChildPacket(J3DPacket* pPacket) {
    J3D_ASSERT_NULLPTR(304, pPacket != NULL);

    if (mpFirstChild == NULL) {
        mpFirstChild = pPacket;
    } else {
        pPacket->setNextPacket(mpFirstChild);
        mpFirstChild = pPacket;
    }
}

static u32 sDifferedRegister[8] = {
    J3DDiffFlag_AmbColor,
    J3DDiffFlag_MatColor,
    J3DDiffFlag_ColorChan,
    J3DDiffFlag_TevReg,
    J3DDiffFlag_Fog,
    J3DDiffFlag_Blend,
    J3DDiffFlag_KonstColor,
    J3DDiffFlag_TevStageIndirect,
};

static s32 sSizeOfDiffered[8] = {
    13,
    13,
    21,
    120,
    55,
    15,
    19,
    45,
};

J3DDrawPacket::J3DDrawPacket() {
    mFlags = 0;
#if defined(DUSK_TARGET_3DS)
    mNativeFlags = 0;
#endif
    mpDisplayListObj = NULL;
    mpTexMtxObj = NULL;
}

J3DDrawPacket::~J3DDrawPacket() {}

J3DError J3DDrawPacket::newDisplayList(u32 size) {
    mpDisplayListObj = JKR_NEW J3DDisplayListObj();

    if (mpDisplayListObj == NULL)
        return kJ3DError_Alloc;

    J3DError ret = mpDisplayListObj->newDisplayList(size);
    if (ret != kJ3DError_Success)
        return ret;

    return kJ3DError_Success;
}

J3DError J3DDrawPacket::newSingleDisplayList(u32 size) {
    mpDisplayListObj = JKR_NEW J3DDisplayListObj();

    if (mpDisplayListObj == NULL)
        return kJ3DError_Alloc;

    J3DError ret = mpDisplayListObj->newSingleDisplayList(size);
    if (ret != kJ3DError_Success)
        return ret;

    return kJ3DError_Success;
}

void J3DDrawPacket::draw() {
#if defined(DUSK_PICA_GAME_DRAW)
    OSPanic(__FILE__,__LINE__,"Native custom J3D draw packet is not implemented");
#else
    callDL();

#endif
}

J3DMatPacket::J3DMatPacket() {
    mpInitShapePacket = NULL;
    mpShapePacket = NULL;
    mpMaterial = NULL;
    mMaterialID = 0xFFFFFFFF;
    mpTexture = NULL;
    mpMaterialAnm = NULL;
}

J3DMatPacket::~J3DMatPacket() {}

void J3DMatPacket::addShapePacket(J3DShapePacket* pShape) {
    if (mpShapePacket == NULL) {
        mpShapePacket = pShape;
    } else {
        pShape->setNextPacket(mpShapePacket);
        mpShapePacket = pShape;
    }
}

void J3DMatPacket::beginDiff() {
    mpInitShapePacket->beginDL();
}

void J3DMatPacket::endDiff() {
    mpInitShapePacket->endDL();
}

bool J3DMatPacket::isSame(J3DMatPacket* pOther) const {
    J3D_ASSERT_NULLPTR(521, pOther != NULL);
    return mMaterialID == pOther->mMaterialID && (mMaterialID & 0x80000000) == 0;
}

void J3DMatPacket::draw() {
#if defined(DUSK_PICA_GAME_DRAW)
    j3dSys.setTexture(mpTexture); // High-level game identity only, no GPU load.
    unsigned packets=0;
    for(auto* packet=getShapePacket();packet;packet=static_cast<J3DShapePacket*>(packet->getNextPacket())){
        if(++packets>1024)OSPanic(__FILE__,__LINE__,"Native model packet chain exceeds its bound");
        drawNativeGameShape(*packet);
    }
#else
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    NATIVE_J3D_PROFILE("j3d.matPacket");
#if defined(DUSK_TARGET_3DS)
    {
        // Calibration: what one named section adds to the section around it
        // (its own reading of the clock); the paint attribution subtracts it.
        NATIVE_J3D_PROFILE("timer.empty");
    }
    static const auto kindIndex = dusk::native::registerGxNamedProfile("j3d.packetKind");
    dusk::native::GxNamedProfileSpan kindSpan(kindIndex);
    const int nativeKind = nativePacketKind(*this);
    if (nativeKind != 0 && duskNativeDirectEnabled() && drawNativeDirect(nativeKind))
        return;
    const bool nativePacket = duskNativePacketBegin(nativeKind);
    kindSpan.stop();
#endif
#if TARGET_PC 
    j3dSys.setTexture(mpTexture);
#endif
    {
        NATIVE_J3D_PROFILE("j3d.materialLoad");
        mpMaterial->load();
    }
    drawAfterLoad();
#if defined(DUSK_TARGET_3DS)
    if (nativePacket) {
        if (duskNativeDirectVerifying())
            duskNativeDirectRealState(NativeJ3DState().save().hash());
        duskNativePacketEnd();
    }
#endif
#endif
}

#if !defined(DUSK_PICA_GAME_DRAW)
// The original code of J3DMatPacket::draw after the material load.
void J3DMatPacket::drawAfterLoad() {
#if DEBUG && TARGET_PC
    if (mpMaterial->mMaterialName != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Mat: %s", mpMaterial->mMaterialName);
        GXPushDebugGroup(buf);
    }
#endif

    {
        NATIVE_J3D_PROFILE("j3d.materialDL");
        callDL();
    }

    J3DShapePacket* packet = getShapePacket();
#if TARGET_PC
    packet->mpModel->getVertexBuffer()->setArray();
#endif
    {
        NATIVE_J3D_PROFILE("j3d.preDraw");
        packet->getShape()->loadPreDrawSetting();
    }

    while (packet != NULL) {
        if (packet->getDisplayListObj() != NULL) {
            NATIVE_J3D_PROFILE("j3d.shapeDL");
            packet->getDisplayListObj()->callDL();
        }

        {
            NATIVE_J3D_PROFILE("j3d.shapeDraw");
            packet->drawFast();
        }
        packet = (J3DShapePacket*)packet->getNextPacket();
    }

    J3DShape::resetVcdVatCache();

#if DEBUG && TARGET_PC
    if (mpMaterial->mMaterialName != nullptr) {
        GXPopDebugGroup();
    }
#endif
}
#endif

#if defined(DUSK_TARGET_3DS) && !defined(DUSK_PICA_GAME_DRAW)
// Direct record (runtime/native_packets.hpp): the record operations this
// packet's original code makes, entered from the J3D data without GX calls
// (the material's texture loads still go through GX, captured). True: the
// packet is done. False: the caller runs the original code (an operation
// could not be entered: nothing of it ran; or verify mode: the record is
// kept, the J3D state put back, and the original code is captured and
// compared with it).
bool J3DMatPacket::drawNativeDirect(int kind) {
    if (mpDisplayListObj == NULL || getShapePacket() == NULL || getShapePacket()->getShape() == NULL) {
        duskNativeDirectFallback("direct: no list or shape");
        return false;
    }
    unsigned count = 0;
    for (const J3DShapePacket* packet = getShapePacket(); packet != NULL;
         packet = static_cast<const J3DShapePacket*>(packet->getNextPacket())) {
        if (++count > 1024) {
            duskNativeDirectFallback("direct: over 1024 shapes");
            return false;
        }
        if (packet->getShape() != NULL) {
            if (const char* why = packet->getShape()->nativeDirectEligible()) {
                duskNativeDirectFallback(why);
                return false;
            }
        }
    }
    NativeJ3DState before;
    before.save();
    if (!duskNativeDirectBegin(kind))
        return false;
    j3dSys.setTexture(mpTexture);
    mpMaterial->load();
    if (!duskNativeDirectOpen()) {
        // The material load ended the capture (a palette load) and ran on
        // the original path: the rest of the packet follows it there.
        duskNativeDirectFallback("direct: material load");
        drawAfterLoad();
        return true;
    }
    bool okay = duskNativeDirectList(mpDisplayListObj->mpDisplayList[0], mpDisplayListObj->mSize);
    J3DShapePacket* packet = getShapePacket();
    if (okay) {
        packet->mpModel->getVertexBuffer()->setArray();
        okay = packet->getShape()->nativeDirectPreDraw();
    }
    for (; okay && packet != NULL; packet = static_cast<J3DShapePacket*>(packet->getNextPacket())) {
        if (packet->getDisplayListObj() != NULL)
            okay = duskNativeDirectList(packet->getDisplayListObj()->mpDisplayList[0], packet->getDisplayListObj()->mSize);
        if (!okay)
            break;
        const int result = packet->drawNativeDirect();
        if (result == J3DShape::nativeRefused) {
            okay = false;
        } else if (result == J3DShape::nativeEnded) {
            // The capture ended inside this shape (what it held ran on the
            // original path, the shape was finished there): the rest of the
            // packet follows on the original path, as drawAfterLoad's loop.
            for (packet = static_cast<J3DShapePacket*>(packet->getNextPacket()); packet != NULL;
                 packet = static_cast<J3DShapePacket*>(packet->getNextPacket())) {
                if (packet->getDisplayListObj() != NULL)
                    packet->getDisplayListObj()->callDL();
                packet->drawFast();
            }
            J3DShape::resetVcdVatCache();
            duskNativeDirectFallback("direct: shape ended the record");
            duskNativePacketEnd();
            return true;
        }
    }
    J3DShape::resetVcdVatCache();
    if (!okay) {
        duskNativeDirectAbort("direct: operation refused");
        before.restore();
        return false;
    }
    if (!duskNativeDirectVerifying()) {
        duskNativeDirectEnd(0);
        return true;
    }
    duskNativeDirectEnd(NativeJ3DState().save().hash());
    before.restore();
    return false;
}

// drawFast as a direct record (J3DShape::nativeDirectDrawFast).
int J3DShapePacket::drawNativeDirect() {
    if (checkFlag(J3DShpFlag_Hidden) || mpShape == NULL)
        return J3DShape::nativeDirect;
    prepareDraw();
    if (mpTexMtxObj != NULL) {
        J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
        J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
    } else {
        J3DDifferedTexMtx::sTexGenBlock = NULL;
    }
    const unsigned casterSlot =
        mpModel != NULL && mpBaseMtxPtr == (Mtx*)j3dSys.getViewMtx() ? mpModel->mNativeShadowSlot : 0;
    if (casterSlot != 0)
        duskNativeShadowCaster(casterSlot);
    const J3DShape::NativeDirect result = mpShape->nativeDirectDrawFast();
    if (casterSlot != 0)
        duskNativeShadowCaster(0);
    return result;
}
#endif

J3DShapePacket::J3DShapePacket() {
    mpShape = NULL;
    mpMtxBuffer = NULL;
    mpBaseMtxPtr = NULL;
    mDiffFlag = 0;
    mpModel = NULL;
#if defined(DUSK_TARGET_3DS)
    mNativeLodShape = NULL;
    mNativeLodPhase = 0;
    mNativeLod = 0;
#endif
}

J3DShapePacket::~J3DShapePacket() {}

u32 J3DShapePacket::calcDifferedBufferSize(u32 diffFlags) {
    u32 bufferSize = 0;

    for (u32 i = 0; i < 8; i++) {
        if ((diffFlags & sDifferedRegister[i]) != 0) {
            bufferSize += sSizeOfDiffered[i];
        }
    }

    u32 lightObjNum = getDiffFlag_LightObjNum(diffFlags);
    bufferSize += lightObjNum * 0x48;

    u32 texGenNum = getDiffFlag_TexGenNum(diffFlags);
    if (texGenNum != 0) {
        u32 mat_texGenNum = mpShape->getMaterial()->getTexGenNum();
        u32 sp30 = texGenNum > mat_texGenNum ? texGenNum : mat_texGenNum;

        if (diffFlags & J3DDiffFlag_TexGen) {
            bufferSize += calcDifferedBufferSize_TexGenSize(sp30);
        } else {
            bufferSize += calcDifferedBufferSize_TexMtxSize(sp30);
        }
    }

    u32 texNoNum = getDiffFlag_TexNoNum(diffFlags);
    if (texNoNum != 0) {
        u8 sp9;
        if (mpShape->getMaterial()->getTevStageNum() > 8) {
            sp9 = 8;
        } else {
            sp9 = mpShape->getMaterial()->getTevStageNum();
        }

        u32 mat_texNoNum = sp9;
        u32 sp24 = texNoNum > mat_texNoNum ? texNoNum : mat_texNoNum;

        if (diffFlags & J3DDiffFlag_TexCoordScale) {
            bufferSize += calcDifferedBufferSize_TexNoAndTexCoordScaleSize(sp24);
        } else {
            bufferSize += calcDifferedBufferSize_TexNoSize(sp24);
        }
    }

    u32 tevStageNum = getDiffFlag_TevStageNum(diffFlags);
    if (tevStageNum != 0) {
        u8 sp8;
        if (mpShape->getMaterial()->getTevStageNum() > 8) {
            sp8 = 8;
        } else {
            sp8 = mpShape->getMaterial()->getTevStageNum();
        }

        u32 mat_tevStageNum = sp8;
        u32 sp18 = tevStageNum > mat_tevStageNum ? tevStageNum : mat_tevStageNum;

        bufferSize += calcDifferedBufferSize_TevStageSize(sp18);
        if (diffFlags & J3DDiffFlag_TevStageIndirect) {
            bufferSize += calcDifferedBufferSize_TevStageDirectSize(sp18);
        }
    }

    return OSRoundUp32B(bufferSize);
}

int J3DShapePacket::newDifferedDisplayList(u32 diffFlags) {
    mDiffFlag = diffFlags;

    u32 bufSize = calcDifferedBufferSize(diffFlags);
    int ret = newDisplayList(bufSize);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    J3DDisplayListObj* dlobj = getDisplayListObj();
    setDisplayListObj(dlobj);
    return kJ3DError_Success;
}

void J3DShapePacket::prepareDraw() const {
#if defined(DUSK_TARGET_3DS)
    // Measured dump: what this shape issues belongs to its model's owner.
    dusk::native::gxPaintOwner = mpModel->mNativeOwner;
#endif
    mpModel->getVertexBuffer()->setArray();
    j3dSys.setModel(mpModel);
#if TARGET_PC
    if (mpModel->getMtxCalcMode() == 2) {
        mpModel->prepare_presentation_view();
    }
#endif
    j3dSys.setShapePacket((J3DShapePacket*)this);

    J3DShapeMtx::setLODFlag(mpModel->checkFlag(J3DMdlFlag_EnableLOD) != 0);

    if (mpModel->checkFlag(J3DMdlFlag_SkinPosCpu)) {
        mpShape->onFlag(J3DShpFlag_SkinPosCpu);
    } else {
        mpShape->offFlag(J3DShpFlag_SkinPosCpu);
    }

    if (mpModel->checkFlag(J3DMdlFlag_SkinNrmCpu) &&
        mpShape->checkFlag(J3DShpFlag_EnableLod) == false)
    {
        mpShape->onFlag(J3DShpFlag_SkinNrmCpu);
    } else {
        mpShape->offFlag(J3DShpFlag_SkinNrmCpu);
    }

    mpShape->setCurrentViewNoPtr(mpMtxBuffer->getCurrentViewNoPtr());
    mpShape->setScaleFlagArray(mpMtxBuffer->getScaleFlagArray());
    mpShape->setDrawMtx(mpMtxBuffer->getDrawMtxPtrPtr());

    if (!mpShape->getNBTFlag()) {
        mpShape->setNrmMtx(mpMtxBuffer->getNrmMtxPtrPtr());
    } else {
        mpShape->setNrmMtx(mpMtxBuffer->mpBumpMtxArr[1][mpShape->getBumpMtxOffset()]);
    }

    mpModel->getModelData()->syncJ3DSysFlags();
}

#if defined(DUSK_TARGET_3DS) && !defined(DUSK_PICA_GAME_DRAW)
// Levels of detail of this draw (include/d/d_native_lod.h rules "levels" and
// "shadowlevels"): the scene level chosen at entry, and for a shadow caster
// the coarsest level whose error stays under the limit in the caster's tile
// of the shadow image. Only the scene's own view, only the packets entered
// in the latest draw phase.
static J3DShape::NativeLod nativeLodOf(const J3DShapePacket& packet, unsigned casterSlot) {
    namespace features = dusk::native::features;
    J3DShape::NativeLod lod = {NULL, 0, 0};
    if (packet.mNativeLodShape == NULL || packet.mNativeLodPhase != dNativeLod::drawPhase || packet.mpModel == NULL ||
        packet.getBaseMtxPtr() != (Mtx*)j3dSys.getViewMtx())
    {
        return lod;
    }
    const auto& shape = *static_cast<const dusk::native::model_lod::Shape*>(packet.mNativeLodShape);
    lod.shape = &shape;
    lod.scene = features::on(features::Id::modelLod) ? packet.mNativeLod : 0;
    if (casterSlot != 0 && features::on(features::Id::shadowLod)) {
        const Vec& scale = *packet.mpModel->getBaseScale();
        f32 s = std::fabs(scale.x);
        s = std::fabs(scale.y) > s ? std::fabs(scale.y) : s;
        s = std::fabs(scale.z) > s ? std::fabs(scale.z) : s;
        // The tile does not move with the camera: no hysteresis needed.
        lod.shadow = u8(dNativeLod::chooseLevel(shape.error.data(), shape.levels, packet.mpModel->mNativeShadowUnitPixels * s,
                                                dNativeLod::config().shadowLevelPixels, shape.levels));
        dusk::native::model_lod::Counters& counted = dusk::native::model_lod::counters();
        counted.shadowShapes++;
        counted.shadowReduced += lod.shadow > lod.scene;
    }
    return lod;
}
#endif

void J3DShapePacket::draw() {
#if defined(DUSK_PICA_GAME_DRAW)
    drawNativeGameShape(*this);
#else
    if (!checkFlag(J3DShpFlag_Hidden) && mpShape != NULL) {
        prepareDraw();

        if (mpTexMtxObj != NULL) {
            J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
            J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = NULL;
        }

        if (mpDisplayListObj != NULL) {
            mpDisplayListObj->callDL();
        }

#if defined(DUSK_TARGET_3DS)
        if (const J3DNativeInstances* instances = J3DNativeInstancesOf(mpModel)) {
            // J3DShape::draw for every model of the batch (J3DNativeInstances.h).
            J3DShape::resetVcdVatCache();
            mpShape->loadPreDrawSetting();
            J3DNativeInstancesDraw(mpShape, *this, *instances);
            return;
        }
        // Only the scene's own view counts: a mirror draws the same model
        // again through a reflected matrix, which is not the caster.
        const unsigned casterSlot =
            mpModel != NULL && mpBaseMtxPtr == (Mtx*)j3dSys.getViewMtx() ?
                mpModel->mNativeShadowSlot : 0;
        if (casterSlot != 0) {
            duskNativeShadowCaster(casterSlot);
        }
        J3DShape::sNativeLod = nativeLodOf(*this, casterSlot);
        mpShape->draw();
        J3DShape::sNativeLod = J3DShape::NativeLod();
        if (casterSlot != 0) {
            duskNativeShadowCaster(0);
        }
#else
        mpShape->draw();
#endif
    }

#endif
}

void J3DShapePacket::drawFast() {
#if defined(DUSK_PICA_GAME_DRAW)
    drawNativeGameShape(*this);
#else
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    if (!checkFlag(J3DShpFlag_Hidden) && mpShape != NULL) {
#if defined(DUSK_TARGET_3DS)
        static const auto prepareIndex = dusk::native::registerGxNamedProfile("j3d.prepareDraw");
        dusk::native::GxNamedProfileSpan prepareSpan(prepareIndex);
#endif
        prepareDraw();

        if (mpTexMtxObj != NULL) {
            J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
            J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = NULL;
        }

#if defined(DUSK_TARGET_3DS)
        if (const J3DNativeInstances* instances = J3DNativeInstancesOf(mpModel)) {
            prepareSpan.stop();
            // drawFast for every model of the batch (J3DNativeInstances.h).
            J3DNativeInstancesDraw(mpShape, *this, *instances);
            return;
        }
        // Only the scene's own view counts: a mirror draws the same model
        // again through a reflected matrix, which is not the caster.
        const unsigned casterSlot =
            mpModel != NULL && mpBaseMtxPtr == (Mtx*)j3dSys.getViewMtx() ?
                mpModel->mNativeShadowSlot : 0;
        prepareSpan.stop();
        if (casterSlot != 0) {
            duskNativeShadowCaster(casterSlot);
        }
        J3DShape::sNativeLod = nativeLodOf(*this, casterSlot);
        mpShape->drawFast();
        J3DShape::sNativeLod = J3DShape::NativeLod();
        if (casterSlot != 0) {
            duskNativeShadowCaster(0);
        }
#else
        mpShape->drawFast();
#endif
    }

#endif
}

void J3DPacket::draw() {}

int J3DMatPacket::entry(J3DDrawBuffer* pBuffer) {
    J3DDrawBuffer::sortFunc func = J3DDrawBuffer::sortFuncTable[pBuffer->getSortMode()];
    return (pBuffer->*func)(this);
}
