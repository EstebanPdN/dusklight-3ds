#!/usr/bin/env python3
"""Separate executed stage probes, visible scenes, and verified story coverage."""
import argparse
import hashlib
import html
import json
from pathlib import Path
import re
from capture_to_png import read_capture, to_lcd_rgb

ROOT = Path(__file__).resolve().parents[2]


def capture_metrics(path):
    if path.stat().st_size > 4 * 1024 * 1024 + 16:
        raise ValueError('capture exceeds framebuffer bound')
    width, height, rgb = to_lcd_rgb(*read_capture(path))
    colors = set(zip(rgb[0::3], rgb[1::3], rgb[2::3]))
    return {'width': width, 'height': height, 'unique_colors': len(colors),
            'black_pixels': sum(r == g == b == 0 for r, g, b in zip(rgb[0::3], rgb[1::3], rgb[2::3])),
            'all_black': not any(rgb), 'uniform': len(colors) == 1,
            'raw_sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def audit(matrix_path, evidence_root, output):
    matrix = json.loads(matrix_path.read_text())
    rows = []
    for result in matrix['results']:
        run = result['run']
        if not re.fullmatch(r'[A-Za-z0-9_-]+', run):
            raise ValueError('invalid run basename')
        evidence = evidence_root / run
        runtime = ROOT / 'Build/azahar-runs' / run / 'xdg-data/azahar-emu/sdmc/3ds/dusklight'
        receipt = json.loads((evidence / 'process.json').read_text()) if (evidence / 'process.json').exists() else {}
        captures = []
        for name in result['captures']:
            if not re.fullmatch(r'frame-\d+\.png', name):
                raise ValueError('invalid capture basename')
            raw = runtime / 'captures' / (Path(name).stem + '.rgba')
            item = {'png': str((evidence / name).resolve()), 'raw': str(raw.resolve())}
            try:
                item.update(capture_metrics(raw))
            except (OSError, ValueError) as error:
                item['error'] = str(error)
            captures.append(item)
        valid = [c for c in captures if 'error' not in c]
        if not valid:
            visual = 'missing_or_unreadable'
        elif all(c['all_black'] for c in valid):
            visual = 'all_black'
        elif all(c['uniform'] for c in valid):
            visual = 'uniform_only'
        else:
            visual = 'nonuniform_capture_needs_review'
        log = (evidence / 'guest.log').read_text(errors='replace') if (evidence / 'guest.log').exists() else ''
        memory = [{key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
                  for line in log.splitlines() if 'Native renderer memory:' in line]
        rows.append({**result, 'candidate_sha256': receipt.get('cxi_sha256'),
                     'entry_kind': 'direct_stage_boot' if result.get('boot_stage') else 'normal_boot',
                     'story_checkpoint_verified': False,
                     'story_limit': 'No verified save/story-state fixture supplied; stage name alone is insufficient.',
                     'visual_status': visual, 'capture_evidence': captures,
                     'memory_samples': memory,
                     'playability_verified': False})
    unique = {row['boot_stage'] for row in rows if row.get('boot_stage')}
    report = {'schema': 1, 'required_distinct_scenarios': 10,
              'minimum_frames_or_trials': None,
              'owner_clarification': '10 scenarios minimum; 10,000 was a transcription error.',
              'distinct_scenarios_attempted': len(unique),
              'scenario_attempt_floor_met': len(unique) >= 10,
              'verified_story_checkpoints': 0,
              'acceptance_complete': False,
              'limitations': ['An attempted scene is not a passed scene.',
                             'A nonuniform image is not visual correctness or progression proof.',
                             'No physical-console validation.'], 'results': rows}
    output.mkdir(parents=True, exist_ok=False)
    (output / 'coverage.json').write_text(json.dumps(report, indent=2) + '\n')
    content = ['<!doctype html><html lang="es"><meta charset="utf-8"><title>Cobertura de escenarios</title>',
               '<style>body{font:16px system-ui;background:#14171d;color:#eee;max-width:1100px;margin:32px auto}section{padding:20px;border:1px solid #454955;margin:16px 0;border-radius:12px}img{width:400px;max-width:100%;image-rendering:auto}code{color:#c4d7ff}p{line-height:1.5}</style>',
               '<h1>Pruebas en escenarios distintos</h1>',
               f'<p>{len(unique)} escenarios intentados · requisito: mínimo 10 · ningún punto de historia validado todavía.</p>',
               '<p>Se conservan los fallos. Una salida limpia, una pantalla negra o una imagen con contenido no bastan para declarar el juego jugable.</p>']
    for row in rows:
        content += [f'<section><h2>{html.escape(row["name"])}</h2>',
                    f'<p><code>{html.escape(row["boot_stage"] or "normal boot")}</code><br>',
                    f'Ejecución: {html.escape(row["status"])} · imagen: {html.escape(row["visual_status"])}</p>']
        if row['problems']:
            content += ['<pre>' + html.escape('\n'.join(row['problems'])) + '</pre>']
        for capture in row['capture_evidence']:
            content += [f'<img src="{html.escape(Path(capture["png"]).as_uri())}" alt="Captura {html.escape(row["name"])}">']
        content += ['</section>']
    content += ['</html>']
    (output / 'index.html').write_text('\n'.join(content))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrix', type=Path)
    parser.add_argument('--evidence-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = audit(args.matrix, args.evidence_root, args.output)
    print(json.dumps({key: value for key, value in report.items() if key != 'results'}, indent=2))
    for row in report['results']:
        print(row['name'], row['status'], row['visual_status'])


if __name__ == '__main__':
    main()
