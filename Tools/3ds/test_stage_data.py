#!/usr/bin/env python3
"""Bounded DZS/DZR test plus optional read-only private metadata inspection."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--private-disc',type=Path);a=p.parse_args()
out=a.output;out.mkdir(parents=True,exist_ok=False);n=ROOT/'Source/platforms/3ds'
sources=[n/x for x in ['runtime/stage_data.cpp','runtime/disc.cpp','runtime/resource_archive.cpp','tests/stage_data_test.cpp']]
headers=[n/x for x in ['runtime/stage_data.hpp','runtime/disc.hpp','runtime/resource_archive.hpp']]
inputs=[Path(__file__).resolve(),*sources,*headers];hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest()for x in inputs}
common=['-std=c++20','-Wall','-Wextra','-Werror']
commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),'-o',str(out/'test')],['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage','-c',str(sources[0]),'-o',str(out/'stage-data-arm.o')]]
r={'status':'running','origin':'Host ASan/UBSan immutable parser + ARM compile; optional private metadata only; no gameplay','commands':commands,'input_sha256':hashes}
report=out/'results.json';report.write_text(json.dumps(r,indent=2)+'\n')
try:
 with (out/'build.log').open('w')as log:
  for c in commands:subprocess.run(c,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=90)
 before=a.private_disc.stat()if a.private_disc else None
 run=subprocess.run([str(out/'test'),*([str(a.private_disc)]if a.private_disc else [])],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
 (out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
 if before:
  after=a.private_disc.stat();assert(before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns)
  r.update(private_source='read-only; size/mtime unchanged; no payload saved',metadata=[json.loads(line)for line in run.stdout.splitlines()if line.startswith('{')])
 assert hashes=={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest()for x in inputs}
 r.update(status='pass',input_unchanged=True)
except (OSError,subprocess.SubprocessError,AssertionError)as e:r.update(status='fail',error=str(e));print((out/'build.log').read_text())
finally:report.write_text(json.dumps(r,indent=2)+'\n')
raise SystemExit(r['status']!='pass')
