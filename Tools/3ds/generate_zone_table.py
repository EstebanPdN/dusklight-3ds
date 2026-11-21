#!/usr/bin/env python3
"""Generate the developer-console zone table from start-point metadata.

Input is the JSON written by list_stage_spawns.py (stage/room names, start
point numbers, start modes and arrival events). Output is a C++ include with
one entry per stage and one per room; labels are this project's own English
descriptions. No game data.
"""
import argparse
import json
from pathlib import Path

# Console order and labels (ASCII: the libctru console font has no accents).
# The third field marks the short "main" tour.
STAGES = [
    ('F_SP103', "Ordon Village", 1), ('F_SP00', "Ordon Ranch", 1), ('F_SP104', "Ordon Spring", 1),
    ('R_SP01', "Ordon houses", 1), ('F_SP108', "Faron Woods", 1), ('R_SP108', "Coro's house", 0),
    ('D_SB10', "Faron Woods cave", 0), ('F_SP117', "Sacred Grove", 1), ('F_SP121', "Hyrule Field", 1),
    ('F_SP122', "Outside Castle Town", 1), ('F_SP109', "Kakariko Village", 1), ('R_SP109', "Kakariko houses", 1),
    ('R_SP209', "Sanctuary basement", 0), ('F_SP111', "Kakariko Graveyard", 1), ('F_SP110', "Death Mountain", 1),
    ('R_SP110', "Goron elder hall", 0), ('F_SP128', "Hidden Village", 0), ('R_SP128', "Impaz's house", 0),
    ('F_SP116', "Castle Town", 1), ('R_SP160', "Castle Town shops", 0), ('R_SP116', "Telma's Bar", 1),
    ('R_SP161', "STAR tent", 0), ('R_SP107', "Castle sewers", 1), ('F_SP115', "Lake Hylia", 1),
    ('F_SP112', "Zora's River", 0), ('F_SP126', "Upper Zora's River", 0), ('F_SP113', "Zora's Domain", 1),
    ('F_SP127', "Fishing pond", 0), ('R_SP127', "Fishing hut", 0), ('F_SP114', "Snowpeak", 1),
    ('F_SP124', "Gerudo Desert", 1), ('F_SP118', "Bulblin Camp", 0), ('F_SP125', "Mirror Chamber", 0),
    ('F_SP123', "King Bulblin bridge", 0), ('F_SP200', "Howling stone arena", 0),
    ('D_MN05', "Forest Temple", 1), ('D_MN05B', "Forest: Ook", 0), ('D_MN05A', "Forest: Diababa", 1),
    ('D_MN04', "Goron Mines", 1), ('D_MN04B', "Mines: Dangoro", 0), ('D_MN04A', "Mines: Fyrus", 1),
    ('D_MN01', "Lakebed Temple", 1), ('D_MN01B', "Lakebed: Deku Toad", 0), ('D_MN01A', "Lakebed: Morpheel", 1),
    ('D_MN10', "Arbiter's Grounds", 1), ('D_MN10B', "Arbiter: Death Sword", 0), ('D_MN10A', "Arbiter: Stallord", 1),
    ('D_MN11', "Snowpeak Ruins", 1), ('D_MN11B', "Ruins: Darkhammer", 0), ('D_MN11A', "Ruins: Blizzeta", 1),
    ('D_MN06', "Temple of Time", 1), ('D_MN06B', "Time: Darknut", 0), ('D_MN06A', "Time: Armogohma", 1),
    ('D_MN07', "City in the Sky", 1), ('D_MN07B', "Sky: Aeralfos", 0), ('D_MN07A', "Sky: Argorok", 1),
    ('D_MN08', "Palace of Twilight", 1), ('D_MN08B', "Palace: Phantom Zant 1", 0), ('D_MN08C', "Palace: Phantom Zant 2", 0),
    ('D_MN08A', "Palace: throne", 0), ('D_MN08D', "Palace: Zant battle", 1),
    ('D_MN09', "Hyrule Castle", 1), ('D_MN09A', "Castle: throne room", 1), ('D_MN09B', "Ganon: horseback", 1),
    ('D_MN09C', "Ganon: final duel", 1),
    ('D_SB01', "Cave of Ordeals", 1), ('D_SB00', "Ice block cave", 0), ('D_SB02', "Eldin lantern cave", 0),
    ('D_SB03', "Lake lantern cave", 0), ('D_SB04', "Goron stockcave", 0), ('D_SB05', "Grotto 1", 0),
    ('D_SB06', "Grotto 2", 0), ('D_SB07', "Grotto 3", 0), ('D_SB08', "Grotto 4", 0), ('D_SB09', "Grotto 5", 0),
    ('R_SP300', "Light arrows scene", 0), ('R_SP301', "Castle scene", 0),
    ('F_SP102', "Title screen stage", 0), ('S_MV000', "Title movie stage", 0),
]


