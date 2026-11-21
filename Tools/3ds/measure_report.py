#!/usr/bin/env python3
"""Where a picture's time goes, from a measured full dump (Alpha 21 and later).

A full dump (L+R+A) first measures a burst of ordinary pictures
(Source/platforms/3ds/runtime/measured_dump.hpp) and writes the result into its
info.txt: the time of every process profile (its execute method, its draw
method, the game thread painting its models, the GPU-owning thread drawing
them), every named section timer and the renderer parts. The last pictures of
the burst send one draw per GPU command list, so dump.dpk holds the GPU's own
time for each draw with the process that owns its model.

  python3 -I Tools/3ds/measure_report.py <dump folder or info.txt> [--extracted <dir>]
  python3 -I Tools/3ds/measure_report.py <zone folder>/measure.txt [--detail]
  python3 -I Tools/3ds/measure_report.py <test folder tour/NNN> [--detail]

The automatic test (START+SELECT) measures each zone several times: between
two ordinary measurements one as a reference with every improvement off, and
one without a single improvement (another one in every zone). Its measure.txt
files are read as a series: one line per measurement, the mean by process,
the improvements against the reference; a test folder ends with what the
improvements are worth together and each by itself, over the zones each was
left out in. --extracted: the directory
`verify_application_dump.py dump.dpk --extract` wrote; adds the per-draw GPU
view of a dump (per owner, per painter phase, the slowest draws).
Standard library only. Private dump data stays local.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gpu_jobs_report as gpu

PHASES = ['frame', 'sky', 'world', 'shadows', 'actors', 'translucent', 'particles', 'effects', '2D']
KINDS = ['execute', 'draw', 'paint', 'replay']


def process_names():
    """Profile number -> name, from the game's own table."""
    header = Path(__file__).resolve().parents[2] / 'Source' / 'include' / 'f_pc' / 'f_pc_name.h'
    names = {}
    if header.exists():
        for match in re.finditer(r'/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*X\(fpcNm_(\w+?)_e\)', header.read_text(errors='replace')):
            names[int(match.group(1), 16)] = match.group(2)
    return names


def blocks(text):
    """The [measure] blocks of a file: each a {section header: lines} dictionary."""
    out, current, name = [], None, None
    for line in text.splitlines():
        if line.strip() == '[measure]':
            current = {'[measure]': []}
            out.append(current)
            name = '[measure]'
        elif line.startswith('['):
            name = line.strip()
            if current is not None and name.startswith('[measure '):
                current[name] = []
            else:
                current = None if not name.startswith('[measure') else current
        elif current is not None and name in current and line.strip():
            current[name].append(line)
    return out


def parse_block(parts):
    head = {}
    stages = {}
    for line in parts['[measure]']:
        if line.startswith('stage='):
            fields = dict(item.split('=', 1) for item in line.split())
            stages[fields['stage']] = {k: float(v) for k, v in fields.items() if k != 'stage'}
        elif '=' in line:
            key, value = line.split('=', 1)
            head[key] = value
    label = dict(item.split('=', 1) for item in head.get('label', '').split() if '=' in item)

    def rows(prefix):
        return next((lines for key, lines in parts.items() if key.startswith(prefix)), [])

    processes, sums = [], {}
    for line in rows('[measure processes]'):
        if line.startswith('sum '):
            sums = {k: float(v) for k, v in (item.split('=') for item in line.split()[1:])}
            continue
        f = line.split()
        processes.append({'profile': int(f[0]), 'ms': [float(f[1 + 2 * k]) for k in range(4)],
                          'calls': [float(f[2 + 2 * k]) for k in range(4)]})
    triple = lambda prefix: [(float(l.split()[0]), float(l.split()[1]), l.split(None, 2)[2]) for l in rows(prefix)]
    owners, owner_sum = [], {}
    for line in rows('[measure gpu owners]'):
        if line.startswith('sum '):
            owner_sum = {k: float(v) for k, v in (item.split('=') for item in line.split()[1:])}
            continue
        f = line.split()
        owners.append({'profile': int(f[0]), 'ms': float(f[1]), 'lists': float(f[2]), 'draws': float(f[3]), 'indices': float(f[4])})
    phases = [(int(l.split()[0]), float(l.split()[1]), float(l.split()[2])) for l in rows('[measure gpu phases]')]
    # Tick sections by the process method they ran in: (kind, profile) -> {name: [ms, calls]}; one name can be
    # registered at several call sites, so the rows of a name are summed.
    owned = {}
    for line in rows('[measure process sections]'):
        f = line.split(None, 4)
        if len(f) < 5 or f[0] == 'refused':
            continue
        cell = owned.setdefault((int(f[0]), int(f[1])), {}).setdefault(f[4], [0.0, 0.0])
        cell[0] += float(f[2])
        cell[1] += float(f[3])
    without = head.get('without', '-')
    # Update per logic tick: what the logic costs whatever the number of ticks a picture carried.
    for s in stages.values():
        s['update_tick_ms'] = s.get('update_ms', 0) / s['ticks_per_picture'] if s.get('ticks_per_picture') else 0.0
    return {'head': head, 'label': label, 'reference': head.get('reference') == '1', 'without': '' if without == '-' else without,
            'rested': int(head.get('worker_rested', 0)),
            'stages': stages, 'processes': processes,
            'sums': sums, 'named': triple('[measure sections]'), 'parts': triple('[measure parts]'),
            'gpu_owners': owners, 'gpu_sum': owner_sum, 'gpu_phases': phases, 'owned': owned}


