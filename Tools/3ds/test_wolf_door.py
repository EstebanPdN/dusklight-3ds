#!/usr/bin/env python3
"""Exercise the Fishing Pond door cut on the host, with optional disc verification."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ALINK = ROOT / 'Source/src/d/actor'
DEMO = ALINK / 'd_a_alink_demo.inc'
ANIMATION = ROOT / 'Source/libs/JSystem/src/J3DGraphAnimator/J3DAnimation.cpp'


def method(source, signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end] + '\n'


FIXTURE = r'''
#include <cassert>
#include <cmath>
#include <cstddef>

using s16 = short;
using f32 = float;
using u8 = unsigned char;
struct cXyz { float x, y, z; };
struct J3DTransformInfo { cXyz mTranslate; };
const cXyz l_waitBaseAnime{1.24279f, 102.00054f, 5.0f};
const cXyz l_wolfBaseAnime{1.0f, 88.63934f, -28.497932f};
constexpr float pi = 3.14159265358979323846f;
float cM_ssin(s16 angle) { return std::sin(angle * (pi / 32768)); }
float cM_scos(s16 angle) { return std::cos(angle * (pi / 32768)); }

#define DUSK_DESKTOP_ONLY(...)
struct J3DFrameCtrl {
    enum { EMode_NONE, EMode_RESET, EMode_LOOP, EMode_REVERSE, EMode_LOOP_REVERSE };
    s16 mStart{}, mEnd{}, mLoop{};
    u8 mAttribute{}, mState{};
    f32 mRate{}, mFrame{};
    void update();
    void setAttribute(u8 value) { mAttribute = value; }
    void setStart(s16 value) { mStart = value; }
    void setEnd(s16 value) { mEnd = value; }
    void setLoop(s16 value) { mLoop = value; }
    void setRate(f32 value) { mRate = value; }
    void setFrame(f32 value) { mFrame = value; }
    f32 getFrame() const { return mFrame; }
    f32 getRate() const { return mRate; }
    bool checkPass(f32 frame) const { return mFrame <= frame && frame < mFrame + mRate; }
};
struct daPy_frameCtrl_c : J3DFrameCtrl {
    bool end{}, nowSet{};
    bool getEndFlg() const { return end; }
    bool getNowSetFlg() const { return nowSet; }
    void onEndFlg() { end = true; }
    void onNowSetFlg() { nowSet = true; }
    void offNowSetFlg() { nowSet = false; }
    void offEndFlg() { end = nowSet = false; }
    bool checkAnmEnd();
    void updateFrame();
    void setFrameCtrl(u8, s16, s16, f32, f32);
};

// Substitute an authored-size clip with a known root curve. A full Wolf
// skeleton evaluation would reach joint 36, beyond this 35-track clip.
struct J3DAnmTransform {
    int frameMax = 111, rootReads{};
    float frame{};
    s16 getFrameMax() const { return frameMax; }
    void setFrame(f32 value) { frame = value; }
    void getTransform(int joint, J3DTransformInfo* out) {
        assert(joint == 0);
        ++rootReads;
        out->mTranslate = {l_waitBaseAnime.x + 0.25f * frame, 102.0f, 5.0f + 2.5f * frame};
    }
};
int cutEnds{};
void dComIfGp_evmng_cutEnd(int) { ++cutEnds; }
enum { Z2SE_AL_V_PUSH_ROCK, Z2SE_OBJ_KNOB_DOOR_LOCKED };

struct daAlink_c {
    enum daAlink_ANM { ANM_DOOR_OPEN_LEFT, ANM_DOOR_OPEN_RIGHT, ANM_DOOR_OPEN_LOCK_LEFT,
                      ANM_DOOR_OPEN_LOCK_RIGHT, ANM_DOOR_OPEN_ROLL, ANM_DOOR_OPEN_SLIDE };
    enum daAlink_WANM { WANM_WAIT, WANM_WALK_A };
    enum { PROC_DOOR_OPEN };
    bool wolf, humanClipBound{}, humanBlend{};
    int wolfClip = -1, wolfBinds{}, field_0x2f99{}, field_0x3198{}, mLeftHandIndex{}, mAlinkStaffId{};
    f32 field_0x3478{}, field_0x347c{}, mNormalSpeed{};
    cXyz field_0x34d4{}, field_0x3588{}, mWolfDoorPreviousRoot{};
    J3DAnmTransform clip;
    J3DAnmTransform* mWolfDoorMotion{};
    daPy_frameCtrl_c mUnderFrameCtrl[1], mUpperFrameCtrl[1];
    struct MotionHeap { void resetIdx() {} } mUpperAnmHeap[1];
    struct BckData { int m_underID; } data;
    struct Demo { int param = 3; int getParam0() const { return param; } } mDemo;
    struct { struct { struct { f32 mBasicInterpolation = 5.0f; } m; } mBasic; } hio, *mpHIO = &hio;
    struct { cXyz pos{780.0f, 200.0f, 11270.0f}; } current;
    struct { s16 y = -29127; } shape_angle;
    struct {
        bool wallOff{}, lineOff{};
        void SetWallNone() { wallOff = true; }
        void ClrWallNone() { wallOff = false; }
        void OnLineCheckNone() { lineOff = true; }
        void OffLineCheckNone() { lineOff = false; }
    } mLinkAcch;
    struct { s16 field_0x3008{}; } mProcVar0;
    struct { s16 field_0x300c{}; } mProcVar2;
    struct { s16 field_0x300e{}; } mProcVar3;
    struct { s16 field_0x3010{}; } mProcVar4;
    struct { s16 field_0x3012{}; } mProcVar5;

    explicit daAlink_c(bool wolfForm) : wolf(wolfForm) {}
    bool checkWolf() const { return wolf; }
    bool commonProcInitNotSameProc(int) { return true; }
    bool checkAnmEnd(daPy_frameCtrl_c* ctrl) { return ctrl->checkAnmEnd(); }
    BckData* getMainBckData(daAlink_ANM anm) { data.m_underID = anm; return &data; }
    J3DAnmTransform* getAnimeResource(MotionHeap* heap, int id, unsigned size) {
        assert(heap == &mUpperAnmHeap[0] && size >= 0x2CE0 && size <= 2 * 0x2C00);
        clip.frameMax = id == ANM_DOOR_OPEN_LOCK_LEFT || id == ANM_DOOR_OPEN_LOCK_RIGHT ? 39 : 111;
        return &clip;
    }
    void setSingleAnime(daAlink_ANM, f32 rate, f32 start, s16 end, f32) {
        humanClipBound = true;
        mUnderFrameCtrl[0].setFrameCtrl(J3DFrameCtrl::EMode_RESET, start, end < 0 ? 111 : end, rate, start);
    }
    void setSingleAnimeBaseMorf(daAlink_ANM anm, f32 morf) { setSingleAnime(anm, 1, 0, -1, morf); }
    void setSingleAnimeWolf(daAlink_WANM anm, f32, f32, s16, f32) { wolfClip = anm; ++wolfBinds; }
    void setSingleAnimeWolfBaseMorf(daAlink_WANM anm, f32) { wolfClip = anm; ++wolfBinds; }
    void setBlendMoveAnime(f32) { humanBlend = true; }
    void seStartMapInfo(int) {}
    void voiceStart(int) {}
    void onModeFlg(int) {}
    void offModeFlg(int) {}
    void animePlay(J3DAnmTransform*, daPy_frameCtrl_c*);
    void setWolfDoorAnime(daAlink_ANM, f32, s16, f32);
    int procDoorOpenInit();
    int procDoorOpen();
};
'''

CHECKS = r'''
int main() {
    daAlink_c wolf(true);
    wolf.procDoorOpenInit();
    assert(!wolf.humanClipBound && "35-track human door clip was bound to the 40-joint wolf rig");
    assert(wolf.wolfClip == wolf.WANM_WALK_A);
    assert(wolf.mUpperFrameCtrl[0].getFrame() == 38 && wolf.clip.frameMax == 111);
    assert(wolf.mLinkAcch.wallOff && wolf.mLinkAcch.lineOff && wolf.field_0x2f99 == 0);
    for (int tick = 0; tick < 73; ++tick) {
        wolf.procDoorOpen();
        assert(std::isfinite(wolf.current.pos.x) && std::isfinite(wolf.current.pos.z));
        assert(wolf.field_0x2f99 == 0 && cutEnds == 0);
    }
    wolf.procDoorOpen();
    const auto& pos = wolf.current.pos;
    const float sine = cM_ssin(wolf.shape_angle.y), cosine = cM_scos(wolf.shape_angle.y);
    assert(std::abs(pos.x - (780.0f + 111 * (0.25f * cosine + 2.5f * sine))) < 0.004f);
    // The original accumulation order rounds each increment at the world position's float precision.
    assert(std::abs(pos.z - (11270.0f + 111 * (2.5f * cosine - 0.25f * sine))) < 0.04f);
    assert(pos.y == 200.0f && wolf.clip.rootReads == 75);
    assert(cutEnds == 1 && wolf.wolfClip == wolf.WANM_WAIT && !wolf.humanBlend);
    assert(!wolf.mLinkAcch.wallOff && !wolf.mLinkAcch.lineOff);
    wolf.procDoorOpen();
    assert(cutEnds == 2 && wolf.wolfBinds == 2 && wolf.clip.rootReads == 75);

    cutEnds = 0;
    daAlink_c human(false);
    human.procDoorOpenInit();
    assert(human.humanClipBound && human.mUnderFrameCtrl[0].getFrame() == 38);
    for (int tick = 0; tick < 75 && cutEnds == 0; ++tick) {
        human.procDoorOpen();
        human.mUnderFrameCtrl[0].updateFrame();
    }
    assert(cutEnds == 1 && human.humanBlend && human.wolfBinds == 0 && human.clip.rootReads == 0);

    cutEnds = 0;
    daAlink_c locked(true);
    locked.mDemo.param = 7;
    locked.procDoorOpenInit();
    for (int tick = 0; tick < 60 && cutEnds == 0; ++tick) {
        locked.procDoorOpen();
    }
    assert(cutEnds == 1 && locked.clip.frameMax == 39 && !locked.humanClipBound && !locked.humanBlend);
}
'''


def verify_disc(path):
    # Import adjacent tooling even when the host runner uses python -I.
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from list_stage_spawns import be32, player_points, rarc_files, read_fst, yaz0

    with path.open('rb') as disc:
        files = read_fst(disc)

        def archive(file):
            offset, size = files[file]
            disc.seek(offset)
            return dict(rarc_files(yaz0(disc.read(size))))

        room = archive('/res/Stage/F_SP127/R00_00.arc')
        spawn = next(point for point in player_points(room['room.dzr']) if point['point'] == 2)
        assert spawn['mode'] == 11 and spawn['event'] == 255
        events = archive('/res/Object/Event.arc')['event_list.dat']
        animation = archive('/res/Object/AlAnm.arc')
        model = yaz0(archive('/res/Object/Wmdl.arc')['wl.bmd'])

    def name(data, offset, size=32):
        return data[offset:offset + size].split(b'\0', 1)[0]

    event_table, count, staff_table, _, cuts, _, data_table, _, _, _, ints = struct.unpack_from('>11I', events)
    event = next(offset for offset in range(event_table, event_table + count * 0xB0, 0xB0)
                 if name(events, offset) == b'KNOB_START_B')
    for index in range(be32(events, event + 0x7C)):
        staff = staff_table + be32(events, event + 0x2C + 4 * index) * 0x50
        if name(events, staff, 8) == b'Link':
            break
    else:
        raise AssertionError('KNOB_START_B has no Link staff')
    first_cut = cuts + be32(events, staff + 0x30) * 0x50
    door_cut = cuts + be32(events, first_cut + 0x3C) * 0x50
    assert name(events, door_cut) == b'035door'
    param = data_table + be32(events, door_cut + 0x38) * 0x40
    assert name(events, param) == b'prm0' and be32(events, ints + 4 * be32(events, param + 0x28)) == 3

    block = 32
    while model[block:block + 4] != b'JNT1':
        block += be32(model, block + 4)
        assert block < len(model)
    joints = struct.unpack_from('>H', model, block + 8)[0]
    for name, frames, tracks in [('dooropb.bck', 111, 35), ('wl_walka.bck', 27, joints),
                                 ('wl_waita.bck', 60, joints)]:
        bck = yaz0(animation[name])
        assert bck[32:36] == b'ANK1'
        assert struct.unpack_from('>2H', bck, 42) == (frames, tracks)
    names = block + be32(model, block + 20)
    foot_name_offset = struct.unpack_from('>H', model, names + 6 + 4 * 36)[0]
    assert joints == 40 and name(model, names + foot_name_offset) == b'BlegR4'
    door = yaz0(animation['dooropb.bck'])
    table, scale = struct.unpack_from('>2I', door, 52)
    scale_offset = struct.unpack_from('>H', door, 32 + table + 36 * 54 + 2)[0]
    assert scale + scale_offset * 4 >= be32(door, 36)
    print('F_SP127 point 2: human door clip misses Wolf joints 35–39, including hind foot 36')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-disc', type=Path)
    parser.add_argument('--source', type=Path, default=DEMO, help='Use an alternate door source for a baseline check')
    args = parser.parse_args()

    if args.private_disc:
        verify_disc(args.private_disc)

    actor = args.source.read_text()
    player = (ALINK / 'd_a_player.cpp').read_text()
    frame = ANIMATION.read_text()
    code = FIXTURE
    code += method(frame, 'void J3DFrameCtrl::update()')
    for signature in ('bool daPy_frameCtrl_c::checkAnmEnd()', 'void daPy_frameCtrl_c::updateFrame()',
                      'void daPy_frameCtrl_c::setFrameCtrl('):
        code += method(player, signature)
    code += method((ALINK / 'd_a_alink.cpp').read_text(), 'void daAlink_c::animePlay(')
    if 'void daAlink_c::setWolfDoorAnime(' in actor:
        code += method(actor, 'void daAlink_c::setWolfDoorAnime(')
    code += method(actor, 'int daAlink_c::procDoorOpenInit()')
    code += method(actor, 'int daAlink_c::procDoorOpen()')
    code += CHECKS

    with tempfile.TemporaryDirectory(prefix='dusklight-wolf-door-') as directory:
        fixture = Path(directory) / 'fixture.cpp'
        binary = Path(directory) / 'wolf-door-test'
        fixture.write_text(code)
        subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DDUSK_TARGET_3DS=1',
                        str(fixture), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS Wolf traversal, cut timing, locked-door transition, and human animation path')


if __name__ == '__main__':
    main()
