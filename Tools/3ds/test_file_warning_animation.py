#!/usr/bin/env python3
"""Compile actual warning/menu methods against bounded host presentation stubs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for at in range(brace, len(source)):
        if source[at] == '{':
            depth += 1
        elif source[at] == '}':
            depth -= 1
            if depth == 0:
                return source[start:at + 1]
    raise ValueError('unterminated method: ' + signature)


PRELUDE = r'''
#include <cstdio>
#include <stdexcept>
using u8 = unsigned char;
using u32 = unsigned;
using BOOL = int;
using f32 = float;
#if TARGET_PC
#define DUSK_IF_ELSE(a,b) a
#else
#define DUSK_IF_ELSE(a,b) b
#endif
#define DUSK_DESKTOP_ORIGINAL(a,b) b
struct Animation { float frame{}; unsigned calls{};
    void setFrame(float v) { frame=v; ++calls; } };
struct Screen { unsigned calls{}; void animation() { ++calls; } };
struct Pane { float getTranslateX() { return 0; } void translate(float,float) {} };
'''

MENU_STUB = r'''
struct { int mCardWaitFrames=90; } g_msHIO;
constexpr int Z2SE_SY_FILE_SAVE_LEVEL=0, PROC_MEMCARD_DATA_SAVE_WAIT=17;
void mDoAud_seStartLevel(int,void*,int,int) {}
struct dMenu_save_c {
    dFile_warning_c* mWarning;
    unsigned blocked=0, writes=0, mSelectedFile=0;
    int mWaitTimer=0, mMenuProc=0;
    bool headerTxtChangeAnm() { return !(blocked&1); }
    bool selectDataMoveAnm() { return !(blocked&2); }
    bool selectWakuAlpahAnm(unsigned) { return !(blocked&4); }
    bool ketteiTxtDispAnm() { return !(blocked&8); }
    bool modoruTxtDispAnm() { return !(blocked&16); }
    void dataWrite() { ++writes; }
    void saveMoveDisp2();
};
'''

TEST = r'''
void check(bool okay,const char* message) {
    if (!okay) throw std::runtime_error(message);
}
int main() {
 try {
    Animation animation; Screen screen; Pane pane;
    dFile_warning_c warning{};
    warning.field_0x24=&animation; warning.mFileWarn.Scr=&screen;
    warning.mpRootPane=&pane;
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
    warning.openInit();
    for(unsigned i=0;i<20;++i) warning._move();
    check(warning.field_0x28==2849 && !warning.getStatus() && screen.calls==0,
          "desktop simulation unexpectedly advanced presentation");
    warning.field_0x28=2859; // Isolated desktop presentation completion control.
    warning._move();
    check(warning.getStatus() && warning.field_0x18==0,"desktop completion failed");
#else
    for(unsigned cycle=0;cycle<4;++cycle) {
        warning.openInit();
        check(!warning.getStatus() && warning.field_0x18==1,"open initial state");
        const unsigned initial=screen.calls;
        for(unsigned i=1;i<=5;++i) {
            warning._move();
            check(warning.field_0x28==2849+2*int(i),"native opening did not advance");
            check(bool(warning.getStatus())==(i==5),"opening completion timing");
            check(animation.frame==warning.field_0x28 && screen.calls==initial+i,
                  "opening was not applied to screen");
        }
        check(warning.field_0x18==0 && warning.field_0x3c==1,"open terminal state");
        warning._move();
        check(screen.calls==initial+5,"idle animation advanced");
        warning.closeInit();
        for(unsigned i=1;i<=5;++i) {
            warning._move();
            check(warning.field_0x28==2859-2*int(i),"closing did not advance");
            check(bool(warning.getStatus())==(i==5),"closing completion timing");
        }
        check(warning.field_0x18==0 && warning.field_0x3c==0,"close terminal state");
    }
    // The real save transition must not dispatch I/O before warning readiness.
    warning.openInit(); dMenu_save_c menu{&warning};
    for(unsigned i=0;i<5;++i) {
        menu.saveMoveDisp2(); check(menu.writes==0,"premature save dispatch");
        warning._move();
    }
    for(unsigned bit=1;bit<=16;bit<<=1) {
        menu.blocked=bit; menu.saveMoveDisp2();
        check(menu.writes==0,"other animation gate bypassed");
    }
    menu.blocked=0; menu.saveMoveDisp2();
    check(menu.writes==1 && menu.mWaitTimer==90 &&
          menu.mMenuProc==PROC_MEMCARD_DATA_SAVE_WAIT,"save transition did not dispatch");
    // Clamp an odd-distance endpoint without overshoot, using actual production code.
    warning.openInit(); warning.field_0x28=2858; warning._move();
    check(warning.field_0x28==2859 && warning.getStatus(),"open overshoot");
    warning.closeInit(); warning.field_0x28=2850; warning._move();
    check(warning.field_0x28==2849 && warning.getStatus(),"close overshoot");
#endif
    std::puts("PASS actual warning methods and save transition gates"); return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 4; }
}
'''


def build_fixture(warning_source):
    header = (ROOT / 'Source/include/d/d_file_sel_warning.h').read_text()
    # Use actual field declarations/types; replace only construction/destruction
    # and external presentation interfaces. No layout/ABI assertion is made here.
    fields = header[header.index('    /* 0x04 */ JKRArchive* mpArchive;'):header.index('\n};\n\n#endif')]
    fields = fields.replace('JKRArchive*', 'void*').replace('dDlst_FileWarn_c', 'FileWarn')
    fields = fields.replace('CPaneMgr*', 'Pane*').replace('J2DTextBox*', 'void*')
    fields = fields.replace('J2DAnmTransform*', 'Animation*')
    declaration = 'struct FileWarn { Screen* Scr; };\nstruct dFile_warning_c {\n'
    declaration += 'void _move(); void modeWait(); void modeMove(); bool baseMoveAnm();\n'
    declaration += 'void openInit(); void closeInit(); BOOL getStatus() { return mStatus != 0; }\n'
    declaration += fields + '\n};\n'
    names = ['void dFile_warning_c::_move()', 'void dFile_warning_c::modeWait()',
             'void dFile_warning_c::modeMove()', 'bool dFile_warning_c::baseMoveAnm()',
             'void dFile_warning_c::openInit()', 'void dFile_warning_c::closeInit()']
    table = 'using procFunc=void(dFile_warning_c::*)();\nstatic procFunc fileWarningProc[]={&dFile_warning_c::modeWait,&dFile_warning_c::modeMove};\n'
    menu = (ROOT / 'Source/src/d/d_menu_save.cpp').read_text()
    return PRELUDE + declaration + table + '\n'.join(method(warning_source,n) for n in names) + MENU_STUB + method(menu,'void dMenu_save_c::saveMoveDisp2()') + TEST


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--baseline',type=Path)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    source=ROOT/'Source/src/d/d_file_sel_warning.cpp'
    results=[]
    cases=[('native',source,['-DTARGET_PC=1','-DDUSK_TARGET_3DS=1'],0),
           ('desktop',source,['-DTARGET_PC=1'],0),('original',source,['-DTARGET_PC=0'],0)]
    if args.baseline:cases.append(('native-before',args.baseline,['-DTARGET_PC=1','-DDUSK_TARGET_3DS=1'],4))
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
    for name,path,defines,expected in cases:
        cpp=args.output/(name+'.cpp'); binary=args.output/name
        cpp.write_text(build_fixture(path.read_text()))
        command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-omit-frame-pointer',*defines,str(cpp),'-o',str(binary)]
        compile_result=subprocess.run(command,capture_output=True,text=True)
        (args.output/(name+'-build.log')).write_text(compile_result.stdout+compile_result.stderr)
        if compile_result.returncode:raise RuntimeError(name+' compile failed')
        run=subprocess.run([str(binary.resolve())],capture_output=True,text=True,env=env)
        (args.output/(name+'.log')).write_text(run.stdout+run.stderr)
        if run.returncode!=expected:raise RuntimeError(name+' unexpected outcome '+str(run.returncode))
        if expected and 'FAIL native opening did not advance' not in run.stderr:raise RuntimeError('wrong negative control')
        results.append({'name':name,'command':command,'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                        'fixture_sha256':hashlib.sha256(cpp.read_bytes()).hexdigest(),'exit_code':run.returncode,'expected_exit':expected})
    (args.output/'results.json').write_text(json.dumps({'passed':True,'runs':results,'limitations':'Actual extracted methods with presentation/card-dispatch substitutes; no native ABI, GPU, storage or gameplay acceptance.'},indent=2)+'\n')
    print('PASS',len(results),'host configurations, including baseline negative control' if args.baseline else 'host configurations')


if __name__=='__main__':main()
