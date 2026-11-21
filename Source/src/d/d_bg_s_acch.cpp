/**
 * d_bg_s_acch.cpp
 * BG collision Actor Check
 */

#include <cstdio>
#include "d/dolzel.h" // IWYU pragma: keep

#include "d/actor/d_a_player.h"
#include "d/d_bg_s.h"
#include "d/d_bg_s_acch.h"
#include "d/d_tick_profile.h"
#include "d/d_com_inf_game.h"
#include "global.h"

#if DEBUG
#include "d/d_debug_viewer.h"
#endif

dBgS_AcchCir::dBgS_AcchCir() {
    m_flags = 0;
    m_wall_rr = 0.0f;
    field_0x2c = 0.0f;
    m_wall_h = 0.0f;
    m_wall_r = 0.0f;
    m_wall_angle_y = 0;
    m_wall_h_direct = 0.0f;
}

void dBgS_AcchCir::ClrWallHit() {
    m_flags &= ~FLAG_WALL_HIT;
    ClearPi();
}

void dBgS_AcchCir::SetWallR(f32 wall_r) {
    m_wall_r = wall_r;
}

void dBgS_AcchCir::CalcWallRR() {
    m_wall_rr = m_wall_r * m_wall_r;
}

void dBgS_AcchCir::SetWall(f32 wall_h, f32 wall_r) {
    SetWallH(wall_h);
    SetWallR(wall_r);
}

void dBgS_AcchCir::SetWallHDirect(f32 h_direct) {
    m_flags |= WALL_H_DIRECT;
    m_wall_h_direct = h_direct;
}

dBgS_Acch::~dBgS_Acch() {}

dBgS_Acch::dBgS_Acch() {
    SetPolyPassChk(GetPolyPassChkInfo());
    SetGrpPassChk(GetGrpPassChkInfo());

    m_flags = 0;
    SetRoofNone();

    pm_pos = NULL;
    pm_old_pos = NULL;
    pm_speed = NULL;
    m_ground_h = -G_CM3D_F_INF;
    field_0x90 = 0.0f;
    m_gnd_chk_offset = 60.0f;
    field_0x94 = 0.0f;
    field_0xb4 = 0;
    field_0xb8 = 0.0f;
    field_0xbc = 0.0f;

    m_tbl_size = 0;
    pm_acch_cir = NULL;

    m_roof_height = G_CM3D_F_INF;
    m_roof_crr_height = 0.0f;
    field_0xcc = 0.0f;

    m_wtr_mode = 1;
    m_wtr_chk_offset = 1000.0f;

    pm_angle = NULL;
    pm_shape_angle = NULL;
    m_my_ac = NULL;
    pm_out_poly_info = NULL;
    field_0xd8 = -G_CM3D_F_INF;
}

void dBgS_Acch::Init() {
    ClrWallHit();
    for (int i = 0; i < m_tbl_size; i++) {
        pm_acch_cir[i].ClrWallHit();
        pm_acch_cir[i].ClrWallHDirect();
    }
}

void dBgS_Acch::Set(cXyz* ppos, cXyz* pold_pos, fopAc_ac_c* pactor, int tbl_size,
                    dBgS_AcchCir* pacchcir, cXyz* pspeed, csXyz* pangle, csXyz* pshape_angle) {
    pm_pos = ppos;
    pm_old_pos = pold_pos;

    JUT_ASSERT(246, pm_pos != NULL);
    JUT_ASSERT(247, pm_old_pos != NULL);

    m_my_ac = pactor;

    fpc_ProcID id = fopAcM_GetID(pactor);
    SetActorPid(id);

    pm_speed = pspeed;
    m_tbl_size = tbl_size;
    pm_acch_cir = pacchcir;
    pm_angle = pangle;
    pm_shape_angle = pshape_angle;
}

void dBgS_Acch::Set(fopAc_ac_c* pactor, int tbl_size, dBgS_AcchCir* pacchcir) {
    m_tbl_size = tbl_size;
    pm_acch_cir = pacchcir;
    m_my_ac = pactor;

    SetActorPid(fopAcM_GetID(pactor));
    pm_pos = fopAcM_GetPosition_p(pactor);
    pm_old_pos = fopAcM_GetOldPosition_p(pactor);
    pm_speed = fopAcM_GetSpeed_p(pactor);
    pm_angle = fopAcM_GetAngle_p(pactor);
    pm_shape_angle = fopAcM_GetShapeAngle_p(pactor);
}

void dBgS_Acch::GroundCheckInit(dBgS& bgs) {
    UNUSED(bgs);

    if (!(m_flags & FLAG_GRND_NONE)) {
        m_ground_h = -G_CM3D_F_INF;
        m_gnd.SetExtChk(*this);
        field_0xb4 = ChkGroundHit();
        ClrGroundHit();
        ClrGroundLanding();
        ClrGroundAway();
    }
}

