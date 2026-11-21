#!/usr/bin/env python3
"""Native PICA blocks (Source/platforms/3ds/runtime/pica_blocks.hpp) on the host.

Builds Source/platforms/3ds/tests/pica_blocks_test.cpp with the real planners
(ASan/UBSan) and runs it: a block captured from one register bank and replayed
on another, with its palette placed from the new matrices and patched into the
kept words, must be byte for byte the words today's replay path emits for the
new bank, whenever the block's checks pass (matrix-only changes, and single
word changes anywhere today's uniform rows read outside the record key). Also
the uniform layout parser, the store and the report line lengths. A host
benchmark (-O2) times a replay through today's planner and emission model, a
block hit and a rows-mode hit. Rows mode:
when the inputs changed (camera, lights, fog, texture matrices), every row
today's planner uploads, patched into the kept words, must again be today's
words byte for byte.

Mutation controls: copies of pica_blocks.cpp with one rule broken (lights left
out of the inputs, the origin fold skipped, the row word order reversed, fog
registers left out, the material row left out of rows mode) must make the test fail. pica_blocks.cpp is also compiled
for the ARM11 (-fstack-usage).

Host floats and a model of Citro3D's emission: the console words are compared
by the game itself with pica-blocks.txt set to v (verify).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT/'Source/platforms/3ds'
OUT = ROOT/'Diagnostics/alpha12/pica-blocks'
MUTATIONS = {
    'no-lights': ('for(unsigned light=0;light<8;++light)if(lights&(1u<<light))if(!add(0,0x600+16*light,16))return false;', '(void)lights;'),
    'no-fold': ('if(apply)v[3]=value;else finite', 'if(apply)v[3]=v[3];else finite'),
    'word-order': ('std::memcpy(&value,&plan.uniforms[row][3-k],4);', 'std::memcpy(&value,&plan.uniforms[row][k],4);'),
    'no-fog': ('!add(1,0xe8,11)', '!add(1,0xe8,1)'),
    'upload-rows': ('return {count,std::max(count,p.paletteLimit()),94u};', 'return {count,std::max(count,p.paletteLimit()),93u};'),
}


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT)
    OUT = parser.parse_args().output.resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    blocks = NATIVE/'runtime/pica_blocks.cpp'
    others = [NATIVE/'runtime'/name for name in ('gx_vertex_decode.cpp', 'gx_rigid_decode.cpp', 'gx_vertex_transform.cpp',
                                                  'gx_pica_math.cpp', 'gx_raster_color.cpp')]
    test = NATIVE/'tests/pica_blocks_test.cpp'
    inputs = [test, blocks, *others, NATIVE/'runtime/pica_blocks.hpp', Path(__file__).resolve()]
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I'+str(ROOT/'Source/extern/aurora/include'),
              '-I'+str(NATIVE/'runtime'), '-Wall', '-Wextra', '-Werror', '-Wno-switch']
    host = ['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    arm = [str(Path(os.environ.get('DEVKITARM', Path.home()/'devkitpro/devkitARM'))/'bin/arm-none-eabi-g++'), *common, '-O2',
           '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mfpu=vfp', '-mtp=soft', '-fno-short-enums', '-D__3DS__',
           '-fstack-usage', '-c', str(blocks), '-o', str(OUT/'pica-blocks-arm.o')]
    env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report = {'status': 'running', 'input_sha256': hashes,
              'origin': 'host Clang ASan/UBSan with the real planners and a model of Citro3D 1.7.1 / libctru 2.7 emission; ARM compile only',
              'limits': ['no PICA, no Citro3D: the emission is a model of C3D_UpdateUniforms and GPUCMD_Add',
                         'host floats; the console compares its own words in verify mode (pica-blocks.txt v)',
                         'the record facts, texture and target checks of gx_command_draw.cpp are not part of this test']}
    path = OUT/'results.json'
    log = OUT/'build.log'
    try:
        with log.open('w') as out:
            subprocess.run([*host, str(test), str(blocks), *map(str, others), '-o', str(OUT/'test')], stdout=out,
                           stderr=subprocess.STDOUT, check=True, timeout=600)
            subprocess.run(arm, stdout=out, stderr=subprocess.STDOUT, check=True, timeout=600)
        with log.open('a') as out:
            subprocess.run(['clang++', *common, '-O2', str(test), str(blocks), *map(str, others), '-o', str(OUT/'bench')],
                           stdout=out, stderr=subprocess.STDOUT, check=True, timeout=600)
        bench = subprocess.run([str(OUT/'bench'), 'bench'], capture_output=True, text=True, timeout=900)
        bench.check_returncode()
        report['benchmark'] = [line for line in (bench.stdout+bench.stderr).splitlines() if line.startswith('BENCH')]
        print('\n'.join(report['benchmark']))
        done = subprocess.run([str(OUT/'test')], capture_output=True, text=True, timeout=900, env=env)
        print(done.stdout+done.stderr, end='')
        done.check_returncode()
        report['test'] = (done.stdout+done.stderr).strip()
        mutants = {}
        source = blocks.read_text()
        for name, (old, new) in MUTATIONS.items():
            assert source.count(old) == 1, f'mutation {name}: the rule is not in pica_blocks.cpp any more'
            mutant = OUT/f'mutant-{name}.cpp'
            mutant.write_text(source.replace(old, new))
            binary = OUT/f'mutant-{name}'
            with log.open('a') as out:
                subprocess.run([*host, '-I'+str(NATIVE/'runtime'), str(test), str(mutant), *map(str, others), '-o', str(binary)],
                               stdout=out, stderr=subprocess.STDOUT, check=True, timeout=600)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=900, env=env)
            text = result.stdout+result.stderr
            caught = result.returncode != 0 and 'Assertion failed' in text and 'AddressSanitizer' not in text and 'runtime error:' not in text
            mutants[name] = 'caught' if caught else 'NOT CAUGHT'
            print(f'mutation {name}: {mutants[name]}')
            assert caught, text[-3000:]
        report['mutations'] = mutants
        report['arm_stack_usage'] = (OUT/'pica-blocks-arm.su').read_text() if (OUT/'pica-blocks-arm.su').exists() else ''
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}, 'inputs changed during the run'
        report.update(status='pass', inputs_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        report.update(status='fail', error=str(error)[-4000:])
        if log.exists():
            print(log.read_text()[-6000:])
    finally:
        path.write_text(json.dumps(report, indent=2)+'\n')
    print('test_pica_blocks:', report['status'])
    return int(report['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
