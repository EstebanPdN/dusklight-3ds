/**
 * c_cc_s.cpp
 *
 */

#if defined(DUSK_TARGET_3DS)
#include "d/d_native_logic.h"
#include <dolphin/ppc_math.h>
#include "d/d_tick_profile.h"
#endif
#include "SSystem/SComponent/c_cc_s.h"
#include "JSystem/JUtility/JUTAssert.h"
#include <cmath>
#include "os_report.h"

#define CHECK_FLOAT_CLASS(line, x) JUT_ASSERT(line, !std::isnan(x));
#define CHECK_FLOAT_RANGE(line, x) JUT_ASSERT(line, -1.0e32f < x && x < 1.0e32f);

cCcS::cCcS() {}

void cCcS::Ct() {
    for (cCcD_Obj** obj = mpObjAt; obj < mpObjAt + ARRAY_SIZEU(mpObjAt); ++obj) {
        *obj = NULL;
    }
    mObjAtCount = 0;

    for (cCcD_Obj** obj = mpObjTg; obj < mpObjTg + ARRAY_SIZEU(mpObjTg); ++obj) {
        *obj = NULL;
    }
    mObjTgCount = 0;

    for (cCcD_Obj** obj = mpObjCo; obj < mpObjCo + ARRAY_SIZEU(mpObjCo); ++obj) {
        *obj = NULL;
    }
    mObjCoCount = 0;

    for (cCcD_Obj** obj = mpObj; obj < mpObj + ARRAY_SIZEU(mpObj); ++obj) {
        *obj = NULL;
    }
    mObjCount = 0;

    #if DEBUG
    m_debug_code = 1234;
    #endif
}

void cCcS::Dt() {
    Ct();
}

WeightType cCcS::GetWt(u8 param_0) const {
    if (param_0 == 0xFF) {
        return WeightType_0;
    }
    if (param_0 == 0xFE) {
        return WeightType_1;
    }
    return WeightType_2;
}

void cCcS::Set(cCcD_Obj* obj) {
    if (obj->ChkAtSet()) {
        if (mObjAtCount >= ARRAY_SIZEU(mpObjAt)) {
            OS_REPORT("\x1b[43;30m");
            OS_REPORT("cCcS::Set AT Overflow.Now Max is %d.\n", ARRAY_SIZEU(mpObjAt));
            OS_REPORT("\x1b[m");
        } else {
            mpObjAt[mObjAtCount] = obj;
            mObjAtCount++;
        }
    }

    if (obj->ChkTgSet()) {
        if (mObjTgCount >= ARRAY_SIZEU(mpObjTg)) {
            OS_REPORT("\x1b[43;30m");
            OS_REPORT("cCcS::Set TG Overflow.Now Max is %d.\n", ARRAY_SIZEU(mpObjTg));
            OS_REPORT("\x1b[m");
        } else {
            mpObjTg[mObjTgCount] = obj;
            mObjTgCount++;
        }
    }

    if (obj->ChkCoSet()) {
        if (mObjCoCount >= ARRAY_SIZEU(mpObjCo)) {
            OS_REPORT("\x1b[43;30m");
            OS_REPORT("cCcS::Set CO Overflow.Now Max is %d.\n", ARRAY_SIZEU(mpObjCo));
            OS_REPORT("\x1b[m");
        } else {
            mpObjCo[mObjCoCount] = obj;
            mObjCoCount++;
        }
    }

    if (mObjCount >= ARRAY_SIZEU(mpObj)) {
        OS_REPORT("\x1b[43;30m");
        OS_REPORT("cCcS::Set SET Overflow.Now Max is %d.\n", ARRAY_SIZEU(mpObj));
        OS_REPORT("\x1b[m");
    } else {
        mpObj[mObjCount] = obj;
        mObjCount++;
    }
}

void cCcS::ClrCoHitInf() {
    for (cCcD_Obj** obj = mpObjCo; obj < mpObjCo + mObjCoCount; ++obj) {
        if (*obj != NULL) {
            (*obj)->GetGObjInf()->ClrCoHit();

            if ((*obj)->GetStts() != NULL) {
                (*obj)->GetStts()->ClrCo();
            }
        }
    }
}

