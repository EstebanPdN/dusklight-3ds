#!/usr/bin/env python3
"""Bounded PRIVATE host texture lab. No game assets, source or installed tools modified.

Requires only Python stdlib and clang++; builds a host GX diagnostic helper and
a host-only rg_etc1 harness. Refuses existing output directories. See --help.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import time

import analyze_texture_dump as analyzer
from capture_to_png import write_png
import texture_lab_formats as f

ROOT=Path(__file__).resolve().parents[2]
DEFAULT_DUMPS=[ROOT/'Build/azahar-runs'/run/'xdg-data/azahar-emu/sdmc/3ds/dusklight/dumps'/name
               for run,name in [('s44-textures2-house','frame-000400-textures.dstx'),
                                ('s44-shadow-outside-motion','frame-000750-textures.dstx')]]


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path,data): path.write_text(json.dumps(data,indent=2,allow_nan=False)+'\n')


def build_tools(directory):
    directory.mkdir(parents=True,exist_ok=True)
    decoder=directory/'gx-decoder'; encoder=directory/'etc1-host'
    upstream=ROOT/'Upstream/tex3ds-host-lab'
    commands=[['clang++','-std=c++20','-O2','-Wall','-Wextra','-Werror',
               str(ROOT/'Tools/3ds/decode_snapshot_texture.cpp'),
               str(ROOT/'Source/platforms/3ds/runtime/texture_gx.cpp'),'-o',str(decoder)],
              ['clang++','-std=c++11','-O2','-DNDEBUG','-I'+str(upstream/'include'),
               str(ROOT/'Tools/3ds/texture_lab_etc_host.cpp'),str(upstream/'source/rg_etc1.cpp'),'-o',str(encoder)]]
    for command,target in zip(commands,(decoder,encoder)):
        # Reuse only a binary with a matching receipt and dependency hashes.
        deps=[Path(x) for x in command if x.endswith(('.cpp','.h'))]
        if target==encoder: deps.append(upstream/'include/rg_etc1.h')
        else: deps.append(ROOT/'Source/platforms/3ds/runtime/texture_gx.hpp')
        key={'command':command,'inputs':{str(p):digest(p) for p in deps}}
        receipt=target.with_suffix('.build.json')
        prior=json.loads(receipt.read_text()) if receipt.exists() else {}
        if not (target.exists() and prior.get('key')==key and prior.get('sha256')==digest(target)):
            start=time.perf_counter()
            run=subprocess.run(command,capture_output=True,text=True,timeout=90)
            target.with_suffix('.build.log').write_text(run.stdout+run.stderr)
            run.check_returncode()
            save_json(receipt,{'key':key,'sha256':digest(target),'build_seconds':time.perf_counter()-start})
    return decoder,encoder


def decode_mips(record,bti,decoder,scratch):
    header=bytearray(bti[:32]); cursor=32
    fmt=header[0]; bw,bh,block=analyzer.BLOCKS[fmt]
    palette=bti[32+record['source_image_bytes']:]
    levels=[]
    for level in range(record['mips']):
        w,h=max(1,record['width']>>level),max(1,record['height']>>level)
        size=((w+bw-1)//bw)*((h+bh-1)//bh)*block
        single=bytearray(header); struct.pack_into('>HH',single,2,w,h); single[24]=1
        struct.pack_into('>I',single,28,32)
        struct.pack_into('>I',single,12,32+size if palette else 0)
        inp,out=scratch/'image.bti',scratch/'image.rgba'
        inp.write_bytes(single+bti[cursor:cursor+size]+palette)
        subprocess.run([str(decoder),str(inp),str(out)],check=True,capture_output=True,timeout=15)
        raw=out.read_bytes()
        if struct.unpack_from('<II',raw)!=(w,h) or len(raw)!=8+w*h*4: raise ValueError('GX decoder extent')
        levels.append((raw[8:],w,h)); cursor+=size
    if cursor!=32+record['source_image_bytes']: raise ValueError('GX mip spans')
    return levels


def gpu_bpp(record):
    return 1 if record['gx_format'] in ('I4','IA4') else 2 if record['gx_format'] in ('I8','IA8') else 4


def policy(record):
    # No name/caller/lifetime binding exists in DSKTEX01. Never infer roles or
    # safe asset conversion from format, dimensions or two unchanged samples.
    reasons=['unproven asset role and lifetime; no runtime allowlist']
    if record['gx_format'] in ('I4','I8','IA4','IA8','CI4','CI8','CI14'):
        reasons.append('possible UI/font/intensity/palette/data resource')
    if record['handle'] in (1,2,3,4,139,248) and record['scene']=='house':
        reasons.append('prior house analysis identifies capture/map/shadow or zero-data resource')
    return {'runtime_candidate':False,'reasons':reasons}


def estimate(record,levels):
    w,h=record['width'],record['height']; mips=record['mips']; base=f.allocation(w,h,mips,gpu_bpp(record))
    classes=[f.alpha_class(rgba) for rgba,_,_ in levels]
    cls='opaque' if set(classes)=={'opaque'} else 'binary' if set(classes)<= {'opaque','binary'} else 'smooth'
    formats=['RGBA4','ETC1A4']
    if cls=='opaque': formats+=['RGB565','ETC1']
    if cls in ('opaque','binary'): formats+=['RGB5A1']
    alternatives={name:{'bytes':f.allocation(w,h,mips,f.FORMATS[name]),
                         'baseline_over_proposed':base/f.allocation(w,h,mips,f.FORMATS[name]),
                         'saved_bytes':base-f.allocation(w,h,mips,f.FORMATS[name])} for name in formats}
    half=None
    if w>=16 and h>=16 and w%2==0 and h%2==0:
        nw,nh=w//2,h//2
        nm=max(1,min(mips,min(nw,nh).bit_length()-3))
        half={'width':nw,'height':nh,'mips':nm,'current_format_bytes':f.allocation(nw,nh,nm,gpu_bpp(record)),
              'formats':{name:f.allocation(nw,nh,nm,f.FORMATS[name]) for name in formats}}
    return {'baseline_gpu_format':('LA4' if gpu_bpp(record)==1 else 'LA8' if gpu_bpp(record)==2 else 'RGBA8'),
            'baseline_gpu_bytes':base,'alpha_class_all_source_mips':cls,'mip_alpha_classes':classes,
            'same_dimensions':alternatives,'half_dimensions':half,
            'interpretation':'hypothetical allocation arithmetic; role policy excludes all unproven resources'}


def preview(path,rgba,w,h):
    write_png(path,w,h,analyzer.composite(rgba,w,h))


def experiment(record,levels,encoder,scratch,output):
    label=f"{record['scene']}-{record['index']:04d}-h{record['handle']}"
    folder=output/label; folder.mkdir()
    original,w,h=levels[0]
    preview(folder/'original.png',original,w,h)
    for c,name in enumerate('rgba'):
        write_png(folder/f'original-{name}.png',w,h,analyzer.channel_rgb(original,c))
    # Save true decoded RGBA, never feed checkerboard-composited previews to a codec.
    (folder/'original.rgba').write_bytes(struct.pack('<II',w,h)+original)
    cls=record['estimate']['alpha_class_all_source_mips']
    variants=['RGBA4','ETC1A4']
    if cls=='opaque': variants+=['RGB565','ETC1']
    if cls in ('opaque','binary'): variants+=['RGB5A1']
    results=[]
    for scale in (1,2):
        if scale==2 and record['estimate']['half_dimensions'] is None: continue
        if scale==1: source_levels=levels
        else:
            # Keep the source mip chain rather than inventing extra levels;
            # downsample each matching source mip, dropping unsupported <8 tails.
            count=record['estimate']['half_dimensions']['mips']
            source_levels=[f.half_box(rgba,lw,lh) for rgba,lw,lh in levels[:count]]
        # Alpha-aware half filtering turns binary cutouts into smooth alpha.
        scale_classes=[f.alpha_class(p) for p,_,_ in source_levels]
        for fmt in variants:
            if fmt=='RGB5A1' and 'smooth' in scale_classes: continue
            start=time.perf_counter(); encoded_levels=[]; timings=[]; decoded_levels=[]; mip_metrics=[]
            for rgba,lw,lh in source_levels:
                if fmt.startswith('ETC'):
                    encoded,timing=f.encode_etc(rgba,lw,lh,fmt,encoder,scratch)
                    decoded=f.decode_etc(encoded,lw,lh,fmt)
                else:
                    t=time.perf_counter(); encoded=f.encode16(rgba,lw,lh,fmt)
                    timing={'python_encode_ms':(time.perf_counter()-t)*1000}
                    decoded=f.decode16(encoded,lw,lh,fmt)
                encoded_levels.append(encoded); timings.append(timing); decoded_levels.append(decoded)
                mip_metrics.append(f.metrics(rgba,decoded))
            complete_ms=(time.perf_counter()-start)*1000
            data=b''.join(encoded_levels); nw,nh=source_levels[0][1:]
            if len(data)!=f.allocation(nw,nh,len(source_levels),f.FORMATS[fmt]): raise ValueError('PICA mip allocation')
            proposed=decoded_levels[0]
            restored=f.expand_nearest(proposed,nw,nh,w,h) if scale==2 else proposed
            name=f'{fmt}-'+('half' if scale==2 else 'full')
            filename=name+'.pica'; (folder/filename).write_bytes(data)
            preview(folder/(name+'.png'),restored,w,h)
            write_png(folder/(name+'-alpha.png'),w,h,analyzer.channel_rgb(restored,3))
            current=record['estimate']['baseline_gpu_bytes']
            result={'name':name,'format':fmt,'scale':scale,'width':nw,'height':nh,'mips':len(source_levels),
                    'encoded_bytes':len(data),'sha256':digest(folder/filename),'pica_data':filename,
                    'original_gpu_bytes':current,'baseline_over_proposed':current/len(data),
                    'saved_bytes':current-len(data),'timings_per_mip':timings,
                    'encode_pack_decode_metrics_wall_ms':complete_ms,
                    'base_vs_original':f.metrics(original,restored),'per_mip_codec_only':mip_metrics,
                    'preview':name+'.png','alpha_preview':name+'-alpha.png',
                    'coordinate_comparison':'same size; nearest expansion of half result, not rendered screen quality',
                    'runtime_candidate':False}
            if scale==1 and fmt=='RGB565' and record['gx_format']=='RGB565':
                if not all(all(c['lossless'] for c in met['channels'].values()) for met in mip_metrics):
                    raise ValueError('Exact GX565 conversion failed')
                result['exact_gx565_all_mips']=True
            results.append(result)
    save_json(folder/'results.json',{'record':record,'variants':results})
    return {'record':record,'directory':label,'variants':results}


def render_gallery(output,actual):
    rows=[]
    for sample in actual:
        r=sample['record']; directory=sample['directory']
        cards=[f'<figure><img src="{directory}/original.png"><figcaption>Original decoded RGBA</figcaption></figure>']
        for variant in sample['variants']:
            cards.append(f'<figure><a href="{directory}/{variant["preview"]}"><img src="{directory}/{variant["preview"]}"></a>'
                         f'<figcaption>{variant["name"]}<br>{variant["encoded_bytes"]:,} B · baseline/proposed {variant["baseline_over_proposed"]:.2f}×'
                         f'<br>alpha max error {variant["base_vs_original"]["channels"]["a"]["max_abs"]}'
                         f'<br><a href="{directory}/{variant["alpha_preview"]}">Alpha channel</a></figcaption></figure>')
        rows.append(f'<section><h2>{html.escape(directory)} · {r["gx_format"]} {r["width"]}×{r["height"]}'
                    f' · {r["estimate"]["alpha_class_all_source_mips"]}</h2><p>Experimental only; role unproven. '
                    f'<a href="{directory}/results.json">All channel errors/timings</a> · '
                    f'<a href="{directory}/original-r.png">Stored R (ignore alpha)</a></p><div>'+''.join(cards)+'</div></section>')
    (output/'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><title>Private texture compression lab</title>'
        '<style>body{background:#17191e;color:#eee;font:15px system-ui;margin:24px}a{color:#aaaaff}'
        'section{border-top:1px solid #666;padding:12px 0}section>div{display:flex;flex-wrap:wrap}figure{margin:6px;width:160px}'
        'img{width:152px;height:152px;object-fit:contain;image-rendering:pixelated}figcaption{font-size:13px}</style>'
        '<h1>Private host texture compression lab</h1><p>Real decoded GX images; checkerboard is display only. '
        'All original stored channels contribute to errors. Half previews use nearest reconstruction for comparison. '
        'No native integration, hardware FPS, visual acceptance, or production allowlist is claimed.</p>'
        '<p><a href="REPORT.md">Report</a> · <a href="census.json">Every registered record and estimated budgets</a> · '
        '<a href="actual-results.json">Actual measurements</a> · <a href="receipts.json">Hashes and commands</a></p>'+''.join(rows)+'</html>')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--tools',type=Path,default=ROOT/'Build/texture-compression-tools')
    parser.add_argument('--snapshot',type=Path,action='append')
    parser.add_argument('--sample-limit',type=int,default=18)
    parser.add_argument('--max-sample-pixels',type=int,default=65536)
    args=parser.parse_args()
    if not 2<=args.sample_limit<=24 or not 64<=args.max_sample_pixels<=65536: parser.error('Bounded limits: samples 2..24, pixels 64..65536')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    snapshots=args.snapshot or DEFAULT_DUMPS
    protected=[ROOT/'Tools/3ds/analyze_texture_dump.py',ROOT/'Tools/3ds/decode_snapshot_texture.cpp',
               ROOT/'Source/platforms/3ds/runtime/texture_gx.cpp',
               Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))/'tools/bin/tex3ds']+snapshots
    before={str(p):digest(p) for p in protected}
    save_json(output/'protected-before.json',before)
    decoder,encoder=build_tools(args.tools.resolve())
    census=[]; cached={}; inputs=[]
    start=time.perf_counter()
    with tempfile.TemporaryDirectory(prefix='texture-lab-',dir=args.tools.resolve()) as temporary:
        scratch=Path(temporary)
        for scene_index,path in enumerate(snapshots):
            data,frame,records=analyzer.read_snapshot(path)
            scene=('house','outside')[scene_index] if snapshots==DEFAULT_DUMPS else f'scene{scene_index}'
            inputs.append({'scene':scene,'path':str(path),'frame':frame,'sha256':hashlib.sha256(data).hexdigest(),'records':len(records)})
            for record,bti in records:
                record=dict(record,scene=scene)
                levels=decode_mips(record,bti,decoder,scratch)
                record['decoded_rgba_sha256']=hashlib.sha256(levels[0][0]).hexdigest()
                record['decoded_mip_sha256']=[hashlib.sha256(p).hexdigest() for p,_,_ in levels]
                record['estimate']=estimate(record,levels)
                record['policy']=policy(record)
                record['base_channel_range']={name:[min(levels[0][0][c::4]),max(levels[0][0][c::4])] for c,name in enumerate('rgba')}
                census.append(record); cached[(scene,record['index'])]=levels
            print(f'{scene}: decoded {len(records)} records, including all exported mips',flush=True)
        # One largest representative per scene / GX encoding / alpha stratum,
        # then prioritize color formats for actual bounded compression work.
        strata={}
        for r in census:
            if r['width']*r['height']>args.max_sample_pixels or r['width']<8 or r['height']<8: continue
            key=(r['scene'],r['gx_format'],r['estimate']['alpha_class_all_source_mips'])
            if key not in strata or strata[key]['estimate']['baseline_gpu_bytes']<r['estimate']['baseline_gpu_bytes']: strata[key]=r
        priority={'RGB565':0,'CMPR':1,'RGB5A3':2,'CI8':3,'IA8':4,'IA4':5,'I8':6,'I4':7,'RGBA8':8}
        selected=sorted(strata.values(),key=lambda r:(priority.get(r['gx_format'],9),r['scene'],r['estimate']['alpha_class_all_source_mips']))[:args.sample_limit]
        actual=[]
        for r in selected:
            actual.append(experiment(r,cached[(r['scene'],r['index'])],encoder,scratch,output))
            print(f"sample {len(actual)}/{len(selected)}: {r['scene']} h{r['handle']} {r['gx_format']}",flush=True)
    # Verify base RGBA hashes against the latest all-channel house inventory.
    prior=ROOT/'Diagnostics/implementation-44/texture-analysis/house-400/inventory.json'
    prior_check={'path':str(prior),'checked_records':0}
    if prior.exists():
        previous=json.loads(prior.read_text()); known={r['handle']:r for r in previous['images']}
        for r in census:
            if r['scene']=='house':
                if r['decoded_rgba_sha256']!=known[r['handle']]['decoded_rgba_sha256']: raise ValueError('Existing analyzer RGBA mismatch')
                prior_check['checked_records']+=1
        prior_check['sha256']=digest(prior)
    save_json(output/'census.json',{'inputs':inputs,'records':census,'existing_analyzer_comparison':prior_check})
    save_json(output/'actual-results.json',actual)
    after={str(p):digest(p) for p in protected}
    if before!=after: raise ValueError('Protected input changed during experiment; inspect concurrent parent work')
    owned=[Path(__file__),ROOT/'Tools/3ds/texture_lab_formats.py',ROOT/'Tools/3ds/texture_lab_etc_host.cpp',
           ROOT/'Tools/3ds/test_texture_compression_lab.py',ROOT/'Tools/3ds/validate_texture_compression_lab.py']
    upstream=ROOT/'Upstream/tex3ds-host-lab'
    receipts={'protected_before':before,'protected_after':after,'unchanged':before==after,
              'lab_sha256':{str(p):digest(p) for p in owned},'host':platform.platform(),
              'python':platform.python_version(),'decoder':str(decoder),'encoder':str(encoder),
              'tool_sha256':{str(p):digest(p) for p in (decoder,encoder)},
              'encoder_repository':'https://github.com/devkitPro/tex3ds.git',
              'encoder_revision':subprocess.check_output(['git','-C',str(upstream),'rev-parse','HEAD'],text=True).strip(),
              'encoder_source_hashes':{str(upstream/p):digest(upstream/p) for p in ('source/rg_etc1.cpp','include/rg_etc1.h','source/encode.cpp','COPYING','configure.ac')},
              'commands':[json.loads(p.with_suffix('.build.json').read_text())['key']['command'] for p in (decoder,encoder)],
              'wall_seconds':time.perf_counter()-start,'sample_limit':args.sample_limit,
              'max_sample_pixels':args.max_sample_pixels,'sampling':'largest per scene/GX format/all-mip alpha class; format priority; no role inference'}
    save_json(output/'receipts.json',receipts)
    summarize(output,census,actual,inputs,prior_check,receipts)
    render_gallery(output,actual)
    print(json.dumps({'output':str(output),'records':len(census),'samples':len(actual),
                      'variants':sum(len(s['variants']) for s in actual),'wall_seconds':receipts['wall_seconds']},indent=2))


def summarize(output,census,actual,inputs,prior,receipts):
    lines=['# Private host texture compression experiment','',
           'Actual offline encodes and independent decoding completed. No native renderer change or hardware FPS measurement.', '',
           '## Allocation census (hypothetical ceilings, not approved savings)','',
           'Every live registration is counted separately. Do not add scenes together as simultaneous memory. '
           'Totals model current RGBA8/LA8/LA4 with per-mip power-of-two dimensions, minimum 8×8. '
           'Registered CPU images are not evidence of concurrent GPU residency. '
           'Allocator alignment/metadata, original GX/archive bytes, staging, sampler and cache residency are excluded. '
           'None of these registered resources has an approved semantic/lifetime allowlist; operational policy admits zero.', '',
           '| Scene | Records | Source + palette B | Modeled GPU B | 16-bit color ceiling saved B | ETC color ceiling saved B | Half current-format ceiling saved B |',
           '|---|---:|---:|---:|---:|---:|---:|']
    aggregate={}
    for scene in [i['scene'] for i in inputs]:
        rows=[r for r in census if r['scene']==scene]; base=sum(r['estimate']['baseline_gpu_bytes'] for r in rows)
        color=[r for r in rows if gpu_bpp(r)==4]
        sixteen=sum(r['estimate']['baseline_gpu_bytes']-r['estimate']['same_dimensions']['RGBA4']['bytes'] for r in color)
        etc=sum(r['estimate']['baseline_gpu_bytes']-r['estimate']['same_dimensions']['ETC1' if r['estimate']['alpha_class_all_source_mips']=='opaque' else 'ETC1A4']['bytes'] for r in color)
        half=sum(r['estimate']['baseline_gpu_bytes']-r['estimate']['half_dimensions']['current_format_bytes'] for r in rows if r['estimate']['half_dimensions'])
        source=sum(r['source_image_bytes']+r['palette_bytes'] for r in rows)
        aggregate[scene]={'records':len(rows),'source_bytes':source,'baseline_gpu_bytes':base,
                          'hypothetical_16bit_color_saved_bytes':sixteen,'hypothetical_etc_color_saved_bytes':etc,
                          'hypothetical_half_current_saved_bytes':half,'runtime_approved_saved_bytes':0,
                          'formats':dict(Counter(r['gx_format'] for r in rows))}
        lines.append(f'| {scene} | {len(rows)} | {source:,} | {base:,} | {sixteen:,} | {etc:,} | {half:,} |')
    lines+=['','Color ceilings leave existing LA8/LA4 allocations unchanged. Alpha suitability checks include every source mip. '
            'These totals deliberately include unproven/dynamic/UI resources to show the broad arithmetic ceiling only; '
            'they are not candidate-policy totals, reclaimable RAM or measured bandwidth. Half and compression ceilings overlap.', '',
            '## Measured findings','']
    for sample in actual:
        r=sample['record']; variants={v['name']:v for v in sample['variants']}
        if r['gx_format']=='RGB565':
            v=variants['RGB565-full']
            lines.append(f'- {sample["directory"]}: GX565 -> PICA RGB565 is exact in every stored channel and mip; '
                         f'{v["original_gpu_bytes"]:,} -> {v["encoded_bytes"]:,} bytes ({v["baseline_over_proposed"]:.0f}× payload reduction).')
        if r['gx_format']=='CMPR' and r['scene']=='outside':
            key='ETC1-full' if r['estimate']['alpha_class_all_source_mips']=='opaque' else 'ETC1A4-full'
            v=variants[key]; m=v['base_vs_original']['channels']
            maximum='/'.join(str(m[c]['max_abs']) for c in 'rgba')
            lines.append(f'- {sample["directory"]}: {key}, {v["original_gpu_bytes"]:,} -> {v["encoded_bytes"]:,} bytes '
                         f'({v["baseline_over_proposed"]:.0f}×), R/G/B/A max errors {maximum}.')
            if r['estimate']['alpha_class_all_source_mips']=='binary' and 'ETC1A4-half' in variants:
                half=variants['ETC1A4-half']; am=half['base_vs_original']['channels']['a']
                coverage=half['base_vs_original']['alpha_coverage_at_least']['128']
                lines.append(f'- The same cutout at half dimensions has alpha max/mean error {am["max_abs"]}/{am["mean_abs"]:.3f}; '
                             f'{coverage["changed_pixels"]:,} original-grid pixels change the alpha >=128 test. '
                             'This is a measured edge-coverage change, so half-size cutouts require explicit visual/sampling review.')
    lines+=['','The previews include excluded map, shadow/data and UI-like examples as negative controls. '
            'Large errors hidden by zero alpha still count; an invisible checkerboard result cannot establish safe compression.', '',
            '## Representative actual results','',
            '| Sample | Format / scale | Padded + mip B | Ratio | R/G/B/A max error | Host encoding ms |',
            '|---|---|---:|---:|---|---:|']
    exact=[]; codec_ms=[]
    for sample in actual:
        for v in sample['variants']:
            maximum='/'.join(str(v['base_vs_original']['channels'][c]['max_abs']) for c in 'rgba')
            ms=sum(t.get('codec_ms_including_init',t.get('python_encode_ms',0)) for t in v['timings_per_mip'])
            if v['format'].startswith('ETC'): codec_ms.append(ms)
            lines.append(f'| {sample["directory"]} | {v["name"]} | {v["encoded_bytes"]:,} | {v["baseline_over_proposed"]:.2f}× | {maximum} | {ms:.3f} |')
            if v.get('exact_gx565_all_mips'): exact.append(sample['directory'])
    lines+=['','Actual byte counts are the emitted PICA payloads, including clamp-to-edge padding and all retained mips. '
            'The full-resolution variants retain every exported source mip. Half variants use alpha-aware 2×2 box filtering '
            'in stored channel space independently per source mip and drop tail levels below 8 texels in either axis. '
            'RGB5A1 is omitted when half filtering produces smooth alpha. No RGB565/ETC1 is tested on nonopaque chains.', '',
            'Per-channel max/mean absolute error, MSE/PSNR and alpha coverage at >=1, >=128 and ==255 are in actual-results.json '
            'for every variant; codec-only metrics are provided for each mip. Infinity PSNR is null plus lossless=true. '
            'Half base errors compare nearest-expanded output to the original grid; they are not a screen-space quality test.', '',
            'Exact GX RGB565 samples (all mips, all channels): '+(', '.join(exact) or 'none in selected set')+'.', '',
            f'All {prior["checked_records"]} house base RGBA hashes match the latest all-channel analyzer inventory. '
            'Measurements consume the real native decoder RGBA bytes, never its composited PNGs.', '',
            '## Encoder, dependencies and licensing','',
            'The installed tex3ds is Linux ELF aarch64 and cannot execute as a Mach-O program on this Mac. It was untouched. '
            'The full upstream CLI requires Magick++ >=6 and FreeType via pkg-config (configure.ac); Magick++ is unavailable. '
            'No package installation or global tool replacement was attempted. Instead the lab builds only the unmodified '
            'rg_etc1.cpp and its new small C++ harness, which need the C++ standard library. This is a codec harness, not a full tex3ds CLI build.', '',
            'tex3ds wrappers are GPL-3.0-or-later. rg_etc1.cpp/.h explicitly use the zlib license with Rich Geldreich attribution. '
            'No GPL source is compiled into the harness or game. The public checkout, notices and host tools remain outside Source. '
            'The independent Python ETC1 decoder follows the Khronos ETC1 bit grammar; the native GX helper is an input decoder, not its oracle.', '',
            f'Pinned encoder checkout: `{receipts["encoder_revision"]}`. '
            '[Primary tex3ds source](https://github.com/devkitPro/tex3ds/tree/'+receipts['encoder_revision']+'). '
            '[ETC1 specification](https://registry.khronos.org/OpenGL/extensions/OES/OES_compressed_ETC1_RGB8_texture.txt).', '',
            '## Validation and interpretation','',
            'test_texture_compression_lab.py checks fixed 16-bit byte vectors, Morton addresses, exhaustive RGB565 roundtrips, '
            'known individual/differential ETC1 blocks, flip/selectors, malformed differential rejection, '
            'PICA block order, byte endianness, x-major alpha nibbles, alpha-aware filtering and mip budget arithmetic. '
            'A deterministic random corpus compares the independently decoded ETC1 pixels to rg_etc1 unpack, '
            'and multi-tile encoded spatial patterns test orientation end to end. See tests.log and validation.json.', '',
            'Timing uses a single host process per encoded mip at medium ETC quality without dithering. '
            'Codec times include codec initialization; process wall times, Python packing and end-to-end '
            'encode/decode/metric wall times are separate in JSON. These measurements say nothing about New3DS runtime cost, '
            'GPU upload time, cache behavior or FPS. Offline ETC conversion may avoid runtime encoding only after a real '
            'immutable-asset cache/invalidation design is integrated by the parent.', '',
            'Provisional integration direction: exact GX565 -> PICA RGB565 first; review opaque immutable color assets '
            'for ETC1, binary-alpha assets for RGB5A1/ETC1A4, and graded-alpha color assets for RGBA4/ETC1A4. '
            'CMPR is decoded by the current runtime to RGBA8, so an 8×/4× GPU payload ratio does not mean the original '
            'GX bytes were reduced 8×/4×. LA8/LA4 -> RGBA4 can increase memory. EFB captures, packed shadow channels, '
            'UI, fonts, palettes and unknown roles remain excluded until ownership and sampling semantics are proven.', '',
            'Host validation is not proof of physical PICA sampling or visual acceptance at 400×240. '
            'No emulator, native target build, Source edits, commits or publication were performed.', '',
            '## Reproduce','', 'From the workspace root, using a fresh output directory:', '', '```sh',
            'python3 -m unittest discover -s Tools/3ds -p test_texture_compression_lab.py -v',
            'python3 Tools/3ds/texture_compression_lab.py --output Diagnostics/implementation-44/texture-compression/run-reproduce --sample-limit 18',
            'python3 Tools/3ds/validate_texture_compression_lab.py Diagnostics/implementation-44/texture-compression/run-reproduce',
            '```', '',
            'Build commands, source/binary hashes and unchanged dump/installed-tool/analyzer/native-input receipts are in receipts.json '
            'and Build/texture-compression-tools/*.build.json. PNGs and PICA payloads are private Nintendo-derived data; retain locally.']
    save_json(output/'summary.json',aggregate)
    (output/'REPORT.md').write_text('\n'.join(lines)+'\n')


if __name__=='__main__': main()
