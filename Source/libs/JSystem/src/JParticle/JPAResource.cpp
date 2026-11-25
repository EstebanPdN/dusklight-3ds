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
#include "../../../../platforms/3ds/runtime/clock.hpp"
#include "../../../../platforms/3ds/runtime/gx_texture_commands.hpp"
#include "../../../../platforms/3ds/runtime/gx_worker_sync.hpp"
#include "../../../../platforms/3ds/runtime/gx_copy_images.hpp"
#include "JSystem/JParticle/JPANativeCull.h"
#include "JSystem/JParticle/JPATexture.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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

// const (3DS port, Alpha 15): see jpa_dl in JPABaseShape.cpp.
ATTRIBUTE_ALIGN(32) static const u8 jpa_pos[324] = {
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

ATTRIBUTE_ALIGN(32) static const u8 jpa_crd[32] = {
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

namespace dusk::native_effects {
// Frame-copy ("dummy") particles: 0 = left out (Alpha 6); 1 = those the
// original draws without a warp; 2 = also the warped ones, with a sway in
// place of the warp (default). At 1 and 2 they are drawn only in frames where
// another draw (the refracting water) has already sampled the frame copy, so
// they never cause a copy themselves. 3 = always: the copy is then made every
// frame they are on screen (Alpha 6 console, Lakebed: 1.2 ms a frame against
// 0.06 deferred). The platform may set it from the SD card before the game starts.
int refraction = 2;
// Masked particles batched with a projected or per-particle-matrix main
// texture: 1 = the mask reads the particle's own coordinates, as the
// original's identity texgen does (default); 0 = the mask follows the main
// texture's coordinates, as in Alpha 6 (A/B only; refraction particles keep 1).
int particleMask = 1;
}

// How a frame-copy resource is drawn here. On the disc 130 resources use the
// frame copy: 37 sample it without an indirect warp (glow streaks, the wolf
// senses overlay); the PICA draws those as the original does. 93 warp it
// through an indirect texture the PICA cannot read per pixel; nearly all of
// those are projected billboards masked by a soft secondary alpha texture
// (ripples, splashes, small heat shimmers). They are drawn as their quad split
// into four triangles whose shared centre samples the copy with a swaying
// offset (emit_batch_quad, JPABaseShape.cpp). The rest stays out: stripes
// (no centre to sway), the screen-wide heat haze (no mask; the screen-effect
// pass sways the frame instead) and particles a callback draws.
struct NativeRefraction {
    enum Kind { none, plain, sway, leftOut };
    Kind kind;
    const char* reason; // leftOut only
    f32 swayS, swayT;   // sway only: largest shift, as a fraction of the copy
};
static NativeRefraction sNativeRefraction;
static const char sNoFrameCopy[] = "frame copy not made this frame";

// Since the last profiled frame: refraction particles drawn with the sway,
// drawn as the original, left out (and of those, left out only because no
// other draw had made the frame copy); batched particles whose mask stage
// reads its own coordinates (see emit_batch_quad).
static struct {
    unsigned swayed, plain, leftOut, noCopy, maskCoords, conditional;
} sNativeRefractionCounts;

// The capture the particles sample has already been copied this frame: a
// draw before them (the refracting water) sampled it, which makes a deferred
// copy. Sampling it first would make the copy here and, through the 30-frame
// window of gx_copy_execution.cpp, keep every later frame's copy eager.
//
// The use of the capture is noted when the water is drawn, not when the game
// issues it: with the second-core worker on the water may still be queued
// (gx_worker.hpp), so what is queued is drawn first. Without that the answer
// was "not made" and the ripples around Link stayed out (Ordon Spring).
static bool native_frame_copy_made() {
    namespace gx = dusk::native::gx;
    // Once the copy is known to be made, it stays made for the rest of the
    // frame and the same capture: later refraction resources ask without a
    // sync (each sync had the game thread draw what the worker still held:
    // 217 syncs and 7,557 shapes in 300 pictures, Azahar, D_SB01). A copy
    // issued since then (counted on this thread when it is queued; one drawn
    // here moves the slot) asks again with a sync.
    static std::uint32_t madeFrame, madeToken;
    static unsigned madeSlot;
    static std::uint64_t madeCopies;
    const std::uint64_t copies = dusk::native::queuedCopyTextures();
    if (madeFrame != 0 && madeFrame == gx::textureFrameCounter && madeToken == gx::watchedCaptureToken &&
        madeSlot == gx::watchedCaptureSlot && madeCopies == copies) {
        return true;
    }
    dusk::native::syncGxWorker();
    const bool made = gx::watchedCaptureUsedThisFrame();
    if (made) {
        madeFrame = gx::textureFrameCounter;
        madeToken = gx::watchedCaptureToken;
        madeSlot = gx::watchedCaptureSlot;
        madeCopies = copies;
    }
    return made;
}

// `conditional`: the answer to "frame copy made?" is left to the thread that
// draws (a conditional section, gx_worker_sync.hpp): the resource is drawn
// as if it was made, and its draws are dropped in their place in the queue
// when it was not. The caller ends the section after the resource's draws.
// Only for emitters whose callbacks draw nothing: the callbacks of a resource
// left out still run (draw() below), and their draws must not be dropped
// with it (dPa_followEcallBack and dPa_simpleEcallBack, the ones that follow
// an actor, draw nothing; a particle callback that draws is left out anyway).
static NativeRefraction native_refraction_plan(JPAEmitterWorkData* work, JPABaseEmitter* emtr, bool& conditional) {
    conditional = false;
    if (!native_refraction_resource(work)) {
        return {NativeRefraction::none, nullptr, 0.0f, 0.0f};
    }
    JPAResource* res = work->mpRes;
    JPAExTexShape* ets = res->getEts();
    const int mode = dusk::native_effects::refraction;
    if (mode <= 0) {
        return {NativeRefraction::leftOut, "switch", 0.0f, 0.0f};
    }
    NativeRefraction plan = {NativeRefraction::plain, nullptr, 0.0f, 0.0f};
    if (ets != NULL && ets->isUseIndirect()) {
        const char* reason = mode < 2 ? "warp (switch)" :
            dusk::native_effects::particles < 2 ? "batches off" :
            !res->mBatchInfo.supported ? "shape" :
            !ets->isUseSecTex() ? "no mask (screen-wide)" :
            !res->getBsp()->isPrjTex() ? "not projected" :
            res->mBatchInfo.hasPtclTex ? "texture animation" :
            emtr->mpPtclCallBack != NULL && emtr->mpPtclCallBack->drawsParticle() ? "particle callback" :
            nullptr;
        if (reason != NULL) {
            return {NativeRefraction::leftOut, reason, 0.0f, 0.0f};
        }
        // The original offset is the indirect matrix times the warp sample
        // (-128..127) times 2^scale, in texels of the 304x224 copy. The heat
        // haze sways by 0.38 of that peak. Here only the centre moves and the
        // shift falls linearly to the border, a root mean square of
        // centre/sqrt(6) over the quad; 0.93 of the peak at the centre gives
        // the haze's 0.38.
        const BE(f32)* m = ets->getIndTexMtx();
        const f32 rowS = std::fabs(f32(m[0])) + std::fabs(f32(m[1])) + std::fabs(f32(m[2]));
        const f32 rowT = std::fabs(f32(m[3])) + std::fabs(f32(m[4])) + std::fabs(f32(m[5]));
        const f32 peak = std::ldexp(127.0f * 0.93f, ets->getExpScale());
        plan = {NativeRefraction::sway, nullptr, rowS * peak / 304.0f, rowT * peak / 224.0f};
    }
    if (mode < 3) {
        if ((emtr->mpEmtrCallBack == NULL || emtr->mpEmtrCallBack->drawsNothing()) &&
            (emtr->mpPtclCallBack == NULL || !emtr->mpPtclCallBack->drawsParticle()) &&
            dusk::native::beginGxSection(dusk::native::gxSectionFrameCopyMade)) {
            conditional = true;
            sNativeRefractionCounts.conditional++;
            return plan;
        }
        if (!native_frame_copy_made()) {
            return {NativeRefraction::leftOut, sNoFrameCopy, 0.0f, 0.0f};
        }
    }
    return plan;
}

// One log line per resource and outcome, the first 48 per particle scene
// (reset by JPANativeParticleLogReset), so late zones of a long tour still
// print theirs. Batched resources whose mask reads TEX1: the first 24.
static struct { u16 id; u8 kind; } sRefractionSeen[48];
static unsigned sRefractionSeenCount;
static u16 sMaskSeen[24];
static unsigned sMaskSeenCount;

// Called by dPa_control_c::removeRoomScene, once per scene change.
void JPANativeParticleLogReset() {
    sRefractionSeenCount = 0;
    sMaskSeenCount = 0;
}

// Called with the outcome: plain and left out from draw(), the sway from
// drawP() once the batch has drawn or refused it.
static void native_refraction_report(JPAResource* res, const NativeRefraction& plan) {
    // Waiting for the frame copy is its own outcome: the same resource is
    // usually drawn later in the scene, once the water is in view.
    const u8 key = plan.reason == sNoFrameCopy ? 4 : u8(plan.kind);
    for (unsigned i = 0; i < sRefractionSeenCount; i++) {
        if (sRefractionSeen[i].id == res->getUsrIdx() && sRefractionSeen[i].kind == key) {
            return;
        }
    }
    if (sRefractionSeenCount >= sizeof(sRefractionSeen) / sizeof(*sRefractionSeen)) {
        return;
    }
    sRefractionSeen[sRefractionSeenCount++] = {res->getUsrIdx(), key};
    if (plan.kind == NativeRefraction::sway) {
        OSReport("Native refraction particles id=%04x: drawn with a sway of %.4f,%.4f of the frame copy\n",
                 unsigned(res->getUsrIdx()), plan.swayS, plan.swayT);
    } else if (plan.kind == NativeRefraction::plain) {
        OSReport("Native refraction particles id=%04x: drawn as the original (no warp)\n",
                 unsigned(res->getUsrIdx()));
    } else {
        OSReport("Native refraction particles id=%04x: left out (%s)\n", unsigned(res->getUsrIdx()),
                 plan.reason);
    }
}

// Profiled frame only: time spent in the batches' GXEnd (the GX translation
// of their quads) and the particles they carried. A resource's remaining time
// (EmitterRow) is the draw functions, the corner transforms and the GX
// writes. This sizes a sprite sink that would write PICA vertices directly
// (Alpha 7 plan, A20).
static struct {
    unsigned long long endTicks;
    unsigned ends, particles;
} sNativeBatchTiming;

// Native emitter loop (JPANativeDrawGroup), since the last profiled frame:
// emitters left out (nothing on screen; of those, by particle-size-cull.txt
// only), emitter runs and the emitters they carried, emitters drawn on their
// own or in a run.
static struct {
    unsigned culled, small, runs, runEmitters, drawn;
} sNativeLoopCounts;

// The platform prints its report at the end of the profiled frame; these
// lines follow at the first resource drawn after that frame, so they need no
// platform hook. Quiet when there is nothing to say.
static void native_particle_report_after_profiled_frame() {
    static bool profiled;
    if (dusk::native_effects::emitterReport) {
        profiled = true;
        return;
    }
    if (!profiled) {
        return;
    }
    profiled = false;
    if (sNativeBatchTiming.ends != 0) {
        OSReport("Native particle batch GXEnd (profiled frame): %.3f ms in %u draws for %u particles\n",
                 double(sNativeBatchTiming.endTicks) * 1000.0 / double(dusk::native::nativeTickFrequency),
                 sNativeBatchTiming.ends, sNativeBatchTiming.particles);
    }
    sNativeBatchTiming = {};
    const auto& c = sNativeRefractionCounts;
    if (c.swayed != 0 || c.plain != 0 || c.leftOut != 0 || c.maskCoords != 0 || c.conditional != 0) {
        // conditional: resources whose "frame copy made?" was left to the
        // thread that draws (the GX worker line counts the ones it dropped).
        OSReport("Native refraction particles since last report: swayed=%u as-original=%u left-out=%u "
                 "(no frame copy yet=%u); batched masks reading TEX1=%u; conditional resources=%u\n",
                 c.swayed, c.plain, c.leftOut, c.noCopy, c.maskCoords, c.conditional);
    }
    sNativeRefractionCounts = {};
    const auto& e = sNativeLoopCounts;
    if (e.culled != 0 || e.drawn != 0) {
        OSReport("Native particle emitters since last report: drawn=%u (%u of them in %u shared batches) "
                 "left out with nothing on screen=%u (too small=%u)\n",
                 e.drawn, e.runEmitters, e.runs, e.culled, e.small);
    }
    sNativeLoopCounts = {};
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
    native_particle_report_after_profiled_frame();
    // A refraction resource samples the frame copy, most of them through an
    // indirect warp the PICA does not have (see NativeRefraction).
    bool conditional;
    const NativeRefraction refraction = native_refraction_plan(work, emtr, conditional);
    const unsigned refractionParticles =
        refraction.kind == NativeRefraction::none ? 0u :
        unsigned(emtr->mAlivePtclBase.getNum() + emtr->mAlivePtclChld.getNum());
    // The sway's outcome is known only after its batch (drawP).
    if ((refraction.kind == NativeRefraction::plain || refraction.kind == NativeRefraction::leftOut) &&
        refractionParticles != 0) {
        native_refraction_report(this, refraction);
    }
    // Profiled frame: time and count this resource (see EmitterRow; flags
    // bit 18: refraction left out, bit 19: refraction drawn with the sway).
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
                         ((pBsp->pBsd->mBlendModeCfg & 3u) << 16) |
                         (refraction.kind == NativeRefraction::leftOut ? 1u << 18 : 0u) |
                         (refraction.kind == NativeRefraction::sway ? 1u << 19 : 0u);
        }
        if (row != nullptr) {
            row->emitters++;
            row->particles += emtr->mAlivePtclBase.getNum() + emtr->mAlivePtclChld.getNum();
            rowBatches = counters.particleBatches;
            rowSingles = counters.particleSingle;
            rowStart = svcGetSystemTick();
        }
    }
    if (refraction.kind == NativeRefraction::leftOut) {
        // "refraction=" in the particle report: particles left out, as in Alpha 6.
        dusk::native_effects::counters.particleRefraction += refractionParticles;
        sNativeRefractionCounts.leftOut += refractionParticles;
        if (refraction.reason == sNoFrameCopy) {
            sNativeRefractionCounts.noCopy += refractionParticles;
        }
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->draw(emtr);
        }
        // draw() may detach the callback from the emitter.
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->drawAfter(emtr);
        }
        return;
    }
    if (refraction.kind == NativeRefraction::plain) {
        sNativeRefractionCounts.plain += refractionParticles;
    }
    // drawP's batch reads it; nothing below draws another resource.
    sNativeRefraction = refraction;
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
    sNativeRefraction = {};
    if (conditional) {
        dusk::native::endGxSection();
    }
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
void JPANativeBatchExtra(bool rawUv, bool sway, f32 swayS, f32 swayT); // JPABaseShape.cpp