void cCcS::ClrTgHitInf() {
    for (cCcD_Obj** obj = mpObjTg; obj < mpObjTg + mObjTgCount; ++obj) {
        if (*obj != NULL) {
            (*obj)->GetGObjInf()->ClrTgHit();

            if ((*obj)->GetStts() != NULL) {
                (*obj)->GetStts()->ClrTg();
            }
        }
    }
}

void cCcS::ClrAtHitInf() {
    for (cCcD_Obj** obj = mpObjAt; obj < mpObjAt + mObjAtCount; ++obj) {
        if (*obj != NULL) {
            (*obj)->GetGObjInf()->ClrAtHit();

            if ((*obj)->GetStts() != NULL) {
                (*obj)->GetStts()->ClrAt();
            }
        }
    }
}

bool cCcS::ChkNoHitAtTg(cCcD_Obj* pat_obj, cCcD_Obj* ptg_obj) {
    if ((pat_obj->GetAtGrp() & ptg_obj->GetTgGrp()) == 0 ||
        (pat_obj->GetAtType() & ptg_obj->GetTgType()) == 0)
    {
        return true;
    }
    fopAc_ac_c* pat_ac = pat_obj->GetAc();
    return (pat_ac != NULL && pat_ac == ptg_obj->GetAc()) ||
           ChkNoHitGAtTg(pat_obj->GetGObjInf(), ptg_obj->GetGObjInf(),
                         pat_obj->GetStts()->GetGStts(), ptg_obj->GetStts()->GetGStts());
}

void cCcS::ChkAtTg() {
    cCcD_Obj** objTgEnd = mpObjTg + mObjTgCount;
    ClrAtHitInf();
    ClrTgHitInf();
    for (cCcD_Obj** pat_obj = mpObjAt; pat_obj < mpObjAt + mObjAtCount; ++pat_obj) {
        if (*pat_obj == NULL || !(*pat_obj)->ChkAtSet())
            continue;

        cCcD_ShapeAttr* pat_sa = (*pat_obj)->GetShapeAttr();
        JUT_ASSERT(338, pat_sa != NULL);

        for (cCcD_Obj** ptg_obj = mpObjTg; ptg_obj < objTgEnd; ++ptg_obj) {
#if defined(DUSK_TARGET_3DS)
            dNativeLogic::census.ccAtTgPairs++;
#endif
            if (*ptg_obj == NULL || !(*ptg_obj)->ChkTgSet())
                continue;
            if (!(*pat_obj)->GetPDivideInfo()->Chk(*(*ptg_obj)->GetPDivideInfo()))
                continue;
#if defined(DUSK_TARGET_3DS)
            dNativeLogic::census.ccAtTgArea++;
#endif
            if (ChkNoHitAtTg(*pat_obj, *ptg_obj))
                continue;
#if defined(DUSK_TARGET_3DS)
            dNativeLogic::census.ccAtTgShape++;
#endif

            cCcD_ShapeAttr* ptg_sa = (*ptg_obj)->GetShapeAttr();
            JUT_ASSERT(354, ptg_sa != NULL);

            static cXyz cross;
            bool didCross = pat_sa->CrossAtTg(*ptg_sa, &cross);
            bool anyBsRevHit = (*pat_obj)->ChkBsRevHit() || (*ptg_obj)->ChkBsRevHit();
            if (!anyBsRevHit && didCross) {
                SetAtTgCommonHitInf(*pat_obj, *ptg_obj, &cross);
            } else if (anyBsRevHit && !didCross) {
                cCcD_ShapeAttr* pat_sa = (*pat_obj)->GetShapeAttr();
                if (pat_sa == NULL) {
                    cross.x = 0.0f;
                    cross.y = 0.0f;
                    cross.z = 0.0f;
                } else {
                    cM3dGAab& aab = pat_sa->GetWorkAab();
                    aab.CalcCenter(&cross);
                }

                SetAtTgCommonHitInf(*pat_obj, *ptg_obj, &cross);
            }
        }
    }
}

