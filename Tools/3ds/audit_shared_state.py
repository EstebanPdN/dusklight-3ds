#!/usr/bin/env python3
"""Which variables can the game thread and the second-core worker both reach?

The second-core worker (Source/platforms/3ds/runtime/gx_worker.hpp) draws
queued shapes while the game thread goes on issuing the picture. That is
only equal to drawing everything on one thread if no code of the game thread
reads or changes, outside the register bank, something a queued draw reads or
changes when it is drawn. Three bugs of that kind were found by their
pictures (the capture state in GXCopyTex, the shadow caster list, the "frame
copy made" question of the particles); this tool lists every candidate from
the build itself, so the rule is checked by reading a list and not by luck.

It reads the ARM object files of a build (compiled with -ffunction-sections
-fdata-sections, no LTO: one section per function and per variable), and
builds the reference graph from their relocations: function -> function
(calls and taken addresses), function -> variable, table -> function.

  consumer side: everything reachable from the functions that draw a queued
                 shape (--consumer, plus callbacks the draw code calls
                 through pointers: --consumer too);
  game side:     everything reachable from the platform functions that game
                 code references, without going through a function that
                 syncs with the worker before anything else (--barrier).
  Neither side follows the functions that end the program (--stop).

Printed: the writable variables both sides reach, each with one call chain
per side. A variable on the list is not a bug by itself (a counter, a value
only read, a call that syncs first inside the function): each one is judged
by reading, and the verdicts are kept in the judgement file so that the next
run prints only what is new (--judged FILE: lines "symbol-regex<TAB>reason").

  python3 -I Tools/3ds/audit_shared_state.py Build/hardware-20261113/build \
      --judged Tools/3ds/shared_state_judged.tsv

Exit status 1 when a variable is on the list without a judgement.
"""
import argparse
import collections
import pathlib
import re
import struct
import subprocess
import sys

SHT_SYMTAB, SHT_REL, SHT_NOBITS = 2, 9, 8
SHF_WRITE, SHF_ALLOC, SHF_EXEC, SHF_TLS = 1, 2, 4, 0x400
CALLS = {10, 27, 28, 29, 30}  # R_ARM_THM_CALL, PLT32, CALL, JUMP24, THM_JUMP24


class Obj:
    """Sections, symbols and relocations of one ELF32 little-endian object."""

    def __init__(self, path):
        self.path = path
        data = path.read_bytes()
        if data[:6] != b"\x7fELF\x01\x01":
            raise ValueError(f"{path}: not an ELF32 little-endian object")
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
        heads = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
        names = heads[shstrndx]

        def text(table_offset, at):
            end = data.index(b"\0", table_offset + at)
            return data[table_offset + at:end].decode("latin-1")

        self.sections = [(text(names[4], h[0]), h[1], h[2]) for h in heads]  # name, type, flags
        self.symbols = []  # name, shndx, bind, type
        self.relocs = collections.defaultdict(list)  # section index -> (symbol index, is a call)
        for h in heads:
            if h[1] == SHT_SYMTAB:
                strings = heads[h[6]][4]
                for at in range(h[4], h[4] + h[5], 16):
                    name, _value, _size, info, _other, shndx = struct.unpack_from("<IIIBBH", data, at)
                    self.symbols.append((text(strings, name), shndx, info >> 4, info & 15))
        for h in heads:
            if h[1] == SHT_REL:
                target = h[7]
                name, _type, flags = self.sections[target]
                if not flags & SHF_ALLOC or name.startswith((".ARM.exidx", ".ARM.extab")):
                    continue
                for at in range(h[4], h[4] + h[5], 8):
                    _offset, info = struct.unpack_from("<II", data, at)
                    self.relocs[target].append((info >> 8, info & 255 in CALLS))


def kind_of(flags):
    if flags & SHF_EXEC:
        return "code"
    if flags & SHF_TLS:
        return "tls"
    if flags & SHF_WRITE:
        return "data"
    return "const"


