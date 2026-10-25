#!/usr/bin/env python3
"""Bounded local PC run: immutable input, confined output, no network access."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',choices=['pc-baseline','pc-trace'],required=True)
p.add_argument('--name',required=True);p.add_argument('--seconds',type=int,default=30)
p.add_argument('--stage',default='')
a=p.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9_-]+',a.name): p.error('Unsafe capture name')
if not 1<=a.seconds<=300: p.error('Duration must be 1..300 seconds')
if a.stage and not re.fullmatch(r'[A-Z0-9_]{1,8}(,-?\d+){0,3}',a.stage): p.error('Invalid stage selector')
exe=ROOT/'Build'/a.build/'Dusklight.app/Contents/MacOS/Dusklight'
if not exe.is_file(): p.error('Build executable is missing')
run=ROOT/'Diagnostics/implementation-01'/a.name
if run.exists(): p.error('Refusing to overwrite a run')
run.mkdir();runtime=ROOT/'Build/pc-runtime'/a.name
runtime.mkdir();(runtime/'home').mkdir();(runtime/'empty-mods').mkdir()
profile=run/'sandbox.sb'
profile.write_text('(version 1)\n(allow default)\n(deny network*)\n(deny file-write*)\n(allow file-write* (subpath '+json.dumps(str(runtime))+') (subpath '+json.dumps(str(run))+') (literal "/dev/null"))\n')
cmd=['/usr/bin/sandbox-exec','-f',str(profile),str(exe),'--dvd',str(ROOT/'twilight.iso'),
     '--user-dir',str(runtime/'user'),'--log-dir',str(runtime/'logs'),'--mods',str(runtime/'empty-mods'),'--backend','metal']
if a.stage: cmd+=['--stage',a.stage]
env=os.environ.copy();env['CFFIXED_USER_HOME']=str(runtime/'home')
if a.build=='pc-trace': env['DUSK_GX_TRACE_PATH']=str(run/'gx.jsonl')
start=time.monotonic();proc=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
forced=False
try: log,_=proc.communicate(timeout=a.seconds)
except subprocess.TimeoutExpired:
 proc.send_signal(signal.SIGTERM)
 try: log,_=proc.communicate(timeout=10)
 except subprocess.TimeoutExpired: forced=True;proc.kill();log,_=proc.communicate()
log=log.replace(str(ROOT/'twilight.iso'),'<private runtime input>')
(run/'runtime.log').write_text(log)
result={'origin':'macOS ARM64 host','build':a.build,'returncode':proc.returncode,'forced_kill':forced,
        'elapsed_seconds':round(time.monotonic()-start,3),'network':'denied','ISO':'read-only by OS policy',
        'writes':'new workspace runtime and diagnostics directories only',
        'source_revision':subprocess.check_output(['git','-C',str(ROOT/'Source'),'rev-parse','HEAD'],text=True).strip(),
        'stage_requested':a.stage or None,'full_gameplay_validated':False}
(run/'run.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
raise SystemExit(0 if proc.returncode==0 and not forced else 1)