bool cCcS::ChkNoHitCo(cCcD_Obj* pco1_obj, cCcD_Obj* pco2_obj) {
    fopAc_ac_c* ac1 = pco1_obj->GetAc();
    fopAc_ac_c* ac2 = pco2_obj->GetAc();
    if (!(((ac1 == NULL || ac2 == NULL) || ac1 != ac2 || pco1_obj->ChkCoSameActorHit() ||
           pco2_obj->ChkCoSameActorHit()) &&
          (pco1_obj->GetCoIGrp() & (pco2_obj->GetCoVsGrp() >> 3)) &&
          ((pco1_obj->GetCoVsGrp() >> 3) & pco2_obj->GetCoIGrp() &&
           !ChkNoHitGCo(pco1_obj, pco2_obj))))
    {
        return true;
    } else {
        return false;
    }
}

#if defined(DUSK_TARGET_3DS)
#include "SSystem/SComponent/c_m3d.h"
#include <typeinfo>

// Native New 3DS (logic.txt ccpre): a pair of push colliders that passes the
// area test is first tested with the very function the double dispatch of
// CrossCo would reach for cylinders and spheres, in the same argument order,
// on the live shapes (pco1->CrossCo(pco2) ends in pco2's typed overload with
// pco1 as the argument: CylCyl(pco2, pco1), CylSph(pco1 cyl, pco2 sph),
// CylSph(pco2 cyl, pco1 sph), SphSph(pco2, pco1)). When it says "no cross",
// the original would also find no cross; ChkNoHitCo (dCcS::ChkNoHitGCo is
// "false") and CrossCo have no side effects, so the pair is left out. A hit
// takes the original path. Other shapes always take it.
namespace {
enum CoKind { kCoOther = 0, kCoCyl = 1, kCoSph = 2 };
struct CoShape {
    cCcD_Obj* obj;  // the entry this was made for (a changed entry takes the original path)
    int kind;
    const cM3dGCyl* cyl;
    const cM3dGSph* sph;
};
CoShape l_coShape[0x100];

// Shape kind by vtable pointer, filled with dynamic_cast on first sight.
struct CoKindCache {
    const void* vptr;
    int kind;
};
CoKindCache l_coKinds[32];
int l_coKindCount;

void coShapeOf(cCcD_Obj* obj, CoShape& out) {
    out.obj = obj;
    out.kind = kCoOther;
    out.cyl = NULL;
    out.sph = NULL;
    if (obj == NULL) {
        return;
    }
    cCcD_ShapeAttr* sa = obj->GetShapeAttr();
    if (sa == NULL) {
        return;
    }
    const void* vptr = *(const void* const*)sa;
    int kind = -1;
    for (int i = 0; i < l_coKindCount; i++) {
        if (l_coKinds[i].vptr == vptr) {
            kind = l_coKinds[i].kind;
            break;
        }
    }
    if (kind < 0) {
        kind = dynamic_cast<cCcD_CylAttr*>(sa) != NULL   ? kCoCyl
               : dynamic_cast<cCcD_SphAttr*>(sa) != NULL ? kCoSph
                                                           : kCoOther;
        if (l_coKindCount < (int)(sizeof(l_coKinds) / sizeof(l_coKinds[0]))) {
            l_coKinds[l_coKindCount].vptr = vptr;
            l_coKinds[l_coKindCount].kind = kind;
            l_coKindCount++;
        }
    }
    if (kind == kCoCyl) {
        out.cyl = static_cast<const cM3dGCyl*>(static_cast<cCcD_CylAttr*>(sa));
    } else if (kind == kCoSph) {
        out.sph = static_cast<const cM3dGSph*>(static_cast<cCcD_SphAttr*>(sa));
    }
    out.kind = kind;
}

// -1: no answer (original path); 0: no cross; 1: cross.
inline int coPreCross(const CoShape& s1, const CoShape& s2) {
    f32 len;
    if (s1.kind == kCoCyl) {
        if (s2.kind == kCoCyl) {
            return cM3d_Cross_CylCyl(s2.cyl, s1.cyl, &len) ? 1 : 0;
        }
        if (s2.kind == kCoSph) {
            return cM3d_Cross_CylSph(s1.cyl, s2.sph, &len) ? 1 : 0;
        }
    } else if (s1.kind == kCoSph) {
        if (s2.kind == kCoCyl) {
            return cM3d_Cross_CylSph(s2.cyl, s1.sph, &len) ? 1 : 0;
        }
        if (s2.kind == kCoSph) {
            // The PPC length uses a reciprocal-square-root estimate. Over this
            // normal binary32 interval its result is at least half the true
            // length. Near contacts and inputs outside the interval use its original length.
            if (dusk_native_frsqrte_mode == 1 && dusk_native_fmaf_mode == 1) {
                Vec delta;
                VECSubtract(s2.sph->GetCP(), s1.sph->GetCP(), &delta);
                const float square = PSVECSquareMag(&delta);
                const float radiusSum = s2.sph->GetR() + s1.sph->GetR();
                const float diameter = 2.0f * radiusSum;
                if (square >= 1.0f && square <= 4294967296.0f && square > diameter * diameter) {
                    return 0;
                }
                // PSVECMag's exact operation on the square already calculated.
                const float distance = square == 0.0f ? 0.0f : square * ppc_rsqrte(square);
                return radiusSum - distance > G_CM3D_F_ABS_MIN;
            }
            return cM3d_Cross_SphSph(s2.sph, s1.sph, &len) ? 1 : 0;
        }
    }
    return -1;
}
}  // namespace
#endif