def load(build):
    objs = [Obj(p) for p in sorted(pathlib.Path(build, "CMakeFiles").rglob("*.obj"))]
    if not objs:
        sys.exit(f"no object files under {build}/CMakeFiles")
    # Global and weak symbols name one thing in the whole program (the linker
    # keeps one copy of an inline function or variable).
    where = {}
    for oi, obj in enumerate(objs):
        for name, shndx, bind, _type in obj.symbols:
            if bind and 0 < shndx < 0xff00 and name:
                if name not in where or (bind == 1 and where[name][2] != 1):
                    where[name] = (oi, shndx, bind)
    canon = {}
    for oi, obj in enumerate(objs):
        for name, shndx, bind, _type in obj.symbols:
            if bind and 0 < shndx < 0xff00 and name:
                canon.setdefault((oi, shndx), where[name][:2])
    label = {}
    for oi, obj in enumerate(objs):
        for name, shndx, bind, stype in obj.symbols:
            if 0 < shndx < 0xff00 and name and stype in (1, 2, 6) and not name.startswith("$"):
                node = canon.get((oi, shndx), (oi, shndx))
                if node not in label or bind:
                    label[node] = name
    edges = collections.defaultdict(set)
    taken = collections.defaultdict(set)  # function -> who refers to it other than by calling it
    external = collections.defaultdict(set)  # node -> names defined outside the build (libraries)
    for oi, obj in enumerate(objs):
        for section, symbols in obj.relocs.items():
            if canon.get((oi, section), (oi, section)) != (oi, section):
                continue  # a duplicate of an inline function: the kept copy is read instead
            source = (oi, section)
            for index, call in symbols:
                name, shndx, bind, _type = obj.symbols[index]
                if bind and name in where:
                    target = where[name][:2]
                elif bind:
                    external[source].add(name)
                    continue
                elif 0 < shndx < 0xff00:
                    target = canon.get((oi, shndx), (oi, shndx))
                else:
                    continue
                edges[source].add(target)
                if not call and target != source:
                    taken[target].add(source)
    return objs, where, label, edges, taken, external