void dBgS_Acch::GroundCheck(dBgS& bgs) {
    if (!(m_flags & FLAG_GRND_NONE)) {
        cXyz grnd_pos;
        grnd_pos = *pm_pos;
        grnd_pos.y += field_0x94 + (m_gnd_chk_offset - field_0x90);

        if (!ChkGndThinCellingOff()) {
            static dBgS_RoofChk tmpRoofChk;
            tmpRoofChk.SetActorPid(m_gnd.GetActorPid());
            tmpRoofChk.SetPos(*pm_pos);

            f32 roof_y = bgs.RoofChk(&tmpRoofChk);
            if (grnd_pos.y > roof_y) {
                grnd_pos.y = roof_y;
            }
        }

        field_0x94 = 0.0f;
        m_gnd.SetPos(&grnd_pos);
        m_ground_h = bgs.GroundCross(&m_gnd);


        if (m_ground_h != -G_CM3D_F_INF) {
            field_0xbc = m_ground_h + field_0x90;
            if (field_0xbc > field_0xb8) {
                pm_pos->y = field_0xbc;

                if (ChkClrSpeedY() && pm_speed != NULL) {
                    pm_speed->y = 0.0f;
                }

                bgs.GetTriPla(m_gnd, &field_0xa0);
                SetGroundFind();
                SetGroundHit();

                if (field_0xc0 == 0) {
                    field_0xc0 = 1;
                    bgs.RideCallBack(m_gnd, m_my_ac);
                }

                if (field_0xb4 == 0) {
                    SetGroundLanding();
                }
            }
        }

        if (field_0xb4 && !ChkGroundHit()) {
            SetGroundAway();
        }
    }
}

void dBgS_Acch::GroundRoofProc(dBgS& bgs) {
    if (m_ground_h != -G_CM3D_F_INF) {
        if (field_0xbc < field_0xcc && field_0xcc < pm_pos->y) {
            pm_pos->y = field_0xcc;
        }

        if (!(m_flags & FLAG_ROOF_NONE) && m_ground_h >= m_roof_height) {
            m_roof.SetExtChk(*this);
            ClrRoofHit();
            cXyz roof_pos(*pm_pos);
            m_roof.SetPos(roof_pos);
            m_roof_height = bgs.RoofChk(&m_roof);
        }
    }
}

void dBgS_Acch::LineCheck(dBgS& bgs) {
    dBgS_RoofChk roof_chk;
    roof_chk.SetActorPid(m_gnd.GetActorPid());
    roof_chk.SetPos(*GetOldPos());

    f32 roof_y = dComIfG_Bgsp().RoofChk(&roof_chk);

    f32 var_f30 = G_CM3D_F_INF;
    bool var_r30 = false;
    bool var_r29 = false;

    for (int i = 0; i < GetTblSize(); i++) {
        cBgS_LinChk lin_chk;
        cXyz old_pos;
        cXyz pos;

        old_pos = *GetOldPos();
        pos = *GetPos();

        f32 wall_h = GetWallH(i);
        if (roof_y < old_pos.y + wall_h) {
            if (var_r29) {
                continue;
            } else {
                wall_h = (roof_y - old_pos.y) - 1.0f;
                var_r29 = true;
            }
        }

        if (var_f30 > wall_h) {
            var_f30 = wall_h;
        }

        old_pos.y += wall_h;
        pos.y += wall_h;

        if (GetSpeedY() < 0.0f) {
            if (m_my_ac != NULL && fopAcM_GetName(m_my_ac) == fpcNm_ALINK_e &&
                !static_cast<daPy_py_c*>(m_my_ac)->checkPlayerFly() && ChkLink())
            {
                pos.y -= GetSpeedY();
            }
        }

        lin_chk.Set2(&old_pos, &pos, GetActorPid());
        lin_chk.SetExtChk(*this);

        if (bgs.LineCross(&lin_chk)) {
            *GetPos() = lin_chk.GetCross();
            OnLineCheckHit();

            if (pm_out_poly_info != NULL)
                *pm_out_poly_info = lin_chk;

            cM3dGPla plane;
            bgs.GetTriPla(lin_chk, &plane);
            if (!cBgW_CheckBGround(plane.mNormal.y)) {
                PSVECAdd(GetPos(), &plane.mNormal, GetPos());
                f32 var_f28 = JMAFastSqrt(SQUARE(plane.mNormal.x) + SQUARE(plane.mNormal.z));
                if (!cM3d_IsZero(var_f28)) {
                    pm_acch_cir[i].SetWallHDirect(GetPos()->y);
                }

                GetPos()->y -= GetWallH(i);
            } else {
                GetPos()->y -= 1.0f;
                GroundCheck(bgs);
                var_r30 = true;
            }
        }
    }

    if (ChkLineDown()) {
        f32 temp_f1 = GetOldPos()->y - GetPos()->y;
        if (var_f30 != G_CM3D_F_INF && !var_r30 && !cM3d_IsZero(temp_f1)) {
            cBgS_LinChk lin_chk;
            cXyz old_pos;
            cXyz pos;

            old_pos = *GetPos();
            pos = old_pos;

            old_pos.y += var_f30;

            lin_chk.Set2(&old_pos, &pos, GetActorPid());
            lin_chk.SetExtChk(*this);

            if (bgs.LineCross(&lin_chk)) {
                *GetPos() = lin_chk.GetCross();
                OnLineCheckHit();

                if (pm_out_poly_info != NULL)
                    *pm_out_poly_info = lin_chk;

                GetPos()->y -= 1.0f;
                GroundCheck(bgs);
            }
        }
    }
}

#if defined(DUSK_TARGET_3DS)
#include "d/d_bg_w.h"
#include "d/d_native_logic.h"
#include "f_pc/f_pc_name.h"
#include <cstring>