void cCcS::ChkCo() {
    ClrCoHitInf();
    if (mObjCoCount <= 1)
        return;
#if defined(DUSK_TARGET_3DS)
    const unsigned preMode = dNativeLogic::config().ccPre;
    if (preMode != dNativeLogic::Off) {
        for (int i = 0; i < mObjCoCount; i++) {
            coShapeOf(mpObjCo[i], l_coShape[i]);
        }
    }
#endif

    cCcD_Obj** objCoEnd = mpObjCo + mObjCoCount;
    for (cCcD_Obj** pco1_obj = mpObjCo; pco1_obj < objCoEnd - 1; ++pco1_obj) {
        if (*pco1_obj == NULL || !(*pco1_obj)->ChkCoSet())
            continue;

        cCcD_ShapeAttr* pco1_sa = (*pco1_obj)->GetShapeAttr();
        JUT_ASSERT(444, pco1_sa != NULL);

        for (cCcD_Obj** pco2_obj = pco1_obj + 1; pco2_obj < objCoEnd; ++pco2_obj) {
#if defined(DUSK_TARGET_3DS)
            dNativeLogic::census.ccCoPairs++;
#endif
            if (*pco2_obj == NULL || !(*pco2_obj)->ChkCoSet())
                continue;
            if (!(*pco1_obj)->GetPDivideInfo()->Chk(*(*pco2_obj)->GetPDivideInfo()))
                continue;
#if defined(DUSK_TARGET_3DS)
            dNativeLogic::census.ccCoArea++;
            if (preMode != dNativeLogic::Off) {
                const CoShape& s1 = l_coShape[pco1_obj - mpObjCo];
                const CoShape& s2 = l_coShape[pco2_obj - mpObjCo];
                const int pre = s1.obj == *pco1_obj && s2.obj == *pco2_obj ? coPreCross(s1, s2) : -1;
                if (pre == 0) {
                    dNativeLogic::census.ccPreSkipped++;
                    if (preMode != dNativeLogic::Verify) {
                        continue;
                    }
                    // Verify: the original predicates must agree ("no hit").
                    dNativeLogic::census.ccPreChecks++;
                    bool originalHit = false;
                    if (!ChkNoHitCo(*pco1_obj, *pco2_obj)) {
                        f32 len;
                        originalHit = (*pco1_obj)->GetShapeAttr()->CrossCo(*(*pco2_obj)->GetShapeAttr(), &len);
                    }
                    if (!originalHit) {
                        continue;
                    }
                    dNativeLogic::census.ccPreMismatches++;
                    // Fall through: the original path below handles the pair.
                }
            }
#endif
            if (ChkNoHitCo(*pco1_obj, *pco2_obj))
                continue;
#if defined(DUSK_TARGET_3DS)
            dNativeLogic::census.ccCoShape++;
#endif

            cCcD_ShapeAttr* pco2_sa = (*pco2_obj)->GetShapeAttr();
            JUT_ASSERT(456, pco2_sa != NULL);

            f32 cross_len;
            if (pco1_sa->CrossCo(*pco2_sa, &cross_len)) {
#if defined(DUSK_TARGET_3DS)
                dNativeLogic::census.ccCoHits++;
#endif
                SetCoCommonHitInf(*pco1_obj, &pco1_sa->GetCoCP(), *pco2_obj, &pco2_sa->GetCoCP(), cross_len);
            }
        }
    }
}

