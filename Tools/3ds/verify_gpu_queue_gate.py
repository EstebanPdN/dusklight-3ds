#!/usr/bin/env python3
"""Verify the actual linked ARM queue/callback gate; never infer interception from flags."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
QUEUE_BODY_SHA256 = '9862fef36f458afefac753a31808f92eac5e59de9562c8145a4a9af0469535d5'


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def verify(elf, link_map, output, sdk):
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    queue = ROOT / 'Source/platforms/3ds/runtime/ctru_gxqueue.c'
    inputs = [elf, link_map, queue, Path(__file__).resolve()]
    receipt = {'status': 'running', 'scope': 'Actual ARM link/call-path proof only; no runtime or hardware acceptance',
               'commands': [], 'input_sha256': {str(p): sha(p) for p in inputs}}
    try:
        with elf.open('rb') as f:
            header = f.read(20)
        if header[:7] != b'\x7fELF\x01\x01\x01' or int.from_bytes(header[18:20], 'little') != 40:
            raise ValueError('Expected little-endian ELF32 ARM')
        body = queue.read_bytes().split(b'*/\n', 1)[1]
        if hashlib.sha256(body).hexdigest() != QUEUE_BODY_SHA256:
            raise ValueError('Pinned queue body changed; review before updating this gate')
        text = link_map.read_text()
        if 'libctru.a(gxqueue.o)' in text:
            raise ValueError('Stock queue object extracted: possible second state machine')
        required = ['gxCmdQueueAdd', 'gxCmdQueueRun', 'gxCmdQueueWait', 'gxCmdQueueStop',
                    'gxCmdQueueClear', 'duskOriginalGxCmdQueueInterrupt']
        for name in required:
            matches = list(re.finditer(r'^ \.text\.' + re.escape(name) + r'\s*\n', text, re.M))
            if not matches or 'runtime/ctru_gxqueue.c.obj' not in text[matches[-1].start():matches[-1].start()+300]:
                raise ValueError('Missing direct queue origin: ' + name)
        dump = sdk / 'devkitARM/bin/arm-none-eabi-objdump'
        disassembly = {}
        for name in ['gspEventThreadMain', 'gxCmdQueueInterrupt', 'GX_ProcessCommandList',
                     'GX_TextureCopy', 'GX_DisplayTransfer', 'GX_MemoryFill']:
            cmd = [str(dump), '-d', '-C', '--disassemble='+name, str(elf)]
            q = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=15)
            receipt['commands'].append(cmd)
            (output/(name+'.s')).write_text(q.stdout)
            disassembly[name] = q.stdout
        if not re.search(r'\bblx?\s+[^\n]*<gxCmdQueueInterrupt>', disassembly['gspEventThreadMain']):
            raise ValueError('Actual GSP call bypasses public gate')
        gate = disassembly['gxCmdQueueInterrupt']
        calls = re.findall(r'\bblx?\s+[^\n]*<([^>]+)>', gate)
        expected = ['LightLock_Lock', 'duskOriginalGxCmdQueueInterrupt', 'LightLock_Unlock']
        # Unlock may be tail-called after restoring the original return address.
        if 'LightLock_Unlock' not in calls and re.search(r'\bb\s+[^\n]*<LightLock_Unlock>', gate):
            calls.append('LightLock_Unlock')
        if any(n not in calls for n in expected) or [calls.index(n) for n in expected] != sorted(calls.index(n) for n in expected):
            raise ValueError('Gate does not enclose original handler/callback in lock/unlock order')
        for name in ['GX_ProcessCommandList', 'GX_TextureCopy', 'GX_DisplayTransfer', 'GX_MemoryFill']:
            if not re.search(r'\bblx?\s+[^\n]*<gxCmdQueueAdd>', disassembly[name]):
                raise ValueError('GX entry does not call direct queue Add: '+name)
        if receipt['input_sha256'] != {str(p): sha(p) for p in inputs}:
            raise ValueError('Input drift during verification')
        receipt.update(status='pass', pinned_body_unchanged=True, single_queue_origin=True,
                       original_callback_forwarding=True, actual_GSP_and_GX_calls_checked=True)
    except (OSError, ValueError, IndexError, subprocess.SubprocessError) as error:
        receipt.update(status='rejected', error=str(error))
    (output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', required=True, type=Path)
    p.add_argument('--map', required=True, type=Path, dest='link_map')
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--devkitpro', type=Path, default=Path(os.environ.get('DEVKITPRO', str(Path.home()/'devkitpro'))))
    a = p.parse_args()
    result = verify(a.elf.resolve(), a.link_map.resolve(), a.output.resolve(), a.devkitpro.resolve())
    print(json.dumps({k: v for k, v in result.items() if k not in ('commands', 'input_sha256')}, indent=2))
    return int(result['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
