#!/usr/bin/env python3
"""Validate DLPC001 and report exact P3D jobs and ordered per-draw state."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SYSCLOCK_ARM11 = 268111856
KINDS = {0: "dma", 1: "list", 2: "fill", 3: "transfer", 4: "copy", 5: "flush"}
DRAW = {0x22E: "arrays", 0x22F: "indexed"}
TEX = ([0x82, 0x83, 0x84, 0x85, 0x8E], [0x92, 0x93, 0x94, 0x95, 0x96],
       [0x9A, 0x9B, 0x9C, 0x9D, 0x9E])
TEV = (0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8)
P3D = 5

def require(condition, message):
    if not condition:
        raise ValueError(message)

class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def read(self, count):
        end = self.at + count
        require(end <= len(self.data), "truncated input")
        result = self.data[self.at:end]
        self.at = end
        return result

    def u32(self, count):
        return struct.unpack(f"<{count}I", self.read(count * 4))

def u64(lo, hi):
    return lo | hi << 32

MASKS = tuple(sum(255 << (i * 8) for i in range(4) if mask & (1 << i))
              for mask in range(16))

class Pica:
    def __init__(self, registers, uniform_words):
        self.registers = list(registers)
        self.uniforms = list(uniform_words)
        self.float_packet = []

    def describe(self, trigger, offset):
        r = self.registers
        index = r[0x227]
        stride = r[0x205] >> 16 & 255
        textures = []
        for unit, regs in enumerate(TEX):
            if (r[0x80] >> unit) & 1:
                dim, param, lod, addr, fmt = [r[k] for k in regs]
                textures.append([unit, dim >> 16 & 2047, dim & 2047, fmt & 15, addr << 3, lod, param])
        return {
            "offset_bytes": offset, "draw": DRAW[trigger], "count": r[0x228], "cull": r[0x40],
            "primitive": r[0x25E] >> 8 & 3, "vertex_stride": stride,
            "vertex_addr": (r[0x200] << 3) + (r[0x203] & 0xfffffff),
            "index_addr": (r[0x200] << 3) + (index & 0xfffffff) if trigger == 0x22F else None,
            "index_width": 16 if index >> 31 else 8, "tex": textures,
            "targets": [r[0x11D] << 3, r[0x11C] << 3, r[0x11E]],
            "depth_color_mask": r[0x107], "alpha_test": r[0x104],
            "early_depth": [r[0x62], r[0x118]], "stencil": r[0x105],
            "scissor": r[0x65:0x68], "color_op": r[0x100:0x104],
            "texenv": [r[base:base+5] for base in TEV],
            "attributes": r[0x201:0x206] + r[0x2BB:0x2BD],
            "program_entry": r[0x2BA] & 0xffff, "b0": r[0x2B0] & 1,
            "raw_uniform_bank_sha256": hashlib.sha256(struct.pack("<384I", *self.uniforms)).hexdigest(),
        }

    def parse(self, raw):
        words = Reader(raw).u32(len(raw) // 4)
        i = 0
        draws = []
        finalized = False
        while i < len(words):
            require(i + 2 <= len(words), "short packet header")
            parameter, header = words[i:i+2]
            reg, mask, extra = header & 65535, MASKS[header >> 16 & 15], header >> 20 & 255
            end = i + 2 + extra
            require(end + (end & 1) <= len(words), "short packet or padding")
            for k, parameter in enumerate((parameter, *words[i+2:end])):
                addr = reg + k if header >> 31 else reg
                require(addr < len(self.registers), f"out of range register {addr:#x}")
                value = self.registers[addr] & ~mask | parameter & mask
                self.registers[addr] = value
                # Reject command-buffer jumps instead of reporting an incomplete trace.
                require(not (mask and addr in (0x23c, 0x23d)),
                        f"external command buffer jump at {i*4:#x}")
                if addr == 0x2C0 and mask:
                    self.float_index, self.f32 = value & 255, bool(value >> 31)
                    self.float_packet.clear()
                elif 0x2C1 <= addr <= 0x2C8 and mask:
                    require(hasattr(self, "float_index"), "uniform stream begins mid-packet")
                    self.float_packet.append(value)
                    if len(self.float_packet) == (4 if self.f32 else 3):
                        # The canary snapshot stores upload-order words, without unpacking f24.
                        at = self.float_index * 4
                        require(at + 4 <= len(self.uniforms), "uniform index exceeds bank")
                        self.uniforms[at:at+len(self.float_packet)] = self.float_packet
                        self.float_index += 1
                        self.float_packet.clear()
                if addr in DRAW and parameter & mask & 1:
                    draws.append(self.describe(addr, i * 4 if not k else (i + k + 1) * 4))
                finalized |= addr == 0x10 and bool(parameter & mask)
            i = end + (end & 1)
        require(finalized, "list has no finalizer")
        return draws

def decode(data, *, retain_words=False):
    reader = Reader(data)
    require(reader.read(8) == b"DLPC001\0", "wrong capture format")
    flo, fhi, slo, shi, room, first, jobs, lists, regs, uniforms = reader.u32(10)
    stage = reader.read(16).split(b"\0", 1)[0].decode("ascii")
    require(0 < lists <= jobs <= 512, "invalid capture counts")
    require((regs, uniforms) == (0x300, 384), "PICA initial state unavailable")
    pica = Pica(reader.u32(regs), reader.u32(uniforms))
    ring = [reader.u32(14) for _ in range(jobs)]
    require(ring[0][0] & 255 == 2 and ring[-1][0] & 255 == 3, "missing fill or display boundary")
    count = 0
    result = []
    previous = 0
    for job_id, words in enumerate(ring, first):
        start, complete = u64(*words[8:10]), u64(*words[10:12])
        require(0 < start <= complete and start >= previous and not words[13],
                f"job {job_id}: unordered, incomplete or assumed completion")
        previous = complete
        kind = KINDS.get(words[0] & 255, "unknown")
        row = {"job_id": job_id, "kind": kind, "submitted": start,
               "completed": complete, "milliseconds": (complete-start)*1000/SYSCLOCK_ARM11,
               "gsp": words[:8], "event": words[12]}
        if kind == "list":
            address, size = reader.u32(2)
            require(count < lists and words[12] == P3D and (address, size) == words[1:3],
                    f"job {job_id}: list does not match completed P3D record")
            require(size > 0 and not size % 16, "invalid command list size")
            payload = reader.read(size)
            row.update(bytes=size, sha256=hashlib.sha256(payload).hexdigest(), draws=pica.parse(payload))
            if retain_words:
                row["payload"] = payload
            count += 1
        result.append(row)
    require(count == lists and reader.at == len(data), "missing or extra command lists")
    return {"frame": u64(flo, fhi), "submit_tick": u64(slo, shi), "stage": stage,
            "room": room, "jobs": result}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    capture = decode(args.capture.read_bytes())
    if args.json:
        print(json.dumps(capture, indent=2))
        return
    print(f'{capture["stage"]}/{capture["room"]} picture {capture["frame"]}')
    for job in capture["jobs"]:
        line = f'  {job["job_id"]:6} {job["kind"]:9} {job["milliseconds"]:7.3f} ms'
        if job["kind"] == "list":
            draws = job["draws"]
            count = sum(draw["count"] for draw in draws)
            line += f'  {job["bytes"]:6} bytes {len(draws):4} draws {count:7} vertices/indices'
        print(line)

if __name__ == "__main__":
    main()