def load(path):
    return [parse_block(parts) for parts in blocks(path.read_text(errors='replace'))]


def owner_name(names, profile):
    return "(nobody's model)" if profile < 0 else names.get(profile, str(profile))


def report_cpu(measure, names):
    stages = measure['stages']
    print('Burst pictures (ms per picture):')
    for name, s in stages.items():
        loop = s.get('loop_ms', 0)
        print(f"  {name:10} {int(s.get('pictures', 0)):2d} pictures  loop {loop:6.2f} ({1000 / loop if loop else 0:4.1f} FPS)  "
              f"exec {s.get('exec_ms', 0):6.2f} = update {s.get('update_ms', 0):5.2f} + draw {s.get('draw_ms', 0):5.2f} + paint {s.get('paint_ms', 0):5.2f}"
              f"  ticks {s.get('ticks_per_picture', 0):.2f}  draws {s.get('draws', 0):5.1f}  GPU lists {s.get('gpu_lists_ms', 0):6.2f}"
              f" idle {s.get('gpu_idle_ms', 0):5.2f}  audio call {s.get('audio_call_ms', 0):.2f}")
    base = stages.get('processes', {})
    sums = measure['sums']
    if measure['processes']:
        print('\nBy process, ms per picture (execute and draw exclusive; paint on the game thread and replay on the GPU thread by owner):')
        print(f"  {'process':18} {'total':>7} {'execute':>8} {'calls':>6} {'draw':>7} {'calls':>6} {'paint':>7} {'packets':>7} {'replay':>7} {'shapes':>7}")
        for row in sorted(measure['processes'], key=lambda r: -sum(r['ms'])):
            if sum(row['ms']) < 0.005:
                continue
            m, c = row['ms'], row['calls']
            print(f"  {owner_name(names, row['profile']):18} {sum(m):7.2f} {m[0]:8.2f} {c[0]:6.1f} {m[1]:7.2f} {c[1]:6.1f} {m[2]:7.2f} {c[2]:7.1f} {m[3]:7.2f} {c[3]:7.1f}")
        print(f"  {'sum':18} {sum(sums.values()):7.2f} {sums.get('execute_ms', 0):8.2f} {'':6} {sums.get('draw_ms', 0):7.2f} {'':6} "
              f"{sums.get('paint_ms', 0):7.2f} {'':7} {sums.get('replay_ms', 0):7.2f}")
        print(f"  measured phases of the same pictures: update {base.get('update_ms', 0):.2f} (outside any process: "
              f"{base.get('update_ms', 0) - sums.get('execute_ms', 0):.2f}), draw {base.get('draw_ms', 0):.2f} (outside: "
              f"{base.get('draw_ms', 0) - sums.get('draw_ms', 0):.2f}), paint {base.get('paint_ms', 0):.2f} (outside the 3D lists' packets: "
              f"{base.get('paint_ms', 0) - sums.get('paint_ms', 0):.2f}); the replay runs on the second core beside them")
    if measure['named']:
        print('\nNamed sections, ms per picture of the sections stage (inclusive, nested; that stage is slower by its own timers):')
        for ms, calls, name in sorted(measure['named'], key=lambda r: -r[0]):
            if ms >= 0.05:
                print(f'  {ms:7.2f} ms  {calls:7.1f} calls  {name}')
    if measure.get('owned'):
        print('\nTick sections inside the heaviest process methods, ms per picture of the sections stage (inclusive, nested):')
        heaviest = sorted(measure['processes'], key=lambda r: -(r['ms'][0] + r['ms'][1]))[:12]
        for row in heaviest:
            for kind in (0, 1):
                cells = measure['owned'].get((kind, row['profile']))
                if not cells or row['ms'][kind] < 0.05:
                    continue
                text = ', '.join(f'{name} {ms:.2f}/{calls:.1f}' for name, (ms, calls) in sorted(cells.items(), key=lambda kv: -kv[1][0])
                                 if ms >= 0.02)
                print(f"  {owner_name(names, row['profile']):16} {KINDS[kind]:7} {row['ms'][kind]:6.2f}: {text}")
    if measure['parts']:
        print('\nRenderer parts, ms per picture of the sections stage:')
        for ms, calls, name in sorted(measure['parts'], key=lambda r: -r[0]):
            print(f'  {ms:7.2f} ms  {calls:7.1f} calls  {name}')
    if measure['gpu_owners']:
        total = measure['gpu_sum'].get('ms', 0)
        print('\nGPU command-list time by owner, ms per picture of the gpu stage (one draw per list, measured by the console itself):')
        for row in sorted(measure['gpu_owners'], key=lambda r: -r['ms']):
            print(f"  {owner_name(names, row['profile']):18} {row['ms']:7.2f} ms {100 * row['ms'] / total if total else 0:5.1f} %  "
                  f"{row['draws']:6.1f} draws {row['indices']:8.0f} indices  {1000 * row['ms'] / row['indices'] if row['indices'] else 0:6.3f} us/index")
        print(f"  {'sum':18} {total:7.2f} ms          {measure['gpu_sum'].get('draws', 0):6.1f} draws {measure['gpu_sum'].get('indices', 0):8.0f} indices")
        print('  by painter phase: ' + ', '.join(f"{PHASES[p] if p < len(PHASES) else p} {ms:.2f}" for p, ms, _ in sorted(measure['gpu_phases'], key=lambda r: -r[1])))