void cCcS::CalcTgPlusDmg(cCcD_Obj* pat_obj, cCcD_Obj* ptg_obj, cCcD_Stts* pat_stts,
                         cCcD_Stts* ptg_stts) {
    UNUSED(ptg_obj);
    UNUSED(pat_stts);

    int atp = pat_obj->GetAtAtp();
    ptg_stts->PlusDmg(atp);
}

void cCcS::SetAtTgCommonHitInf(cCcD_Obj* pat_obj, cCcD_Obj* ptg_obj, cXyz* pcross) {
    cCcD_Stts* pat_stts = pat_obj->GetStts();
    cCcD_Stts* ptg_stts = ptg_obj->GetStts();
    if (!ChkAtTgHitAfterCross(!ptg_obj->ChkTgNoAtHitInfSet(), !pat_obj->ChkAtNoTgHitInfSet(),
                              pat_obj->GetGObjInf(), ptg_obj->GetGObjInf(), pat_stts, ptg_stts,
                              pat_stts->GetGStts(), ptg_stts->GetGStts()))
    {
        if (!ptg_obj->ChkTgNoAtHitInfSet()) {
            pat_obj->SetAtHit(ptg_obj);
        }

        bool tmp = !(pat_obj->ChkAtNoTgHitInfSet() ||
                     (pat_obj->ChkAtType(AT_TYPE_SLINGSHOT) && ptg_obj->ChkTgNoSlingHitInfSet()));
        if (tmp) {
            ptg_obj->SetTgHit(pat_obj);
            CalcTgPlusDmg(pat_obj, ptg_obj, pat_stts, ptg_stts);
        }

        SetAtTgGObjInf(!ptg_obj->ChkTgNoAtHitInfSet(), tmp, pat_obj, ptg_obj, pat_obj->GetGObjInf(),
                       ptg_obj->GetGObjInf(), pat_stts, ptg_stts, pat_stts->GetGStts(),
                       ptg_stts->GetGStts(), pcross);
    }
}

void cCcS::SetCoCommonHitInf(cCcD_Obj* pco1_obj, cXyz* ppos1, cCcD_Obj* pco2_obj, cXyz* ppos2,
                             f32 cross_len) {
    bool co2_inf_set = !pco2_obj->ChkCoNoCoHitInfSet();
    bool co1_inf_set = !pco1_obj->ChkCoNoCoHitInfSet();
    if (co2_inf_set) {
        pco1_obj->SetCoHit(pco2_obj);
    }

    if (co1_inf_set) {
        pco2_obj->SetCoHit(pco1_obj);
    }

    if (co2_inf_set && co1_inf_set) {
        SetPosCorrect(pco1_obj, ppos1, pco2_obj, ppos2, cross_len);
    }

    cCcD_Stts* pco1_stts = pco1_obj->GetStts();
    cCcD_Stts* pco2_stts = pco2_obj->GetStts();
    SetCoGObjInf(co2_inf_set, co1_inf_set, pco1_obj->GetGObjInf(), pco2_obj->GetGObjInf(),
                 pco1_stts, pco2_stts, pco1_stts->GetGStts(), pco2_stts->GetGStts());
}

