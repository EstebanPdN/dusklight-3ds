#!/usr/bin/env python3
"""Compile every bootstrap TU; retain failures, dependencies and categorized evidence."""
import argparse
import collections
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CATEGORIES = ('endian/resource parsing', 'pointer-size/layout', 'standard-library availability',
              'OS/threading/filesystem', 'PPC assembly/intrinsics', 'graphics', 'audio',
              'file I/O', 'linker/system dependencies', 'unclassified')

def category(message):
    # Diagnostic triage hints only; never asserted as complete root-cause analysis.
    checks = [
        ('endian/resource parsing', r'BSWAP|TARGET_LITTLE_ENDIAN|byte.swapp|endian'),
        ('standard-library availability', r'fmt/|<format>|std::|bits/|type_traits|new exception|exception specifier|declaration of.*operator new'),
        ('graphics', r'aurora/math|webgpu|dawn|GX|J3D|glm/'),
        ('audio', r'JAudio|JAS|JAI|ndsp'),
        ('OS/threading/filesystem', r'SDL|thread|mutex|pthread|condition_variable'),
        ('PPC assembly/intrinsics', r'assembler|opcode|register name|__cntlzw|__rlwimi'),
        ('pointer-size/layout', r'pointer|static assertion|alignment|precision|sizeof'),
        ('file I/O', r'fopen|fseek|DVD|CARD'),
        ('linker/system dependencies', r'undefined reference|cannot find -l|nlohmann|absl/'),
    ]
    for group, pattern in checks:
        if re.search(pattern, message, re.I): return group
    return 'unclassified'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--label', default='current')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.label): parser.error('Unsafe label')
    out = ROOT/'Diagnostics/implementation-01'/('arm32-'+args.label)
    out.mkdir(parents=True, exist_ok=True)
    db = json.loads((ROOT/'Build/arm32-inventory/compile_commands.json').read_text())
    units = []; headers = set(); counts = collections.Counter()
    for i, entry in enumerate(db):
        cmd = entry.get('arguments') or shlex.split(entry['command'])
        obj = Path(entry['directory'])/cmd[cmd.index('-o')+1]
        obj.parent.mkdir(parents=True, exist_ok=True)
        obj.unlink(missing_ok=True)  # A failed rerun must not leave a stale success artifact.
        cmd += ['-fdiagnostics-color=never', '-H']
        run = subprocess.run(cmd, cwd=entry['directory'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = out/(str(i).zfill(2)+'-'+Path(entry['file']).stem+'.log')
        log.write_text(run.stdout)
        errors = []
        for line in run.stdout.splitlines():
            match = re.match(r'^\.+ (/.+)$',line)
            if match:
                p=Path(match[1])
                if p.is_file() and p.is_relative_to(ROOT/'Source'): headers.add(p)
            if 'error:' in line:
                group = category(line); counts[group] += 1
                errors.append({'category':group,'diagnostic':line.replace(str(ROOT)+'/', '')})
        unit = str(Path(entry['file']).relative_to(ROOT/'Source'))
        headers.add(Path(entry['file']))
        units.append({'unit':unit,'returncode':run.returncode,'object':str(obj.relative_to(ROOT)) if run.returncode==0 else None,
                      'command':cmd,'diagnostics':errors,'log':str(log.relative_to(ROOT))})
        print(('PASS' if run.returncode==0 else 'FAIL'),unit,flush=True)
    conditionals=[]
    # Match actual platform/ABI/feature identifiers, not GX include-guard names.
    pattern=re.compile(r'^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b(?:TARGET_PC|DUSK_TARGET_3DS|DUSK_BUILDING_GAME|DUSK_GX_TRACE|DUSK_GFX_DEBUG_GROUPS|__3DS__|TARGET_LITTLE_ENDIAN|BIT_64|__LP64__|_WIN32|_WIN64|_MSC_VER|_MSVC_LANG|__GNUC__|__clang__|__MWERKS__|__arm__|__aarch64__|__ARMEL__|__BYTE_ORDER__|ANDROID|__ANDROID__|__APPLE__|PLATFORM_[A-Z_]+|GEKKO|MTX_USE_PS|MTX_USE_C|WIDESCREEN_SUPPORT|DEBUG|PARTIAL_DEBUG)\b')
    for path in sorted(headers):
        for no,line in enumerate(path.read_text(errors='replace').splitlines(),1):
            if pattern.search(line):
                conditionals.append({'file':str(path.relative_to(ROOT/'Source')),'line':no,'condition':line.strip(),
                                     'semantic_review':'pending','pc_behavior':None,'intended_3ds_behavior':None,'test':None})
    result={'schema':1,'label':args.label,'origin':'host cross-compiler; not execution',
            'source_revision':subprocess.check_output(['git','-C',str(ROOT/'Source'),'rev-parse','HEAD'],text=True).strip(),
            'compiler':subprocess.check_output([db[0].get('arguments',[None])[0] or shlex.split(db[0]['command'])[0],'--version'],text=True).splitlines()[0],
            'units':units,'passed':sum(x['returncode']==0 for x in units),'failed':sum(x['returncode']!=0 for x in units),
            'categories':{k:{'diagnostic_count':counts[k], 'status':'diagnostics observed' if counts[k] else 'not established by this compile subset'} for k in CATEGORIES},
            'g2a_complete':False,'conditional_inventory_scope':'only headers reached before compile failure; not a complete preprocessor branch audit',
            'conditionals':conditionals}
    (out/'inventory.json').write_text(json.dumps(result,indent=2)+'\n')
    (ROOT/'Docs/3ds/arm32_portability_inventory.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Passed',result['passed'],'failed',result['failed'],'conditional candidates',len(conditionals))
    return int(result['failed']>0)
if __name__=='__main__': sys.exit(main())