def one_line(m):
    s = m['stages'].get('processes', {})
    g = m['stages'].get('gpu', {})
    loop = s.get('loop_ms', 0)
    return (f"loop {loop:6.2f} ({1000 / loop if loop else 0:4.1f} FPS) exec {s.get('exec_ms', 0):6.2f} update {s.get('update_ms', 0):5.2f} "
            f"draw {s.get('draw_ms', 0):5.2f} paint {s.get('paint_ms', 0):5.2f} ticks {s.get('ticks_per_picture', 0):.2f} draws {s.get('draws', 0):5.1f} "
            f"GPU lists {s.get('gpu_lists_ms', 0):6.2f} | one-draw lists {m['gpu_sum'].get('ms', 0):6.2f} ms {m['gpu_sum'].get('indices', 0):7.0f} indices"
            + (f" (shadow casters {m['gpu_sum']['casters']:.0f})" if 'casters' in m['gpu_sum'] else '')
            + (f" (gpu stage loop {g.get('loop_ms', 0):.1f})" if g else ''))


COMPARED = (('loop_ms', 'loop'), ('exec_ms', 'exec'), ('update_ms', 'update'), ('update_tick_ms', 'update/tick'), ('draw_ms', 'draw'),
            ('paint_ms', 'paint'), ('gpu_lists_ms', 'GPU lists'), ('draws', 'draws'))