void cCcS::SetPosCorrect(cCcD_Obj* pco1_obj, cXyz* ppos1, cCcD_Obj* pco2_obj, cXyz* ppos2,
                         f32 cross_len) {
    CHECK_FLOAT_CLASS(616, cross_len);
    CHECK_FLOAT_RANGE(617, cross_len);

    if (pco1_obj->ChkCoNoCrr() || pco2_obj->ChkCoNoCrr())
        return;
    if (pco1_obj->GetStts() == NULL || pco2_obj->GetStts() == NULL)
        return;
    if (pco1_obj->GetStts()->GetActor() != NULL &&
        pco1_obj->GetStts()->GetActor() == pco2_obj->GetStts()->GetActor())
        return;

    if (!(fabsf(cross_len) < (1.0f / 125.0f))) {
        SetCoGCorrectProc(pco1_obj, pco2_obj);
        bool bothCoSph3DCrr = pco1_obj->ChkCoSph3DCrr() && pco2_obj->ChkCoSph3DCrr();
        WeightType obj1WeightType = GetWt(pco1_obj->GetStts()->GetWeightUc());
        WeightType obj2WeightType = GetWt(pco2_obj->GetStts()->GetWeightUc());
        f32 obj1SrcWeight = pco1_obj->GetStts()->GetWeightF();
        f32 obj2SrcWeight = pco2_obj->GetStts()->GetWeightF();
        f32 combinedWeight = obj1SrcWeight + obj2SrcWeight;

        f32 obj2Weight, obj1Weight;
        if (cM3d_IsZero(combinedWeight)) {
            obj1SrcWeight = 1;
            obj2SrcWeight = 1;
            combinedWeight = 2;
        }

        f32 invCombinedWeight = 1 / combinedWeight;
        if (obj1WeightType == WeightType_0) {
            if (obj2WeightType == WeightType_0) {
                return;
            } else {
                obj2Weight = 0;
                obj1Weight = 1;
            }
        } else if (obj1WeightType == WeightType_1) {
            if (obj2WeightType == WeightType_0) {
                obj2Weight = 1;
                obj1Weight = 0;
            } else if (obj2WeightType == WeightType_1) {
                obj2Weight = 0.5;
                obj1Weight = 0.5;
            } else {
                obj2Weight = 0;
                obj1Weight = 1;
            }
        } else {
            if (obj2WeightType == WeightType_2) {
                obj2Weight = obj2SrcWeight * invCombinedWeight;
                obj1Weight = obj1SrcWeight * invCombinedWeight;
            } else {
                obj2Weight = 1;
                obj1Weight = 0;
            }
        }

        f32 objDistLen;
        cXyz vec1;
        cXyz vec2;
        Vec objsDist;
        if (bothCoSph3DCrr) {
            PSVECSubtract(ppos2, ppos1, &objsDist);
            objDistLen = PSVECMag(&objsDist);
        } else {
            objsDist.x = ppos2->x - ppos1->x;
            objsDist.y = 0;
            objsDist.z = ppos2->z - ppos1->z;
            objDistLen = sqrtf(objsDist.x * objsDist.x + objsDist.z * objsDist.z);
        }

        if (!cM3d_IsZero(objDistLen)) {
            if (bothCoSph3DCrr) {
                f32 sp1C = cross_len / objDistLen;
                PSVECScale(&objsDist, &objsDist, sp1C);
                obj2Weight *= -1;
                PSVECScale(&objsDist, &vec1, obj2Weight);
                PSVECScale(&objsDist, &vec2, obj1Weight);
            } else {
                f32 pushFactor = cross_len / objDistLen;
                objsDist.x *= pushFactor;
                objsDist.z *= pushFactor;
                vec1.x = -objsDist.x * obj2Weight;
                vec1.y = 0;
                vec1.z = -objsDist.z * obj2Weight;
                vec2.x = objsDist.x * obj1Weight;
                vec2.y = 0;
                vec2.z = objsDist.z * obj1Weight;
            }
        } else {
            vec1.y = 0;
            vec1.z = 0;
            vec2.y = 0;
            vec2.z = 0;
            if (!cM3d_IsZero(cross_len)) {
                vec1.x = -cross_len * obj2Weight;
                vec2.x = cross_len * obj1Weight;
            } else {
                vec1.x = -obj2Weight;
                vec2.x = obj1Weight;
            }
        }

        CHECK_FLOAT_CLASS(767, vec1.x);
        CHECK_FLOAT_CLASS(768, vec1.y);
        CHECK_FLOAT_CLASS(769, vec1.z);

        CHECK_FLOAT_CLASS(771, vec2.x);
        CHECK_FLOAT_CLASS(772, vec2.y);
        CHECK_FLOAT_CLASS(773, vec2.z);

        CHECK_FLOAT_RANGE(775, vec1.x);
        CHECK_FLOAT_RANGE(776, vec1.y);
        CHECK_FLOAT_RANGE(777, vec1.z);

        CHECK_FLOAT_RANGE(779, vec2.x);
        CHECK_FLOAT_RANGE(780, vec2.y);
        CHECK_FLOAT_RANGE(781, vec2.z);

        pco1_obj->GetStts()->PlusCcMove(vec1.x, vec1.y, vec1.z);
        pco2_obj->GetStts()->PlusCcMove(vec2.x, vec2.y, vec2.z);
        (*ppos1) += vec1;
        (*ppos2) += vec2;

        CHECK_FLOAT_CLASS(790, ppos1->x);
        CHECK_FLOAT_CLASS(791, ppos1->y);
        CHECK_FLOAT_CLASS(792, ppos1->z);

        CHECK_FLOAT_CLASS(794, ppos2->x);
        CHECK_FLOAT_CLASS(795, ppos2->y);
        CHECK_FLOAT_CLASS(796, ppos2->z);

        CHECK_FLOAT_RANGE(798, ppos1->x);
        CHECK_FLOAT_RANGE(799, ppos1->y);
        CHECK_FLOAT_RANGE(800, ppos1->z);

        CHECK_FLOAT_RANGE(802, ppos2->x);
        CHECK_FLOAT_RANGE(803, ppos2->y);
        CHECK_FLOAT_RANGE(804, ppos2->z);
    }
}

