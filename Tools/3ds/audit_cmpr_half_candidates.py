#!/usr/bin/env python3
"""Bounded private GX14 census; metadata only, never approves startup sources.

Adapts the existing read-only archive/MAT3 walker in memory without editing it.
12 stage/room pairs, authored banks, common objects, Layout and Particle scope.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
import sys
import audit_texture_half_candidates as base

ROOT = base.ROOT
OUT_ROOT = ROOT / 'Diagnostics/implementation-44/texture-half-cmpr'
original_record = base.texture_record


def cmpr_levels(payload, width, height, mips):
    """Independent logical alpha oracle; never allocates a decoded full image."""
    levels, cursor = [], 0
    for k in range(mips):
        w, h = max(1, width >> k), max(1, height >> k)
        bw, bh = (w+7)//8, (h+7)//8
        count, three = 0, 0
        for ty in range(bh):
            for tx in range(bw):
                for sub in range(4):
                    at = cursor + (ty*bw+tx)*32+sub*8
                    a, b = struct.unpack_from('>HH', payload, at)
                    if a > b:
                        continue
                    three += 1
                    for y in range(4):
                        for x in range(4):
                            if tx*8+(sub % 2)*4+x < w and ty*8+(sub//2)*4+y < h:
                                count += ((payload[at+4+y] >> (6-2*x)) & 3) == 3
        size = bw*bh*32
        levels.append({'level': k, 'width': w, 'height': h, 'gx_bytes': size,
                       'logical_texels': w*h, 'transparent_logical_texels': count,
                       'three_color_blocks': three})
        cursor += size
    assert cursor == len(payload)
    return levels


def texture_record(data, header, end, origin):
    b = base.resource.span(data, header, 32)
    if b[0] != 14:
        return original_record(data, header, end, origin)
    fmt, alpha, mips = b[0], b[1], b[24]
    w, h = struct.unpack_from('>HH', b, 2)
    row = {**origin, 'gx_format': fmt, 'bti_alpha_enabled': alpha,
           'original_width': w, 'original_height': h, 'original_mips': mips,
           'header_offset': header, 'original_header_hex': b.hex()}
    offset = base.resource.u32(b, 28)
    if not offset:
        return {**row, 'external_named_reference': True}
    if not (1 <= w <= 1024 and 1 <= h <= 1024 and 1 <= mips <= 11):
        raise ValueError('CMPR extent/mip bounds')
    if offset < 32 or b[8] or base.u16(b, 10):
        raise ValueError('CMPR direct header/palette bounds')
    count = sum(((max(1,w >> k)+7)//8)*((max(1,h >> k)+7)//8)*32 for k in range(mips))
    start = header+offset
    if start+count > end:
        raise ValueError('CMPR payload crosses owning TEX1/member')
    payload = base.resource.span(data, start, count)
    levels = cmpr_levels(payload, w, h, mips)
    reasons = []
    if min(w,h) < 16: reasons.append('original_dimension_below_16')
    if mips < 2: reasons.append('no_authored_mip_1')
    if mips > 8: reasons.append('native_mip_limit')
    if w & (w-1) or h & (h-1): reasons.append('non_power_of_two_extent')
    if min(w >> (mips-1),h >> (mips-1)) < 8: reasons.append('provided_chain_tail_below_8')
    if levels[0]['transparent_logical_texels']: reasons.append('original_base_has_alpha')
    if any(l['transparent_logical_texels'] for l in levels[1:]): reasons.append('retained_mip_has_alpha')
    full = sum(max(8,1 << (l['width']-1).bit_length())*max(8,1 << (l['height']-1).bit_length())*4 for l in levels)
    base_gpu = max(8,1 << (w-1).bit_length())*max(8,1 << (h-1).bit_length())*4
    return {**row, 'canonical_sha256': base.sha(struct.pack('>BHHB',fmt,w,h,mips)+payload),
            'image_sha256': base.sha(payload), 'pixel_offset': start, 'source_payload_bytes': count,
            'levels': levels, 'mechanical_exclusions': reasons, 'mechanically_eligible': not reasons,
            'pica_full_rgba8_bytes': full, 'pica_authored_tail_rgba8_bytes': full-base_gpu if mips > 1 else None,
            'admission_status': 'not_approved'}


class Audit(base.Audit):
    def add(self, row, item):
        self.headers += 1
        if self.headers > base.MAX_HEADERS:
            raise ValueError('texture header budget')
        item['texture_headers'] += 1
        item['formats'][row['gx_format']] += 1
        if row['gx_format'] == 14:
            self.rows.append(row)


def run(args):
    out = args.output.resolve()
    if not out.is_relative_to(OUT_ROOT) or out == OUT_ROOT:
        raise ValueError('output must be a new texture-half-cmpr subdirectory')
    out.mkdir(parents=True, exist_ok=False)
    # The inherited parser resolves its module's function at call time.
    base.texture_record = texture_record
    audit = Audit()
    disc = args.private_disc.resolve(); before = disc.stat()
    with disc.open('rb') as stream:
        files, identity = base.resource.read_fst(stream, before.st_size)
        requested = {f'/res/Stage/{s}/{a}.arc': 'stage_room_static_reference_scope'
                     for s,r in base.SITES for a in ('STG_00', f'R{r:02d}_00')}
        requested.update({f'/res/Object/{n}.arc': 'common_or_selected_ordon_object'
                          for n in base.OBJECTS if f'/res/Object/{n}.arc' in files})
        requested.update({p: 'protected_layout' for p in files if p.startswith('/res/Layout/')})
        requested.update({p: 'particle_role_unresolved' for p in files if p.startswith('/res/Particle/')})
        done = set()
        while requested.keys()-done:
            path = sorted(requested.keys()-done)[0];done.add(path)
            if len(done) > base.MAX_FILES: raise ValueError('top-level count bound')
            try:
                at,size = files[path];audit.stored += size
                if size > base.resource.MAX_ARCHIVE or audit.stored > base.MAX_STORED:
                    raise ValueError('stored byte bound')
                audit.inspect(base.resource.read_exact(stream,at,size),path,requested[path])
                for meta in audit.metadata:
                    for name in meta.get('bank_names',[]):
                        p = f'/res/Object/{name}.arc'
                        if p in files: requested.setdefault(p,'authored_stage_bank_possible_story_layers')
            except (ValueError,KeyError,IndexError,struct.error) as e:
                audit.errors.append({'path':path,'error':str(e)})
    after = disc.stat()
    assert (before.st_size,before.st_mtime_ns) == (after.st_size,after.st_mtime_ns)
    groups = defaultdict(list)
    for row in audit.rows:
        if row.get('canonical_sha256'): groups[row['canonical_sha256']].append(row)
    aliases = {key:{'aliases':rows, 'protected_layout_aliases':sum(r['scope_purpose']=='protected_layout' for r in rows),
                       'nonzero_alpha_aliases':sum(bool(r['bti_alpha_enabled']) for r in rows),
                       'all_game_uses_reviewed':False,'approved':False} for key,rows in groups.items()}
    sites=[]
    for stage,room in base.SITES:
        rows=[r for r in audit.rows if r['original_path'].startswith(f'/res/Stage/{stage}/')]
        eligible=[r for r in rows if r.get('mechanically_eligible')]
        sites.append({'stage':stage,'room':room,'cmpr_headers':len(rows),'mip_histogram':dict(Counter(r['original_mips'] for r in rows)),
                      'mechanical_headers':len(eligible),'mechanical_fingerprints':len({r['canonical_sha256'] for r in eligible}),
                      'runtime_story_pass':False})
    summary={'sites':sites,'disc':{**identity,'path':str(disc),'bytes':before.st_size,'size_mtime_unchanged':True,'whole_disc_sha256':None},
             'actual':{'top_level_files':len(done),'containers':len(audit.archives),'stored_bytes_read':audit.stored,
                       'expanded_bytes':audit.expanded,'models':sum(a['models'] for a in audit.archives),
                       'all_format_headers':audit.headers,'cmpr_headers':len(audit.rows),'cmpr_fingerprints':len(groups),
                       'mechanical_headers':sum(r.get('mechanically_eligible',False) for r in audit.rows)},
             'limits':{'files_including_nested':base.MAX_FILES,'stored_bytes':base.MAX_STORED,'expanded_bytes':base.MAX_EXPANDED,
                       'per_input_expanded_bytes':base.resource.MAX_ARCHIVE,'headers':base.MAX_HEADERS,'nested_archive_depth':3},
             'errors':audit.errors,'enabled':False,'approved_sources':[],'payload_saved':False,
             'limitations':['Static paths/MAT3 initial references are not runtime/full-game proof.',
                            'Unexamined archives/raw font/data and dynamic BTP/BTK/substitution consumers remain unknown.',
                            'Mechanical opacity/dimensions are not audited all-alias source eligibility.']}
    for name,obj in [('summary.json',summary),('resources.json',audit.rows),('aliases.json',aliases),('archives.json',audit.archives),('stage-metadata.json',audit.metadata)]:
        (out/name).write_text(json.dumps(obj,indent=2,sort_keys=True)+'\n')
    inputs=[Path(__file__),Path(base.__file__),Path(base.resource.__file__)]
    (out/'reproduction.json').write_text(json.dumps({'argv':sys.argv,'helper_sha256':{str(p.relative_to(ROOT)):base.sha(p.read_bytes()) for p in inputs},
                                                   'output_sha256':{p.name:base.sha(p.read_bytes()) for p in out.glob('*.json')}},indent=2)+'\n')
    print(json.dumps({'actual':summary['actual'],'sites':sites,'errors':audit.errors},indent=2))
    return bool(audit.errors)

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--private-disc',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    raise SystemExit(run(p.parse_args()))
