#!/usr/bin/env python3
"""Evaluate a story benchmark folder against its ordered plan and observed gameplay."""
import argparse
import csv
import math
from pathlib import Path


FACTS = (
    'checkpoint', 'rendered', 'movement', 'cutscene', 'exit-request', 'arrival',
    'lantern', 'attack', 'multiple-enemies', 'enemy-damage', 'midna', 'portal', 'chapter-action', 'player-warp',
)
KNOWN_FACTS = (1 << len(FACTS)) - 1
PLAYER_WARP = 1 << FACTS.index('player-warp')
PLAYER_WARP_POINT = -4


def rows(path):
    with path.open(newline='', encoding='ascii') as file:
        for row in csv.DictReader(file):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f'incomplete CSV row in {path.name}')
            yield row


def integer(row, key):
    return int(row[key])


def location(row, prefix):
    return row[prefix + 'stage'], integer(row, prefix + 'room')


def destination_matches(plan, actual, prefix):
    return location(plan, 'to_') == location(actual, prefix) and all(
        integer(plan, 'to_' + key) == -1 or integer(plan, 'to_' + key) == integer(actual, prefix + key)
        for key in ('point', 'layer')
    )


def chapter_witness(plan, result):
    goal = plan.get('chapter_goal', 'legacy')
    if goal == 'ability':
        return integer(result, 'ability_ticks') >= 3
    if (goal in ('event', 'boss-intro') and plan.get('full_cutscene') == '1'
            and not plan['to_stage'] and integer(result, 'demo_ended') != 1):
        return False
    if goal == 'event':
        return (bool(plan['subject']) and integer(result, 'demo_ticks') >= 7
                and integer(result, 'timeline_ticks') >= 7)
    if goal == 'boss-intro':
        end_room = int(plan.get('intro_end_room', '-1'))
        if end_room >= 0 and not (
            location(result, 'final_') == (plan['stage'], end_room)
            and integer(result, 'final_scene') == integer(result, 'origin_scene')
            and integer(result, 'arrival_pictures') >= 3 and integer(result, 'demo_ended') == 1
        ):
            return False
        return bool(plan['subject']) and integer(result, 'demo_ticks') >= 7
    if goal == 'hit':
        return bool(plan['subject']) and integer(result, 'target_hits') > 0
    if goal == 'duel':
        return (
            bool(plan['subject']) and integer(result, 'target_hits') > 0
            and integer(result, 'finishing_blow') == 1 and integer(result, 'duel_ending') == 1
        )
    return False


