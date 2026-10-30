#!/usr/bin/env python3
"""Exercise production card consumer transitions with synthetic native CARD storage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_file_warning_animation import method

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source'
CONTROLLER = SOURCE / 'src/m_Do/m_Do_MemCard.cpp'
SELECT = SOURCE / 'src/d/d_file_select.cpp'
SELECT_HEADER = SOURCE / 'include/d/d_file_select.h'
FIXTURE = SOURCE / 'platforms/3ds/tests/memcard_permission_test.cpp'
CARD = SOURCE / 'platforms/3ds/runtime/card.cpp'


def controller_methods(source):
    # Preserve original method bodies, constants and the selected CARD_OPEN definition.
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith('#define CHECKSPACE_RESULT_'))
    opening = source[source.index('#if PLATFORM_WII\n#define CARD_OPEN'):
                     source.index('#if PLATFORM_WII\n#define NAND_OPEN')]
    work = source[source.index('ATTRIBUTE_ALIGN(32) static u8 MemCardWorkArea0'):
                  source.index('s32 mDoMemCd_Ctrl_c::mount()')]
    signatures = ['mDoMemCd_Ctrl_c::mDoMemCd_Ctrl_c()',
                  'void mDoMemCd_Ctrl_c::main()', 'void mDoMemCd_Ctrl_c::load()',
                  'u32 mDoMemCd_Ctrl_c::getStatus(u32)',
                  'void mDoMemCd_Ctrl_c::attach()', 'void mDoMemCd_Ctrl_c::command_attach()',
                  'void mDoMemCd_Ctrl_c::detach()', 's32 mDoMemCd_Ctrl_c::mount()',
                  's32 mDoMemCd_Ctrl_c::loadfile()', 's32 mDoMemCd_Ctrl_c::checkspace()',
                  'void mDoMemCd_Ctrl_c::setCardState(s32 i_result)',
                  'const char* mDoMemCd_Ctrl_c::getFileName()']
    return constants + '\n' + opening + work + '\n'.join(method(source, name) for name in signatures) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'clang++'))
    parser.add_argument('--output', type=Path, default=ROOT / 'Build/memcard-permission')
    parser.add_argument('--baseline', type=Path, help='Optional original controller TU; must fail with busy exit 4')
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler:
        parser.error('A C++20 host compiler is required')
    output = args.output.expanduser().resolve()
    if not output.is_relative_to(ROOT / 'Build'):
        parser.error('Synthetic storage and generated fixtures must stay under Build/')
    output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=output))
    env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    cases = [('usa', CONTROLLER, 0, 0), ('pal', CONTROLLER, 1, 0)]
    if args.baseline:
        cases.append(('before', args.baseline.resolve(), 1, 4))
    results = []
    for name, controller, version, expected in cases:
        case = work / name
        case.mkdir()
        (case / 'memcard_methods.inc').write_text(controller_methods(controller.read_text()))
        (case / 'memcard_select_enum.inc').write_text(method(SELECT_HEADER.read_text(), 'enum MemCardCheckProc_e') + ';\n')
        (case / 'memcard_select_method.inc').write_text(method(SELECT.read_text(), 'void dFile_select_c::MemCardStatCheck()') + '\n')
        binary = case / 'memcard-permission'
        command = [compiler, '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-pthread', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
                   '-DVERSION=' + str(version), '-I' + str(case),
                   '-I' + str(SOURCE / 'include'), '-I' + str(SOURCE / 'extern/aurora/include'),
                   '-I' + str(SOURCE / 'extern/aurora/include/dolphin'),
                   str(FIXTURE), str(CARD), '-o', str(binary)]
        build = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=180)
        (case / 'build.log').write_text(build.stdout + build.stderr)
        if build.returncode:
            print(build.stdout + build.stderr, end='')
            build.check_returncode()
        run = subprocess.run([str(binary)], cwd=case, env=env, capture_output=True, text=True, timeout=30)
        (case / 'run.log').write_text(run.stdout + run.stderr)
        print(run.stdout + run.stderr, end='')
        if run.returncode != expected:
            raise RuntimeError(f'{name}: exit {run.returncode}, expected {expected}; see {case}')
        inputs = [controller, SELECT, SELECT_HEADER, FIXTURE, CARD,
                  SOURCE / 'include/m_Do/m_Do_MemCard.h', SOURCE / 'extern/aurora/include/dolphin/card.h']
        results.append({'name': name, 'command': command, 'exit_code': run.returncode,
                        'expected_exit': expected,
                        'source_sha256': {str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path):
                                          hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}})
    report = work / 'results.json'
    report.write_text(json.dumps({'passed': True, 'runs': results,
                                 'limitations': 'Extracted actual controller/file-select methods and original enums; '
                                                'real runtime CARD with synthetic storage. Synchronous worker and '
                                                'UI presentation boundaries; no native ABI, thread scheduling, '
                                                'save-payload decode or gameplay acceptance.'}, indent=2) + '\n')
    print('PASS memcard permission consumer: ' + str(report))


if __name__ == '__main__':
    main()
