#include "helpers/platform_features.h"
#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JParticle/JPAResource.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JParticle/JPABaseShape.h"
#include "JSystem/JParticle/JPAChildShape.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JParticle/JPAExTexShape.h"
#include "JSystem/JParticle/JPAExtraShape.h"
#include "JSystem/JParticle/JPAFieldBlock.h"
#include "JSystem/JParticle/JPAKeyBlock.h"
#include "JSystem/JParticle/JPAParticle.h"
#include "JSystem/JParticle/JPAResourceManager.h"
#include <gx.h>
#include "global.h"

#if TARGET_PC
#include "dusk/interp/particle.h"

#if !defined(DUSK_TARGET_3DS)
#include <tracy/Tracy.hpp>
#endif

#if defined(DUSK_TARGET_3DS)
#include "helpers/native_effects.h"
#include <algorithm>
#include <cmath>
#endif

#define JPA_DRAW_CTX_ARG , &ctx
#else
#define JPA_DRAW_CTX_ARG
#endif

JPAResource::JPAResource() {
    mpCalcEmitterFuncList = mpDrawEmitterFuncList = mpDrawEmitterChildFuncList = NULL;
#if TARGET_PC
    mpCalcParticleFuncList = mpCalcParticleChildFuncList = NULL;
    mpDrawParticleFuncList = mpDrawParticleChildFuncList = NULL;
    mBatchInfo = {};
    mChildBatchInfo = {};
#else
    mpCalcParticleFuncList = mpDrawParticleFuncList = mpCalcParticleChildFuncList = mpDrawParticleChildFuncList = NULL;
#endif
    pBsp = NULL;
    pEsp = NULL;
    pCsp = NULL;
    pEts = NULL;
    pDyn = NULL;
    ppFld = NULL;
    ppKey = NULL;
    mpTDB1 = NULL;
    mUsrIdx = fldNum = keyNum = texNum = mpCalcEmitterFuncListNum = mpDrawEmitterFuncListNum = mpDrawEmitterChildFuncListNum = mpCalcParticleFuncListNum = mpDrawParticleFuncListNum = mpCalcParticleChildFuncListNum = mpDrawParticleChildFuncListNum = 0;
}

ATTRIBUTE_ALIGN(32) static u8 jpa_pos[324] = {
    0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0xE7, 0x00, 0x00, 0x19,
    0x00, 0x00, 0x19, 0xCE, 0x00, 0xE7, 0xCE, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCE,
    0x00, 0xCE, 0xCE, 0x00, 0x00, 0x19, 0x00, 0x32, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x00, 0xE7, 0x00,
    0xE7, 0x19, 0x00, 0x19, 0x19, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0xE7, 0x00, 0xCE, 0x19, 0x00, 0x00,
    0x19, 0x00, 0x00, 0xE7, 0x00, 0xCE, 0xE7, 0x00, 0x00, 0x32, 0x00, 0x32, 0x32, 0x00, 0x32, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x32, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00,
    0xCE, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32,
    0x00, 0x00, 0x32, 0x00, 0x32, 0x00, 0x00, 0x32, 0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0x00,
    0x32, 0xE7, 0x00, 0x32, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x32,
    0x00, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x00, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0x19,
    0x00, 0xE7, 0x19, 0x00, 0x19, 0xE7, 0x00, 0x19, 0xCE, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0x00, 0x00,
    0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE7, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xCE, 0x00, 0xCE, 0x00,
    0x00, 0xCE, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0xCE,
    0x32, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0xE7,
    0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0xCE, 0x00, 0x19, 0x00, 0x00,
    0x19, 0x32, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x00, 0x00, 0x19, 0xE7, 0x00, 0x19, 0x19, 0x00, 0xE7,
    0x19, 0x00, 0xE7, 0xE7, 0x00, 0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0xCE,
    0x00, 0x32, 0x00, 0x00, 0x32, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x32, 0xE7, 0x00,
    0x32, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x32, 0xCE, 0x00, 0x32, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xCE,
};

ATTRIBUTE_ALIGN(32) static u8 jpa_crd[32] = {
    0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x02, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x02, 0x00, 0x02,
};

#if TARGET_PC
void JPAResource::initBatchInfo() {
    mBatchInfo = {};

    bool hasDrawFunc = false;
    for (int i = 0; i < mpDrawParticleFuncListNum; i++) {
        DrawParticleFunc func = mpDrawParticleFuncList[i];
        if (func == JPADrawBillboard || func == JPADrawRotBillboard ||
            func == JPADrawYBillboard || func == JPADrawRotYBillboard ||
            func == JPADrawDirection || func == JPADrawRotDirection || func == JPADrawRotation)
        {
            hasDrawFunc = true;
        } else if (func == JPADrawParticleCallBack) {
            // Batchable only for emitters without a particle callback; checked per draw
        } else if (func == JPALoadCalcTexCrdMtxAnm) {
            mBatchInfo.hasPtclTexMtx = true;
        } else if (func == JPARegistAlpha || func == JPARegistPrmAlpha ||
                   func == JPARegistPrmAlphaEnv || func == JPARegistAlphaEnv ||
                   func == static_cast<DrawParticleFunc>(JPARegistEnv)) // overloaded
        {
            mBatchInfo.hasPtclColor = true;
#if defined(DUSK_TARGET_3DS)
        } else if (func == JPADrawDBillboard) {
            hasDrawFunc = true;
        } else if (func == static_cast<DrawParticleFunc>(JPALoadTexAnm)) { // overloaded
            mBatchInfo.hasPtclTex = true;
#endif
        } else {
            // JPADrawPoint, JPADrawLine, JPADrawDBillboard, JPALoadTexAnm,
            // JPASetPointSize, JPASetLineWidth
            return;
        }
    }
    if (!hasDrawFunc) {
        return;
    }

    // Template array offsets, same math as setPTev
    int base_plane_type = (pBsp->getType() == 3 || pBsp->getType() == 7) ?
        pBsp->getBasePlaneType() : 0;
    int center_offset = pEsp != nullptr ? (pEsp->getScaleCenterX() + 3 * pEsp->getScaleCenterY()) * 0xC : 0x30;
    const s8* pos = reinterpret_cast<const s8*>(jpa_pos) + center_offset + base_plane_type * 0x6C;
    const s8* crd = reinterpret_cast<const s8*>(jpa_crd) + (pBsp->getTilingS() + 2 * pBsp->getTilingT()) * 8;

    bool cross = pBsp->getType() == 4 || pBsp->getType() == 8;
    mBatchInfo.vtxCount = cross ? 8 : 4;
    for (int i = 0; i < mBatchInfo.vtxCount; i++) {
        int posIdx = i < 4 ? i : 72 + (i - 4);
        int crdIdx = i & 3;
        mBatchInfo.vtxPos[i][0] = pos[posIdx * 3 + 0];
        mBatchInfo.vtxPos[i][1] = pos[posIdx * 3 + 1];
        mBatchInfo.vtxPos[i][2] = pos[posIdx * 3 + 2];
        mBatchInfo.vtxUv[i][0] = crd[crdIdx * 2 + 0];
        mBatchInfo.vtxUv[i][1] = crd[crdIdx * 2 + 1];
    }

    mBatchInfo.supported = true;
}

#if defined(DUSK_TARGET_3DS)
// Child particles: same quad templates as setCTev, same per-particle rules.
static void initChildBatchInfo(JPAResource* res) {
    JPAResource::BatchInfo& info = res->mChildBatchInfo;
    info = {};
    JPAChildShape* csp = res->getCsp();
    if (csp == nullptr || res->mpDrawParticleChildFuncList == nullptr) {
        return;
    }

    bool hasDrawFunc = false;
    for (int i = 0; i < res->mpDrawParticleChildFuncListNum; i++) {
        JPAResource::DrawParticleFunc func = res->mpDrawParticleChildFuncList[i];
        if (func == JPADrawBillboard || func == JPADrawRotBillboard ||
            func == JPADrawYBillboard || func == JPADrawRotYBillboard ||
            func == JPADrawDirection || func == JPADrawRotDirection || func == JPADrawRotation)
        {
            // The batch loop runs the draw function last, as the list does.
            if (i != 0) {
                return;
            }
            hasDrawFunc = true;
        } else if (func == JPADrawDBillboard) {
            if (i != 0) {
                return;
            }
            hasDrawFunc = true;
        } else if (func == JPADrawParticleCallBack) {
        } else if (func == JPARegistPrmAlphaEnv) {
            info.hasPtclColor = true;
        } else {
            return;
        }
    }
    if (!hasDrawFunc) {
        return;
    }

    int base_plane_type = (csp->getType() == 3 || csp->getType() == 7) ?
        csp->getBasePlaneType() : 0;
    const s8* pos = reinterpret_cast<const s8*>(jpa_pos) + 0x30 + base_plane_type * 0x6C;
    const s8* crd = reinterpret_cast<const s8*>(jpa_crd);
    bool cross = csp->getType() == 4 || csp->getType() == 8;
    info.vtxCount = cross ? 8 : 4;
    for (int i = 0; i < info.vtxCount; i++) {
        int posIdx = i < 4 ? i : 72 + (i - 4);
        int crdIdx = i & 3;
        info.vtxPos[i][0] = pos[posIdx * 3 + 0];
        info.vtxPos[i][1] = pos[posIdx * 3 + 1];
        info.vtxPos[i][2] = pos[posIdx * 3 + 2];
        info.vtxUv[i][0] = crd[crdIdx * 2 + 0];
        info.vtxUv[i][1] = crd[crdIdx * 2 + 1];
    }
    info.supported = true;
}
#endif
#endif