void cCcS::CalcArea() {
    cM3dGAab aab;
    aab.ClearForMinMax();
    for (cCcD_Obj** pset_obj = mpObj; pset_obj < mpObj + mObjCount; ++pset_obj) {
        if (*pset_obj != NULL) {
            cCcD_ShapeAttr* pset_sa = (*pset_obj)->GetShapeAttr();
            JUT_ASSERT(826, pset_sa != NULL);

            pset_sa->CalcAabBox();
            aab.SetMinMax(pset_sa->GetWorkAab());
        }
    }

    mDivideArea.SetArea(aab);
    for (cCcD_Obj** pset_obj = mpObj; pset_obj < mpObj + mObjCount; ++pset_obj) {
        if (*pset_obj != NULL) {
            cCcD_ShapeAttr* pset_sa = (*pset_obj)->GetShapeAttr();
            JUT_ASSERT(839, pset_sa != NULL);

            cCcD_DivideInfo* divideInfo = (*pset_obj)->GetPDivideInfo();
            mDivideArea.CalcDivideInfo(divideInfo, pset_sa->GetWorkAab(),
                                       (*pset_obj)->ChkBsRevHit());
        }
    }
}

void cCcS::Move() {
    #if DEBUG
    JUT_ASSERT(866, m_debug_code == 1234);
    #endif

#if defined(DUSK_TARGET_3DS)
    {
        DUSK_SPLIT_SPAN(split_area, "cc.s.area");
        CalcArea();
    }
    {
        DUSK_SPLIT_SPAN(split_attg, "cc.s.attg");
        ChkAtTg();
    }
    {
        DUSK_SPLIT_SPAN(split_co, "cc.s.co");
        ChkCo();
    }
#else
    CalcArea();
    ChkAtTg();
    ChkCo();
#endif
    MoveAfterCheck();

    field_0x280c = mObjAtCount;
    field_0x280e = mObjTgCount;
    field_0x2810 = mObjCoCount;
    field_0x2812 = mObjCount;

    mObjAtCount = 0;
    mObjTgCount = 0;
    mObjCoCount = 0;
    mObjCount = 0;
}

