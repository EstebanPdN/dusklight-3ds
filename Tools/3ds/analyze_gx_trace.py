#!/usr/bin/env python3
"""Validate private metadata-only GX captures and report workload, never 3DS FPS."""
import argparse
import collections
import json
import math
from pathlib import Path

def percentile(values, p):
    if not values: return None
    values = sorted(values)
    return values[max(0, math.ceil(len(values)*p)-1)]

def timing_summary(rows, field):
    values = [f[field] for f in rows if f[field] is not None]
    return {'samples': len(values), **{name: percentile(values, p)
            for name, p in [('p50', .5), ('p95', .95), ('p99', .99)]}}

def workload_summary(rows):
    totals = {key: sum(f[key] for f in rows) for key in
              ['draws', 'vertices', 'triangles', 'indexed_draws', 'indirect_draws',
               'lit_draws', 'efb_copies', 'target_switches']}
    formats = collections.Counter()
    copies = collections.Counter()
    for f in rows:
        # The same resource can appear in multiple frames. This is deliberately
        # an observation count, not a claim about persistent unique resources.
        formats.update(t['format'] for t in f['textures'])
        copies.update((tuple(c['src']), tuple(c['dst']), c['format']) for c in f['copies'])
    return {**totals, 'max_draws': max(f['draws'] for f in rows),
            'cpu_update_ms': timing_summary(rows, 'cpu_update_ms'),
            'cpu_draw_prepare_ms': timing_summary(rows, 'cpu_draw_prepare_ms'),
            'host_frame_interval_ms': timing_summary(rows, 'frame_interval_ms'),
            'texture_format_frame_observations': dict(sorted(formats.items())),
            'max_frame_local_textures': max(len(f['textures']) for f in rows),
            'max_rgba8_mip0_estimate_bytes': max(f['decoded_rgba8_mip0_estimate_bytes'] for f in rows),
            'copy_shapes': [{'src': src, 'dst': dst, 'format': fmt, 'count': count}
                            for (src, dst, fmt), count in copies.most_common()]}

def analyze(path):
    records = [json.loads(line) for line in path.read_text().splitlines() if line]
    if not records or records[0].get('type') != 'schema' or records[0].get('schema') != 1:
        raise ValueError('Unsupported or missing schema')
    if records[-1].get('type') != 'end' or records[-1].get('write_failed'):
        raise ValueError('Capture did not finalize successfully')
    if any(r.get('type') != 'frame' for r in records[1:-1]):
        raise ValueError('Unexpected record inside capture')
    frames = [r for r in records if r.get('type') == 'frame']
    if not frames: raise ValueError('No frames captured')
    ids = [f['frame'] for f in frames]
    if ids != sorted(set(ids)) or min(ids) < 0 or max(ids) >= records[-1]['frames_seen']:
        raise ValueError('Frame IDs invalid, duplicated or out of order')
    if len(frames)+records[-1]['frames_dropped'] != records[-1]['frames_seen']:
        raise ValueError('Frame accounting mismatch')
    scenes=collections.defaultdict(list);programs={};tev=[0]*17
    overflow=False
    for f in frames:
        if len(f['tev_histogram'])!=17 or sum(f['tev_histogram'])!=f['draws']:
            raise ValueError('Invalid TEV accounting')
        if len(f['texgen_histogram'])!=9 or sum(f['texgen_histogram'])!=f['draws']:
            raise ValueError('Invalid texgen accounting')
        if sum(m['draws'] for m in f['materials'])+f['overflow']['material_draws']!=f['draws']:
            raise ValueError('Invalid material accounting')
        overflow |= any(f['overflow'].values())
        for i,n in enumerate(f['tev_histogram']): tev[i]+=n
        for m in f['materials']:
            blob=m['canonical_program_hex']
            if len(bytes.fromhex(blob)) != records[0]['pipeline_bytes']: raise ValueError('Invalid program encoding')
            key=m['program_key']
            if key in programs and programs[key]['canonical_program_hex']!=blob:
                raise ValueError('Program hash collision; keys must not be merged')
            target=programs.setdefault(key,{**m,'draws':0});target['draws']+=m['draws']
        scenes[(f['scene'],f['room'])].append(f)
    reports=[]
    for (scene,room), rows in scenes.items():
        reports.append({'scene': scene, 'room': room, 'frames': len(rows), **workload_summary(rows)})
    return {'schema':1,'capture':path.name,'origin':'host; never physical-New3DS evidence',
            'renderer_scope_ready':False,'frames':len(frames),'frames_dropped':records[-1]['frames_dropped'],
            'overflow_observed':overflow, **workload_summary(frames), 'tev_histogram':tev,
            'program_count':len(programs),'scenes':reports,
            'limitations':['GPU wait, audio and I/O timing unmeasured','program keys exclude uniform values',
                           'texture identity is frame-local; estimate excludes mipmaps and palette memory',
                           'geometry identity and transition identity not instrumented',
                           'recorded intervals include trace overhead and startup; not acceptance timings',
                           'representative scene matrix incomplete'],
            'programs':sorted(programs.values(),key=lambda x:-x['draws'])}

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();result=analyze(a.capture);a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='programs'},indent=2))