COMPARED_GPU = (('ms', 'draw lists'), ('indices', 'indices'), ('casters', 'shadow ind.'))  # each when the measurement has it


def changed(m):
    """What a measurement had turned off: 'all' (a reference), an improvement's name, or '' (ordinary)."""
    return 'all' if m['reference'] else m['without']


def comparisons(measures):
    """(index, what was off, {figure: (on, off)}) for every measurement with improvements off that
    has an ordinary neighbour: `on` is the mean of the ordinary measurements right before and after.
    A measurement that shares pictures with a rest of the second-core worker (a dump's can) is
    pictures of another kind: it is neither compared nor a neighbour."""
    out = []
    for n, m in enumerate(measures):
        if not changed(m) or m['rested']:
            continue
        around = [x for x in (measures[n - 1] if n else None, measures[n + 1] if n + 1 < len(measures) else None)
                  if x is not None and not changed(x) and not x['rested']]
        if not around:
            continue
        mean = lambda pick: sum(pick(x) for x in around) / len(around)
        off = m['stages'].get('processes', {})
        figures = {text: (mean(lambda x: x['stages'].get('processes', {}).get(key, 0)), off.get(key, 0)) for key, text in COMPARED}
        figures.update({text: (mean(lambda x: x['gpu_sum'].get(key, 0)), m['gpu_sum'][key]) for key, text in COMPARED_GPU if key in m['gpu_sum']})
        out.append((n, changed(m), figures, len(around), around[0]['head'].get('features', '?')))
    return out


def report_series(measures, names, title):
    """Several measurements of one zone: each in a line, the mean by process, and
    each measurement with improvements off against the ordinary ones around it."""
    print(f'{title}: {len(measures)} measurements')
    for n, m in enumerate(measures):
        label = m['label']
        kind = 'REFERENCE' if m['reference'] else f"WITHOUT {m['without']}" if m['without'] else 'ordinary'
        print(f"  {n + 1} {kind:14} {label.get('phase', ''):10} picture {label.get('picture', '?'):>5}  {one_line(m)}"
              + (f"  WORKER RESTED in {m['rested']} pictures: not compared" if m['rested'] else ''))
    ordinary = [m for m in measures if not changed(m) and not m['rested']]
    if ordinary:
        totals = {}
        for m in ordinary:
            for row in m['processes']:
                t = totals.setdefault(row['profile'], [0.0] * 4)
                for k in range(4):
                    t[k] += row['ms'][k] / len(ordinary)
        print(f'  mean of the {len(ordinary)} ordinary measurements, ms per picture by process (execute, draw, paint, replay):')
        for profile, t in sorted(totals.items(), key=lambda item: -sum(item[1]))[:14]:
            print(f"    {owner_name(names, profile):18} {sum(t):6.2f} = {t[0]:5.2f} + {t[1]:5.2f} + {t[2]:5.2f} + {t[3]:5.2f}")
    for n, what, figures, around, features in comparisons(measures):
        against = 'the reference' if what == 'all' else f'the same without {what}'
        print(f"  improvements on (mean of {around} measurements around) against {against} ({n + 1}), features {features}:")
        for text, (on, off) in figures.items():
            print(f"    {text:10} {on:7.2f} against {off:7.2f}  ({on - off:+.2f})")