#if defined(DUSK_TARGET_3DS)
unsigned cCcS::RemoveActor(const fopAc_ac_c* actor, const void* begin0, const void* end0,
                           const void* begin1, const void* end1) {
    unsigned removed = 0;
    // Address ranges first: they need no read through the collider.
    auto owned = [&](cCcD_Obj* obj) {
        if (obj == NULL) {
            return false;
        }
        const void* at = obj;
        if ((at >= begin0 && at < end0) || (at >= begin1 && at < end1)) {
            return true;
        }
        cCcD_Stts* stts = obj->GetStts();
        return stts != NULL && stts->GetActor() == actor;
    };
    auto compact = [&](cCcD_Obj** list, u16& count) {
        u16 kept = 0;
        for (u16 i = 0; i < count; ++i) {
            if (owned(list[i])) {
                ++removed;
            } else {
                list[kept++] = list[i];
            }
        }
        count = kept;
    };
    compact(mpObjAt, mObjAtCount);
    compact(mpObjTg, mObjTgCount);
    compact(mpObjCo, mObjCoCount);
    compact(mpObj, mObjCount);
    return removed;
}
#endif

void cCcS::DrawClear() {
    for (cCcD_Obj** obj = mpObjAt; obj < mpObjAt + mObjAtCount; ++obj) {
        *obj = NULL;
    }

    mObjAtCount = 0;
    for (cCcD_Obj** obj = mpObjTg; obj < mpObjTg + mObjTgCount; ++obj) {
        *obj = NULL;
    }

    mObjTgCount = 0;
    for (cCcD_Obj** obj = mpObjCo; obj < mpObjCo + mObjCoCount; ++obj) {
        *obj = NULL;
    }

    mObjCoCount = 0;
    for (cCcD_Obj** obj = mpObj; obj < mpObj + mObjCount; ++obj) {
        *obj = NULL;
    }

    mObjCount = 0;
}

void cCcS::SetCoGCorrectProc(cCcD_Obj* pco1_obj, cCcD_Obj* pco2_obj) {}

void cCcS::SetCoGObjInf(bool co2_set, bool co1_set, cCcD_GObjInf* pco1_gobj,
                        cCcD_GObjInf* pco2_gobj, cCcD_Stts* pco1_stts, cCcD_Stts* pco2_stts,
                        cCcD_GStts* pco1_gstts, cCcD_GStts* pco2_gstts) {}

void cCcS::SetAtTgGObjInf(bool param_0, bool param_1, cCcD_Obj* pat_obj, cCcD_Obj* ptg_obj,
                          cCcD_GObjInf* pat_gobj, cCcD_GObjInf* ptg_gobj, cCcD_Stts* pat_stts,
                          cCcD_Stts* ptg_stts, cCcD_GStts* pat_gstts, cCcD_GStts* ptg_gstts,
                          cXyz* pcross) {}

bool cCcS::ChkNoHitGAtTg(const cCcD_GObjInf* pat_gobj, const cCcD_GObjInf* ptg_gobj,
                         cCcD_GStts* pat_gstts, cCcD_GStts* ptg_gstts) {
    return false;
}

bool cCcS::ChkAtTgHitAfterCross(bool param_0, bool param_1, const cCcD_GObjInf* pat_gobj,
                                const cCcD_GObjInf* ptg_gobj, cCcD_Stts* pat_stts, cCcD_Stts* ptg_stts,
                                cCcD_GStts* pat_gstts, cCcD_GStts* ptg_gstts) {
    return false;
}

bool cCcS::ChkNoHitGCo(cCcD_Obj* pco1_obj, cCcD_Obj* pco2_obj) {
    return false;
}

void cCcS::MoveAfterCheck() {}