def evaluate_step(plan, result):
    required = integer(plan, 'required')
    facts = integer(result, 'facts')
    if required < 3 or required & 3 != 3 or required & ~KNOWN_FACTS or facts < 0 or facts & ~KNOWN_FACTS:
        raise ValueError('invalid story evidence mask')
    if plan['step'] != result['step'] or required != integer(result, 'required'):
        raise ValueError('step or requirements differ from the plan')
    if result['result'] != 'pass':
        missing = [name for index, name in enumerate(FACTS) if required & (1 << index) and not facts & (1 << index)]
        return result['result'] + (f" (missing {', '.join(missing)})" if missing else '')
    if integer(result, 'life') <= 0 or integer(result, 'rejected') != 0 or integer(result, 'ticks') <= 0:
        return 'failed runtime health or renderer check'

    source, target = location(result, 'from_'), location(result, 'to_')
    scene = integer(result, 'origin_scene')
    checked_origin = scene > 0
    if 'chapter_goal' in plan:
        checked_origin = (
            checked_origin and location(result, 'origin_') == location(plan, '')
            and integer(result, 'origin_point') == integer(plan, 'point')
            and (integer(plan, 'layer') < 0 or integer(result, 'origin_layer') == integer(plan, 'layer'))
            and result['origin_form'] == plan['form']
        )
    scripted_reload = (plan.get('chapter_goal') == 'event' and integer(plan, 'point') >= 0
                       and integer(plan, 'to_point') != integer(plan, 'point'))
    is_warp = integer(plan, 'to_point') == PLAYER_WARP_POINT
    if is_warp != bool(required & PLAYER_WARP):
        raise ValueError('player warp requires its own evidence bit and destination point')
    warp_witnessed = is_warp and (
        integer(result, 'to_point') == PLAYER_WARP_POINT
        and integer(plan, 'warp_portal') == integer(result, 'warp_portal') >= 0
        and integer(plan, 'warp_spawn') == integer(result, 'warp_spawn') >= 0
        and integer(result, 'warp_confirmed') == 1 and integer(result, 'warp_departure_pictures') >= 3
    )
    exit_matches = (
        source == location(plan, '') and (source != target or (scripted_reload and not is_warp)) and source[0] and target[0]
        and (warp_witnessed if is_warp else integer(result, 'to_point') >= 0)
        and destination_matches(plan, result, 'to_')
        and integer(result, 'exit') == integer(plan, 'exit')
    )
    movement = float(result['movement'])
    witnesses = (
        checked_origin,
        integer(result, 'pictures') >= 3,
        math.isfinite(movement) and movement >= 80,
        integer(result, 'cutscene_last') - integer(result, 'cutscene_first') > 6,
        exit_matches,
        exit_matches and destination_matches(plan, result, 'final_') and integer(result, 'arrival_pictures') >= 3
        and integer(result, 'final_scene') > 0 and integer(result, 'final_scene') != scene,
        integer(result, 'lantern_pictures') > 0,
        integer(result, 'attack_pictures') > 0,
        integer(result, 'enemies') >= 2,
        integer(result, 'damage_samples') > 0,
        integer(result, 'midna_pictures') > 0,
        integer(result, 'portal_activated') == 1,
        chapter_witness(plan, result) if required & (1 << 12) else False,
        warp_witnessed,
    )
    missing = [
        name for index, name in enumerate(FACTS)
        if required & (1 << index) and (not facts & (1 << index) or not witnesses[index])
    ]
    return f"missing {', '.join(missing)}" if missing else 'pass'


def evaluate(tour):
    fields = dict(line.split('=', 1) for line in (tour / 'tour.txt').read_text().splitlines() if '=' in line)
    if fields.get('mode') not in ('story', 'everything') or fields.get('story_version') not in ('1', '2', '3', '4', '5', '6'):
        raise ValueError('unsupported story benchmark version')
    expected = list(range(integer(fields, 'first_step'), integer(fields, 'last_step') + 1))
    plan = list(rows(tour / 'story-plan.csv'))
    if not expected or expected[0] < 1 or [integer(row, 'zone') for row in plan] != expected:
        raise ValueError('story plan has missing, duplicate or reordered steps')
    if len({row['step'] for row in plan}) != len(plan):
        raise ValueError('duplicate story step ID')
    results = {}
    if int(fields['story_version']) >= 3:
        for item in plan:
            item['full_cutscene'] = '1'
    for row in rows(tour / 'story.csv'):
        zone = integer(row, 'zone')
        if zone not in expected or (results and zone <= next(reversed(results))):
            raise ValueError('story results have unexpected, duplicate or reordered steps')
        results[zone] = row
    verdicts = []
    for zone, item in zip(expected, plan):
        try:
            verdict = evaluate_step(item, results[zone]) if zone in results else 'missing outcome'
        except KeyError as error:
            # Older receipts may predate a required witness. Preserve the
            # independent verdicts for the other steps in the same tour.
            verdict = f'missing evidence field {error.args[0]}'
        verdicts.append((item['step'], verdict))
    return verdicts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tour', type=Path, help='a completed tour/NNN folder')
    args = parser.parse_args()
    try:
        results = evaluate(args.tour)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(2, f'{error}\n')
    for step, result in results:
        print(f'{step}  {result}')
    passed = sum(result == 'pass' for _, result in results)
    print(f'{passed}/{len(results)} story outcomes passed')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
