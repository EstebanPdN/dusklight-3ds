#!/usr/bin/env python3
"""Exercise the native model and dependent Z-sort packet entry with host draw-list substitutes."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def body(source, signature):
    start = source.index(signature)
    # These functions use column-zero closing braces in m_Do_ext.cpp.
    return source[start:source.index('\n}', start) + 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'Source/src/m_Do/m_Do_ext.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    # Compile the production definitions, including their native preprocessor branches.
    signatures = [r'static (?:bool|void) mDoExt_entry\(', r'static (?:bool|void) mDoExt_modelDiff\(',
                  r'void mDoExt_modelEntryDL\(', r'void mDoExt_invisibleModel::entryDL\(']
    if 'static bool enterModelDisplayList(' in source:
        signatures.insert(2, 'static bool enterModelDisplayList\\(')
    compiled = '\n'.join(body(source, re.search(signature, source)[0]) for signature in signatures)
    fixture = r"""
#include <cassert>
#include <cstddef>
#include <cstdio>
#define TARGET_PC 1
#define DUSK_TARGET_3DS 1
using u16 = unsigned short;
struct cXyz {};
struct J3DDrawBuffer {};
J3DDrawBuffer original[2], zxlu;
struct {
    J3DDrawBuffer* b[2]{&original[0],&original[1]};
    J3DDrawBuffer* getDrawBuffer(int i) { return b[i]; }
    void setDrawBuffer(J3DDrawBuffer* p,int i) { b[i]=p; }
} j3dSys;
void dComIfGd_setListZxlu() { j3dSys.b[0]=j3dSys.b[1]=&zxlu; }
namespace dusk::game_clock { bool sim; bool is_sim_frame(){return sim;} }
namespace dNativeLod {
bool drawOwnerTiny;
struct Config {bool census{true};};
Config config(){return {};}
}
struct Shape {void show(){}};
struct Material {bool shared; void* getSharedDisplayListObj(){return shared?this:nullptr;}};
struct J3DModelData {
    Material mat{}; Shape shape; bool locked{};
    bool isLocked(){return locked;}
    Material* getMaterialNodePointer(int){return &mat;}
    Shape* getShapeNodePointer(int){return &shape;}
};
struct J3DModel {
    J3DModelData data; bool packetLocked{true};
    unsigned entries{},materials{},diffs{},views{},listSize{};
    J3DModelData* getModelData(){return &data;}
    void calcMaterial(){++materials;}
    void diff(){++diffs;}
    void lock(){packetLocked=true;}
    void unlock(){packetLocked=false;}
    void viewCalc(){++views;}
    void entry(){
        assert(j3dSys.b[0]==&zxlu && j3dSys.b[1]==&zxlu);
        if(!data.mat.shared || data.locked) assert(!packetLocked);
        ++entries;
        listSize=128;
    }
};
void modelMtxErrorCheck(J3DModel*){}
int lodHideParts(J3DModel*,u16*,int){return 0;}
void lodCountEntered(J3DModel*,bool){}
unsigned lodChoices;
void lodChooseLevels(J3DModel*){++lodChoices;}
struct mDoExt_invisibleModel {
    J3DModel* mModel; unsigned depth{},orphan{},lastEntry{};
    void entryJoint(cXyz*) {
        assert(j3dSys.b[0]==&zxlu && j3dSys.b[1]==&zxlu);
        ++depth;
        if(!mModel->listSize || lastEntry==mModel->entries) ++orphan;
        lastEntry=mModel->entries;
    }
    void entryDL(cXyz*);
};
"""
    test = r"""
int main(){
    for(bool shared:{false,true}) for(bool locked:{false,true}){
        J3DModel model; model.data.mat.shared=shared; model.data.locked=locked;
        if(shared) model.listSize=128;
        mDoExt_invisibleModel invisible{&model};
        for(bool tiny:{true,false,true,false}) for(bool sim:{true,false}){
            dNativeLod::drawOwnerTiny=tiny; dusk::game_clock::sim=sim;
            const unsigned e=model.entries, d=invisible.depth, v=model.views, m=model.materials, c=lodChoices;
            invisible.entryDL(nullptr);
            assert(model.entries-e==unsigned(sim&&!tiny));
            assert(lodChoices-c==model.entries-e); // levels of detail chosen once per entered model, never for a tiny one
            assert(invisible.depth-d==model.entries-e && !invisible.orphan);
            assert(model.packetLocked && model.views-v==unsigned(sim));
            assert(model.materials-m==unsigned(!sim||(shared&&!locked)));
            assert(model.materials==model.diffs);
            assert(j3dSys.b[0]==&original[0] && j3dSys.b[1]==&original[1]);
        }
    }
    std::puts("PASS paired color/depth entry on first use, after a populated packet, on re-entry and presentation-only updates; private/shared and locked/unlocked models, draw-buffer restoration");
}
"""
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp)
        cpp = path / 'entry.cpp'
        cpp.write_text('#include <initializer_list>\n' + fixture + compiled + test)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        str(cpp), '-o', str(path / 'entry')], check=True)
        subprocess.run([str(path / 'entry')], check=True)


if __name__ == '__main__':
    main()