// Native New 3DS (logic.txt crrskip): a door (DOOR20), a carried object
// (Obj_Carry) and, since Alpha 21, a pumpkin (OBJ_PUMPKIN), a barrel
// (Obj_GpTaru) or an item on the ground (ITEM) standing still runs CrrPos
// every tick with the same inputs (Alpha 21 measurement: 31 barrel calls in
// Hidden Village, 41 pumpkin calls in Ordon, 39 item calls in Zora's Domain
// per picture, 3.5, 1.5 and 1.3 ms in the emulator). CrrPos is the same
// function for every caller: its only effects beyond the kept images are the
// ride callback (excluded below) and the census. Its
// last call is kept: the whole dBgS_Acch and its wall circles, and the
// actor's position, old position, speed, angle and shape angle, before (input)
// and after (output) the call, with the collision-world clock and the slots
// whose objects could reach its queries (an XZ column around the old, new and
// corrected positions widened by twice the widest wall circle). A call whose
// input is byte-identical, while no such slot has changed (d_bg_s.cpp slot
// versions), gets the stored output instead of the queries. Not kept: calls
// with an output poly-info pointer, with a horse in play (special walls ask
// it), a ground/wall/roof object with its move flag set (the moving-BG
// correction reads its live matrix), or a stored call that reached a ride
// callback. Verify (crrskip=2) makes the call anyway and compares every byte.
namespace {
enum { kSkipEntries = 64, kSkipCircles = 2 };

struct SkipImage {
    u8 acch[sizeof(dBgS_Acch)];
    u8 cir[kSkipCircles][sizeof(dBgS_AcchCir)];
    cXyz pos, oldPos, speed;
    csXyz angle, shapeAngle;
};

struct SkipEntry {
    const dBgS_Acch* key;
    bool valid;
    u32 clock;
    int end;
    u32 mask[8];
    SkipImage in;
    SkipImage out;
};

SkipEntry l_skip[kSkipEntries];
unsigned l_skipMismatchLogs;

// Census of one window: why eligible calls were not served from the store,
// the first input region that differed, the slots that blocked.
struct SkipCount {
    const char* name;
    u32 count;
};
SkipCount l_skipReasons[8];
SkipCount l_skipRegions[16];
u32 l_skipBlockers[0x100];

// The kept profiles. Each is accepted on its own verify evidence (crrskip=2):
// eligible calls and checks/mismatches of the census window, and the checks
// and mismatches since the start.
const s16 kSkipProfiles[] = {fpcNm_DOOR20_e, fpcNm_Obj_Carry_e, fpcNm_OBJ_PUMPKIN_e, fpcNm_Obj_GpTaru_e,
                             fpcNm_ITEM_e};
const char* const kSkipProfileNames[] = {"DOOR20", "Obj_Carry", "OBJ_PUMPKIN", "Obj_GpTaru", "ITEM"};
enum { kSkipProfileCount = sizeof(kSkipProfiles) / sizeof(kSkipProfiles[0]) };
struct SkipProfileCount {
    u32 eligible, checks, mismatches;
    u32 totalChecks, totalMismatches;
};
SkipProfileCount l_skipProfiles[kSkipProfileCount];

int skipProfileIndex(s16 prof) {
    for (int i = 0; i < kSkipProfileCount; i++) {
        if (kSkipProfiles[i] == prof) {
            return i;
        }
    }
    return -1;
}

void skipCount(SkipCount* table, int size, const char* name) {
    for (int i = 0; i < size; i++) {
        if (table[i].name == name || table[i].name == NULL) {
            table[i].name = name;
            table[i].count++;
            return;
        }
    }
}

SkipEntry* skipFind(const dBgS_Acch* acch, bool create) {
    const unsigned home = (unsigned)((uintptr_t)acch >> 4) % kSkipEntries;
    for (unsigned k = 0; k < kSkipEntries; k++) {
        SkipEntry& e = l_skip[(home + k) % kSkipEntries];
        if (e.key == acch) {
            return &e;
        }
        if (e.key == NULL) {
            if (!create) {
                return NULL;
            }
            e.key = acch;
            e.valid = false;
            return &e;
        }
    }
    if (!create) {
        return NULL;
    }
    // Full: the home entry is taken over (exactness never depends on a hit).
    SkipEntry& e = l_skip[home];
    e.key = acch;
    e.valid = false;
    return &e;
}

struct AcchField {
    const char* name;
    unsigned offset;
};

}  // namespace

static void duskAcchCapture(const dBgS_Acch* acch, SkipImage& img) {
    std::memset(&img, 0, sizeof(img));
    std::memcpy(img.acch, acch, sizeof(dBgS_Acch));
    for (int k = 0; k < acch->m_tbl_size && k < kSkipCircles; k++) {
        std::memcpy(img.cir[k], &acch->pm_acch_cir[k], sizeof(dBgS_AcchCir));
    }
    img.pos = *acch->pm_pos;
    img.oldPos = *acch->pm_old_pos;
    if (acch->pm_speed != NULL) {
        img.speed = *acch->pm_speed;
    }
    if (acch->pm_angle != NULL) {
        img.angle = *acch->pm_angle;
    }
    if (acch->pm_shape_angle != NULL) {
        img.shapeAngle = *acch->pm_shape_angle;
    }
}