static GXTevColorArg native_batch_color_arg(GXTevColorArg arg) {
    return arg == GX_CC_C0 ? GX_CC_RASC : arg == GX_CC_A0 ? GX_CC_RASA : arg;
}

static GXTevAlphaArg native_batch_alpha_arg(GXTevAlphaArg arg) {
    return arg == GX_CA_A0 ? GX_CA_RASA : arg;
}

bool JPANativeDrawKeepsBaseAxis(JPAResource::DrawParticleFunc func); // JPABaseShape.cpp
void JPANativeKeepBaseAxis(JPAEmitterWorkData* work, JPABaseParticle* ptcl); // JPABaseShape.cpp

// One batch: the GX state of one resource's particles, set once, and the
// draws its particles go into. A batch normally carries one emitter's list;
// an emitter run (JPANativeDrawGroup) feeds it the lists of several emitters
// of the same resource in turn, so the open primitive and the bound
// environment colour and texture carry over from one emitter to the next.
struct NativeBatch {
    bool child, sway, projected, useClr0, trackEnv, trackTex, changedTexGen, rawUv, clipOff;
    // The draw function also steps the particle's base axis (direction
    // shapes): a particle left out off screen still takes that step.
    bool keepsBaseAxis;
    GXTexCoordID maskCoord;
    ParticleDrawCtx ctx;
    jpa_native::CullPlanes cull;
    f32 quadRadius;
    unsigned count;  // particles walked, those left out included
    bool envKnown;   // TEV register 1 holds `env`
    GXColor env;
    int texIdx;      // texture the batch bound to GX_TEXMAP0, -1 for the emitter's own
};

