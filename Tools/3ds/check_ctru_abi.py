#!/usr/bin/env python3
"""Fail the build when a platform source reads an SDK struct member whose
offset depends on the enum size.

libctru and Citro3D are prebuilt with short enums; this tree is built with
-fno-short-enums (see Source/platforms/3ds/runtime/ctru_abi.hpp). The members
of C3D_FrameBuf, C3D_RenderTarget and ERRF_ExceptionInfo from the first enum
on must be read through the accessors of that header only; DVLE_s through
runtime/ctru_shader_abi.hpp, where a program of a shader binary other than the
first is found with abi::dvle().
"""
import re
import sys
from pathlib import Path

MEMBERS = ('colorFmt', 'depthFmt', 'block32', 'colorMask', 'depthMask', 'ownsColor', 'ownsDepth', 'transferFlags',
           # DVLE_s (every member is at or after its first enum; 'type' is too common a name to list)
           'mergeOutmaps', 'gshMode', 'gshFixedVtxStart', 'gshVariableVtxNum', 'gshFixedVtxNum', 'dvlp', 'mainOffset',
           'endmainOffset', 'constTableSize', 'constTableData', 'outTableSize', 'outTableData', 'uniformTableSize',
           'uniformTableData', 'symbolTableData', 'outmapMask', 'outmapData', 'outmapMode', 'outmapClock')
# DVLB_s::DVLE indexed past the first program steps by this tree's DVLE_s size.
DVLE_INDEX = re.compile(r'\bDVLE\s*\[\s*(?!0\s*\])')
ACCESS = re.compile(r'(?:\.|->)\s*(' + '|'.join(MEMBERS) + r')\b')
# SDK static inlines compiled into this tree that read or write those members
# with this tree's (wrong) offsets: C3D_RenderTargetDetachOutput read
# target->screen/side past the end of the SDK struct (Alpha 2).
INLINES = re.compile(r'\b(C3D_RenderTargetDetachOutput|C3D_FrameBufAttrib|C3D_FrameBufColor|C3D_FrameBufDepth)\s*\(')
# ERRF_ExceptionInfo fields, wherever such a pointer is named.
EXCEPTION = re.compile(r'\bERRF_ExceptionInfo\b')
EXCEPTION_FIELD = re.compile(r'(?:\.|->)\s*(fsr|far|fpexc|fpinst|fpinst2|type)\b')


def scan(root):
    problems = []
    for path in sorted(root.rglob('*')):
        if path.suffix not in ('.cpp', '.hpp', '.c', '.h') or not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative.parts[0] in ('tests', 'build') or path.name in ('ctru_abi.hpp', 'ctru_shader_abi.hpp'):
            continue
        text = path.read_text(errors='replace')
        uses_exception = bool(EXCEPTION.search(text))
        for number, line in enumerate(text.splitlines(), 1):
            code = line.split('//', 1)[0]
            match = ACCESS.search(code)
            if match:
                problems.append(f'{relative}:{number}: direct access to SDK member "{match.group(1)}"')
            if DVLE_INDEX.search(code):
                problems.append(f'{relative}:{number}: DVLE_s array indexed with this tree\'s struct size (use abi::dvle)')
            inline = INLINES.search(code)
            if inline:
                problems.append(f'{relative}:{number}: SDK inline "{inline.group(1)}" uses this tree\'s enum offsets')
            if uses_exception and 'sdkInfo' in code and EXCEPTION_FIELD.search(code):
                problems.append(f'{relative}:{number}: direct access to an ERRF_ExceptionInfo field')
    return problems


def main():
    if len(sys.argv) != 2:
        print('usage: check_ctru_abi.py <Source/platforms/3ds>', file=sys.stderr)
        return 2
    problems = scan(Path(sys.argv[1]))
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print('Read these through dusk::native::abi (runtime/ctru_abi.hpp): the SDK is built with short enums.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