# Room used when a test or the list takes one room per stage (default: the first).
PRIMARY = {'F_SP103': 1, 'R_SP01': 4}


# Starts the test must not use. Boar and canoe: they wait for an actor an
# empty save does not have, so the game would never begin. F_SP118 room 3
# point 4: Link spawns 1,500 units straight above the exit to Arbiter's
# Grounds and falls into it during the load (Alpha 6 zone 92: 59 s load,
# ended in D_MN10).
SPECIAL_STARTS = {('F_SP118', 3, 0), ('F_SP115', 0, 2), ('F_SP118', 3, 4)}  # boar, canoe, falls into an exit

# Event byte of a start point (top byte of the PLYR parameters) meaning "no
# room event on arrival". Older spawn lists without the field count as none.
NO_EVENT = 0xff


def start_point(stage, room, points):
    """Lowest start point on foot without an arrival event: mode 0 first,
    never the horse (mode 2).

    A start point with an event byte starts a room map event on arrival (a
    cutscene, or a camera event whose exit takes Link elsewhere). In Alpha 6,
    9 of 94 zones began inside one, and three of them (14, 35, 74) left their
    stage through the event's exit. Event points are used only when a room
    has nothing else."""
    usable = [p for p in points if p.get('mode') != 2 and (stage, room, p['point']) not in SPECIAL_STARTS]
    quiet = [p for p in usable if p.get('event', NO_EVENT) == NO_EVENT]
    candidates = quiet or usable
    plain = [p for p in candidates if p.get('mode') == 0]
    chosen = plain or candidates or points
    return min(p['point'] for p in chosen)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spawns', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    spawns = json.loads(args.spawns.read_text())['stages']
    missing = sorted(set(spawns) - {name for name, _, _ in STAGES})
    if missing:
        raise SystemExit(f'stages without a label: {missing}')
    rooms, stages, eventful = [], [], []
    for name, label, main_tour in STAGES:
        entries = sorted((int(room), start_point(name, int(room), points)) for room, points in spawns[name].items())
        for room, point in entries:
            event = next(p.get('event', NO_EVENT) for p in spawns[name][str(room)] if p['point'] == point)
            if event != NO_EVENT:
                eventful.append(f'{name} r{room} p{point} event {event}')
        if not entries:
            raise SystemExit(f'{name}: no start points')
        if len(name) > 8 or len(label) > 22:
            raise SystemExit(f'{name}: name or label too long')
        primary = [room for room, _ in entries].index(PRIMARY[name]) if name in PRIMARY else 0
        stages.append((name, label, len(rooms), len(entries), primary, main_tour))
        rooms += entries
    lines = ['// Generated by Tools/3ds/generate_zone_table.py from start-point metadata. Do not edit.',
             f'inline constexpr ZoneRoom zoneRooms[{len(rooms)}]={{']
    for at in range(0, len(rooms), 12):
        lines.append('    ' + ''.join(f'{{{room},{point}}},' for room, point in rooms[at:at + 12]))
    lines += ['};', f'inline constexpr ZoneStage zoneStages[{len(stages)}]={{']
    for name, label, first, count, primary, main_tour in stages:
        lines.append(f'    {{"{name}","{label}",{first},{count},{primary},{main_tour}}},')
    lines.append('};')
    args.output.write_text('\n'.join(lines) + '\n')
    print(f'{len(stages)} stages, {len(rooms)} rooms -> {args.output}')
    if eventful:
        # Rooms whose every usable start point begins an arrival event.
        print(f'{len(eventful)} rooms start inside an event (no other point): ' + ', '.join(eventful))


if __name__ == '__main__':
    main()
