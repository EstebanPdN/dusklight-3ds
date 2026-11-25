#!/usr/bin/env python3
"""Exercise the original font owner on a host with the Alpha 17 pane budget."""
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
STUB_HEADERS = (
    'types.h', 'd/dolzel.h', 'd/d_meter2_info.h', 'd/d_msg_object.h',
    'f_op/f_op_msg_mng.h', 'JSystem/J2DGraph/J2DTextBox.h',
    'JSystem/JUtility/JUTTexture.h', 'dusk/interp/user_interface.h',
    'dusk/version.hpp',
)


def main():
    with tempfile.TemporaryDirectory(prefix='dusk-font-panes-') as work:
        work = Path(work)
        for name in STUB_HEADERS:
            header = work / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.touch()
        executable = work / 'test'
        subprocess.run([
            'clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wno-multichar',
            '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(work),
            '-I' + str(ROOT / 'Source/include'),
            str(ROOT / 'Source/platforms/3ds/tests/font_pane_lifecycle_test.cpp'),
            '-o', str(executable),
        ], check=True, timeout=60)
        subprocess.run([executable], check=True, timeout=30, env={
            **os.environ, 'ASAN_OPTIONS': 'halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1',
        })


if __name__ == '__main__':
    main()