def demangle(names, tool):
    names = list(names)
    try:
        out = subprocess.run([tool], input="\n".join(names), capture_output=True, text=True, check=True).stdout.split("\n")
    except (OSError, subprocess.CalledProcessError) as error:
        # The roots and the verdicts are written against demangled names.
        sys.exit(f"cannot demangle with {tool!r}: {error} (pass --cxxfilt)")
    return dict(zip(names, out))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("build", help="build directory (holds CMakeFiles/*/…/*.obj)")
    parser.add_argument("--consumer", action="append", default=[], help="regex (demangled) of a function the worker runs")
    parser.add_argument("--barrier", action="append", default=[], help="regex (demangled) of a function that syncs first")
    parser.add_argument("--game", action="append", default=[], help="regex (demangled) of a platform function the game thread reaches through a pointer")
    parser.add_argument("--stop", action="append", default=[], help="regex (demangled) of a function followed on neither side (the fatal error path)")
    parser.add_argument("--roots", help="file with lines 'consumer<TAB>regex', 'game<TAB>regex', 'barrier<TAB>regex<TAB>why', 'stop<TAB>regex<TAB>why'")
    parser.add_argument("--judged", help="file with lines 'symbol-regex<TAB>reason'")
    parser.add_argument("--game-path", default="/Source/", help="object path part that marks game code (default /Source/)")
    parser.add_argument("--platform-path", default="/platforms/3ds/", help="... and what is platform code among those")
    parser.add_argument("--all", action="store_true", help="print the judged variables too")
    parser.add_argument("--entries", type=int, default=3, help="game-side entry points shown per variable")
    parser.add_argument("--taken", action="store_true", help="list platform functions whose address is taken (callback candidates)")
    parser.add_argument("--cxxfilt", default="c++filt", help="demangler (default c++filt; the toolchain's arm-none-eabi-c++filt in the build)")
    parser.add_argument("--quiet", action="store_true", help="print only what has no judgement, and one closing line (the build's check)")
    parser.add_argument("--game-data", action="store_true", help="list the game's own variables the worker side reaches (any game code may write them)")
    args = parser.parse_args()

    patterns = {"consumer": list(args.consumer), "barrier": list(args.barrier), "game": list(args.game), "stop": list(args.stop)}
    if args.roots:
        for number, line in enumerate(pathlib.Path(args.roots).read_text().splitlines(), 1):
            parts = line.split("\t")
            if line.startswith("#") or not line.strip():
                continue
            if len(parts) < 2 or parts[0] not in patterns:
                sys.exit(f"{args.roots}:{number}: expected consumer|game|barrier|stop<TAB>regex")
            patterns[parts[0]].append(parts[1])
    objs, where, label, edges, taken, external = load(args.build)

    def node_name(node):
        oi, section = node
        return label.get(node) or f"{objs[oi].sections[section][0]} [{objs[oi].path.name}]"

    def node_kind(node):
        return kind_of(objs[node[0]].sections[node[1]][2])

    def is_game(oi):
        path = str(objs[oi].path)
        return args.game_path in path and args.platform_path not in path

    nodes = set(edges)
    for targets in edges.values():
        nodes |= targets
    pretty = demangle({node_name(n) for n in nodes}, args.cxxfilt)

    def show(node):
        return pretty.get(node_name(node), node_name(node))

    stale = []

    def matching(patterns):
        found = set()
        for pattern in patterns:
            rx = re.compile(pattern)
            hits = {n for n in nodes if node_kind(n) == "code" and rx.search(show(n))}
            if not hits:
                stale.append(pattern)
            found |= hits
        return found

    def reach(roots, blocked=frozenset()):
        parent = {r: None for r in roots}
        queue = collections.deque(roots)
        while queue:
            node = queue.popleft()
            for target in edges.get(node, ()):
                if target not in parent and target not in blocked:
                    parent[target] = node
                    queue.append(target)
        return parent

    def chain(parent, node, limit=7):
        out = []
        while node is not None:
            out.append(show(node))
            node = parent[node]
        out.reverse()
        if len(out) > limit:
            out = out[:3] + ["…"] + out[-(limit - 4):]
        return " -> ".join(out)

    if args.taken:
        # Platform functions something refers to without calling them are
        # reached through pointers: candidates for --consumer.
        for target in sorted(taken, key=show):
            if node_kind(target) == "code" and not is_game(target[0]):
                print(f"{show(target)}\t<- {'; '.join(sorted(show(s) for s in taken[target])[:3])}")
        return 0

    stops = matching(patterns["stop"])
    consumer_roots = matching(patterns["consumer"])
    barriers = matching(patterns["barrier"]) | stops
    game_roots = matching(patterns["game"])
    if stale:
        # A root or a barrier that names nothing was renamed or removed: the
        # audit would silently look at less (or more) than it was told to.
        sys.exit("no function matches: " + "; ".join(stale) + f" (update {args.roots})")
    consumer = reach(consumer_roots, stops)
    # Game side: the platform functions and the platform variables that game
    # code refers to (a variable read straight from game code counts: the
    # particles asked a platform counter whether the frame copy was made).
    entries = set()
    referrer = {}
    for source, targets in edges.items():
        if is_game(source[0]):
            for target in targets:
                if not is_game(target[0]) and node_kind(target) in ("code", "data"):
                    entries.add(target)
                    referrer.setdefault(target, source)
    entries |= game_roots
    entries -= barriers
    game = reach(entries, barriers)
    shared = sorted((n for n in consumer if n in game and node_kind(n) == "data"), key=show)
    if args.game_data:
        # Platform code that draws calls a few game functions (the heaps):
        # what they touch is the game's, written from anywhere in it.
        for node in sorted((n for n in consumer if node_kind(n) == "data" and is_game(n[0])), key=show):
            print(f"{show(node)}   [{objs[node[0]].path.name}]\n  worker: {chain(consumer, node)}")
        return 0

    judged = []
    if args.judged and pathlib.Path(args.judged).exists():
        for line in pathlib.Path(args.judged).read_text().splitlines():
            parts = line.split("\t")
            if not line.startswith("#") and len(parts) >= 2:
                judged.append((re.compile(parts[0]), parts[1]))

    # For the entry list of a variable: which entries reach it at all.
    reaching = collections.defaultdict(list)
    if shared:
        wanted = set(shared)
        for entry in sorted(entries, key=show):
            for node in reach([entry], barriers):
                if node in wanted and len(reaching[node]) < args.entries:
                    reaching[node].append(entry)
    unjudged = 0
    if not args.quiet:
        print(f"objects {len(objs)}, consumer roots {len(consumer_roots)}, game entries {len(entries)}, barriers {len(barriers)}")
        print(f"writable variables reached by both sides: {len(shared)}")
    for node in shared:
        name = show(node)
        # A verdict regex may name the object too ("symbol [file.cpp.obj]"):
        # file-local names (an anonymous-namespace `active`) repeat across files.
        where = f"{name} [{objs[node[0]].path.name}]"
        verdict = next((why for rx, why in judged if rx.search(name) or rx.search(where)), None)
        if verdict and not args.all:
            continue
        unjudged += verdict is None
        print(f"\n{name}   [{objs[node[0]].path.name}]" + (f"   judged: {verdict}" if verdict else ""))
        print(f"  worker: {chain(consumer, node)}")
        for entry in reaching[node]:
            local = reach([entry], barriers)
            origin = f"{show(referrer[entry])} [game code] -> " if entry in referrer and node_kind(entry) == "data" else ""
            print(f"  game:   {origin}{chain(local, node)}")
    if args.quiet:
        print(f"shared-state audit: {len(shared)} variables reached by the game thread and by the drawing side, {unjudged} without a judgement"
              + (f" (add each to {args.judged} once both call chains are read)" if unjudged else ""))
    else:
        print(f"\nwithout a judgement: {unjudged}")
    return 1 if unjudged else 0


if __name__ == "__main__":
    sys.exit(main())