def report_worth(series):
    """Over the zones of a test: what the improvements are worth together (the references) and each
    by itself (the zones it was left out in). Negative: the improvements save that much."""
    worth = {}
    for title, measures in series:
        for _, what, figures, _, _ in comparisons(measures):
            worth.setdefault(what, []).append((title, figures))
    if not worth:
        return
    print('What the improvements are worth over the zones of this test (on minus off, mean per picture; negative = saved):')
    print(f"  {'left out':8} {'zones':>5} {'loop ms':>8} {'+-':>5} {'exec':>7} {'update':>7} {'per tick':>8} {'draw':>7} {'paint':>7} {'GPU lists':>9} {'draws':>7} "
          f"{'draw lists':>10} {'indices':>8}  {'zones faster':>12}  largest saving")
    for what in sorted(worth, key=lambda w: (w != 'all', w)):
        rows = worth[what]
        mean = lambda text: sum(f[text][0] - f[text][1] for _, f in rows) / len(rows)
        faster = sum(1 for _, f in rows if f['loop'][0] < f['loop'][1])
        # Standard error of the mean loop difference: a mean within about two of it is not told from zero.
        loops = [f['loop'][0] - f['loop'][1] for _, f in rows]
        error = (sum((x - mean('loop')) ** 2 for x in loops) / (len(loops) - 1) / len(loops)) ** 0.5 if len(loops) > 1 else float('nan')
        best = min(rows, key=lambda row: row[1]['loop'][0] - row[1]['loop'][1])
        print(f"  {what:8} {len(rows):5d} {mean('loop'):+8.2f} {error:5.2f} {mean('exec'):+7.2f} {mean('update'):+7.2f} {mean('update/tick'):+8.2f} {mean('draw'):+7.2f} {mean('paint'):+7.2f} "
              f"{mean('GPU lists'):+9.2f} {mean('draws'):+7.1f} {mean('draw lists'):+10.2f} {mean('indices'):+8.0f}  {faster:5d} of {len(rows):<4d}  "
              f"{best[1]['loop'][0] - best[1]['loop'][1]:+.2f} ms in {best[0]}")
    print('  +-: standard error of the loop difference; one zone moves by itself between two measurements (the view changes),')
    print('  so read a row over many zones, and a mean within about two standard errors of zero as no difference')


def last_picture_range(ring):
    for start in reversed([n for n, j in enumerate(ring) if j['type'] == 2]):
        ends = [n for n in range(start, len(ring)) if ring[n]['type'] == 3]
        if ends and all(j['ms'] is not None for j in ring[start:ends[0] + 1]):
            return start, ends[0] + 1
    return None


