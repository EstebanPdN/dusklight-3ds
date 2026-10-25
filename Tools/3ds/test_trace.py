#!/usr/bin/env python3
"""Synthetic collector regression checks; never reads game input."""
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
from analyze_gx_trace import analyze

ROOT=Path(__file__).resolve().parents[2]

def main():
    results=[]
    with tempfile.TemporaryDirectory(prefix='trace-test-',dir=ROOT/'Build') as temp:
        out=Path(temp)
        for i in range(2):
            f=out/f'accounting-{i}.jsonl'
            subprocess.run([str(ROOT/'Build/pc-trace/dusk_gx_trace_test'),str(f)],check=True)
            r=analyze(f)
            assert r['frames']==1 and r['draws']==8 and r['triangles']==12
            assert r['program_count']==2 and r['tev_histogram'][1]==8
            assert r['efb_copies']==1 and r['target_switches']==1
            assert r['cpu_update_ms']=={'samples':1,'p50':1.25,'p95':1.25,'p99':1.25}
            assert r['cpu_draw_prepare_ms']['p99']==2.5
            data=json.loads(f.read_text().splitlines()[1])
            assert data['efb_copies']==1 and data['target_switches']==1
            assert data['cpu_update_ms']==1.25 and data['cpu_draw_prepare_ms']==2.5
            assert data['gpu_wait_ms'] is None and data['audio_ms'] is None
            assert not r['renderer_scope_ready'] and not r['overflow_observed']
            results.append(r)
        assert [x['program_key'] for x in results[0]['programs']]==[x['program_key'] for x in results[1]['programs']]
        original=(out/'accounting-0.jsonl').read_text()
        truncated=out/'incomplete.jsonl';truncated.write_text('\n'.join(original.splitlines()[:-1])+'\n')
        try: analyze(truncated)
        except ValueError: pass
        else: raise AssertionError('Unfinalized capture accepted')
        corrupt=out/'corrupt.jsonl'
        records=[json.loads(line) for line in original.splitlines()]
        records[1]['draws']+=1
        corrupt.write_text('\n'.join(json.dumps(r) for r in records)+'\n')
        try: analyze(corrupt)
        except ValueError: pass
        else: raise AssertionError('Inconsistent draw accounting accepted')
        subprocess.run([str(ROOT/'Build/pc-trace/dusk_gx_trace_test'),str(out/'accounting-0.jsonl')],check=True)
        assert (out/'accounting-0.jsonl').read_text()==original
    db=json.loads((ROOT/'Build/pc-trace/compile_commands.json').read_text())
    disabled=[]
    for suffix in ['lib/gx/fifo.cpp','lib/gx/command_processor.cpp','lib/gx/regs.cpp','src/f_pc/f_pc_manager.cpp','src/m_Do/m_Do_main.cpp']:
        e=next(e for e in db if e['file'].endswith(suffix));cmd=shlex.split(e['command'])
        cmd=[s for s in cmd if s!='-DDUSK_GX_TRACE=1'];oi=cmd.index('-o');del cmd[oi:oi+2];cmd.remove('-c');cmd+=['-E','-P']
        r=subprocess.run(cmd,cwd=e['directory'],capture_output=True,text=True,check=True)
        for token in ['dusk_gx_trace_cpu(', 'dusk_gx_trace_scene(', 'trace::draw(', 'trace::initialize(']:
            assert token not in r.stdout
        disabled.append(suffix)
    receipt={'origin':'synthetic host; not gameplay','accounting':'8 draws, 12 triangles, 2 canonical programs',
             'inactive_stage_normalization':'pass','deterministic_programs':'pass','unknown_timings_null':'pass',
             'unfinalized_capture_rejected':'pass','exclusive_create':'pass','disabled_hooks_absent':disabled}
    receipt['inconsistent_accounting_rejected']='pass'
    receipt['workload_and_scope_percentiles']='pass'
    (ROOT/'Diagnostics/implementation-01/trace-regression-tests.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
if __name__=='__main__': main()
