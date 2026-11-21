#!/usr/bin/env python3
"""Summarize an automatic-test folder (sdmc:/3ds/DuskLight/tour/NNN).

Reads summary.csv and each zone's report.txt. Prints the zones ordered from
slowest to fastest, the failures, and how many zones meet 30 and 25 FPS.
With --sheet, also writes a contact sheet of the screenshots (needs Pillow).
Times come from the device that ran the test: a console folder gives console
figures, an emulator folder gives emulator figures.

Zone status: ok; moved (the game left the zone's stage: report.txt
scene_change_in_zone says how, from builds after Alpha 6); died (Link died
anyway, from Alpha 7: the frames after the death are already left out of the
zone's figures, and the zone is left out of the totals below). shot1.bmp is
the view after the camera phase; shot2.bmp the end of the zone, after the
profile phase, where an item from the items phase may still be out.
"""
import argparse
import csv
from pathlib import Path

ROOM_TIMINGS = ('ok', 'moved', 'died', 'render-rejected')


def number(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def zone_report(tours, zone):
    """key=value lines of one zone's report.txt (folders are NNN-STAGE-rRR-f)."""
    for tour in tours:
        for folder in tour.glob(f'{int(zone):03d}-*'):
            report = folder / 'report.txt'
            if report.exists():
                return dict(line.split('=', 1) for line in report.read_text(errors='replace').splitlines() if '=' in line)
    return {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tour', type=Path, nargs='+', help='one tour folder, or the folders of a run split in parts')
    parser.add_argument('--sheet', type=Path, help='PNG contact sheet of shot1/shot2 per zone')
    args = parser.parse_args()
    rows, story = [], []
    for tour in args.tour:
        metadata = tour / 'tour.txt'
        info = dict(line.split('=', 1) for line in metadata.read_text().splitlines() if '=' in line) if metadata.exists() else {}
        last_story = int(info.get('last_step', 0)) if info.get('mode') == 'everything' else 0
        with (tour / 'summary.csv').open(newline='') as summary:
            for row in csv.DictReader(summary):
                if info.get('mode') == 'story' or int(row['zone']) <= last_story:
                    story.append(row)
                else:
                    rows.append(row)
    # "moved": the game left the zone by itself during the test (a story event).
    # "died": Link died; the Game Over frames are not zone performance.
    measured = [row for row in rows if row['status'] in ROOM_TIMINGS and number(row, 'frames')]
    failed = [row for row in rows if row['status'] not in ('ok', 'moved', 'died')]
    moved = [row for row in measured if row['status'] == 'moved']
    # A death in the first phase can leave no frame in the statistics at all.
    died = [row for row in rows if row['status'] == 'died']
    done = [row for row in measured if row['status'] != 'died']
    header = (args.tour[0] / 'tour.txt').read_text() if (args.tour[0] / 'tour.txt').exists() else ''
    print(header.strip())
    if story:
        print(f"\nstory summary: {sum(row['status'] == 'pass' for row in story)}/{len(story)} reported pass")
        for row in story:
            if row['status'] != 'pass':
                print(f"  FAILED story step {row['zone']} {row['stage']} room {row['room']}: {row['status']}")
    print(f'\nzones measured: {len(done)}   died (left out of the totals): {len(died)}   failed: {len(failed)}')
    for row in failed:
        print(f"  FAILED zone {row['zone']} {row['stage']} room {row['room']} {row['form']}: {row['status']}")
    if not measured:
        return
    # "still" and "plain" (builds from b15): the zone standing still with the native effects
    # on, then off; their difference is what shadows, particles, bloom and haze cost there.
    plain = 'plain_ms' in measured[0] and 'still_ms' in measured[0]
    # "onecore" (builds from Alpha 10): standing still with the second-core worker off; onecore
    # minus still is what the worker gains there (zero when the worker is off for the session).
    onecore = 'onecore_ms' in measured[0] and 'still_ms' in measured[0]
    print('\nzone stage    room form  status  fps  loop  p95   p99   max   exec paint update gpuw draws rej deg  idle  cam  walk  act   tgt   item'
          + (' still plain effects' if plain else '') + (' 1core core2-gain' if onecore else ''))
    for row in sorted(measured, key=lambda r: -number(r, 'loop_ms')):
        print(f"{int(row['zone']):4d} {row['stage']:8s} {int(row['room']):3d} {row['form']:5s} {row['status']:6s} {number(row, 'fps'):5.1f} "
              f"{number(row, 'loop_ms'):5.1f} {number(row, 'loop_p95'):5.1f} {number(row, 'loop_p99'):5.1f} {number(row, 'loop_max'):5.0f} "
              f"{number(row, 'exec_ms'):5.1f} {number(row, 'paint_ms'):5.1f} {number(row, 'update_ms'):5.1f} {number(row, 'gpu_wait_ms'):4.1f} "
              f"{number(row, 'draws'):5.0f} {int(number(row, 'rejected')):3d} {int(number(row, 'degraded')):4d} "
              + ' '.join(f"{number(row, key + '_ms'):5.1f}" for key in ('idle', 'camera', 'walk', 'action', 'target', 'items'))
              + (f" {number(row, 'still_ms'):5.1f} {number(row, 'plain_ms'):5.1f} {number(row, 'still_ms') - number(row, 'plain_ms'):6.1f}"
                 if plain else '')
              + (f" {number(row, 'onecore_ms'):5.1f} {number(row, 'onecore_ms') - number(row, 'still_ms'):6.1f}" if onecore else ''))
    if not done:
        print('\nno zone without a death to total')
        return
    at30 = sum(number(row, 'loop_p95') <= 34.0 for row in done)
    at25 = sum(number(row, 'loop_p95') <= 40.5 for row in done)
    mean = sum(number(row, 'loop_ms') for row in done) / len(done)
    print(f'\nzones with p95 frame time within 30 FPS: {at30}/{len(done)}; within 25 FPS: {at25}/{len(done)}; mean frame {mean:.1f} ms')
    if plain:
        both = [row for row in done if number(row, 'plain_ms') and number(row, 'still_ms')]
        if both:
            cost = sum(number(row, 'still_ms') - number(row, 'plain_ms') for row in both) / len(both)
            print(f'native effects (still minus plain), mean over {len(both)} zones: {cost:.1f} ms')
    if onecore:
        both = [row for row in done if number(row, 'onecore_ms') and number(row, 'still_ms')]
        if both:
            gain = sum(number(row, 'onecore_ms') - number(row, 'still_ms') for row in both) / len(both)
            slower = sum(number(row, 'onecore_ms') < number(row, 'still_ms') for row in both)
            print(f'second-core worker (onecore minus still), mean over {len(both)} zones: {gain:.1f} ms a picture gained; '
                  f'slower with the worker in {slower} zones')
    if moved:
        print('zones the game left by itself during the test: ' + ', '.join(f"{row['stage']}/{row['room']}" for row in moved))
        for row in moved:
            how = zone_report(args.tour, row['zone']).get('scene_change_in_zone')
            if how:
                print(f"  zone {row['zone']} {row['stage']}/{row['room']}: {how}")
    if died:
        print('zones where Link died (left out of the totals): ' + ', '.join(
            f"{row['zone']} {row['stage']}/{row['room']} ({zone_report(args.tour, row['zone']).get('died_in_phase', '?')} phase)" for row in died))
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