void JPAResource::init(JKRHeap* heap) {
    BOOL is_glbl_clr_anm = pBsp->isGlblClrAnm();
    BOOL is_glbl_tex_anm = pBsp->isGlblTexAnm();
    BOOL is_prm_anm = pBsp->isPrmAnm();
    BOOL is_env_anm = pBsp->isEnvAnm();
    BOOL is_tex_anm = pBsp->isTexAnm();
    BOOL is_tex_crd_anm = pBsp->isTexCrdAnm();
    BOOL is_prj_tex = pBsp->isPrjTex();
    BOOL is_enable_scale_anm = pEsp != NULL && pEsp->isEnableScaleAnm();
    BOOL is_enable_alpha_anm = pEsp != NULL && pEsp->isEnableAlphaAnm();
    BOOL is_enable_alpha_flick = pEsp != NULL && pEsp->isEnableAlphaAnm()
                                                      && pEsp->isEnableAlphaFlick();
    BOOL is_enable_rotate_anm = pEsp != NULL && pEsp->isEnableRotateAnm();
    BOOL is_rotate_on = is_enable_rotate_anm
                        || (pCsp != NULL && pCsp->isRotateOn());
    BOOL base_type_5_6 = pBsp->getType() == 5 || pBsp->getType() == 6;
    BOOL base_type_0 = pBsp->getType() == 0;
    BOOL base_type_0_1 = pBsp->getType() == 0 || pBsp->getType() == 1;
    BOOL child_type_5_6 = pCsp != NULL
                          && (pCsp->getType() == 5 || pCsp->getType() == 6);
    BOOL child_type_0 = pCsp != NULL && pCsp->getType() == 0;
    BOOL child_type_0_1 = pCsp != NULL
                          && (pCsp->getType() == 0 || pCsp->getType() == 1);
    BOOL is_draw_parent = !pBsp->isNoDrawParent();
    BOOL is_draw_child = !pBsp->isNoDrawChild();
    int func_no = 0;

    if (is_glbl_tex_anm && is_tex_anm) {
        mpCalcEmitterFuncListNum++;
    }

    if (is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcEmitterFuncListNum++;
        }
        if (is_env_anm) {
            mpCalcEmitterFuncListNum++;
        }
        if (is_prm_anm || is_env_anm) {
            mpCalcEmitterFuncListNum++;
        }
    }

    if (mpCalcEmitterFuncListNum != 0) {
        mpCalcEmitterFuncList =
            (EmitterFunc*)JKRAllocFromHeap(
                heap,
                mpCalcEmitterFuncListNum * sizeof(EmitterFunc),
                alignof(EmitterFunc));
    }

    func_no = 0;

    if (is_glbl_tex_anm && is_tex_anm) {
        switch (pBsp->getTexAnmType()) {
        case 0:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxNormal;
            break;
        case 1:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxRepeat;
            break;
        case 2:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxReverse;
            break;
        case 3:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxMerge;
            break;
        case 4:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxRandom;
            break;
        }
        func_no++;
    }

    if (is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcEmitterFuncList[func_no] = &JPACalcPrm;
            func_no++;
        }
        if (is_env_anm) {
            mpCalcEmitterFuncList[func_no] = &JPACalcEnv;
            func_no++;
        }
        if (is_prm_anm || is_env_anm) {
            switch (pBsp->getClrAnmType()) {
            case 0:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxNormal;
                break;
            case 1:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxRepeat;
                break;
            case 2:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxReverse;
                break;
            case 3:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxMerge;
                break;
            case 4:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxRandom;
                break;
            }
            func_no++;
        }
    }

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpCalcParticleFuncListNum++;
    }

    if (!base_type_5_6 && (is_enable_alpha_anm || is_enable_alpha_flick)) {
        mpCalcParticleFuncListNum++;
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcParticleFuncListNum++;
        }
        if (is_env_anm) {
            mpCalcParticleFuncListNum++;
        }
        if (is_prm_anm || is_env_anm) {
            mpCalcParticleFuncListNum++;
        }
    } else {
        mpCalcParticleFuncListNum++;
    }

    if (is_enable_scale_anm) {
        if (pBsp->getType() != 0) {
            if (pEsp->isScaleXYDiff()) {
                if (pEsp->getScaleAnmTypeX() == 0 && pEsp->getScaleAnmTypeY() == 0) {
                    mpCalcParticleFuncListNum++;
                } else {
                    mpCalcParticleFuncListNum++;
                    mpCalcParticleFuncListNum++;
                }
            } else {
                mpCalcParticleFuncListNum++;
            }
        }
        mpCalcParticleFuncListNum++;
        mpCalcParticleFuncListNum++;
    }

    if (mpCalcParticleFuncListNum != 0) {
        mpCalcParticleFuncList =
            (ParticleFunc*)JKRAllocFromHeap(heap, mpCalcParticleFuncListNum * sizeof(ParticleFunc), alignof(ParticleFunc));
    }

    func_no = 0;

    if (!is_glbl_tex_anm && is_tex_anm) {
        switch (pBsp->getTexAnmType()) {
        case 0:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxNormal;
            break;
        case 1:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxRepeat;
            break;
        case 2:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxReverse;
            break;
        case 3:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxMerge;
            break;
        case 4:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxRandom;
            break;
        }
        func_no++;
    }

    if (!base_type_5_6 && (is_enable_alpha_anm || is_enable_alpha_flick)) {
        if (is_enable_alpha_flick) {
            mpCalcParticleFuncList[func_no] = &JPACalcAlphaFlickAnm;
            func_no++;
        } else {
            mpCalcParticleFuncList[func_no] = &JPACalcAlphaAnm;
            func_no++;
        }
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcParticleFuncList[func_no] = &JPACalcPrm;
            func_no++;
        }
        if (is_env_anm) {
            mpCalcParticleFuncList[func_no] = &JPACalcEnv;
            func_no++;
        }
        if (is_prm_anm || is_env_anm) {
            switch (pBsp->getClrAnmType()) {
            case 0:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxNormal;
                break;
            case 1:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxRepeat;
                break;
            case 2:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxReverse;
                break;
            case 3:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxMerge;
                break;
            case 4:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxRandom;
                break;
            }
            func_no++;
        }
    } else {
        mpCalcParticleFuncList[func_no] = &JPACalcColorCopy;
        func_no++;
    }

    if (is_enable_scale_anm) {
        if (pBsp->getType() != 0) {
            if (pEsp->isScaleXYDiff()) {
                mpCalcParticleFuncList[func_no] = &JPACalcScaleY;
                func_no++;
                if (pEsp->getScaleAnmTypeY() != 0 || pEsp->getScaleAnmTypeX() != 0) {
                    switch (pEsp->getScaleAnmTypeY()) {
                    case 0:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmNormal;
                        break;
                    case 1:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmRepeatY;
                        break;
                    case 2:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmReverseY;
                        break;
                    }
                    func_no++;
                }
            } else {
                mpCalcParticleFuncList[func_no] = &JPACalcScaleCopy;
                func_no++;
            }
        }
        mpCalcParticleFuncList[func_no] = &JPACalcScaleX;
        func_no++;
        switch (pEsp->getScaleAnmTypeX()) {
        case 0:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmNormal;
            break;
        case 1:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmRepeatX;
            break;
        case 2:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmReverseX;
            break;
        }
        func_no++;
    }

    if (pCsp != NULL && pCsp->isScaleOutOn()) {
        mpCalcParticleChildFuncListNum++;
    }

    if (pCsp != NULL && pCsp->isAlphaOutOn()) {
        mpCalcParticleChildFuncListNum++;
    }

    if (mpCalcParticleChildFuncListNum != 0) {
        mpCalcParticleChildFuncList =
            (ParticleFunc*)JKRAllocFromHeap(heap, mpCalcParticleChildFuncListNum * sizeof(ParticleFunc), alignof(ParticleFunc));
    }

    func_no = 0;

    if (pCsp != NULL && pCsp->isScaleOutOn()) {
        mpCalcParticleChildFuncList[func_no] = &JPACalcChildScaleOut;
        func_no++;
    }

    if (pCsp != NULL && pCsp->isAlphaOutOn()) {
        mpCalcParticleChildFuncList[func_no] = &JPACalcChildAlphaOut;
        func_no++;
    }

    if (is_draw_parent && base_type_5_6) {
        mpDrawEmitterFuncListNum++;
    }

    mpDrawEmitterFuncListNum++;

    if (pEts != NULL) {
        mpDrawEmitterFuncListNum++;
    }

    if (is_glbl_tex_anm || !is_tex_anm) {
        mpDrawEmitterFuncListNum++;
    }

    mpDrawEmitterFuncListNum++;

    if (base_type_0_1) {
        mpDrawEmitterFuncListNum++;
    }
    
    if (base_type_0_1 && !is_enable_scale_anm) {
        mpDrawEmitterFuncListNum++;
    }

    if (is_glbl_clr_anm || (!is_prm_anm && !is_enable_alpha_anm) || !is_env_anm) {
        mpDrawEmitterFuncListNum++;
    }

    if (mpDrawEmitterFuncListNum != 0) {
        mpDrawEmitterFuncList =
            (EmitterFunc*)JKRAllocFromHeap(heap, mpDrawEmitterFuncListNum * sizeof(EmitterFunc), alignof(EmitterFunc));
    }

    func_no = 0;

    if (is_draw_parent && base_type_5_6) {
        if (pBsp->getType() == 5) {
            mpDrawEmitterFuncList[func_no] = &JPADrawStripe;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPADrawStripeX;
            func_no++;
        }
    }

    mpDrawEmitterFuncList[func_no] = &JPADrawEmitterCallBackB;
    func_no++;

    if (pEts != NULL) {
        mpDrawEmitterFuncList[func_no] = &JPALoadExTex;
        func_no++;
    }

    if (!is_tex_anm) {
        mpDrawEmitterFuncList[func_no] = &JPALoadTex;
        func_no++;
    } else if (is_glbl_tex_anm) {
        mpDrawEmitterFuncList[func_no] = &JPALoadTexAnm;
        func_no++;
    }

    if (base_type_0_1) {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxIdt;
        func_no++;
    } else if (is_prj_tex) {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxPrj;
        func_no++;
    } else if (is_tex_crd_anm) {
        if (base_type_5_6) {
            mpDrawEmitterFuncList[func_no] = &JPAGenCalcTexCrdMtxAnm;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxAnm;
            func_no++;
        }
    } else {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxIdt;
        func_no++;
    }

    if (base_type_0_1) {
        mpDrawEmitterFuncList[func_no] = &JPALoadPosMtxCam;
        func_no++;
    }

    if (base_type_0_1 && !is_enable_scale_anm) {
        if (base_type_0) {
            mpDrawEmitterFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    }

    if (is_glbl_clr_anm) {
        if (base_type_5_6 || !is_enable_alpha_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrmEnv;
            func_no++;
        } else if (is_enable_alpha_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistEnv;
            func_no++;
        }
    } else if (!is_prm_anm && !is_enable_alpha_anm) {
        if (!is_env_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrmEnv;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrm;
            func_no++;
        }
    } else if (!is_env_anm) {
        mpDrawEmitterFuncList[func_no] = &JPARegistEnv;
        func_no++;
    }

    if (is_draw_child && child_type_5_6) {
        mpDrawEmitterChildFuncListNum++;
    }

    mpDrawEmitterChildFuncListNum++;

    if (child_type_0_1) {
        mpDrawEmitterChildFuncListNum++;
    }

    if (pCsp != NULL && !pCsp->isAlphaOutOn() && !pCsp->isAlphaInherited()
                             && !pCsp->isColorInherited()) {
        mpDrawEmitterChildFuncListNum++;
    }

    if (mpDrawEmitterChildFuncListNum != 0) {
        mpDrawEmitterChildFuncList =
            (EmitterFunc*)JKRAllocFromHeap(heap, mpDrawEmitterChildFuncListNum * sizeof(EmitterFunc), alignof(EmitterFunc));
    }

    func_no = 0;

    if (is_draw_child && child_type_5_6) {
        if (pCsp->getType() == 5) {
            mpDrawEmitterChildFuncList[func_no] = &JPADrawStripe;
            func_no++;
        } else {
            mpDrawEmitterChildFuncList[func_no] = &JPADrawStripeX;
            func_no++;
        }
    }

    mpDrawEmitterChildFuncList[func_no] = &JPADrawEmitterCallBackB;
    func_no++;

    if (child_type_0_1) {
        mpDrawEmitterChildFuncList[func_no] = &JPALoadPosMtxCam;
        func_no++;
    }

    if (pCsp != NULL && !pCsp->isAlphaOutOn() && !pCsp->isAlphaInherited()
                             && !pCsp->isColorInherited()) {
        mpDrawEmitterChildFuncList[func_no] = &JPARegistChildPrmEnv;
        func_no++;
    }

    if (is_draw_parent && !base_type_5_6) {
        mpDrawParticleFuncListNum++;
    }

    mpDrawParticleFuncListNum++;

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpDrawParticleFuncListNum++;
    }

    if ((base_type_0_1 && is_enable_scale_anm) || (is_tex_crd_anm && !is_prj_tex)) {
        mpDrawParticleFuncListNum++;
    }

    if ((!is_glbl_clr_anm && (is_prm_anm || is_env_anm || is_enable_alpha_anm))
                || (is_glbl_clr_anm && is_enable_alpha_anm && !base_type_5_6)) {
        mpDrawParticleFuncListNum++;
    }

    if (mpDrawParticleFuncListNum != 0) {
        mpDrawParticleFuncList =
            (DrawParticleFunc*)JKRAllocFromHeap(
                heap,
                mpDrawParticleFuncListNum * sizeof(DrawParticleFunc),
                alignof(DrawParticleFunc));
    }

    func_no = 0;

    if (is_draw_parent && !base_type_5_6) {
        switch (pBsp->getType()) {
        case 2:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotBillboard;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawBillboard;
            }
            break;
        case 10:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotYBillboard;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawYBillboard;
            }
            break;
        case 3:
        case 4:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotDirection;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawDirection;
            }
            break;
        case 9:
            mpDrawParticleFuncList[func_no] = &JPADrawDBillboard;
            break;
        case 7:
        case 8:
            mpDrawParticleFuncList[func_no] = &JPADrawRotation;
            break;
        case 0:
            mpDrawParticleFuncList[func_no] = &JPADrawPoint;
            break;
        case 1:
            mpDrawParticleFuncList[func_no] = &JPADrawLine;
            break;
        }
        func_no++;
    }

    mpDrawParticleFuncList[func_no] = &JPADrawParticleCallBack;
    func_no++;

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpDrawParticleFuncList[func_no] = &JPALoadTexAnm;
        func_no++;
    }

    if (base_type_0_1 && is_enable_scale_anm) {
        if (base_type_0) {
            mpDrawParticleFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawParticleFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    } else if (is_tex_crd_anm && !is_prj_tex) {
        mpDrawParticleFuncList[func_no] = &JPALoadCalcTexCrdMtxAnm;
        func_no++;
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            if (is_env_anm) {
                mpDrawParticleFuncList[func_no] = &JPARegistPrmAlphaEnv;
                func_no++;
            } else {
                mpDrawParticleFuncList[func_no] = &JPARegistPrmAlpha;
                func_no++;
            }
        } else if (is_enable_alpha_anm) {
            if (is_env_anm) {
                mpDrawParticleFuncList[func_no] = &JPARegistAlphaEnv;
                func_no++;
            } else {
                mpDrawParticleFuncList[func_no] = &JPARegistAlpha;
                func_no++;
            }
        } else if (is_env_anm) {
            mpDrawParticleFuncList[func_no] = &JPARegistEnv;
            func_no++;
        }
    } else if (is_enable_alpha_anm && !base_type_5_6) {
        mpDrawParticleFuncList[func_no] = &JPARegistAlpha;
        func_no++;
    }

    if (is_draw_child && pCsp != NULL && !child_type_5_6) {
        mpDrawParticleChildFuncListNum++;
    }

    mpDrawParticleChildFuncListNum++;

    if (child_type_0_1) {
        mpDrawParticleChildFuncListNum++;
    }

    if (pCsp != NULL && (pCsp->isAlphaOutOn() || pCsp->isAlphaInherited()
                                || pCsp->isColorInherited())) {
        mpDrawParticleChildFuncListNum++;
    }

    if (mpDrawParticleChildFuncListNum != 0) {
        mpDrawParticleChildFuncList =
            (DrawParticleFunc*)JKRAllocFromHeap(
                heap,
                mpDrawParticleChildFuncListNum * sizeof(DrawParticleFunc),
                alignof(DrawParticleFunc));
    }

    func_no = 0;

    if (is_draw_child && pCsp != NULL && !child_type_5_6) {
        switch (pCsp->getType()) {
        case 2:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotBillboard;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawBillboard;
            }
            break;
        case 10:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotYBillboard;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawYBillboard;
            }
            break;
        case 3:
        case 4:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotDirection;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawDirection;
            }
            break;
        case 9:
            mpDrawParticleChildFuncList[func_no] = &JPADrawDBillboard;
            break;
        case 7:
        case 8:
            mpDrawParticleChildFuncList[func_no] = &JPADrawRotation;
            break;
        case 0:
            mpDrawParticleChildFuncList[func_no] = &JPADrawPoint;
            break;
        case 1:
            mpDrawParticleChildFuncList[func_no] = &JPADrawLine;
            break;
        }
        func_no++;
    }

    mpDrawParticleChildFuncList[func_no] = &JPADrawParticleCallBack;
    func_no++;

    if (child_type_0_1) {
        if (child_type_0) {
            mpDrawParticleChildFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawParticleChildFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    }

    if (pCsp != NULL && (pCsp->isAlphaOutOn() || pCsp->isAlphaInherited()
                                || pCsp->isColorInherited())) {
        mpDrawParticleChildFuncList[func_no] = &JPARegistPrmAlphaEnv;
        func_no++;
    }

#if TARGET_PC
    initBatchInfo();
#if defined(DUSK_TARGET_3DS)
    // The batch loop runs the draw function last, as the list does.
    if (mBatchInfo.supported) {
        DrawParticleFunc first = mpDrawParticleFuncList[0];
        if (!(first == JPADrawBillboard || first == JPADrawRotBillboard ||
              first == JPADrawYBillboard || first == JPADrawRotYBillboard ||
              first == JPADrawDirection || first == JPADrawRotDirection ||
              first == JPADrawRotation || first == JPADrawDBillboard)) {
            mBatchInfo.supported = false;
        }
    }
    initChildBatchInfo(this);
#endif
#endif
}

