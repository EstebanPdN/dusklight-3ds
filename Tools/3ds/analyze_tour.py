#!/usr/bin/env python3
"""Summarize an automatic-test folder (sdmc:/3ds/DuskLight/tour/NNN).

Reads summary.csv and each zone's report.txt. Prints the zones ordered from
slowest to fastest, the failures, and how many zones meet 30 and 25 FPS.
With --sheet, also writes a contact sheet of the screenshots (needs Pillow).
Times come from the device that ran the test: a console folder gives console
figures, an emulator folder gives emulator figures.
"""
import argparse
import csv
from pathlib import Path


def number(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tour', type=Path, nargs='+', help='one tour folder, or the folders of a run split in parts')
    parser.add_argument('--sheet', type=Path, help='PNG contact sheet of shot1/shot2 per zone')
    args = parser.parse_args()
    rows = [row for tour in args.tour for row in csv.DictReader((tour / 'summary.csv').open())]
    # "moved": the game left the zone by itself during the test (a story event).
    done = [row for row in rows if row['status'] in ('ok', 'moved') and number(row, 'frames')]
    failed = [row for row in rows if row['status'] not in ('ok', 'moved')]
    moved = [row for row in done if row['status'] == 'moved']
    header = (args.tour[0] / 'tour.txt').read_text() if (args.tour[0] / 'tour.txt').exists() else ''
    print(header.strip())
    print(f'\nzones measured: {len(done)}   failed: {len(failed)}')
    for row in failed:
        print(f"  FAILED zone {row['zone']} {row['stage']} room {row['room']} {row['form']}: {row['status']}")
    if not done:
        return
    # "still" and "plain" (builds from b15): the zone standing still with the native effects
    # on, then off; their difference is what shadows, particles, bloom and haze cost there.
    plain = 'plain_ms' in done[0] and 'still_ms' in done[0]
    print('\nzone stage    room form   fps  loop  p95   p99   max   exec paint update gpuw draws rej deg  idle  cam  walk  act   tgt   item'
          + (' still plain effects' if plain else ''))
    for row in sorted(done, key=lambda r: -number(r, 'loop_ms')):
        print(f"{int(row['zone']):4d} {row['stage']:8s} {int(row['room']):3d} {row['form']:5s} {number(row, 'fps'):5.1f} "
              f"{number(row, 'loop_ms'):5.1f} {number(row, 'loop_p95'):5.1f} {number(row, 'loop_p99'):5.1f} {number(row, 'loop_max'):5.0f} "
              f"{number(row, 'exec_ms'):5.1f} {number(row, 'paint_ms'):5.1f} {number(row, 'update_ms'):5.1f} {number(row, 'gpu_wait_ms'):4.1f} "
              f"{number(row, 'draws'):5.0f} {int(number(row, 'rejected')):3d} {int(number(row, 'degraded')):4d} "
              + ' '.join(f"{number(row, key + '_ms'):5.1f}" for key in ('idle', 'camera', 'walk', 'action', 'target', 'items'))
              + (f" {number(row, 'still_ms'):5.1f} {number(row, 'plain_ms'):5.1f} {number(row, 'still_ms') - number(row, 'plain_ms'):6.1f}"
                 if plain else ''))
    at30 = sum(number(row, 'loop_p95') <= 34.0 for row in done)
    at25 = sum(number(row, 'loop_p95') <= 40.5 for row in done)
    mean = sum(number(row, 'loop_ms') for row in done) / len(done)
    print(f'\nzones with p95 frame time within 30 FPS: {at30}/{len(done)}; within 25 FPS: {at25}/{len(done)}; mean frame {mean:.1f} ms')
    if plain:
        both = [row for row in done if number(row, 'plain_ms') and number(row, 'still_ms')]
        if both:
            cost = sum(number(row, 'still_ms') - number(row, 'plain_ms') for row in both) / len(both)
            print(f'native effects (still minus plain), mean over {len(both)} zones: {cost:.1f} ms')
    if moved:
        print('zones the game left by itself during the test: ' + ', '.join(f"{row['stage']}/{row['room']}" for row in moved))
    rejected = [row for row in done if number(row, 'rejected')]
    if rejected:
        print('zones with rejected draws (missing geometry): ' + ', '.join(f"{row['stage']}/{row['room']}" for row in rejected))
    if args.sheet:
        from PIL import Image, ImageDraw
        shots = []
        for folder in sorted(path for tour in args.tour for path in tour.iterdir() if path.is_dir()):
            for name in ('shot1.bmp', 'shot2.bmp'):
                if (folder / name).exists():
                    shots.append((folder.name + ' ' + name[:5], folder / name))
        if not shots:
            print('no screenshots found')
            return
        columns = 4
        sheet = Image.new('RGB', (columns * 400, ((len(shots) + columns - 1) // columns) * 256), 'black')
        draw = ImageDraw.Draw(sheet)
        for index, (label, path) in enumerate(shots):
            x, y = index % columns * 400, index // columns * 256
            sheet.paste(Image.open(path).convert('RGB'), (x, y + 16))
            draw.text((x + 2, y + 2), label, fill='white')
        sheet.save(args.sheet)
        print(f'contact sheet: {args.sheet} ({len(shots)} screenshots)')


if __name__ == '__main__':
    main()