// Whether a batch takes `particles` particles of the current emitter's base
// or child list; the reason is counted when it refuses several.
static bool native_batch_accepts(JPAEmitterWorkData* work, bool child, unsigned particles) {
    JPAResource* res = work->mpRes;
    JPABaseEmitter* emtr = work->mpEmtr;
    const JPAResource::BatchInfo& info = child ? res->mChildBatchInfo : res->mBatchInfo;
    // Refraction with a sway is drawn only here, even one particle: the
    // per-particle route would warp it through the translator's CPU path.
    const bool sway = !child && sNativeRefraction.kind == NativeRefraction::sway;
    // A single particle gains nothing from the state change around a batch.
    if (!sway && (dusk::native_effects::particles < 2 || particles < 2)) {
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
        reason += particles;
        return false;
    }
    if (info.hasPtclColor && res->getBsp()->getTevColorArgSel() >= 6) {
        dusk::native_effects::counters.particleUnbatched[3] += particles;
        return false;
    }
    return true;
}

static void native_batch_close(ParticleDrawCtx& ctx) {
    if (!ctx.open) {
        return;
    }
    if (dusk::native_effects::emitterReport) {
        const unsigned long long start = svcGetSystemTick();
        GXEnd();
        sNativeBatchTiming.endTicks += svcGetSystemTick() - start;
        sNativeBatchTiming.ends++;
    } else {
        GXEnd();
    }
    ctx.open = false;
    dusk::native_effects::counters.particleBatches++;
}

