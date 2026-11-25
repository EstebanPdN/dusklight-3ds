"""worldMips register check: ties the texture cache in a dump's RAM to the
texture registers of the last complete picture. For every draw and enabled
unit: the image bound (cache entry by GPU address), whether it carries
generated levels, and the max level and mip filter its registers really hold.
A bind counter cannot show a draw path that binds the base level (Alpha 21:
the worker's record replays did, and every count looked right); the
registers can. Exit 1 when images with generated levels are bound in that
picture and fewer than 90 % of their draw-units sample them.
usage: mip_register_check.py <extracted dump> <the build's elf>
(take the dump with features.txt mips=1; verify_application_dump.py --extract)"""
import struct, subprocess, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gpu_jobs_report as g

NM = Path.home() / 'devkitpro/devkitARM/bin/arm-none-eabi-nm'
ENTRY = 160  # sizeof(CachedTexture), checked against the symbol size


def symbol(elf, name):
    for line in subprocess.run([str(NM), '-C', '-S', str(elf)], capture_output=True, text=True).stdout.splitlines():
        parts = line.split(' ', 3)
        if len(parts) == 4 and parts[3] == name:
            return int(parts[0], 16), int(parts[1], 16)
    raise SystemExit(f'no symbol {name}')


def main():
    d, elf = Path(sys.argv[1]), Path(sys.argv[2])
    base, size = symbol(elf, 'dusk::native::(anonymous namespace)::cache')
    assert size == 256 * ENTRY, size
    blob = None
    for f in d.glob('ram-*.bin'):
        lo, n = (int(x, 16) for x in f.stem.split('-')[1:3])
        if lo <= base < lo + n: blob = f.read_bytes()[base - lo:base - lo + size]
    entries = {}
    for i in range(256):
        e = blob[i * ENTRY:(i + 1) * ENTRY]
        handle, = struct.unpack_from('<Q', e, 0)
        if not handle: continue
        data, = struct.unpack_from('<I', e, 8)
        w, h, pw, ph = struct.unpack_from('<4I', e, 32)
        mips, orig, source = e[62], e[80], e[81]
        world = e[100]
        physical = 0x20000000 + data - 0x14000000 if 0x14000000 <= data < 0x1C000000 else data
        entries[physical] = dict(w=w, h=h, mips=mips, generated=source == 0 and mips > orig, authored=orig > 1, world=world)
    memory = g.Memory(d)
    picture = g.last_picture(g.jobs(d / 'gpu-jobs.bin'))
    state = g.State(); lists = [j for j in picture if j['type'] == 1]
    for job in lists:
        try: g.describe_list(memory, state, job)
        except Exception: pass
    hist = Counter()
    for job in lists:
        address, size = job['words'][1], job['words'][2]
        for register, value, mask in g.commands(memory.words(address, size // 4)):
            if not state.write(register, value, mask): continue
            enabled = state.get(0x80)
            for unit, (param, addr, lod) in enumerate(((0x83, 0x85, 0x84), (0x93, 0x95, 0x94), (0x9b, 0x9d, 0x9c))):
                if not enabled >> unit & 1: continue
                e = entries.get(state.get(addr) << 3)
                kind = 'not in cache' if e is None else 'generated' if e['generated'] else 'authored' if e['authored'] else 'one level'
                hist[(kind, bool(e and e['world']), (state.get(lod) >> 16) & 0xf > 0, bool(state.get(param) >> 24 & 1))] += 1
    print(f'{d}: {len(entries)} cache entries, {sum(1 for e in entries.values() if e["generated"])} with generated levels')
    print('  draw-units by image kind, bound by a world use, register max level > 0, PARAM mip filter bit:')
    for k, n in sorted(hist.items(), key=lambda kv: -kv[1]):
        print(f'  {n:5d}  {k[0]:13s} world={int(k[1])} maxLevel>0={int(k[2])} mipFilter={int(k[3])}')
    generated = sum(n for k, n in hist.items() if k[0] == 'generated')
    sampled = sum(n for k, n in hist.items() if k[0] == 'generated' and k[2] and k[3])
    print(f'images with generated levels: {sampled} of {generated} draw-units sample them')
    return 1 if generated and sampled * 10 < generated * 9 else 0


sys.exit(main())