static void duskAcchRestore(dBgS_Acch* acch, const SkipImage& img) {
    std::memcpy((void*)acch, img.acch, sizeof(dBgS_Acch));
    for (int k = 0; k < acch->m_tbl_size && k < kSkipCircles; k++) {
        std::memcpy((void*)&acch->pm_acch_cir[k], img.cir[k], sizeof(dBgS_AcchCir));
    }
    *acch->pm_pos = img.pos;
    *acch->pm_old_pos = img.oldPos;
    if (acch->pm_speed != NULL) {
        *acch->pm_speed = img.speed;
    }
    if (acch->pm_angle != NULL) {
        *acch->pm_angle = img.angle;
    }
    if (acch->pm_shape_angle != NULL) {
        *acch->pm_shape_angle = img.shapeAngle;
    }
}

static dBgS_NativeColumn duskAcchColumn(const dBgS_Acch* acch, const SkipImage& in,
                                        const SkipImage& out) {
    f32 r = 0.0f;
    for (int k = 0; k < acch->m_tbl_size && k < kSkipCircles; k++) {
        dBgS_AcchCir* cir = (dBgS_AcchCir*)in.cir[k];
        if (cir->GetWallR() > r) {
            r = cir->GetWallR();
        }
    }
    const f32 margin = 2.0f * r + 100.0f;
    dBgS_NativeColumn c;
    c.minX = c.maxX = in.pos.x;
    c.minZ = c.maxZ = in.pos.z;
    const cXyz* pts[2] = {&in.oldPos, &out.pos};
    for (int k = 0; k < 2; k++) {
        if (pts[k]->x < c.minX) c.minX = pts[k]->x;
        if (pts[k]->x > c.maxX) c.maxX = pts[k]->x;
        if (pts[k]->z < c.minZ) c.minZ = pts[k]->z;
        if (pts[k]->z > c.maxZ) c.maxZ = pts[k]->z;
    }
    c.minX -= margin;
    c.maxX += margin;
    c.minZ -= margin;
    c.maxZ += margin;
    return c;
}

// The moving-BG correction at the start of CrrPos (dBgS::MoveBgCrrPos) reads
// a live matrix when the object under one of these poly infos has its move
// flag set: such a call is never kept or replayed.
static bool duskPolyOnMovingBg(dBgS& bgs, const cBgS_PolyInfo& poly) {
    if (!poly.ChkBgIndex()) {
        return false;
    }
    const int slot = poly.GetBgIndex();
    if (slot < 0 || slot >= 0x100 || !bgs.m_chk_element[slot].ChkUsed()) {
        return false;
    }
    return bgs.m_chk_element[slot].m_bgw_base_ptr->ChkMoveFlag() != 0;
}

static bool duskAcchMovingBg(dBgS& bgs, const dBgS_Acch* acch) {
    if (duskPolyOnMovingBg(bgs, acch->m_gnd) || duskPolyOnMovingBg(bgs, acch->m_roof)) {
        return true;
    }
    for (int k = 0; k < acch->m_tbl_size; k++) {
        if (duskPolyOnMovingBg(bgs, acch->pm_acch_cir[k])) {
            return true;
        }
    }
    return false;
}

// A stored call that reached RideCallBack (field_0xc0 is set exactly then)
// is replayable only when that callback does nothing.
static bool duskAcchRideCallbackSilent(dBgS& bgs, const SkipImage& out) {
    const dBgS_Acch* stored = (const dBgS_Acch*)out.acch;
    if (stored->field_0xc0 == 0) {
        return true;
    }
    if (!stored->m_gnd.ChkBgIndex()) {
        return false;
    }
    const int slot = stored->m_gnd.GetBgIndex();
    if (slot < 0 || slot >= 0x100 || !bgs.m_chk_element[slot].ChkUsed()) {
        return false;
    }
    const dBgW* dbgw = dynamic_cast<const dBgW*>(bgs.m_chk_element[slot].m_bgw_base_ptr);
    return dbgw == NULL || dbgw->m_ride_callback == NULL;
}

// The first byte where two images differ, named (acch member, wall circle,
// position, old position, speed, angles).
static const char* duskAcchRegion(const dBgS_Acch* acch, const SkipImage& stored,
                                  const SkipImage& real, unsigned* o_at, unsigned* o_offset) {
    const u8* a = (const u8*)&stored;
    const u8* b = (const u8*)&real;
    unsigned at = 0;
    while (at < sizeof(SkipImage) && a[at] == b[at]) {
        at++;
    }
    *o_at = at;
    const char* region = "acch";
    unsigned offset = at;
    const unsigned cirAt = (unsigned)((const u8*)stored.cir - a);
    const unsigned posAt = (unsigned)((const u8*)&stored.pos - a);
    if (at >= posAt) {
        region = at < posAt + 12 ? "pos" : at < posAt + 24 ? "old_pos" : at < posAt + 36 ? "speed"
                                                                        : "angle/shape_angle";
        offset = at - posAt;
    } else if (at >= cirAt) {
        region = (at - cirAt) < sizeof(dBgS_AcchCir) ? "circle0" : "circle1";
        offset = (at - cirAt) % sizeof(dBgS_AcchCir);
    } else {
        // Nearest named member of dBgS_Acch at or before the offset.
        const u8* base = (const u8*)acch;
        const AcchField fields[] = {
            {"m_flags", (unsigned)((const u8*)&acch->m_flags - base)},
            {"m_lin", (unsigned)((const u8*)&acch->m_lin - base)},
            {"m_wall_cyl", (unsigned)((const u8*)&acch->m_wall_cyl - base)},
            {"m_bg_index", (unsigned)((const u8*)&acch->m_bg_index - base)},
            {"field_0x90..0x94", (unsigned)((const u8*)&acch->field_0x90 - base)},
            {"m_ground_h", (unsigned)((const u8*)&acch->m_ground_h - base)},
            {"field_0xa0 (ground plane)", (unsigned)((const u8*)&acch->field_0xa0 - base)},
            {"field_0xb4..0xc0", (unsigned)((const u8*)&acch->field_0xb4 - base)},
            {"m_roof_height..", (unsigned)((const u8*)&acch->m_roof_height - base)},
            {"m_gnd", (unsigned)((const u8*)&acch->m_gnd - base)},
            {"m_roof", (unsigned)((const u8*)&acch->m_roof - base)},
            {"m_wtr", (unsigned)((const u8*)&acch->m_wtr - base)},
        };
        for (const AcchField& f : fields) {
            if (f.offset <= at) {
                region = f.name;
            }
        }
    }
    *o_offset = offset;
    return region;
}