// The batch's GX state, after native_batch_accepts. The emitter's own state
// (setGX, setPTev and the emitter draw functions) is already set.
static void native_batch_begin(JPAEmitterWorkData* work, bool child, NativeBatch& b) {
    JPAResource* res = work->mpRes;
    const JPAResource::BatchInfo& info = child ? res->mChildBatchInfo : res->mBatchInfo;
    JPABaseShape* shape = res->getBsp();
    const u32 colorSel = shape->getTevColorArgSel();
    b = {};
    b.child = child;
    b.sway = !child && sNativeRefraction.kind == NativeRefraction::sway;
    b.projected = !child && work->mPrjType != 0;
    b.useClr0 = info.hasPtclColor;
    // Only these two colour formulas read the environment register.
    b.trackEnv = b.useClr0 && (colorSel == 3 || colorSel == 4);
    b.trackTex = !child && info.hasPtclTex;
    b.texIdx = -1;

    if (b.useClr0) {
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
    }
    b.changedTexGen = !child && (info.hasPtclTexMtx || b.projected);
    if (b.changedTexGen) {
        // UVs are transformed here; drop the texture matrix.
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    }
    // The secondary (mask) texture reads the particle's own coordinates
    // (JPALoadExTex: TEX0 through the identity). When the batch rewrites TEX0
    // they travel in TEX1 instead; its coordinate follows the indirect one.
    // particleMask 0 keeps Alpha 6's mask for A/B pictures; refraction
    // particles, never drawn in Alpha 6, always take the original's.
    b.rawUv = b.changedTexGen && ets != nullptr && ets->isUseSecTex() &&
              (dusk::native_effects::particleMask > 0 || sNativeRefraction.kind != NativeRefraction::none);
    b.maskCoord = ets != nullptr && ets->isUseIndirect() ? GX_TEXCOORD2 : GX_TEXCOORD1;
    if (b.rawUv) {
        GXSetTexCoordGen(b.maskCoord, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
        // The first 24 resources per particle scene, once each.
        bool known = false;
        for (unsigned i = 0; i < sMaskSeenCount && !known; i++) {
            known = sMaskSeen[i] == res->getUsrIdx();
        }
        if (!known && sMaskSeenCount < sizeof(sMaskSeen) / sizeof(*sMaskSeen)) {
            sMaskSeen[sMaskSeenCount++] = res->getUsrIdx();
            OSReport("Native particle batch id=%04x: mask texture reads the particle's own coordinates (TEX1; %s)\n",
                     unsigned(res->getUsrIdx()), b.projected ? "projected" : "texture matrix");
        }
    }

    // Clipping stays on for the batch. Quads wholly off screen never leave
    // the loop below, and a draw with clipping off is transformed and clipped
    // vertex by vertex on the CPU (20 microseconds a vertex) instead of on
    // the PICA. Only a quad that crosses the near plane is drawn differently:
    // it is clipped there.
    b.clipOff = child ? !res->getCsp()->isClipOn() : !shape->isClipOn();
    if (b.clipOff) {
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
    if (b.useClr0) {
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    if (b.rawUv) {
        GXSetVtxDesc(GX_VA_TEX1, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
    }
    JPANativeBatchExtra(b.rawUv, b.sway, sNativeRefraction.swayS, sNativeRefraction.swayT);

    ParticleDrawCtx& ctx = b.ctx;
    ctx.batch = true;
    ctx.child = child;
    ctx.useTexMtx = !child && info.hasPtclTexMtx;
    ctx.useClr0 = b.useClr0;
    ctx.useClr1 = b.trackEnv;
    ctx.prj = b.projected;
    if (b.projected) {
        JPANativePrjTexMtx(work, ctx.texMtx);
    }

    // Early cull: a particle whose bounding sphere lies outside one side of
    // the view skips its draw functions and corner transforms altogether
    // (a twilight field carried 900 such particles a frame). The sphere is
    // twice the scaled quad, which covers its pivot shift and rotation; the
    // exact per-corner test in the draw function still follows.
    jpa_native::makeCullPlanes(work->mPrjMtx, b.cull);
    b.quadRadius = 0.0f;
    for (int i = 0; i < info.vtxCount; i++) {
        b.quadRadius = std::max(b.quadRadius, std::sqrt(info.vtxPos[i][0] * info.vtxPos[i][0] + info.vtxPos[i][1] * info.vtxPos[i][1] +
                                                        info.vtxPos[i][2] * info.vtxPos[i][2]));
    }
    b.quadRadius *= 2.0f;
    JPAResource::DrawParticleFunc* funcs = child ? res->mpDrawParticleChildFuncList : res->mpDrawParticleFuncList;
    b.keepsBaseAxis = JPANativeDrawKeepsBaseAxis(funcs[0]);
}

// The particles of the current emitter (work->mpEmtr), base or child list.
// The emitter's per-draw work data (scale, directions) is already set.
static void native_batch_particles(JPAEmitterWorkData* work, NativeBatch& b) {
    JPAResource* res = work->mpRes;
    JPABaseEmitter* emtr = work->mpEmtr;
    const bool child = b.child;
    JPAList<JPABaseParticle>& list = child ? emtr->mAlivePtclChld : emtr->mAlivePtclBase;
    ParticleDrawCtx& ctx = b.ctx;
    JPAExTexShape* ets = res->getEts();
    if (!child && !b.sway && ets != nullptr && ets->isUseIndirect()) {
        dusk::native_effects::counters.particleDirect += list.getNum();
    }

    JPAResource::DrawParticleFunc* funcs = child ? res->mpDrawParticleChildFuncList : res->mpDrawParticleFuncList;
    const int funcNum = child ? res->mpDrawParticleChildFuncListNum : res->mpDrawParticleFuncListNum;
    const bool fwdAhead = res->getBsp()->isDrawFwdAhead();
    JPANode<JPABaseParticle>* node = fwdAhead ? list.getLast() : list.getFirst();

    const auto drift = [](u8 a, u8 b, int limit) { return a > b ? a - b > limit : b - a > limit; };
    const auto step = [fwdAhead](JPANode<JPABaseParticle>* n) {
        return fwdAhead ? n->getPrev() : n->getNext();
    };
    const f32 scaleX = std::fabs(work->mGlobalPtclScl.x), scaleY = std::fabs(work->mGlobalPtclScl.y);
    const auto offScreen = [&](const JPABaseParticle* particle) {
        Vec centre;
        MTXMultVec(work->mPosCamMtx, &particle->mPosition, &centre);
        const f32 radius = b.quadRadius * std::max(scaleX * std::fabs(particle->mParticleScaleX),
                                                   scaleY * std::fabs(particle->mParticleScaleY));
        const f32 point[3] = {centre.x, centre.y, centre.z};
        return jpa_native::sphereOutside(b.cull, point, radius);
    };
    // A particle left out still takes the base-axis step of its draw function.
    const auto leaveOut = [&](JPANode<JPABaseParticle>* n) {
        ctx.culled++;
        b.count++;
        if (b.keepsBaseAxis) {
            work->mpCurNode = n;
            JPANativeKeepBaseAxis(work, n->getObject());
        }
    };

    // The environment colour follows each particle's age and the texture its
    // animation frame, and neither can travel with a vertex: the TEV register
    // and the bound texture belong to the draw. Drawn in list order, a fire
    // emitter changed one of them at nearly every particle (242 draws for 279
    // particles). The particles are grouped instead: one draw per texture and
    // per environment colour within 24/255 a channel (9%), groups in order of
    // their first particle. Inside one emitter the order between groups only
    // matters where its particles overlap with a non-additive blend. In an
    // emitter run the state the previous emitter left bound is the first
    // group, for particles whose environment colour is exactly that one: they
    // continue the open draw with the very register value they would set.
    struct Group {
        GXColor env;
        int tex;
    };
    constexpr unsigned groupLimit = 12, particleLimit = 768;
    constexpr u8 ungrouped = 0xff, culledGroup = 0xfe;
    static u8 groupOf[particleLimit];
    Group groups[groupLimit];
    unsigned groupCount = 0;
    const auto sameEnv = [](GXColor a, GXColor b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
    const bool grouped = (b.trackEnv || b.trackTex) && list.getNum() <= particleLimit;
    if (grouped) {
        const bool seeded = ctx.open && (!b.trackEnv || b.envKnown) && (!b.trackTex || b.texIdx >= 0);
        if (seeded) {
            groups[groupCount++] = {b.env, b.trackTex ? b.texIdx : -1};
        }
        unsigned index = 0;
        for (JPANode<JPABaseParticle>* n = node; n != list.getEnd(); n = step(n)) {
            if (offScreen(n->getObject())) {
                groupOf[index++] = culledGroup;
                leaveOut(n);
                continue;
            }
            work->mpCurNode = n;
            for (int i = funcNum - 1; i >= 1; i--) {
                (*funcs[i])(work, n->getObject(), &ctx);
            }
            const int tex = b.trackTex ? ctx.texIdx : -1;
            unsigned g = 0;
            for (; g < groupCount; g++) {
                const bool envFits = !b.trackEnv ||
                    (seeded && g == 0 ? sameEnv(ctx.clr1, groups[g].env)
                                      : !(drift(ctx.clr1.r, groups[g].env.r, 24) || drift(ctx.clr1.g, groups[g].env.g, 24) ||
                                          drift(ctx.clr1.b, groups[g].env.b, 24)));
                if (groups[g].tex == tex && envFits) {
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
            bool started = false;
            index = 0;
            for (JPANode<JPABaseParticle>* n = node; n != list.getEnd(); n = step(n)) {
                if (groupOf[index++] != g) {
                    continue;
                }
                if (!started) {
                    started = true;
                    // A group whose state is already bound continues the
                    // open draw; any other closes it and binds its own.
                    const bool envBound = !b.trackEnv || (b.envKnown && sameEnv(groups[g].env, b.env));
                    const bool texBound = !b.trackTex || groups[g].tex == b.texIdx;
                    if (!envBound || !texBound) {
                        native_batch_close(ctx);
                        if (b.trackEnv) {
                            b.env = groups[g].env;
                            b.envKnown = true;
                            GXSetTevColor(GX_TEVREG1, b.env);
                        }
                        if (b.trackTex) {
                            b.texIdx = groups[g].tex;
                            work->mpResMgr->load(res->getTexIdx(b.texIdx), GX_TEXMAP0);
                        }
                    }
                }
                work->mpCurNode = n;
                for (int i = funcNum - 1; i >= 0; i--) {
                    (*funcs[i])(work, n->getObject(), &ctx);
                }
                b.count++;
            }
        }
    }

    // Particles left over (more groups than the limit), or an emitter that
    // needs no grouping: list order, a new draw at each change. The first of
    // them keeps a register bound before (by a group or the previous emitter
    // of a run) only when its colour is exactly that one.
    bool firstHere = true;
    unsigned index = 0;
    for (; node != list.getEnd(); node = step(node)) {
        if (grouped && groupOf[index++] != ungrouped) {
            continue;
        }
        work->mpCurNode = node;
        JPABaseParticle* particle = node->getObject();
        if (!grouped && offScreen(particle)) {
            leaveOut(node);
            continue;
        }
        for (int i = funcNum - 1; i >= 1; i--) {
            (*funcs[i])(work, particle, &ctx);
        }
        const bool envChanged =
            !b.envKnown || (firstHere ? !sameEnv(ctx.clr1, b.env)
                                      : drift(ctx.clr1.r, b.env.r, 16) || drift(ctx.clr1.g, b.env.g, 16) ||
                                            drift(ctx.clr1.b, b.env.b, 16));
        firstHere = false;
        if (b.trackEnv && envChanged) {
            if (ctx.open) {
                dusk::native_effects::counters.particleBreaks[0]++;
            }
            native_batch_close(ctx);
            b.env = ctx.clr1;
            b.envKnown = true;
            GXSetTevColor(GX_TEVREG1, b.env);
        }
        if (b.trackTex && ctx.texIdx != b.texIdx) {
            if (ctx.open) {
                dusk::native_effects::counters.particleBreaks[1]++;
            }
            native_batch_close(ctx);
            b.texIdx = ctx.texIdx;
            work->mpResMgr->load(res->getTexIdx(b.texIdx), GX_TEXMAP0);
        }
        // The draw function opens the primitive when its quad is on screen.
        (*funcs[0])(work, particle, &ctx);
        b.count++;
    }
}

// Closes the last draw and puts back the per-particle layout the rest of the
// emitter loop expects.
static void native_batch_end(JPAEmitterWorkData* work, NativeBatch& b) {
    ParticleDrawCtx& ctx = b.ctx;
    native_batch_close(ctx);
    JPANativeBatchExtra(false, false, 0.0f, 0.0f);
    dusk::native_effects::counters.particleBatched += b.count - ctx.culled;
    dusk::native_effects::counters.particleCulled += ctx.culled;
    if (dusk::native_effects::emitterReport) {
        sNativeBatchTiming.particles += b.count - ctx.culled;
    }
    if (b.sway) {
        sNativeRefractionCounts.swayed += b.count - ctx.culled;
    } else if (b.rawUv) {
        sNativeRefractionCounts.maskCoords += b.count - ctx.culled;
    }

    if (b.clipOff) {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetCurrentMtx(GX_PNMTX0);
    if (b.changedTexGen) {
        if (b.projected) {
            GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX0);
        } else {
            GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
        }
    }
    if (b.rawUv) {
        GXSetTexCoordGen(b.maskCoord, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    }
    // The indirect stage comes back with the next setPTev, which precedes
    // every particle loop of this resource.
    if (b.useClr0) {
        sNativeTevDirty = true;
    }
}

// The batch is game-thread state: one at a time (drawP, drawC and emitter
// runs never nest).
static NativeBatch sNativeBatch;

static bool native_draw_particle_batch(JPAEmitterWorkData* work, bool child) {
    JPAList<JPABaseParticle>& list = child ? work->mpEmtr->mAlivePtclChld : work->mpEmtr->mAlivePtclBase;
    if (!native_batch_accepts(work, child, list.getNum())) {
        return false;
    }
    native_batch_begin(work, child, sNativeBatch);
    native_batch_particles(work, sNativeBatch);
    native_batch_end(work, sNativeBatch);
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
    const bool swayPlanned = sNativeRefraction.kind == NativeRefraction::sway;
    const bool batched = native_draw_particle_batch(work, false);
    if (swayPlanned && work->mpEmtr->mAlivePtclBase.getNum() != 0) {
        native_refraction_report(this, batched ? sNativeRefraction :
                                 NativeRefraction{NativeRefraction::leftOut, "batch refused", 0.0f, 0.0f});
    }
    if (batched) {
        GXSetMisc(GX_MT_XF_FLUSH, 0);
        if (work->mpEmtr->mpEmtrCallBack != nullptr) {
            work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
        }
        return;
    }
    if (swayPlanned) {
        // The batch refused it (a texture matrix on a projected texture, a
        // colour formula it does not know): left out as before, never through
        // the per-particle indirect route.
        const unsigned particles = work->mpEmtr->mAlivePtclBase.getNum();
        dusk::native_effects::counters.particleRefraction += particles;
        sNativeRefractionCounts.leftOut += particles;
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

#if defined(DUSK_TARGET_3DS)
// ---------------------------------------------------------------------------
// Native emitter loop (JPAEmitterManager::draw on the 3DS).
//
// Each emitter used to set its whole GX state (setGX, setPTev, the emitter
// draw functions, the batch layout and its restore: about sixty GX calls,
// some 0.1 ms on the console) before its first particle, and to end in its
// own draw (some 0.2 ms each in the translator). Two things change here,
// both before any GX call:
//  1. An emitter none of whose particles can reach the picture (every
//     bounding sphere off screen, the same test the batch applies particle
//     by particle) sets no state at all. Calculation is untouched: lifetimes,
//     the shared random generator and the callbacks run as before (this loop
//     only draws). Its particles still take the base-axis step of their draw
//     function.
//  2. Emitters of one resource whose emitter-level state is the same (same
//     texture frame, same TEV register colours) are drawn as one batch: the
//     state is set once and the particles of all of them go into the same
//     draws. To bring them together, an emitter may be drawn before others
//     that came earlier in the list only where its screen footprint does not
//     meet theirs (JPANativeCull.h): blending, the depth test and depth
//     writes act per pixel, so the picture is the same. Emitters whose
//     footprint is unknown (emitter callbacks, particle callbacks that draw,
//     stripes, points, lines, quads reaching the eye plane, frame-copy
//     refraction) are drawn as before, and nothing is moved past them.
//     Single-particle emitters, which used to take the one-draw-per-particle
//     route, join a run like the others.
// particle-runs.txt: 0 = Alpha 11 loop, 1 = only (1), 2 = both (default).
// particle-size-cull.txt (pixels): an emitter whose whole footprint is
// smaller in both directions is left out like one off screen; 0 = off
// (default).
namespace dusk::native_effects {
int particleRuns = 2;
float particleCullPixels = 0.0f;
}

namespace {

struct NativeEmitterSurvey {
    enum Kind : u8 {
        hidden,   // status 0x04: not drawn at all (original)
        empty,    // no particle alive: only the draw callbacks run (original)
        culled,   // nothing on screen: left out
        normal,   // drawn by JPAResource::draw at its place
        runnable, // may share a batch with emitters of its resource
    };
    JPABaseEmitter* emtr;
    Kind kind;
    bool barrier;          // may draw anywhere: nothing is moved past it
    bool small;            // culled only by particle-size-cull.txt
    jpa_native::Rect rect; // footprint (when not a barrier)
};

constexpr unsigned kSurveyCapacity = 256; // emitters per group looked at together
constexpr unsigned kRunLimit = 32;        // emitters per run
constexpr unsigned kRunWindow = 64;       // how far ahead a run looks

NativeEmitterSurvey sSurveys[kSurveyCapacity];
// The same emitters as the run search sees them (JPANativeCull.h).
jpa_native::RunCandidate sCandidates[kSurveyCapacity];

// Particle scale of the base or child shape, as drawP/drawC compute it.
void native_particle_scale(JPAResource* res, JPABaseEmitter* emtr, bool child, f32& x, f32& y) {
    JPABaseShape* bsp = res->getBsp();
    if (!child) {
        x = emtr->mGlobalPScl.x * bsp->getBaseSizeX();
        y = emtr->mGlobalPScl.y * bsp->getBaseSizeY();
        if (bsp->getType() == 0) {
            x *= 1.02f;
        } else if (bsp->getType() == 1) {
            x *= 1.02f;
            y *= 0.4f;
        }
        return;
    }
    JPAChildShape* csp = res->getCsp();
    if (csp->isScaleInherited()) {
        x = emtr->mGlobalPScl.x * bsp->getBaseSizeX();
        y = emtr->mGlobalPScl.y * bsp->getBaseSizeY();
    } else {
        x = emtr->mGlobalPScl.x * csp->getScaleX();
        y = emtr->mGlobalPScl.y * csp->getScaleY();
    }
    if (csp->getType() == 0) {
        x *= 1.02f;
    } else if (csp->getType() == 1) {
        x *= 1.02f;
        y *= 0.4f;
    }
}

f32 native_quad_radius(const JPAResource::BatchInfo& info) {
    f32 radius = 0.0f;
    for (int i = 0; i < info.vtxCount; i++) {
        radius = std::max(radius, std::sqrt(info.vtxPos[i][0] * info.vtxPos[i][0] + info.vtxPos[i][1] * info.vtxPos[i][1] +
                                            info.vtxPos[i][2] * info.vtxPos[i][2]));
    }
    return radius * 2.0f;
}

// Walks one list: true when a particle may reach the picture; its footprint
// joins `s.rect` (or `s` becomes a barrier when a sphere reaches the eye
// plane). Invisible particles draw nothing (their draw function returns).
bool native_survey_list(JPAEmitterWorkData* work, const jpa_native::CullPlanes& planes, JPAList<JPABaseParticle>& list,
                        const JPAResource::BatchInfo& info, f32 scaleX, f32 scaleY, NativeEmitterSurvey& s) {
    const f32 quadRadius = native_quad_radius(info);
    scaleX = std::fabs(scaleX);
    scaleY = std::fabs(scaleY);
    bool visible = false;
    for (JPANode<JPABaseParticle>* n = list.getFirst(); n != list.getEnd(); n = n->getNext()) {
        const JPABaseParticle* particle = n->getObject();
        if (particle->checkStatus(JPAPtclStts_Invisible)) {
            continue;
        }
        Vec centre;
        MTXMultVec(work->mPosCamMtx, &particle->mPosition, &centre);
        const f32 point[3] = {centre.x, centre.y, centre.z};
        const f32 radius = quadRadius * std::max(scaleX * std::fabs(particle->mParticleScaleX),
                                                 scaleY * std::fabs(particle->mParticleScaleY));
        if (jpa_native::sphereOutside(planes, point, radius)) {
            continue;
        }
        visible = true;
        if (!s.barrier) {
            jpa_native::Rect rect;
            if (jpa_native::sphereRect(work->mPrjMtx, point, radius, rect)) {
                jpa_native::rectAdd(s.rect, rect);
            } else {
                s.barrier = true;
            }
        }
    }
    return visible;
}

bool native_resource_is_refraction(JPAResourceManager* manager, JPAResource* res) {
    JPATexture* texture = manager->pTexAry[res->getTexIdx(res->getBsp()->getTexIdx())];
    const char* name = texture->getName();
    return name[0] == 'd' && std::strcmp(name, "dummy") == 0;
}

void native_survey(JPAEmitterWorkData* work, JPAResourceManager* const* managers, const jpa_native::CullPlanes& planes,
                   NativeEmitterSurvey& s) {
    JPABaseEmitter* emtr = s.emtr;
    s.barrier = true;
    s.small = false;
    s.rect = jpa_native::emptyRect();
    if (emtr->getParticleNumber() == 0) {
        s.kind = NativeEmitterSurvey::empty;
        // Its draw callback may draw anything.
        s.barrier = !emtr->checkStatus(0x04) && emtr->mpEmtrCallBack != NULL;
        return;
    }
    if (emtr->checkStatus(0x04)) {
        s.kind = NativeEmitterSurvey::hidden;
        s.barrier = false;
        return;
    }
    s.kind = NativeEmitterSurvey::normal;
    JPAResource* res = emtr->pRes;
    JPABaseShape* bsp = res->getBsp();
    JPAChildShape* csp = res->getCsp();
    // The footprint is known only for quads the batch knows (the draw
    // function is last and builds the template quad) and no callback that
    // draws. The particle callbacks that say they do not draw have an empty
    // draw() (d_particle.h, d_a_alink.h); the batch already leaves them out
    // with the particles it culls.
    const bool baseKnown = emtr->mAlivePtclBase.getNum() == 0 || res->mBatchInfo.supported;
    const bool childKnown = csp == NULL || emtr->mAlivePtclChld.getNum() == 0 || res->mChildBatchInfo.supported;
    const bool particleCallbackDraws = emtr->mpPtclCallBack != NULL && emtr->mpPtclCallBack->drawsParticle();
    if (emtr->mpEmtrCallBack != NULL || particleCallbackDraws || emtr->getDrawTimes() == 0 || !baseKnown ||
        !childKnown) {
        return;
    }
    JPAResourceManager* manager = managers[emtr->getResourceManagerID()];
    if (manager == NULL) {
        return;
    }
    s.barrier = false;
    bool visible = false;
    f32 scaleX, scaleY;
    if (emtr->mAlivePtclBase.getNum() != 0) {
        native_particle_scale(res, emtr, false, scaleX, scaleY);
        visible |= native_survey_list(work, planes, emtr->mAlivePtclBase, res->mBatchInfo, scaleX, scaleY, s);
    }
    if (csp != NULL && emtr->mAlivePtclChld.getNum() != 0) {
        native_particle_scale(res, emtr, true, scaleX, scaleY);
        visible |= native_survey_list(work, planes, emtr->mAlivePtclChld, res->mChildBatchInfo, scaleX, scaleY, s);
    }
    if (!visible) {
        s.kind = NativeEmitterSurvey::culled;
        return;
    }
    const f32 pixels = dusk::native_effects::particleCullPixels;
    if (pixels > 0.0f && !s.barrier && (s.rect.s1 - s.rect.s0) * 400.0f < pixels &&
        (s.rect.t1 - s.rect.t0) * 240.0f < pixels) {
        s.kind = NativeEmitterSurvey::culled;
        s.small = true;
        return;
    }
    // Frame-copy refraction is drawn by JPAResource::draw (its plan needs the
    // frame copy); nothing is moved past it.
    const bool refraction = native_resource_is_refraction(manager, res);
    if (refraction) {
        s.barrier = true;
        return;
    }
    if (csp == NULL && emtr->getDrawTimes() == 1 && !bsp->isPrjTex() &&
        (!res->mBatchInfo.hasPtclColor || bsp->getTevColorArgSel() < 6)) {
        s.kind = NativeEmitterSurvey::runnable;
    }
}

GXColor native_emitter_prm(const JPABaseEmitter* emtr) {
    GXColor prm = emtr->mPrmClr;
    prm.r = COLOR_MULTI(prm.r, emtr->mGlobalPrmClr.r);
    prm.g = COLOR_MULTI(prm.g, emtr->mGlobalPrmClr.g);
    prm.b = COLOR_MULTI(prm.b, emtr->mGlobalPrmClr.b);
    prm.a = COLOR_MULTI(prm.a, emtr->mGlobalPrmClr.a);
    return prm;
}

GXColor native_emitter_env(const JPABaseEmitter* emtr) {
    GXColor env = emtr->mEnvClr;
    env.r = COLOR_MULTI(env.r, emtr->mGlobalEnvClr.r);
    env.g = COLOR_MULTI(env.g, emtr->mGlobalEnvClr.g);
    env.b = COLOR_MULTI(env.b, emtr->mGlobalEnvClr.b);
    return env;
}

bool native_same_color(GXColor a, GXColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Two emitters of one resource draw with the same GX state when the emitter
// draw functions that read the emitter write the same values: the TEV
// register colours (JPARegistPrm/Env/PrmEnv) and the texture frame of a
// global texture animation (JPALoadTexAnm). The rest of the emitter-level
// state (setGX, setPTev, JPALoadTex, JPALoadExTex, the texture coordinate
// generators) depends on the resource alone.
bool native_run_compatible(const JPABaseEmitter* a, const JPABaseEmitter* b) {
    if (a->pRes != b->pRes || a->mResMgrID != b->mResMgrID) {
        return false;
    }
    const JPAResource* res = a->pRes;
    for (int i = 0; i < res->mpDrawEmitterFuncListNum; i++) {
        const JPAResource::EmitterFunc func = res->mpDrawEmitterFuncList[i];
        const bool prm = func == static_cast<JPAResource::EmitterFunc>(JPARegistPrm) ||
                         func == static_cast<JPAResource::EmitterFunc>(JPARegistPrmEnv);
        const bool env = func == static_cast<JPAResource::EmitterFunc>(JPARegistEnv) ||
                         func == static_cast<JPAResource::EmitterFunc>(JPARegistPrmEnv);
        if (prm && !native_same_color(native_emitter_prm(a), native_emitter_prm(b))) {
            return false;
        }
        if (env && !native_same_color(native_emitter_env(a), native_emitter_env(b))) {
            return false;
        }
        if (func == static_cast<JPAResource::EmitterFunc>(JPALoadTexAnm) && a->mTexAnmIdx != b->mTexAnmIdx) {
            return false;
        }
    }
    return true;
}

// The emitters drawn together with sSurveys[head] (JPANativeCull.h, gatherRun).
unsigned native_gather_run(unsigned head, unsigned count, JPABaseEmitter** run) {
    unsigned members[kRunLimit];
    const unsigned found = jpa_native::gatherRun(
        sCandidates, count, head, kRunWindow, kRunLimit, members,
        [](unsigned a, unsigned b) { return native_run_compatible(sSurveys[a].emtr, sSurveys[b].emtr); });
    for (unsigned i = 0; i < found; i++) {
        run[i] = sSurveys[members[i]].emtr;
    }
    return found;
}

// The status bit drawP clears and drawC sets: what the original leaves.
void native_leave_draw_status(JPABaseEmitter* emtr) {
    JPAResource* res = emtr->pRes;
    if (emtr->getDrawTimes() != 0 && res->getCsp() != NULL && !res->getBsp()->isDrawPrntAhead()) {
        emtr->setStatus(0x80);
    } else if (emtr->getDrawTimes() != 0) {
        emtr->clearStatus(0x80);
    }
}

// An emitter with nothing on screen: no GX state. Its particles still take
// the base-axis step of a direction draw function, once per draw as in the
// original (getDrawTimes).
void native_leave_out(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    JPAResource* res = emtr->pRes;
    work->mpEmtr = emtr;
    work->mpRes = res;
    bool calculated = false;
    for (int pass = 0; pass < 2; pass++) {
        const bool child = pass == 1;
        if (child && res->getCsp() == NULL) {
            break;
        }
        JPAList<JPABaseParticle>& list = child ? emtr->mAlivePtclChld : emtr->mAlivePtclBase;
        JPAResource::DrawParticleFunc* funcs = child ? res->mpDrawParticleChildFuncList : res->mpDrawParticleFuncList;
        const int funcNum = child ? res->mpDrawParticleChildFuncListNum : res->mpDrawParticleFuncListNum;
        if (list.getNum() == 0 || funcs == NULL || funcNum == 0 || !JPANativeDrawKeepsBaseAxis(funcs[0])) {
            continue;
        }
        if (!calculated) {
            res->calcWorkData_d(work);
            calculated = true;
        }
        work->mDirType = child ? res->getCsp()->getDirType() : res->getBsp()->getDirType();
        work->mpAlivePtcl = &list;
        for (int times = 0; times < emtr->getDrawTimes(); times++) {
            for (JPANode<JPABaseParticle>* n = list.getFirst(); n != list.getEnd(); n = n->getNext()) {
                work->mpCurNode = n;
                JPANativeKeepBaseAxis(work, n->getObject());
            }
        }
    }
    native_leave_draw_status(emtr);
}

// drawP's work data for the current emitter (work->mpEmtr), base shape.
void native_base_draw_work(JPAResource* res, JPAEmitterWorkData* work) {
    JPABaseShape* bsp = res->getBsp();
    JPAExtraShape* esp = res->getEsp();
    work->mpEmtr->clearStatus(0x80);
    native_particle_scale(res, work->mpEmtr, false, work->mGlobalPtclScl.x, work->mGlobalPtclScl.y);
    if (esp != NULL && esp->isEnableScaleAnm()) {
        work->mPivot.x = esp->getScaleCenterX() - 1.0f;
        work->mPivot.y = esp->getScaleCenterY() - 1.0f;
    } else {
        work->mPivot.x = work->mPivot.y = 0.0f;
    }
    work->mDirType = bsp->getDirType();
    work->mRotType = bsp->getRotType();
    work->mDLType = bsp->getType() == 4 || bsp->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : bsp->getBasePlaneType();
    work->mPrjType = bsp->isPrjTex() ? (bsp->isTexCrdAnm() ? 2 : 1) : 0;
    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclBase;
}

// Several emitters of one resource (native_gather_run) in one batch: what
// draw() and drawP() do for each, with the GX state set once.
void native_draw_run(JPAEmitterWorkData* work, JPABaseEmitter* const* run, unsigned members) {
    JPABaseEmitter* first = run[0];
    JPAResource* res = first->pRes;
    unsigned particles = 0;
    for (unsigned i = 0; i < members; i++) {
        particles += run[i]->mAlivePtclBase.getNum();
    }
    work->mpEmtr = first;
    work->mpRes = res;
    work->mDrawCount = 0;
    native_particle_report_after_profiled_frame();
    sNativeRefraction = {};
    // Not refraction (native_survey), so the batch takes it unless a switch
    // or the list says otherwise: then each emitter is drawn on its own.
    res->calcWorkData_d(work);
    native_base_draw_work(res, work);
    if (!native_batch_accepts(work, false, particles)) {
        for (unsigned i = 0; i < members; i++) {
            res->draw(work, run[i]);
        }
        return;
    }
    dusk::native_effects::EmitterRow* row = nullptr;
    unsigned long long rowStart = 0;
    unsigned rowBatches = 0;
    if (dusk::native_effects::emitterReport) {
        using namespace dusk::native_effects;
        for (unsigned i = 0; i < emitterRowCount && row == nullptr; i++) {
            if (emitterRows[i].resource == res) {
                row = &emitterRows[i];
            }
        }
        if (row == nullptr && emitterRowCount < sizeof(emitterRows) / sizeof(*emitterRows)) {
            row = &emitterRows[emitterRowCount++];
            *row = {};
            row->resource = res;
            row->id = res->getUsrIdx();
            JPABaseShape* bsp = res->getBsp();
            JPAExTexShape* ets = res->getEts();
            row->flags = (ets != NULL && ets->isUseIndirect() ? 2u : 0u) |
                         (res->mBatchInfo.hasPtclTex ? 64u : 0u) | (res->mBatchInfo.hasPtclTexMtx ? 128u : 0u) |
                         (bsp->getType() << 8) | (bsp->getTevColorArgSel() << 12) |
                         ((bsp->pBsd->mBlendModeCfg & 3u) << 16);
        }
        if (row != nullptr) {
            row->emitters += members;
            row->particles += particles;
            rowBatches = counters.particleBatches;
            rowStart = svcGetSystemTick();
        }
    }

    // draw() and drawP() of the first emitter up to its particles.
    res->getBsp()->setGX(work);
    JPANativeBatchEnd();
    work->mDrawCount = 1;
    res->setPTev();
    for (int i = res->mpDrawEmitterFuncListNum - 1; i >= 0; i--) {
        (*res->mpDrawEmitterFuncList[i])(work);
    }
    dusk::native_effects::counters.particleEmitters += members;
    native_batch_begin(work, false, sNativeBatch);
    for (unsigned i = 0; i < members; i++) {
        JPABaseEmitter* emtr = run[i];
        if (i != 0) {
            work->mpEmtr = emtr;
            work->mDrawCount = 1;
            res->calcWorkData_d(work);
            native_base_draw_work(res, work);
        }
        native_batch_particles(work, sNativeBatch);
    }
    native_batch_end(work, sNativeBatch);
    GXSetMisc(GX_MT_XF_FLUSH, 0);
    sNativeLoopCounts.runs++;
    sNativeLoopCounts.runEmitters += members;
    if (row != nullptr) {
        row->ticks += svcGetSystemTick() - rowStart;
        row->batches += dusk::native_effects::counters.particleBatches - rowBatches;
    }
}

// The Alpha 11 loop body for one emitter.
void native_draw_one(JPAEmitterWorkData* work, JPAResourceManager* const* managers, JPABaseEmitter* emtr) {
    if (emtr->getParticleNumber() == 0) {
        if (!emtr->checkStatus(0x04) && emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->draw(emtr);
            // draw() may detach the callback from the emitter.
            if (emtr->mpEmtrCallBack != NULL) {
                emtr->mpEmtrCallBack->drawAfter(emtr);
            }
        }
        return;
    }
    if (!emtr->checkStatus(0x04)) {
        work->mpResMgr = managers[emtr->mResMgrID];
        emtr->pRes->draw(work, emtr);
        sNativeLoopCounts.drawn++;
    }
}

} // namespace

bool JPANativeDrawGroup(JPAEmitterWorkData* work, JPAResourceManager* const* managers,
                        JSUList<JPABaseEmitter>& list) {
    // The survey arrays are this loop's own: a group drawn from inside a
    // callback of another (none known) takes the Alpha 11 loop.
    static bool active;
    if (dusk::native_effects::particleRuns <= 0 || dusk::native_effects::particles < 2 || active) {
        return false;
    }
    struct Active {
        Active() { active = true; }
        ~Active() { active = false; }
    } guard;
    unsigned count = 0;
    JSULink<JPABaseEmitter>* link = list.getFirst();
    for (; link != list.getEnd() && count < kSurveyCapacity; link = link->getNext()) {
        sSurveys[count++].emtr = link->getObject();
    }
    jpa_native::CullPlanes planes;
    jpa_native::makeCullPlanes(work->mPrjMtx, planes);
    for (unsigned i = 0; i < count; i++) {
        NativeEmitterSurvey& s = sSurveys[i];
        native_survey(work, managers, planes, s);
        using jpa_native::RunCandidate;
        const RunCandidate::Draws draws =
            s.kind == NativeEmitterSurvey::runnable ? RunCandidate::runnable :
            s.kind == NativeEmitterSurvey::normal || (s.kind == NativeEmitterSurvey::empty && s.barrier) ?
                RunCandidate::own : RunCandidate::nothing;
        sCandidates[i] = {draws, s.barrier, false, s.rect};
    }
    JPABaseEmitter* run[kRunLimit];
    for (unsigned i = 0; i < count; i++) {
        NativeEmitterSurvey& s = sSurveys[i];
        if (sCandidates[i].done) {
            continue;
        }
        sCandidates[i].done = true;
        JPABaseEmitter* emtr = s.emtr;
        switch (s.kind) {
        case NativeEmitterSurvey::hidden:
        case NativeEmitterSurvey::empty:
        case NativeEmitterSurvey::normal:
            native_draw_one(work, managers, emtr);
            break;
        case NativeEmitterSurvey::culled:
            work->mpResMgr = managers[emtr->mResMgrID];
            native_leave_out(work, emtr);
            sNativeLoopCounts.culled++;
            sNativeLoopCounts.small += s.small ? 1u : 0u;
            break;
        case NativeEmitterSurvey::runnable: {
            const unsigned members =
                dusk::native_effects::particleRuns >= 2 ? native_gather_run(i, count, run) : 1u;
            work->mpResMgr = managers[emtr->mResMgrID];
            if (members == 1) {
                emtr->pRes->draw(work, emtr);
                sNativeLoopCounts.drawn++;
            } else {
                native_draw_run(work, run, members);
                sNativeLoopCounts.drawn += members;
            }
            break;
        }
        }
    }
    // A group longer than the survey: the rest one by one, in order.
    for (; link != list.getEnd(); link = link->getNext()) {
        native_draw_one(work, managers, link->getObject());
    }
    return true;
}
#endif
