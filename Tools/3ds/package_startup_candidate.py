#!/usr/bin/env python3
"""Package an immutable local original-startup CXI; no ROMFS or game data."""
import argparse
import os
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import struct

ROOT = Path(__file__).resolve().parents[2]
# The pinned toolchain may live in /opt or in the owner's home directory.
DEVKITPRO = Path(os.environ.get('DEVKITPRO') or
                 next((p for p in (Path.home() / 'devkitpro', Path('/opt/devkitpro')) if p.is_dir()), '/opt/devkitpro'))

def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def load_segments(path):
    """Compare the actual ARM load image after removing only debug sections."""
    data=path.read_bytes()
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected little-endian ELF32')
    phoff=struct.unpack_from('<I',data,28)[0]
    phsize,phnum=struct.unpack_from('<HH',data,42)
    result=[]
    for i in range(phnum):
        kind,offset,vaddr,paddr,filesz,memsz,flags,alignment=struct.unpack_from('<8I',data,phoff+i*phsize)
        if kind==1:
            result.append((vaddr,paddr,filesz,memsz,flags,alignment,hashlib.sha256(data[offset:offset+filesz]).hexdigest()))
    return result

def dsp_mappings(capabilities):
    """Decode the actual NCCH ARM11 range descriptors, not the RSF text."""
    result=[];i=0
    while i<len(capabilities):
        first=capabilities[i]
        if (first>>20)&0xffe==0xff8 and i+1<len(capabilities) and (capabilities[i+1]>>20)&0xffe==0xff8:
            last=capabilities[i+1]
            result.append({'start':(first<<12)&0xffffffff,'end_exclusive':(last<<12)&0xffffffff,
                           'read_only':bool(first&(1<<20))})
            i+=2
        else:i+=1
    for start,end in [(0x1ff50000,0x1ff58000),(0x1ff70000,0x1ff78000)]:
        if not any(m['start']<=start and end<=m['end_exclusive'] and not m['read_only'] for m in result):
            raise RuntimeError('Actual packaged exheader lacks writable NDSP shared RAM bank '+hex(start))
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--build-log', type=Path, required=True)
    parser.add_argument('--compress', action='store_true', help='Compress final package; local iteration defaults to identical uncompressed code')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'Build/new3ds-current')
    parser.add_argument('--elf', type=Path, help='Original debug ELF; defaults to the build directory output')
    parser.add_argument('--banner', type=Path, default=ROOT / 'Build/new3ds-tooling/diagnostic.bnr')
    args = parser.parse_args()
    if not args.name.replace('-', '').isalnum():
        parser.error('Use an alphanumeric/hyphen candidate name')
    build_text=args.build_log.read_text()
    if '[100%] Built target native_3dsx' not in build_text or 'fatal error:' in build_text:
        parser.error('Build log does not confirm a completed native executable and 3DSX')
    output = ROOT / 'Build/original-candidates' / (args.name + '.cxi')
    if output.exists():
        parser.error('Candidate already exists; preserve it and choose a new name')
    output.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.mkdir(parents=True, exist_ok=True)
    elf = (args.elf or args.build_dir / 'dusklight-native.elf').resolve()
    command = [str(DEVKITPRO / 'tools/bin/makerom'), '-f', 'ncch', '-o', str(output), '-rsf', '',
               '-elf', str(elf), '-icon', str(elf.with_suffix('.smdh')), '-banner', str(args.banner.resolve())]
    # Read the current source policy, not a prior build's stale copy.
    rsf=ROOT/'Source/platforms/3ds/dusklight-startup.rsf'
    candidate_rsf=output.with_suffix('.rsf')
    source_rsf=rsf.read_text()
    if not args.compress:
        if 'EnableCompress: true' not in source_rsf:
            raise ValueError('Unexpected compression template')
        source_rsf=source_rsf.replace('EnableCompress: true','EnableCompress: false')
    candidate_rsf.write_text(source_rsf)
    command[command.index('-rsf')+1]=str(candidate_rsf)
    original_elf=Path(command[command.index('-elf')+1])
    packaged_elf=output.with_suffix('.elf')
    subprocess.run([str(DEVKITPRO / 'devkitARM/bin/arm-none-eabi-objcopy'),'--strip-debug',
                    str(original_elf),str(packaged_elf)],check=True)
    if load_segments(original_elf)!=load_segments(packaged_elf):
        raise RuntimeError('Debug stripping changed the ARM load image')
    command[command.index('-elf')+1]=str(packaged_elf)
    inputs = [Path(command[command.index(flag) + 1]) for flag in ['-rsf', '-elf', '-icon', '-banner']]
    receipt = {'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'command': command, 'build_log': str(args.build_log),
               'inputs_sha256': {str(p.relative_to(ROOT)): sha(p) for p in inputs},
               'original_startup': True, 'gameplay_verified': False, 'physical_console_verified': False,
               'game_data_in_package': False, 'code_compression': args.compress}
    receipt['original_debug_elf_sha256']=sha(original_elf)
    receipt['stripped_load_segments_identical']=True
    result = subprocess.run(command, text=True, capture_output=True)
    (args.evidence / (args.name + '-package.log')).write_text(result.stdout + result.stderr)
    result.check_returncode()
    container=output.read_bytes()
    if container[0x100:0x104]!=b'NCCH' or len(container)<0x600:
        raise RuntimeError('Packaged output is not the expected NCCH container')
    capabilities=struct.unpack_from('<28I',container,0x200+0x370)
    kernel_flags=[value for value in capabilities if value&0xff800000==0xff000000]
    if len(kernel_flags)!=1 or not kernel_flags[0]&0x2000:
        raise RuntimeError('Actual packaged exheader lacks New3DS core2 permission')
    receipt['actual_kernel_flags']=hex(kernel_flags[0])
    receipt['actual_core2_permission']=True
    receipt['actual_memory_mappings']=dsp_mappings(capabilities)
    receipt['output'] = str(output.relative_to(ROOT)); receipt['sha256'] = sha(output)
    receipt['size'] = output.stat().st_size
    (args.evidence / (args.name + '-package.json')).write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))

if __name__ == '__main__':
    main()