static void duskAcchReportMismatch(const dBgS_Acch* acch, const SkipImage& stored,
                                   const SkipImage& real) {
    const u8* a = (const u8*)&stored;
    const u8* b = (const u8*)&real;
    unsigned at = 0, offset = 0;
    const char* region = duskAcchRegion(acch, stored, real, &at, &offset);
    const fopAc_ac_c* actor = acch->m_my_ac;
    OSReport("Native logic crrskip mismatch: actor profname=%d id=%u region=%s offset=0x%x stored=%02x "
             "real=%02x\n",
             actor != NULL ? (int)fopAcM_GetProfName(actor) : -1,
             actor != NULL ? (unsigned)fopAcM_GetID(actor) : 0u, region, offset,
             at < sizeof(SkipImage) ? a[at] : 0, at < sizeof(SkipImage) ? b[at] : 0);
}

// Census lines (f_pc_executor.cpp endPicture), then a reset.
void dBgS_Acch_NativeReportSkip(f32 perPicture) {
    char line[320];
    int at = std::snprintf(line, sizeof(line),
                           "Native logic crrskip by profile (eligible per picture, verify checks/mismatches of the "
                           "window, since start):");
    for (int i = 0; i < kSkipProfileCount; i++) {
        SkipProfileCount& c = l_skipProfiles[i];
        at += std::snprintf(line + at, sizeof(line) - at, " %s %.1f %u/%u %u/%u", kSkipProfileNames[i],
                            c.eligible * perPicture, c.checks, c.mismatches, c.totalChecks, c.totalMismatches);
        c.eligible = c.checks = c.mismatches = 0;
    }
    OSReport("%s\n", line);
    at = std::snprintf(line, sizeof(line), "Native logic crrskip misses (per picture):");
    for (SkipCount& c : l_skipReasons) {
        if (c.name != NULL && at < (int)sizeof(line) - 40) {
            at += std::snprintf(line + at, sizeof(line) - at, " %s=%.1f", c.name, c.count * perPicture);
        }
        c.name = NULL;
        c.count = 0;
    }
    at += std::snprintf(line + at, sizeof(line) - at, "; input first differs in:");
    for (SkipCount& c : l_skipRegions) {
        if (c.name != NULL && at < (int)sizeof(line) - 40) {
            at += std::snprintf(line + at, sizeof(line) - at, " %s=%.1f", c.name, c.count * perPicture);
        }
        c.name = NULL;
        c.count = 0;
    }
    OSReport("%s\n", line);
    at = std::snprintf(line, sizeof(line), "Native logic crrskip blocked by slot (slot:profname=per picture):");
    for (int rank = 0; rank < 8 && at < (int)sizeof(line) - 32; rank++) {
        int best = -1;
        for (int i = 0; i < 0x100; i++) {
            if (l_skipBlockers[i] != 0 && (best < 0 || l_skipBlockers[i] > l_skipBlockers[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        const cBgS_ChkElm& elm = dComIfG_Bgsp().m_chk_element[best];
        const int prof = elm.ChkUsed() && elm.m_actor_ptr != NULL
                             ? (int)fopAcM_GetProfName((fopAc_ac_c*)elm.m_actor_ptr)
                             : -1;
        at += std::snprintf(line + at, sizeof(line) - at, " %d:%d=%.1f", best, prof,
                            l_skipBlockers[best] * perPicture);
        l_skipBlockers[best] = 0;
    }
    OSReport("%s\n", line);
    for (int i = 0; i < 0x100; i++) {
        l_skipBlockers[i] = 0;
    }
}

void dBgS_Acch::CrrPos(dBgS& bgs) {
    const unsigned mode = dNativeLogic::config().crrSkip;
    if (mode == dNativeLogic::Off || (m_flags & 1) || m_my_ac == NULL) {
        CrrPosOriginal(bgs);
        return;
    }
    const int kept = skipProfileIndex(fopAcM_GetProfName(m_my_ac));
    if (kept < 0) {
        CrrPosOriginal(bgs);
        return;
    }
    // A kept profile: the census says why a call is not eligible.
    const char* notEligible = pm_out_poly_info != NULL               ? "out poly info"
                              : m_tbl_size > kSkipCircles             ? "wall circles"
                              : pm_pos == NULL || pm_old_pos == NULL  ? "no position"
                              : dComIfGp_getHorseActor() != NULL      ? "horse in play"
                                                                       : NULL;
    if (notEligible != NULL) {
        skipCount(l_skipReasons, 8, notEligible);
        CrrPosOriginal(bgs);
        return;
    }
    dNativeLogic::Census& census = dNativeLogic::census;
    census.crrEligible++;
    l_skipProfiles[kept].eligible++;

    SkipImage now;  // on the stack: CrrPos may run inside another CrrPos (ride callbacks)
    duskAcchCapture(this, now);
    SkipEntry* e = skipFind(this, true);
    bool hit = false;
    if (!e->valid) {
        skipCount(l_skipReasons, 8, "no stored call");
    } else if (std::memcmp(&e->in, &now, sizeof(SkipImage)) != 0) {
        unsigned at = 0, offset = 0;
        skipCount(l_skipReasons, 8, "input differs");
        skipCount(l_skipRegions, 16, duskAcchRegion(this, e->in, now, &at, &offset));
    } else if (duskAcchMovingBg(bgs, this)) {
        skipCount(l_skipReasons, 8, "moving BG under it");
    } else if (!duskAcchRideCallbackSilent(bgs, e->out)) {
        skipCount(l_skipReasons, 8, "ride callback");
    } else {
        const dBgS_NativeColumn column = duskAcchColumn(this, e->in, e->out);
        int blocker = -1;
        hit = dBgS_NativeUnchanged(bgs, e->clock, e->mask, e->end, column, &blocker);
        if (!hit) {
            skipCount(l_skipReasons, 8, "collision object changed");
            if (blocker >= 0 && blocker < 0x100) {
                l_skipBlockers[blocker]++;
            }
        }
    }
    if (hit) {
        census.crrHits++;
        if (mode == dNativeLogic::On) {
            duskAcchRestore(this, e->out);
            census.crrSkipped++;
            return;
        }
    }

    const u32 clockBefore = dBgS_NativeClock();
    // The moving-BG correction at the start reads the input poly infos.
    const bool movingBefore = duskAcchMovingBg(bgs, this);
    CrrPosOriginal(bgs);
    SkipImage out;
    duskAcchCapture(this, out);

    if (hit) {
        // Verify: the stored output against the real one, every byte.
        census.crrChecks++;
        l_skipProfiles[kept].checks++;
        l_skipProfiles[kept].totalChecks++;
        if (std::memcmp(&e->out, &out, sizeof(SkipImage)) != 0) {
            census.crrMismatches++;
            l_skipProfiles[kept].mismatches++;
            l_skipProfiles[kept].totalMismatches++;
            if (l_skipMismatchLogs++ < 8) {
                duskAcchReportMismatch(this, e->out, out);
            }
        }
    }

    // Keep this call (only when the world did not change during it).
    e->valid = dBgS_NativeClock() == clockBefore && !movingBefore;
    if (e->valid) {
        e->in = now;
        e->out = out;
        e->clock = clockBefore;
        dBgS_NativeTouchMask(bgs, duskAcchColumn(this, now, out), e->mask, &e->end);
    }
}

#endif

#if defined(DUSK_TARGET_3DS)
void dBgS_Acch::CrrPosOriginal(dBgS& bgs) {
#else
void dBgS_Acch::CrrPos(dBgS& bgs) {
#endif
    DUSK_TICK_PROFILE(tick_profile, "acch.CrrPos");
    bool bvar9;
    if (!(m_flags & 1)) {
        DUSK_SPLIT_SPAN(split_move, "acch.s.move");
        JUT_ASSERT(792, pm_pos != NULL);
        JUT_ASSERT(793, pm_old_pos != NULL);

        JUT_ASSERT(833, !isnan(pm_pos->x));
        JUT_ASSERT(834, !isnan(pm_pos->y));
        JUT_ASSERT(835, !isnan(pm_pos->z));

        JUT_ASSERT(837, -1.0e32f < pm_pos->x && pm_pos->x < 1.0e32f);
        JUT_ASSERT(838, -1.0e32f < pm_pos->y);
        JUT_ASSERT(839, pm_pos->y < 1.0e32f);
        JUT_ASSERT(840, -1.0e32f < pm_pos->z && pm_pos->z < 1.0e32f);

        bgs.MoveBgCrrPos(m_gnd, ChkGroundHit(), pm_pos, pm_angle, pm_shape_angle, false, false);

        if (!ChkGroundHit()) {
            bvar9 = false;

            if (ChkWallHit()) {
                for (int i = 0; i < m_tbl_size; i++) {
                    if (pm_acch_cir[i].ChkWallHit()) {
                        bgs.MoveBgCrrPos(pm_acch_cir[i], true, pm_pos, pm_angle, pm_shape_angle,
                                           true, false);
                        bvar9 = true;
                        break;
                    }
                }
            }

            if (!bvar9 && ChkRoofHit()) {
                bgs.MoveBgCrrPos(m_roof, true, pm_pos, pm_angle, pm_shape_angle, false, true);
            }
        }

        GroundCheckInit(bgs);
        Init();

        f32 wall_all_low_h_r = GetWallAllLowH_R();
        f32 old_pos_dist2XZ = GetOldPos()->abs2XZ(*GetPos());
        f32 old_pos_dist_y = GetOldPos()->y - GetPos()->y;
        f32 wall_all_low_h = GetWallAllLowH();

        field_0xb8 = GetPos()->y;
        field_0xc0 = 0;
        f32 fvar12 = wall_all_low_h + GetOldPos()->y;
        f32 fvar1 = m_gnd_chk_offset + GetPos()->y;
        bool bvar2 = false;

        split_move.stop();
        OffLineCheckHit();
        if (!ChkLineCheckNone() && !cM3d_IsZero(wall_all_low_h_r) &&
            (old_pos_dist2XZ > SQUARE(wall_all_low_h_r) || fvar12 > fvar1 || old_pos_dist_y > m_gnd_chk_offset || ChkLineCheck()))
        {
            bvar2 = true;
            DUSK_SPLIT_SPAN(split_line, "acch.s.line");
            LineCheck(bgs);
            split_line.stop();
        }

        DUSK_SPLIT_SPAN(split_wall, "acch.s.wall");
        if (!(m_flags & FLAG_WALL_NONE)) {
            if (ChkWallSort()) {
                bgs.WallCorrectSort(this);
            } else {
                bgs.WallCorrect(this);
            }
        }

        split_wall.stop();

        if (ChkWallHit() && bvar2) {
            DUSK_SPLIT_SPAN(split_line2, "acch.s.line2");
            LineCheck(bgs);
            split_line2.stop();
        }

        DUSK_SPLIT_SPAN(split_roof, "acch.s.roof");
        field_0xcc = G_CM3D_F_INF;

        if (!(m_flags & FLAG_ROOF_NONE)) {
            m_roof.SetExtChk(*this);
            ClrRoofHit();

            cXyz roof_pos(*pm_pos);
            m_roof.SetPos(roof_pos);
            m_roof_height = bgs.RoofChk(&m_roof);

            if (m_roof_height != G_CM3D_F_INF) {
                if (pm_pos->y + m_roof_crr_height > m_roof_height) {
                    field_0xcc = m_roof_height - m_roof_crr_height;
                    SetRoofHit();
                }
            }
        }

        split_roof.stop();
        DUSK_SPLIT_SPAN(split_ground, "acch.s.ground");
        if (!(m_flags & FLAG_GRND_NONE)) {
            ClrGroundFind();
            GroundCheck(bgs);
            GroundRoofProc(bgs);
        } else {
            if (field_0xcc < pm_pos->y) {
                pm_pos->y = field_0xcc;
            }
        }

        split_ground.stop();
        DUSK_SPLIT_SPAN(split_water, "acch.s.water");
        if (!(m_flags & FLAG_WATER_NONE)) {
            ClrWaterHit();
            ClrWaterIn();
            m_wtr.SetHeight(-G_CM3D_F_INF);

            bool spD = false;

            f32 var_f29;
            f32 top;
            if (m_ground_h == -G_CM3D_F_INF) {
                var_f29 = pm_pos->y - 50.0f;
                spD = true;
            } else {
                var_f29 = m_ground_h;
            }

            if (m_wtr_mode == 1) {
                top = var_f29 + m_wtr_chk_offset;
            } else {
                dBgS_RoofChk roof_chk;
                roof_chk.SetUnderwaterRoof();
                roof_chk.SetPos(*pm_pos);

                top = bgs.RoofChk(&roof_chk);
                if (top == G_CM3D_F_INF) {
                    top = pm_pos->y + 1000000.0f;
                }
            }

            cXyz ground;
            ground = *pm_pos;
            ground.y = var_f29;

            m_wtr.Set(ground, top);
            m_wtr.SetPassChkInfo(*this);

            if (bgs.WaterChk(&m_wtr)) {
                SetWaterHit();
                if (m_wtr.GetHeight() > pm_pos->y) {
                    SetWaterIn();
                }

                JUT_ASSERT(1095, m_wtr.GetHeight() >= ground.y);
                JUT_ASSERT(1096, m_wtr.GetHeight() <= top);
            }
        }

        split_water.stop();
        JUT_ASSERT(1124, !isnan(pm_pos->x));
        JUT_ASSERT(1125, !isnan(pm_pos->y));
        JUT_ASSERT(1126, !isnan(pm_pos->z));

        JUT_ASSERT(1128, -1.0e32f < pm_pos->x && pm_pos->x < 1.0e32f);
        JUT_ASSERT(1129, -1.0e32f < pm_pos->y && pm_pos->y < 1.0e32f);
        JUT_ASSERT(1130, -1.0e32f < pm_pos->z && pm_pos->z < 1.0e32f);
    }
}

f32 dBgS_Acch::GetWallAllR() {
    f32 rt = 0.0f;

    for (int i = 0; i < m_tbl_size; i++) {
        if (rt < pm_acch_cir[i].GetWallR()) {
            rt = pm_acch_cir[i].GetWallR();
        }
    }

    return rt;
}

void dBgS_Acch::SetWallCir() {
    for (int i = 0; i < m_tbl_size; i++) {
        pm_acch_cir[i].SetCir(*pm_pos);
    }
}

void dBgS_Acch::CalcWallBmdCyl() {
    if (m_tbl_size <= 0) {
        m_wall_cyl.Set(*pm_pos, 0.0f, 0.0f);
    } else {
        f32 wall_all_r = GetWallAllR();
        f32 wall_h_max = pm_acch_cir->GetWallH();
        f32 wall_h_min = pm_acch_cir->GetWallH();

        if (m_tbl_size >= 1) {
            for (int i = 0; i < m_tbl_size; i++) {
                if (wall_h_max > pm_acch_cir[i].GetWallH()) {
                    wall_h_max = pm_acch_cir[i].GetWallH();
                }

                if (wall_h_min < pm_acch_cir[i].GetWallH()) {
                    wall_h_min = pm_acch_cir[i].GetWallH();
                }
            }
        }

        cXyz xyz(*pm_pos);
        xyz.y += wall_h_max;

        m_wall_cyl.Set(xyz, wall_all_r, wall_h_min - wall_h_max);
    }
}

void dBgS_Acch::SetGroundUpY(f32 param_0) {
    field_0x94 = param_0 - field_0x90;
    field_0x90 = param_0;
}

f32 dBgS_Acch::GetWallAllLowH() {
    if (m_tbl_size <= 0) {
        return 0.0f;
    }

    f32 wall_low_h = pm_acch_cir->GetWallH();
    for (int i = 1; i < m_tbl_size; i++) {
        if (wall_low_h > pm_acch_cir[i].GetWallH()) {
            wall_low_h = pm_acch_cir[i].GetWallH();
        }
    }

    return wall_low_h;
}

f32 dBgS_Acch::GetWallAllLowH_R() {
    if (m_tbl_size <= 0) {
        return 0.0f;
    }

    int index = 0;
    f32 wall_h = pm_acch_cir->GetWallH();
    for (int i = 1; i < m_tbl_size; i++) {
        if (wall_h > pm_acch_cir[i].GetWallH()) {
            wall_h = pm_acch_cir[i].GetWallH();
            index = i;
        }
    }

    return pm_acch_cir[index].GetWallR();
}

f32 dBgS_Acch::GetSpeedY() {
    if (pm_speed != NULL) {
        return pm_speed->y;
    }

    return 0.0f;
}

#if DEBUG
static GXColor blue = {0, 0, 0xFF, 0x80};
static GXColor red = {0xFF, 0, 0, 0x80};

void dBgS_Acch::DrawWall(dBgS& bgs) {
    if (bgs.m_hio.ChkAcchDrawWall()) {
        if (!(m_flags & FLAG_WALL_NONE)) {
            for (int i = 0; i < m_tbl_size; i++) {
                cM3dGCir cir;
                cir = *pm_acch_cir[i].GetCirP();

                cXyz pos;
                pos.x = cir.GetCx();
                pos.y = cir.GetHeight();
                pos.z = cir.GetCy();

                f32 radius = cir.GetR();
                dDbVw_drawCylinderXlu(pos, radius, 3.0f, blue, TRUE);

                if (pm_acch_cir[i].ChkWallHit()) {
                    bgs.DrawPoly(pm_acch_cir[i], blue);
                }
            }
        } else {
            SetWallCir();

            for (int i = 0; i < m_tbl_size; i++) {
                cM3dGCir cir;
                cir = *pm_acch_cir[i].GetCirP();

                cXyz pos;
                pos.x = cir.GetCx();
                pos.y = cir.GetHeight();
                pos.z = cir.GetCy();

                f32 radius = cir.GetR();
                dDbVw_drawCylinderXlu(pos, radius, 3.0f, red, TRUE);
            }
        }
    }
}
#endif

f32 dBgS_Acch::GetWallAddY(Vec& param_0) {
    if (!ChkGroundFind() || field_0xa0.mNormal.y < 0.5f) {
        return 0.0f;
    }

    f32 tmp;
    if (field_0xa0.getCrossYLessD(param_0, &tmp)) {
        if (tmp > 0.0f) {
            tmp = 0.0f;
        }
        return -tmp;
    }

    return 0.0f;
}

void dBgS_Acch::SetNowActorInfo(int bg_index, void* param_1, fpc_ProcID proc_id) {
    m_bg_index = bg_index;
    field_0x7c = param_1;
    field_0x80 = proc_id;
}

void dBgS_Acch::SetWallPolyIndex(int index, int poly_index) {
    JUT_ASSERT(1534, index <= m_tbl_size);

    pm_acch_cir[index].SetActorInfo(m_bg_index, field_0x7c, field_0x80);
    pm_acch_cir[index].SetWallPolyIndex(poly_index);
}

void dBgS_Acch::CalcMovePosWork() {
    SetWallCir();
    SetLin();
    CalcWallBmdCyl();
}

void dBgS_Acch::CalcWallRR() {
    for (int i = 0; i < m_tbl_size; i++) {
        pm_acch_cir[i].CalcWallRR();
    }
}

void dBgS_Acch::SetMoveBGOnly() {
    m_flags |= FLAG_MOVE_BG_ONLY;
    m_wtr.OnMoveBGOnly();
}

void dBgS_Acch::ClrMoveBGOnly() {
    m_flags &= ~FLAG_MOVE_BG_ONLY;
    m_wtr.OffMoveBGOnly();
}

void dBgS_Acch::SetGndThinCellingOff() {
    m_flags |= FLAG_GND_THIN_CELLING_OFF;
}

void dBgS_Acch::ClrGndThinCellingOff() {
    m_flags &= ~FLAG_GND_THIN_CELLING_OFF;
}

bool dBgS_Acch::ChkGndThinCellingOff() {
    return m_flags & FLAG_GND_THIN_CELLING_OFF;
}

void dBgS_Acch::OnWallSort() {
    m_flags |= FLAG_WALL_SORT;
}

bool dBgS_Acch::ChkWallSort() {
    return m_flags & FLAG_WALL_SORT;
}

bool dBgS_Acch::ChkLineDown() {
    return m_flags & FLAG_LINE_DOWN;
}