bool JPAResource::calc(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    work->mpEmtr = emtr;
    work->mpRes = this;
    work->mEmitCount = 0;

    if (!emtr->processTillStartFrame()) {
        return false;
    }

    if (emtr->processTermination()) {
        return true;
    }

    if (emtr->checkStatus(2)) {
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->execute(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
            emtr->mpEmtrCallBack->executeAfter(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

    } else {
        calcKey(work);

        for (int i = fldNum - 1; i >= 0; i--) {
            ppFld[i]->initOpParam();
        }

        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->execute(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

        calcWorkData_c(work);

        for (int i = mpCalcEmitterFuncListNum - 1; i >= 0; i--) {
            (*mpCalcEmitterFuncList[i])(work);
        }

        for (int i = fldNum - 1; i >= 0; i--) {
            ppFld[i]->prepare(work);
        }

        if (!emtr->checkStatus(8)) {
            pDyn->create(work);
        }

        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->executeAfter(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

        JPANode<JPABaseParticle>* next = NULL;
        for (JPANode<JPABaseParticle>* node = emtr->mAlivePtclBase.getFirst(); node != emtr->mAlivePtclBase.getEnd(); node = next) {
            next = node->getNext();
            if (node->getObject()->calc_p(work)) {
                emtr->mpPtclPool->push_front(emtr->mAlivePtclBase.erase(node));
            }
        }

        for (JPANode<JPABaseParticle>* node = emtr->mAlivePtclChld.getFirst(); node != emtr->mAlivePtclChld.getEnd(); node = next) {
            next = node->getNext();
            if (node->getObject()->calc_c(work)) {
                emtr->mpPtclPool->push_front(emtr->mAlivePtclChld.erase(node));
            }
        }

        emtr->mTick++;
        DUSK_DESKTOP_ONLY(dusk::interp::particle::capture(emtr));
    }

    return false;
}

#if defined(DUSK_TARGET_3DS)
// A native batch reads the particle colour from the vertex: it leaves one
// colour channel on and TEV stage 0 reading the rasterised colour. The next
// batch sets both again, so they are only put back when a per-particle draw
// follows (and at the end of the group).
static bool sNativeTevDirty;

extern "C" unsigned long long svcGetSystemTick(void);

// The game swaps the frame-buffer copy in for the texture named "dummy":
// a resource whose main texture is that one draws a refraction (heat haze,
// water lens), not a picture of its own.
static bool native_refraction_resource(JPAEmitterWorkData* work) {
    JPAResource* res = work->mpRes;
    JPATexture* texture = work->mpResMgr->pTexAry[res->getTexIdx(res->getBsp()->getTexIdx())];
    const char* name = texture->getName();
    return name[0] == 'd' && std::strcmp(name, "dummy") == 0;
}

void JPANativeBatchEnd() {
    if (sNativeTevDirty) {
        GXSetNumChans(0);
        sNativeTevDirty = false;
    }
}
#endif

void JPAResource::draw(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    work->mpEmtr = emtr;
    work->mpRes = this;
    work->mDrawCount = 0;
    DUSK_DESKTOP_ONLY(dusk::interp::particle::EmitterDraw presentation(emtr));
#if defined(DUSK_TARGET_3DS)
    // A refraction resource samples the frame copy through an indirect warp.
    // The PICA has no indirect stage, and drawn without it the particle is
    // either invisible or, with its own texture matrix, a misplaced piece of
    // the frame. Its particles are not drawn; the heat haze actor asks the
    // screen-effect pass for its sway instead (native_effects::haze).
    if (native_refraction_resource(work)) {
        dusk::native_effects::counters.particleRefraction +=
            emtr->mAlivePtclBase.getNum() + emtr->mAlivePtclChld.getNum();
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->draw(emtr);
        }
        // draw() may detach the callback from the emitter.
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->drawAfter(emtr);
        }
        return;
    }
    // Profiled frame: time and count this resource (see EmitterRow).
    dusk::native_effects::EmitterRow* row = nullptr;
    unsigned long long rowStart = 0;
    unsigned rowBatches = 0, rowSingles = 0;
    if (dusk::native_effects::emitterReport) {
        using namespace dusk::native_effects;
        for (unsigned i = 0; i < emitterRowCount && row == nullptr; i++) {
            if (emitterRows[i].resource == this) {
                row = &emitterRows[i];
            }
        }
        if (row == nullptr && emitterRowCount < sizeof(emitterRows) / sizeof(*emitterRows)) {
            row = &emitterRows[emitterRowCount++];
            *row = {};
            row->resource = this;
            row->id = mUsrIdx;
            row->flags = (native_refraction_resource(work) ? 1u : 0u) |
                         (pEts != NULL && pEts->isUseIndirect() ? 2u : 0u) |
                         (pBsp->isPrjTex() ? 4u : 0u) | (pCsp != NULL ? 8u : 0u) |
                         (emtr->mpPtclCallBack != nullptr ? 16u : 0u) |
                         (emtr->mpEmtrCallBack != nullptr ? 32u : 0u) |
                         (mBatchInfo.hasPtclTex ? 64u : 0u) | (mBatchInfo.hasPtclTexMtx ? 128u : 0u) |
                         (pBsp->getType() << 8) | (pBsp->getTevColorArgSel() << 12) |
                         ((pBsp->pBsd->mBlendModeCfg & 3u) << 16);
        }
        if (row != nullptr) {
            row->emitters++;
            row->particles += emtr->mAlivePtclBase.getNum() + emtr->mAlivePtclChld.getNum();
            rowBatches = counters.particleBatches;
            rowSingles = counters.particleSingle;
            rowStart = svcGetSystemTick();
        }
    }
#endif
    calcWorkData_d(work);
    pBsp->setGX(work);
#if defined(DUSK_TARGET_3DS)
    JPANativeBatchEnd();
#endif
    for (s32 i = 1; i <= emtr->getDrawTimes(); i++) {
        work->mDrawCount++;
        if (pBsp->isDrawPrntAhead() && pCsp != NULL)
            drawC(work);
        drawP(work);
        if (!pBsp->isDrawPrntAhead() && pCsp != NULL)
            drawC(work);
    }
#if defined(DUSK_TARGET_3DS)
    if (row != nullptr) {
        row->ticks += svcGetSystemTick() - rowStart;
        row->batches += dusk::native_effects::counters.particleBatches - rowBatches;
        row->singles += dusk::native_effects::counters.particleSingle - rowSingles;
    }
#endif
}

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
static GXTevAlphaArg to_vtx_alpha_arg(GXTevAlphaArg arg) {
    return arg == GX_CA_A0 ? GX_CA_RASA : arg;
}

static void batch_set_tev_op(GXTevStageID stage) {
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void batch_setup_tev(JPAEmitterWorkData* work, bool useClr1) {
    JPABaseShape* shape = work->mpRes->getBsp();
    JPAExTexShape* ets = work->mpRes->getEts();
    bool useIndirect = ets != nullptr && ets->isUseIndirect();

    // JPAEmitterManager::draw configures both channels to pass vertex color through
    GXSetNumChans(useClr1 ? 2 : 1);

    const GXTevAlphaArg* alphaArg = shape->getTevAlphaArg();
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevAlphaIn(GX_TEVSTAGE0, to_vtx_alpha_arg(alphaArg[0]), to_vtx_alpha_arg(alphaArg[1]),
                    to_vtx_alpha_arg(alphaArg[2]), to_vtx_alpha_arg(alphaArg[3]));
    batch_set_tev_op(GX_TEVSTAGE0);
    if (!useIndirect) {
        GXSetTevDirect(GX_TEVSTAGE0);
    }
    GXTevStageID nextStage = GX_TEVSTAGE1;

    switch (shape->getTevColorArgSel()) {
    case 0: // TEXC
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
        break;
    case 1: // C0 * TEXC
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
        break;
    case 2: // lerp(C0, 1, TEXC)
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        break;
    case 3: // lerp(C1, C0, TEXC) = C0 * TEXC (stage 0) + C1 * (1 - TEXC) (stage 1)
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevOrder(nextStage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR1A1);
        GXSetTevColorIn(nextStage, GX_CC_RASC, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV);
        GXSetTevAlphaIn(nextStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        batch_set_tev_op(nextStage);
        GXSetTevDirect(nextStage);
        nextStage = static_cast<GXTevStageID>(nextStage + 1);
        break;
    case 4: // TEXC * C0 + C1: C0 * TEXC (stage 0), + C1 (stage 1)
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevOrder(nextStage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR1A1);
        GXSetTevColorIn(nextStage, GX_CC_CPREV, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        GXSetTevAlphaIn(nextStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        batch_set_tev_op(nextStage);
        GXSetTevDirect(nextStage);
        nextStage = static_cast<GXTevStageID>(nextStage + 1);
        break;
    case 5: // C0
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        break;
    }

    if (ets != nullptr && ets->isUseSecTex()) {
        // Mirrors setPTev's secondary texture stage, at the next free stage
        GXTexCoordID texCoord = useIndirect ? GX_TEXCOORD2 : GX_TEXCOORD1;
        GXSetTevOrder(nextStage, texCoord, GX_TEXMAP3, GX_COLOR_NULL);
        GXSetTevColorIn(nextStage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
        GXSetTevAlphaIn(nextStage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO);
        batch_set_tev_op(nextStage);
        GXSetTevDirect(nextStage);
        nextStage = static_cast<GXTevStageID>(nextStage + 1);
    }

    GXSetNumTevStages(nextStage);
}

static void batch_setup_vtx_desc(bool useClr0, bool useClr1) {
    static Mtx identityMtx = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };

    GXLoadPosMtxImm(identityMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    if (useClr0) {
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    }
    if (useClr1) {
        GXSetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    }
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    if (useClr0) {
        GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    if (useClr1) {
        GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void batch_restore_gx(JPAEmitterWorkData* work, bool changedTev, bool changedTexMtx) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetCurrentMtx(GX_PNMTX0);

    if (changedTexMtx) {
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
    }

    if (changedTev) {
        GXSetNumChans(0);
        work->mpRes->getBsp()->setGX(work);
        work->mpRes->setPTev();
    }
}

static bool draw_particle_batch(JPAEmitterWorkData* work) {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif

    JPAResource* res = work->mpRes;
    const JPAResource::BatchInfo& info = res->mBatchInfo;
    if (!info.supported || work->mPrjType != 0 || work->mpEmtr->mpPtclCallBack != nullptr) {
        return false;
    }

    bool useClr0 = false;
    bool useClr1 = false;
    if (info.hasPtclColor) {
        u32 colorSel = res->getBsp()->getTevColorArgSel();
        if (colorSel >= 6) {
            return false;
        }
        useClr0 = true;
        useClr1 = colorSel == 3 || colorSel == 4;
        batch_setup_tev(work, useClr1);
    }

    if (info.hasPtclTexMtx) {
        // UVs are CPU-transformed; drop the texgen
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    }

    batch_setup_vtx_desc(useClr0, useClr1);

    ParticleDrawCtx ctx{};
    ctx.batch = true;
    ctx.useTexMtx = info.hasPtclTexMtx;
    ctx.useClr0 = useClr0;
    ctx.useClr1 = useClr1;

    bool fwdAhead = res->getBsp()->isDrawFwdAhead();
    JPANode<JPABaseParticle>* node = fwdAhead ? work->mpEmtr->mAlivePtclBase.getLast() :
                                                work->mpEmtr->mAlivePtclBase.getFirst();

    GXBegin(GX_QUADS, GX_VTXFMT1, GX_AUTO);
    while (node != work->mpEmtr->mAlivePtclBase.getEnd()) {
        work->mpCurNode = node;
        JPABaseParticle scratch;
        JPABaseParticle* particle = dusk::interp::particle::present(node->getObject(), scratch, ctx.age);
        for (int i = res->mpDrawParticleFuncListNum - 1; i >= 0; i--) {
            (*res->mpDrawParticleFuncList[i])(work, particle, &ctx);
        }
        node = fwdAhead ? node->getPrev() : node->getNext();
    }
    GXEnd();

    batch_restore_gx(work, useClr0, info.hasPtclTexMtx);
    return true;
}
#endif

#if defined(DUSK_TARGET_3DS)
// Native particle batch: one GX draw per emitter instead of one per particle
// (each particle used to load a position matrix, a TEV colour and often a
// texture matrix before its own four-vertex draw). Corners are transformed
// here; the particle's primary colour travels as vertex colour, so the
// rasterised colour takes the place of TEV register 0. The environment colour
// stays in TEV register 1: when it differs between two particles, the
// primitive is closed and a new one starts.
void JPANativePrjTexMtx(JPAEmitterWorkData const* work, Mtx out); // JPABaseShape.cpp

static GXTevColorArg native_batch_color_arg(GXTevColorArg arg) {
    return arg == GX_CC_C0 ? GX_CC_RASC : arg == GX_CC_A0 ? GX_CC_RASA : arg;
}

static GXTevAlphaArg native_batch_alpha_arg(GXTevAlphaArg arg) {
    return arg == GX_CA_A0 ? GX_CA_RASA : arg;
}

static bool native_draw_particle_batch(JPAEmitterWorkData* work, bool child) {
    JPAResource* res = work->mpRes;
    JPABaseEmitter* emtr = work->mpEmtr;
    const JPAResource::BatchInfo& info = child ? res->mChildBatchInfo : res->mBatchInfo;
    JPAList<JPABaseParticle>& list = child ? emtr->mAlivePtclChld : emtr->mAlivePtclBase;
    // A single particle gains nothing from the state change around a batch.
    if (dusk::native_effects::particles < 2 || list.getNum() < 2) {
        return false;
    }
    // A projected texture is evaluated per corner; with a per-particle
    // texture matrix on top the original has no defined combination.
    const bool projected = !child && work->mPrjType != 0;
    // A particle callback that draws (lit leaves, models) draws each particle itself.
    const bool callbackDraws = emtr->mpPtclCallBack != nullptr && emtr->mpPtclCallBack->drawsParticle();
    if (!info.supported || (projected && info.hasPtclTexMtx) || callbackDraws) {
        // Diagnostics: why an emitter with several particles stayed unbatched.
        unsigned& reason = dusk::native_effects::counters.particleUnbatched[
            !info.supported ? 0 : projected ? 1 : 2];
        reason += list.getNum();
        return false;
    }

    JPABaseShape* shape = res->getBsp();
    const u32 colorSel = shape->getTevColorArgSel();
    const bool useClr0 = info.hasPtclColor;
    if (useClr0 && colorSel >= 6) {
        dusk::native_effects::counters.particleUnbatched[3] += list.getNum();
        return false;
    }
    // Only these two colour formulas read the environment register.
    const bool trackEnv = useClr0 && (colorSel == 3 || colorSel == 4);

    if (useClr0) {
        const GXTevColorArg* colorArg = shape->getTevColorArg();
        const GXTevAlphaArg* alphaArg = shape->getTevAlphaArg();
        GXSetNumChans(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, child ? GX_TEXMAP1 : GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevColorIn(GX_TEVSTAGE0, native_batch_color_arg(colorArg[0]),
                        native_batch_color_arg(colorArg[1]), native_batch_color_arg(colorArg[2]),
                        native_batch_color_arg(colorArg[3]));
        GXSetTevAlphaIn(GX_TEVSTAGE0, native_batch_alpha_arg(alphaArg[0]),
                        native_batch_alpha_arg(alphaArg[1]), native_batch_alpha_arg(alphaArg[2]),
                        native_batch_alpha_arg(alphaArg[3]));
    }
    // Indirect texture: the PICA has none, and the translator evaluates the
    // warp once per vertex on the CPU (about 40 microseconds a vertex). Over
    // the four corners of a particle that is a small shift of the image, not
    // a warp, so a batch draws the texture directly.
    JPAExTexShape* ets = res->getEts();
    const bool dropIndirect = !child && ets != nullptr && ets->isUseIndirect();
    if (dropIndirect) {
        GXSetNumIndStages(0);
        GXSetTevDirect(GX_TEVSTAGE0);
        dusk::native_effects::counters.particleDirect += list.getNum();
    }
    const bool changedTexGen = !child && (info.hasPtclTexMtx || projected);
    if (changedTexGen) {
        // UVs are transformed here; drop the texture matrix.
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    }

    // Clipping stays on for the batch. Quads wholly off screen never leave
    // the loop below, and a draw with clipping off is transformed and clipped
    // vertex by vertex on the CPU (20 microseconds a vertex) instead of on
    // the PICA. Only a quad that crosses the near plane is drawn differently:
    // it is clipped there.
    const bool clipOff = child ? !res->getCsp()->isClipOn() : !shape->isClipOn();
    if (clipOff) {
        GXSetClipMode(GX_CLIP_ENABLE);
    }

    static Mtx identityMtx = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    GXLoadPosMtxImm(identityMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    if (useClr0) {
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    ParticleDrawCtx ctx{};
    ctx.batch = true;
    ctx.child = child;
    ctx.useTexMtx = !child && info.hasPtclTexMtx;
    ctx.useClr0 = useClr0;
    ctx.useClr1 = trackEnv;
    ctx.prj = projected;
    if (projected) {
        JPANativePrjTexMtx(work, ctx.texMtx);
    }
    const bool trackTex = !child && info.hasPtclTex;
    int texIdx = -1;

    JPAResource::DrawParticleFunc* funcs = child ? res->mpDrawParticleChildFuncList : res->mpDrawParticleFuncList;
    const int funcNum = child ? res->mpDrawParticleChildFuncListNum : res->mpDrawParticleFuncListNum;
    const bool fwdAhead = shape->isDrawFwdAhead();
    JPANode<JPABaseParticle>* node = fwdAhead ? list.getLast() : list.getFirst();

    const auto drift = [](u8 a, u8 b, int limit) { return a > b ? a - b > limit : b - a > limit; };
    const auto closeDraw = [&ctx]() {
        if (ctx.open) {
            GXEnd();
            ctx.open = false;
            dusk::native_effects::counters.particleBatches++;
        }
    };
    const auto step = [fwdAhead](JPANode<JPABaseParticle>* n) {
        return fwdAhead ? n->getPrev() : n->getNext();
    };
    u32 count = 0;

    // Early cull: a particle whose bounding sphere lies outside one side of
    // the view skips its draw functions and corner transforms altogether
    // (a twilight field carried 900 such particles a frame). The sphere is
    // twice the scaled quad, which covers its pivot shift and rotation; the
    // exact per-corner test in the draw function still follows.
    f32 cullPlanes[5][4];
    {
        const f32 (*m)[4] = work->mPrjMtx;
        for (int i = 0; i < 4; i++) {
            cullPlanes[0][i] = m[2][i];           // in front of the eye
            cullPlanes[1][i] = m[0][i];           // s >= 0
            cullPlanes[2][i] = m[2][i] - m[0][i]; // s <= 1
            cullPlanes[3][i] = m[1][i];           // t >= 0
            cullPlanes[4][i] = m[2][i] - m[1][i]; // t <= 1
        }
        for (auto& plane : cullPlanes) {
            const f32 length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
            if (length > 1.0e-12f) {
                for (f32& value : plane) {
                    value /= length;
                }
            } else {
                // Orthographic view: no plane through the eye.
                plane[0] = plane[1] = plane[2] = 0.0f;
                plane[3] = 1.0f;
            }
        }
    }
    f32 quadRadius = 0.0f;
    for (int i = 0; i < info.vtxCount; i++) {
        quadRadius = std::max(quadRadius, std::sqrt(info.vtxPos[i][0] * info.vtxPos[i][0] + info.vtxPos[i][1] * info.vtxPos[i][1] +
                                                    info.vtxPos[i][2] * info.vtxPos[i][2]));
    }
    quadRadius *= 2.0f;
    const f32 scaleX = std::fabs(work->mGlobalPtclScl.x), scaleY = std::fabs(work->mGlobalPtclScl.y);
    const auto offScreen = [&](const JPABaseParticle* particle) {
        Vec centre;
        MTXMultVec(work->mPosCamMtx, &particle->mPosition, &centre);
        const f32 radius = quadRadius * std::max(scaleX * std::fabs(particle->mParticleScaleX),
                                                 scaleY * std::fabs(particle->mParticleScaleY));
        for (const auto& plane : cullPlanes) {
            if (plane[0] * centre.x + plane[1] * centre.y + plane[2] * centre.z + plane[3] < -radius) {
                return true;
            }
        }
        return false;
    };

    // The environment colour follows each particle's age and the texture its
    // animation frame, and neither can travel with a vertex: the TEV register
    // and the bound texture belong to the draw. Drawn in list order, a fire
    // emitter changed one of them at nearly every particle (242 draws for 279
    // particles). The particles are grouped instead: one draw per texture and
    // per environment colour within 24/255 a channel (9%), groups in order of
    // their first particle. Inside one emitter the order between groups only
    // matters where its particles overlap with a non-additive blend.
    struct Group {
        GXColor env;
        int tex;
    };
    constexpr unsigned groupLimit = 12, particleLimit = 768;
    constexpr u8 ungrouped = 0xff, culledGroup = 0xfe;
    static u8 groupOf[particleLimit];
    Group groups[groupLimit];
    unsigned groupCount = 0;
    const bool grouped = (trackEnv || trackTex) && list.getNum() <= particleLimit;
    if (grouped) {
        unsigned index = 0;
        for (JPANode<JPABaseParticle>* n = node; n != list.getEnd(); n = step(n)) {
            if (offScreen(n->getObject())) {
                groupOf[index++] = culledGroup;
                ctx.culled++;
                count++;
                continue;
            }
            work->mpCurNode = n;
            for (int i = funcNum - 1; i >= 1; i--) {
                (*funcs[i])(work, n->getObject(), &ctx);
            }
            const int tex = trackTex ? ctx.texIdx : -1;
            unsigned g = 0;
            for (; g < groupCount; g++) {
                if (groups[g].tex == tex &&
                    (!trackEnv || !(drift(ctx.clr1.r, groups[g].env.r, 24) || drift(ctx.clr1.g, groups[g].env.g, 24) ||
                                    drift(ctx.clr1.b, groups[g].env.b, 24)))) {
                    break;
                }
            }
            if (g == groupCount) {
                if (groupCount < groupLimit) {
                    groups[groupCount++] = {ctx.clr1, tex};
                } else {
                    g = ungrouped;
                }
            }
            groupOf[index++] = u8(g);
        }
        for (unsigned g = 0; g < groupCount; g++) {
            if (trackEnv) {
                GXSetTevColor(GX_TEVREG1, groups[g].env);
            }
            if (trackTex) {
                work->mpResMgr->load(res->getTexIdx(groups[g].tex), GX_TEXMAP0);
            }
            index = 0;
            for (JPANode<JPABaseParticle>* n = node; n != list.getEnd(); n = step(n)) {
                if (groupOf[index++] != g) {
                    continue;
                }
                work->mpCurNode = n;
                for (int i = funcNum - 1; i >= 0; i--) {
                    (*funcs[i])(work, n->getObject(), &ctx);
                }
                count++;
            }
            closeDraw();
        }
    }

    // Particles left over (more groups than the limit), or an emitter that
    // needs no grouping: list order, a new draw at each change.
    bool envKnown = false;
    GXColor env = {0, 0, 0, 0};
    unsigned index = 0;
    for (; node != list.getEnd(); node = step(node)) {
        if (grouped && groupOf[index++] != ungrouped) {
            continue;
        }
        work->mpCurNode = node;
        JPABaseParticle* particle = node->getObject();
        if (!grouped && offScreen(particle)) {
            ctx.culled++;
            count++;
            continue;
        }
        for (int i = funcNum - 1; i >= 1; i--) {
            (*funcs[i])(work, particle, &ctx);
        }
        if (trackEnv && (!envKnown || drift(ctx.clr1.r, env.r, 16) || drift(ctx.clr1.g, env.g, 16) ||
                         drift(ctx.clr1.b, env.b, 16))) {
            if (ctx.open) {
                dusk::native_effects::counters.particleBreaks[0]++;
            }
            closeDraw();
            env = ctx.clr1;
            envKnown = true;
            GXSetTevColor(GX_TEVREG1, env);
        }
        if (trackTex && ctx.texIdx != texIdx) {
            if (ctx.open) {
                dusk::native_effects::counters.particleBreaks[1]++;
            }
            closeDraw();
            texIdx = ctx.texIdx;
            work->mpResMgr->load(res->getTexIdx(texIdx), GX_TEXMAP0);
        }
        // The draw function opens the primitive when its quad is on screen.
        (*funcs[0])(work, particle, &ctx);
        count++;
    }
    closeDraw();
    dusk::native_effects::counters.particleBatched += count - ctx.culled;
    dusk::native_effects::counters.particleCulled += ctx.culled;

    // Back to the per-particle layout the rest of the emitter loop expects.
    if (clipOff) {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetCurrentMtx(GX_PNMTX0);
    if (changedTexGen) {
        if (projected) {
            GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX0);
        } else {
            GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
        }
    }
    // The indirect stage comes back with the next setPTev, which precedes
    // every particle loop of this resource.
    if (useClr0) {
        sNativeTevDirty = true;
    }
    return true;
}
#endif

void JPAResource::drawP(JPAEmitterWorkData* work) {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    work->mpEmtr->clearStatus(0x80);

    work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * pBsp->getBaseSizeX();
    work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * pBsp->getBaseSizeY();

    if (pBsp->getType() == 0) {
        work->mGlobalPtclScl.x *= 1.02f;
    } else if (pBsp->getType() == 1) {
        work->mGlobalPtclScl.x *= 1.02f;
        work->mGlobalPtclScl.y *= 0.4f;
    }

    if (pEsp != NULL && pEsp->isEnableScaleAnm()) {
        work->mPivot.x = pEsp->getScaleCenterX() - 1.0f;
        work->mPivot.y = pEsp->getScaleCenterY() - 1.0f;
    } else {
        work->mPivot.x = work->mPivot.y = 0.0f;
    }

    work->mDirType = pBsp->getDirType();
    work->mRotType = pBsp->getRotType();
    work->mDLType = pBsp->getType() == 4 || pBsp->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : pBsp->getBasePlaneType();
    work->mPrjType = pBsp->isPrjTex() ? (pBsp->isTexCrdAnm() ? 2 : 1) : 0;
    
    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclBase;
    setPTev();

    for (int i = mpDrawEmitterFuncListNum - 1; i >= 0; i--) {
        (*mpDrawEmitterFuncList[i])(work);
    }

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
    if (draw_particle_batch(work)) {
        GXSetMisc(GX_MT_XF_FLUSH, 0);
        if (work->mpEmtr->mpEmtrCallBack != nullptr) {
            work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
        }
        return;
    }

    ParticleDrawCtx ctx{}; // immediate mode
#elif defined(DUSK_TARGET_3DS)
    dusk::native_effects::counters.particleEmitters++;
    if (native_draw_particle_batch(work, false)) {
        GXSetMisc(GX_MT_XF_FLUSH, 0);
        if (work->mpEmtr->mpEmtrCallBack != nullptr) {
            work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
        }
        return;
    }
    dusk::native_effects::counters.particleSingle += work->mpEmtr->mAlivePtclBase.getNum();
    if (sNativeTevDirty) {
        GXSetNumChans(0);
        pBsp->setGX(work);
        sNativeTevDirty = false;
    }

    ParticleDrawCtx ctx{}; // original particle attributes, no presentation history
#endif

    if (pBsp->isDrawFwdAhead()) {
        JPANode<JPABaseParticle>* node = work->mpEmtr->mAlivePtclBase.getLast();
        for (; node != work->mpEmtr->mAlivePtclBase.getEnd(); node = node->getPrev()) {
            work->mpCurNode = node;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
            JPABaseParticle scratch;
            JPABaseParticle* particle = dusk::interp::particle::present(node->getObject(), scratch, ctx.age);
#endif
            if (mpDrawParticleFuncList != NULL) {
                for (int i = mpDrawParticleFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleFuncList[i])(work, DUSK_DESKTOP_ORIGINAL(particle, node->getObject()) JPA_DRAW_CTX_ARG);
                }
            }
        }
    } else {
        JPANode<JPABaseParticle>* node = work->mpEmtr->mAlivePtclBase.getFirst();
        for (; node != work->mpEmtr->mAlivePtclBase.getEnd(); node = node->getNext()) {
            work->mpCurNode = node;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
            JPABaseParticle scratch;
            JPABaseParticle* particle = dusk::interp::particle::present(node->getObject(), scratch, ctx.age);
#endif
            if (mpDrawParticleFuncList != NULL) {
                for (int i = mpDrawParticleFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleFuncList[i])(work, DUSK_DESKTOP_ORIGINAL(particle, node->getObject()) JPA_DRAW_CTX_ARG);
                }
            }
        }
    }

    GXSetMisc(GX_MT_XF_FLUSH, 0);

    if (work->mpEmtr->mpEmtrCallBack != NULL) {
        work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
    }
}

void JPAResource::drawC(JPAEmitterWorkData* work) {
#if !defined(DUSK_TARGET_3DS)
    ZoneScoped;
#endif
    work->mpEmtr->setStatus(0x80);

    if (pCsp->isScaleInherited()) {
        work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * pBsp->getBaseSizeX();
        work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * pBsp->getBaseSizeY();
    } else {
        work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * pCsp->getScaleX();
        work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * pCsp->getScaleY();
    }

    if (pCsp->getType() == 0) {
        work->mGlobalPtclScl.x *= 1.02f;
    } else if (pCsp->getType() == 1) {
        work->mGlobalPtclScl.x *= 1.02f;
        work->mGlobalPtclScl.y *= 0.4f;
    }

    work->mPivot.x = work->mPivot.y = 0.0f;

    work->mDirType = pCsp->getDirType();
    work->mRotType = pCsp->getRotType();
    work->mDLType = pCsp->getType() == 4 || pCsp->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : pCsp->getBasePlaneType();
    work->mPrjType = 0;
    
    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclChld;
    setCTev(work);

    for (int i = mpDrawEmitterChildFuncListNum - 1; i >= 0; i--) {
        (*mpDrawEmitterChildFuncList[i])(work);
    }

#if defined(DUSK_TARGET_3DS)
    if (native_draw_particle_batch(work, true)) {
        GXSetMisc(GX_MT_XF_FLUSH, 0);
        if (work->mpEmtr->mpEmtrCallBack != NULL) {
            work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
        }
        return;
    }
    dusk::native_effects::counters.particleSingle += work->mpEmtr->mAlivePtclChld.getNum();
    if (sNativeTevDirty) {
        GXSetNumChans(0);
        pBsp->setGX(work);
        sNativeTevDirty = false;
    }
#endif
#if TARGET_PC
    ParticleDrawCtx ctx{}; // immediate mode
#endif

    if (pBsp->isDrawFwdAhead()) {
        JPANode<JPABaseParticle>* node = work->mpEmtr->mAlivePtclChld.getLast();
        for (; node != work->mpEmtr->mAlivePtclChld.getEnd(); node = node->getPrev()) {
            work->mpCurNode = node;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
            JPABaseParticle scratch;
            JPABaseParticle* particle = dusk::interp::particle::present(node->getObject(), scratch, ctx.age);
#endif
            if (mpDrawParticleChildFuncList != NULL) {
                for (int i = mpDrawParticleChildFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleChildFuncList[i])(work, DUSK_DESKTOP_ORIGINAL(particle, node->getObject()) JPA_DRAW_CTX_ARG);
                }
            }
        }
    } else {
        JPANode<JPABaseParticle>* node = work->mpEmtr->mAlivePtclChld.getFirst();
        for (; node != work->mpEmtr->mAlivePtclChld.getEnd(); node = node->getNext()) {
            work->mpCurNode = node;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
            JPABaseParticle scratch;
            JPABaseParticle* particle = dusk::interp::particle::present(node->getObject(), scratch, ctx.age);
#endif
            if (mpDrawParticleChildFuncList != NULL) {
                for (int i = mpDrawParticleChildFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleChildFuncList[i])(work, DUSK_DESKTOP_ORIGINAL(particle, node->getObject()) JPA_DRAW_CTX_ARG);
                }
            }
        }
    }

    GXSetMisc(GX_MT_XF_FLUSH, 0);

    if (work->mpEmtr->mpEmtrCallBack != NULL) {
        work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
    }
}

void JPAResource::setPTev() {
    GXTexCoordID tex_coord = GX_TEXCOORD1;
    u8 tev_stages = 1;
    u8 tex_gens = 1;
    u8 ind_stages = 0;

    int base_plane_type = (pBsp->getType() == 3 || pBsp->getType() == 7) ?
        pBsp->getBasePlaneType() : 0;
    int center_offset = pEsp != NULL ? (pEsp->getScaleCenterX() + 3 * pEsp->getScaleCenterY()) * 0xC : 0x30;
    int pos_offset = center_offset + base_plane_type * 0x6C;
    int crd_offset = (pBsp->getTilingS() + 2 * pBsp->getTilingT()) * 8;
    GXSETARRAY(GX_VA_POS, jpa_pos + pos_offset, ARRAY_SIZEU(jpa_pos) - pos_offset, 3, true);
    GXSETARRAY(GX_VA_TEX0, jpa_crd + crd_offset, ARRAY_SIZEU(jpa_crd) - crd_offset, 2, true);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);

    if (pEts != NULL) {
        if (pEts->isUseIndirect()) {
            GXSetIndTexOrder(GX_INDTEXSTAGE0, tex_coord, GX_TEXMAP2);
            GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
            f32 indMtx[6];
            std::memcpy(indMtx, pEts->getIndTexMtx(), sizeof(indMtx));
            be_swap(indMtx);
            GXSetIndTexMtx(GX_ITM_0, indMtx, pEts->getExpScale());
            GXSetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0,
                             GX_ITW_OFF, GX_ITW_OFF, 0, 0, GX_ITBA_OFF);
            ind_stages++;
            tex_gens++;
            tex_coord = GX_TEXCOORD2;
        }
        if (pEts->isUseSecTex()) {
            GXSetTevOrder(GX_TEVSTAGE1, tex_coord, GX_TEXMAP3, GX_COLOR_NULL);
            GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
            GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                            GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                            GX_TEVPREV);
            tev_stages++;
            tex_gens++;
        }
    }

    GXSetNumTevStages(tev_stages);
    GXSetNumIndStages(ind_stages);
    if (pBsp->isClipOn()) {
        GXSetMisc(GX_MT_XF_FLUSH, 8);
        GXSetClipMode(GX_CLIP_ENABLE);
    } else {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXSetNumTexGens(tex_gens);
}

void JPAResource::setCTev(JPAEmitterWorkData* work) {
    int base_plane_type = (pCsp->getType() == 3 || pCsp->getType() == 7) ?
        pCsp->getBasePlaneType() : 0;
    int pos_offset = 0x30 + base_plane_type * 0x6C;
    GXSETARRAY(GX_VA_POS, jpa_pos + pos_offset, ARRAY_SIZEU(jpa_pos) - pos_offset, 3, true);
    GXSETARRAY(GX_VA_TEX0, jpa_crd, ARRAY_SIZEU(jpa_crd), 2, true);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR_NULL);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3C);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetNumTevStages(1);
    GXSetNumIndStages(0);
    if (pCsp->isClipOn()) {
        GXSetMisc(GX_MT_XF_FLUSH, 8);
        GXSetClipMode(GX_CLIP_ENABLE);
    } else {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXSetNumTexGens(1);
    work->mpResMgr->load(work->mpRes->getTexIdx(pCsp->getTexIdx()), GX_TEXMAP1);
}

void JPAResource::calc_p(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    if (mpCalcParticleFuncList != NULL) {
        for (int i = mpCalcParticleFuncListNum - 1; i >= 0; i--) {
            (*mpCalcParticleFuncList[i])(work, ptcl);
        }
    }
}

void JPAResource::calc_c(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    if (mpCalcParticleChildFuncList != NULL) {
        for (int i = mpCalcParticleChildFuncListNum - 1; i >= 0; i--) {
            (*mpCalcParticleChildFuncList[i])(work, ptcl);
        }
    }
}

void JPAResource::calcField(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    for (int i = fldNum - 1; i >= 0; i--) {
        ppFld[i]->calc(work, ptcl);
    }
}

void JPAResource::calcKey(JPAEmitterWorkData* work) {
    for (int i = keyNum - 1; i >= 0; i--) {
        f32 val = ppKey[i]->calc(work->mpEmtr->mTick);
        switch (ppKey[i]->getID()) {
        case 0:
            work->mpEmtr->mRate = val;
            break;
        case 1:
            work->mpEmtr->mVolumeSize = val;
            break;
        case 3:
            work->mpEmtr->mVolumeMinRad = val;
            break;
        case 4:
            work->mpEmtr->mLifeTime = val;
            break;
        case 6:
            work->mpEmtr->mAwayFromCenterSpeed = val;
            break;
        case 7:
            work->mpEmtr->mAwayFromAxisSpeed = val;
            break;
        case 8:
            work->mpEmtr->mDirSpeed = val;
            break;
        case 9:
            work->mpEmtr->mSpread = val;
            break;
        case 10:
            work->mpEmtr->mScaleOut = val;
            break;
        default:
            JUT_WARN(917, "%s", "JPA : WRONG ID in key data\n");
            break;
        }
    }
}

void JPAResource::calcWorkData_c(JPAEmitterWorkData* work) {
    work->mVolumeSize = work->mpEmtr->mVolumeSize;
    work->mVolumeMinRad = work->mpEmtr->mVolumeMinRad;
    work->mVolumeSweep = work->mpEmtr->mVolumeSweep;
    work->mVolumeAngleNum = work->mVolumeX = 0;
    work->mVolumeAngleMax = 1;
    work->mDivNumber = pDyn->getDivNumber() * 2 + 1;
    Mtx local_scl_mtx, local_rot_mtx, global_mtx;
    MTXScale(local_scl_mtx, work->mpEmtr->mLocalScl.x, work->mpEmtr->mLocalScl.y,
             work->mpEmtr->mLocalScl.z);
    JPAGetXYZRotateMtx(work->mpEmtr->mLocalRot.x * 182, work->mpEmtr->mLocalRot.y * 182,
                       work->mpEmtr->mLocalRot.z * 182, local_rot_mtx);
    MTXScale(global_mtx, work->mpEmtr->mGlobalScl.x, work->mpEmtr->mGlobalScl.y,
             work->mpEmtr->mGlobalScl.z);
    MTXConcat(work->mpEmtr->mGlobalRot, global_mtx, global_mtx);
    global_mtx[0][3] = work->mpEmtr->mGlobalTrs.x;
    global_mtx[1][3] = work->mpEmtr->mGlobalTrs.y;
    global_mtx[2][3] = work->mpEmtr->mGlobalTrs.z;
    MTXCopy(work->mpEmtr->mGlobalRot, work->mRotationMtx);
    MTXConcat(work->mRotationMtx, local_rot_mtx, work->mGlobalRot);
    MTXConcat(work->mGlobalRot, local_scl_mtx, work->mGlobalSR);
    work->mEmitterPos.set(work->mpEmtr->mLocalTrs);
    work->mGlobalScl.mul(work->mpEmtr->mGlobalScl, work->mpEmtr->mLocalScl);
    JPAGetDirMtx(work->mpEmtr->mLocalDir, work->mDirectionMtx);
    work->mPublicScale.set(work->mpEmtr->mGlobalScl);
    MTXMultVec(global_mtx, &work->mpEmtr->mLocalTrs, &work->mGlobalPos);
}

void JPAResource::calcWorkData_d(JPAEmitterWorkData* work) {
    Mtx mtx;
    JPAGetXYZRotateMtx(work->mpEmtr->mLocalRot.x * 0xB6, work->mpEmtr->mLocalRot.y * 0xB6,
                       work->mpEmtr->mLocalRot.z * 0xB6, mtx);
    MTXConcat(work->mpEmtr->mGlobalRot, mtx, work->mGlobalRot);
    MTXMultVecSR(work->mGlobalRot, &work->mpEmtr->mLocalDir, &work->mGlobalEmtrDir);
}

#pragma push
#pragma force_active on
static u8 jpa_resource_padding[28] = {0};
#pragma pop
