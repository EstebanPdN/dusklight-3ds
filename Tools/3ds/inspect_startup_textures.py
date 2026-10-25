#!/usr/bin/env python3
"""Bounded private startup texture metadata inventory; no asset payload output."""
import argparse,datetime,hashlib,json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--private-disc',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
paths=[root/'Source/platforms/3ds'/x for x in ['tests/startup_texture_inventory.cpp','runtime/disc.cpp','runtime/resource_archive.cpp','runtime/texture_gx.cpp']]
inputs=paths+[root/'Source/platforms/3ds/runtime'/x for x in ['disc.hpp','resource_archive.hpp','texture_gx.hpp']]+[Path(__file__).resolve()]
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
hashes={str(x.relative_to(root)):sha(x)for x in inputs}
cmd=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,paths),'-o',str(out/'inspect')]
receipt={'status':'running','origin':'Private read-only actual Disc/RARC/BTI parser metadata under host ASan/UBSan; maximum complete archive potential, not measured simultaneous runtime/cache or gameplay','command':cmd,'input_sha256':hashes,'created':datetime.datetime.now(datetime.timezone.utc).isoformat()}
try:
 with(out/'build.log').open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
 before=a.private_disc.stat()
 run=subprocess.run([str(out/'inspect'),str(a.private_disc.resolve())],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
 (out/'inspect.log').write_text(run.stderr);run.check_returncode();records=[json.loads(x)for x in run.stdout.splitlines()]
 (out/'metadata.json').write_text(json.dumps(records,indent=2)+'\n');print(run.stdout,end='')
 after=a.private_disc.stat();assert(before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns)
 assert hashes=={str(x.relative_to(root)):sha(x)for x in inputs}
 receipt.update(status='pass',input_unchanged=True,private_disc_size_mtime_unchanged=True,payload_saved=False)
except(OSError,subprocess.SubprocessError,AssertionError)as error:
 receipt.update(status='fail',error=str(error));print((out/'build.log').read_text());print((out/'inspect.log').read_text()if(out/'inspect.log').exists()else'')
(out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n');raise SystemExit(receipt['status']!='pass')
