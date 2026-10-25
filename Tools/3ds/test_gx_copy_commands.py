#!/usr/bin/env python3
"""Compare native copy setup to original GX owners; real transfers remain separate."""
import argparse, datetime, hashlib, json, os, re, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[2]
src = root / 'Source'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
def function(path, name):
    text = path.read_text(); match = re.search(r'\b' + re.escape(name) + r'\(', text)
    start = text.rfind('\n', 0, match.start()) + 1
    body = text.index('{', match.start()); end = body + 1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]

frame = src / 'libs/dolphin/src/gx/GXFrameBuf.c'
texture = src / 'libs/dolphin/src/gx/GXTexture.c'
names = ['GXSetDispCopySrc', 'GXSetTexCopySrc', 'GXSetDispCopyDst', 'GXSetTexCopyDst', 'GXSetCopyClamp',
         '__GXGetNumXfbLines', 'GXSetDispCopyYScale', 'GXSetCopyClear', 'GXSetCopyFilter', 'GXSetDispCopyGamma']
body = '\n'.join(function(texture, name) for name in ['__GXGetTexTileShift', '__GetImageTileCount'])
body += '\n' + '\n'.join(function(frame, name) for name in names)
for name in names:
    body = re.sub(r'\b' + re.escape(name) + r'\b', 'Original' + name, body)
reference = out / 'original-copy-reference.cpp'
reference.write_text('''#include <dolphin/gx.h>
#include <stdexcept>
#include <string>
#include <vector>
struct OriginalState {u32 cpDispSrc,cpDispSize,cpTexSrc,cpTexSize,cpDispStride,cpTexStride,cpDisp,cpTex,cpTexZ,bpSentNot;};
static OriginalState state;static OriginalState* __GXData=&state;static std::vector<u32> writes;
void OriginalCopyReset(u32 display,u32 texture){state={};state.cpDisp=display;state.cpTex=texture;writes.clear();}
u32 OriginalCopyField(unsigned index){switch(index){case 0:return state.cpDispSrc;case 1:return state.cpDispSize;case 2:return state.cpTexSrc;case 3:return state.cpTexSize;case 4:return state.cpDispStride;case 5:return state.cpTexStride;case 6:return state.cpDisp;case 7:return state.cpTex;case 8:return state.cpTexZ;default:throw std::runtime_error("reference field");}}
const std::vector<u32>& OriginalCopyWrites(){return writes;}
#define CHECK_GXBEGIN(line,name) do {} while(0)
#define ASSERTMSGLINE(line,condition,message) do {if(!(condition))throw std::runtime_error(message);}while(0)
#define ASSERTMSGLINEV(line,condition,...) do {if(!(condition))throw std::runtime_error("reference parameter at " + std::to_string(line));}while(0)
#define SET_REG_FIELD(line,reg,width,shift,value) reg=(reg&~(((1u<<width)-1)<<shift))|((u32(value)&((1u<<width)-1))<<shift)
#define GET_REG_FIELD(reg,width,shift) ((reg>>shift)&((1u<<width)-1))
#define GX_WRITE_RAS_REG(reg) writes.push_back(reg)
''' + body + '\n')
units = ['platforms/3ds/runtime/gx_copy_commands.cpp', 'platforms/3ds/runtime/gx_commands.cpp', 'platforms/3ds/runtime/gx_stream.cpp',
         'platforms/3ds/gx_command_api.cpp', 'platforms/3ds/runtime/gx_framebuffer_scale.cpp', 'platforms/3ds/runtime/gx_vertex_decode.cpp',
         'platforms/3ds/runtime/resource_archive.cpp', 'extern/aurora/lib/gx/dl.cpp', 'extern/aurora/lib/gx/attr_fmt.cpp',
         'platforms/3ds/tests/gx_copy_commands_test.cpp']
headers = ['platforms/3ds/runtime/gx_copy_setup.hpp', 'platforms/3ds/runtime/gx_commands.hpp', 'platforms/3ds/runtime/gx_stream.hpp',
           'extern/aurora/include/dolphin/gx/GXFrameBuffer.h', 'extern/aurora/include/dolphin/gx/GXEnum.h']
inputs = [Path(__file__).resolve(), frame, texture, *[src / p for p in units + headers]]
hashes = {str(path.relative_to(root)): sha(path) for path in inputs}
flags = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(src / 'extern/aurora/include'),
         '-Wall', '-Wextra', '-Werror', '-Wno-switch', '-Wno-unused-value', '-Wno-unused-parameter']
commands = [['clang++', *flags, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             *[str(src / p) for p in units], str(reference), '-o', str(out / 'test')]]
arm = ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *flags, '-O2', '-g', '-march=armv6k', '-mtune=mpcore',
       '-mfloat-abi=hard', '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage']
for path in units[:3]:
    commands.append([*arm, '-c', str(src / path), '-o', str(out / (Path(path).stem + '-arm.o'))])
rec = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'input_sha256': hashes,
       'reference_sha256': sha(reference), 'commands': commands,
       'origin': 'Host ASan/UBSan actual original-GX comparison and production ARM compilation',
       'limitations': ['Copy settings/packets only; no EFB transfer or framebuffer presentation', 'BP52 copy triggers reject until a real native sink is present', 'No gameplay or emulator acceptance']}
report = out / 'results.json'; report.write_text(json.dumps(rec, indent=2) + '\n')
try:
    for index, command in enumerate(commands):
        with (out / (str(index) + '-build.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    run = subprocess.run([str(out / 'test')], capture_output=True, text=True, timeout=60,
        env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
    (out / 'test.log').write_text(run.stdout + run.stderr); print(run.stdout + run.stderr, end=''); run.check_returncode()
    assert all(sha(root / p) == digest for p, digest in hashes.items())
    rec.update(status='pass', input_unchanged=True, binary_sha256=sha(out / 'test'), arm_stack_usage={p.name: p.read_text() for p in out.glob('*.su')})
except (OSError, subprocess.SubprocessError, AssertionError) as error:
    rec.update(status='fail', error=str(error)); print(str(error), flush=True)
finally:
    rec['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat(); report.write_text(json.dumps(rec, indent=2) + '\n')
raise SystemExit(rec['status'] != 'pass')