def report_gpu(extracted, names, top):
    owners_path = extracted / 'gpu-job-owners.bin'
    ring = gpu.jobs(extracted / 'gpu-jobs.bin')
    span = last_picture_range(ring)
    if not span:
        print('\nGPU: no complete picture in the job ring.')
        return
    owners = None
    if owners_path.exists():
        data = owners_path.read_bytes()
        if len(data) == 4 * len(ring):
            owners = struct.unpack(f'<{len(ring)}I', data)
    memory = gpu.Memory(extracted)
    state = gpu.State()
    picture = ring[span[0]:span[1]]
    for job in picture:
        if job['type'] == 1:
            try:
                gpu.describe_list(memory, state, job)
            except (ValueError, struct.error):
                pass
    rows, other = [], {}
    for offset, job in enumerate(picture):
        kind = gpu.KIND.get(job['type'], str(job['type']))
        if job['type'] != 1:
            other[kind] = other.get(kind, 0.0) + job['ms']
            continue
        word = owners[span[0] + offset] if owners else 0
        try:
            draws = gpu.describe_list(memory, state, job)
        except (ValueError, struct.error):
            draws = []
        rows.append({'ms': job['ms'], 'owner': word & 0xffff, 'phase': (word >> 16) & 0xff, 'draws': draws,
                     'indices': sum(d['indices'] for d in draws)})
    total = sum(r['ms'] for r in rows)
    busy = total + sum(other.values())
    wall = (picture[-1]['completed'] - picture[0]['submitted']) * 1000 / gpu.TICKS_PER_SECOND
    single = sum(1 for r in rows if len(r['draws']) == 1)
    print(f"\nGPU, last picture of the burst: {len(rows)} command lists ({single} of one draw), lists {total:.2f} ms, "
          + ', '.join(f'{k} {v:.2f} ms' for k, v in other.items()) + f'; busy {busy:.2f} of {wall:.2f} ms')
    if owners is None:
        print('  (no gpu-job-owners in this dump: owners unknown)')

    def owner_name(owner):
        return "(nobody's model)" if not owner else names.get(owner - 1, str(owner - 1))

    def table(title, key, label):
        groups = {}
        for r in rows:
            g = groups.setdefault(key(r), [0.0, 0, 0, 0])
            g[0] += r['ms']; g[1] += 1; g[2] += len(r['draws']); g[3] += r['indices']
        print(f'\n  {title}:')
        for k, g in sorted(groups.items(), key=lambda item: -item[1][0]):
            print(f'    {label(k):18} {g[0]:7.2f} ms  {100 * g[0] / total if total else 0:5.1f} %  {g[1]:4d} lists {g[2]:4d} draws {g[3]:7d} indices'
                  f'  {1000 * g[0] / g[3] if g[3] else 0:6.3f} us/index')

    table('GPU time by owner', lambda r: r['owner'], owner_name)
    table('GPU time by painter phase', lambda r: r['phase'], lambda p: PHASES[p] if p < len(PHASES) else str(p))
    print(f'\n  Slowest {top} lists:')
    for r in sorted(rows, key=lambda r: -r['ms'])[:top]:
        d = r['draws'][0] if len(r['draws']) == 1 else None
        phase = PHASES[r['phase']] if r['phase'] < len(PHASES) else str(r['phase'])
        if d:
            rigid = d['rigid']
            geometry = (f"unique {rigid['unique']:5d} inside {rigid['inside']:5d} tris {rigid['screens']:5.2f} screens" if rigid
                        else 'other vertex layout')
            detail = (f"{d['indices']:6d} indices  {geometry}  z{d['depth_test']}{d['depth_write']} alpha-test {d['alpha_test']}  "
                      + '; '.join(d['textures']))
        else:
            detail = f"{len(r['draws'])} draws, {r['indices']} indices"
        print(f"    {r['ms']:6.2f} ms  {owner_name(r['owner']):16} {phase:11} {detail}")
    timed = [(r['ms'], r['draws'][0]) for r in rows if len(r['draws']) == 1 and r['draws'][0]['rigid']]
    if len(timed) >= 8:
        fit = gpu.least_squares([(d['indices'] / 1000, d['rigid']['screens'], 1.0) for _, d in timed], [ms for ms, _ in timed])
        if fit:
            print(f'\n  Fit over {len(timed)} one-draw lists: {fit[0]:.3f} ms per 1000 indices, {fit[1]:.3f} ms per screen of area, {fit[2]:.3f} ms per draw')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path, help="a dump folder or its info.txt, a zone's measure.txt, or a test folder (tour/NNN)")
    parser.add_argument('--extracted', type=Path, help='directory written by verify_application_dump.py --extract (dumps only)')
    parser.add_argument('--top', type=int, default=30, help='slowest GPU lists to print (with --extracted)')
    parser.add_argument('--detail', action='store_true', help='the full tables of every measurement of a series')
    args = parser.parse_args()
    names = process_names()
    source = args.source
    if source.is_dir() and not (source / 'info.txt').exists():
        files = sorted(source.glob('*/measure.txt')) or sorted(source.glob('measure.txt'))
        if not files:
            print('No measure.txt in this folder.')
            return 1
        series = []
        for file in files:
            measures = load(file)
            series.append((file.parent.name, measures))
            report_series(measures, names, file.parent.name)
            if args.detail:
                for m in measures:
                    print()
                    report_cpu(m, names)
            print()
        if len(series) > 1:
            report_worth(series)
        return 0
    info = source / 'info.txt' if source.is_dir() else source
    text = info.read_text(errors='replace')
    measures = [parse_block(parts) for parts in blocks(text)]
    if info.name == 'info.txt':
        head = {k: v for k, v in (line.split('=', 1) for line in text.splitlines()[:40] if '=' in line)}
        print(f"{head.get('build', '?')}  {head.get('stage', '?')} room {head.get('room', '?')}  frame {head.get('frame', '?')}  "
              f"{head.get('fps', '?')} FPS when asked")
    if not measures:
        print('No [measure] section: the dump is older than Alpha 21.')
    elif len(measures) == 1:
        report_cpu(measures[0], names)
    else:
        report_series(measures, names, info.name)
        if args.detail:
            for m in measures:
                print()
                report_cpu(m, names)
    if args.extracted:
        report_gpu(args.extracted, names, args.top)
    return 0


if __name__ == '__main__':
    sys.exit(main())
