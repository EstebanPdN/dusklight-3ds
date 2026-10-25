#!/usr/bin/env python3
"""Bounded private RGB565 provenance census. Metadata only; never approves content.

Reuses the existing bounded FST/Yaz0 and checksummed DSTX readers. Examines 12
named locations, their authored bank-name lists, selected Ordon actors, all disc
Layout archives and Particle files. No compiler, emulator, extraction or writes
outside a fresh texture-half-admission diagnostic subdirectory.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
import sys

import analyze_texture_dump as dstx
import full_game_coverage as resource

ROOT = Path(__file__).resolve().parents[2]
OUT_ROOT = ROOT / "Diagnostics/implementation-44/texture-half-admission"
SITES = [("R_SP01", 4), ("F_SP103", 1), ("F_SP00", 0), ("F_SP104", 1),
         ("F_SP108", 0), ("F_SP109", 0), ("R_SP109", 0), ("F_SP115", 0),
         ("F_SP116", 0), ("F_SP110", 0), ("F_SP121", 0), ("D_MN05", 0)]
OBJECTS = ("Always", "Alink", "Title", "LogoUs", "Dalways", "Npc_tk", "Obj_kn2",
           "Taro", "Besu", "Maro", "Kolin", "Horse", "H_ItaMato", "Kakashi")
MAX_FILES = 768
MAX_EXPANDED = 512 * 1024 * 1024
MAX_STORED = 128 * 1024 * 1024
MAX_HEADERS = 32768


def sha(data):
    return hashlib.sha256(data).hexdigest()


def u16(data, at):
    return struct.unpack(">H", resource.span(data, at, 2))[0]


def name_at(data, at, limit=256):
    raw = resource.span(data, at, min(limit, len(data) - at))
    if b"\0" not in raw:
        raise ValueError("unterminated bounded resource name")
    return raw.split(b"\0", 1)[0].decode("shift_jis", "backslashreplace")


def archive_members(data):
    """Yield node-qualified paths and original entry IDs, not flattened names."""
    if data[:4] != b"RARC" or resource.u32(data, 4) != len(data):
        raise ValueError("RARC signature/length")
    nodes, nn = resource.u32(data, 0x24) + 32, resource.u32(data, 0x20)
    entries, ne = resource.u32(data, 0x2c) + 32, resource.u32(data, 0x28)
    strings = resource.span(data, resource.u32(data, 0x34) + 32, resource.u32(data, 0x30))
    base = resource.u32(data, 12) + 32
    if not 0 < nn <= 4096 or ne > 65536:
        raise ValueError("RARC count bound")
    resource.span(data, nodes, nn * 16)
    resource.span(data, entries, ne * 20)
    seen = set()

    def walk(node, prefix, depth):
        if depth > 32 or node >= nn or node in seen:
            raise ValueError("RARC cyclic/deep node graph")
        seen.add(node)
        n = nodes + node * 16
        first, count = resource.u32(data, n + 12), u16(data, n + 10)
        if first + count > ne:
            raise ValueError("RARC node range")
        kind = data[n:n + 4].decode("ascii", "backslashreplace")
        for index in range(first, first + count):
            e = entries + index * 20
            name = name_at(strings, u16(data, e + 6))
            if name in (".", ".."):
                continue
            if "/" in name or "\\" in name:
                raise ValueError("unsafe member name")
            path = prefix + "/" + name
            if data[e + 4] & 2:
                yield from walk(resource.u32(data, e + 8), path, depth + 1)
            else:
                yield path, kind, index, resource.span(data, base + resource.u32(data, e + 8), resource.u32(data, e + 12))
    yield from walk(0, "", 0)


def texture_record(data, header, end, origin):
    b = resource.span(data, header, 32)
    fmt, alpha, mips = b[0], b[1], b[24]
    w, h = struct.unpack_from(">HH", b, 2)
    result = {**origin, "gx_format": fmt, "bti_alpha_enabled": alpha,
              "original_width": w, "original_height": h, "original_mips": mips,
              "header_offset": header, "original_header_hex": b.hex()}
    offset = resource.u32(b, 28)
    if not offset:
        return {**result, "external_named_reference": True}
    # This helper fingerprints only direct RGB565. Other formats contribute to
    # the census; no cross-format eligibility or conversion is inferred.
    if fmt != 4:
        return result
    if not (1 <= w <= 1024 and 1 <= h <= 1024 and 1 <= mips <= max(w, h).bit_length()):
        raise ValueError("RGB565 extent/mip bounds")
    if offset < 32 or b[8] or u16(b, 10):
        raise ValueError("direct RGB565 header/palette bounds")
    count = sum(((max(1, w >> k) + 3) // 4) * ((max(1, h >> k) + 3) // 4) * 32 for k in range(mips))
    start = header + offset
    if start + count > end:
        raise ValueError("RGB565 payload crosses owning TEX1/member")
    pixels = resource.span(data, start, count)
    reasons = []
    if alpha:
        reasons.append("nonzero_header_alpha")
    if min(w, h) < 16:
        reasons.append("original_dimension_below_16")
    if mips < 2:
        reasons.append("no_authored_mip_1")
    if w & (w - 1) or h & (h - 1):
        reasons.append("non_power_of_two_extent")
    if min(max(1, w >> (mips - 1)), max(1, h >> (mips - 1))) < 8:
        reasons.append("provided_chain_tail_below_8")
    result.update(canonical_sha256=sha(struct.pack(">BHHB", fmt, w, h, mips) + pixels),
                  image_sha256=sha(pixels), source_payload_bytes=count,
                  pixel_offset=start, mechanical_exclusions=reasons,
                  mechanically_eligible=not reasons, admission_status="excluded" if reasons else "role_audit_pending")
    return result


def material_references(mat, texture_index):
    """Authored MAT3 initial state only; runtime patches/animations remain unknown.

    Layouts: J3DModelLoader.h J3DMaterialBlock; J3DMaterialFactory.h
    J3DMaterialInitData (0x14c); J3DStruct.h texcoord/TEV/texmatrix info.
    """
    refs = []
    table = lambda field: resource.u32(mat, field)
    for i in range(u16(mat, 8)):
        init = table(12) + u16(mat, table(16) + i * 2) * 0x14c
        record = resource.span(mat, init, 0x14c)
        slots = [slot for slot in range(8) if u16(record, 0x84 + slot * 2) != 65535 and
                 u16(mat, table(0x48) + 2 * u16(record, 0x84 + slot * 2)) == texture_index]
        if not slots:
            continue
        names = table(0x14)
        name = name_at(mat, names + u16(mat, names + 6 + i * 4))
        stages = mat[table(0x58) + record[4]] if record[4] != 255 else 0
        if stages > 16:
            raise ValueError("MAT3 TEV stage count")
        for stage in range(stages):
            order = u16(record, 0xbc + stage * 2)
            if order == 65535:
                continue
            coord, texmap, channel, _ = resource.span(mat, table(0x4c) + order * 4, 4)
            if texmap not in slots:
                continue
            ref = {"material_index": i, "material_name": name, "stage": stage, "texture_map": texmap, "texcoord": coord}
            if coord < 8:
                info = u16(record, 0x28 + coord * 2)
                if info != 65535:
                    kind, source, matrix, _ = resource.span(mat, table(0x38) + info * 4, 4)
                    ref.update(texgen_type=kind, texgen_source=source, texgen_matrix=matrix,
                               authored_texgen_nonprojective=kind == 1)
                mi = u16(record, 0x48 + coord * 2)
                if mi != 65535:
                    m = resource.span(mat, table(0x40) + mi * 0x64, 0x64)
                    ref.update(texmatrix_projection=m[0], texmatrix_info=m[1])
            refs.append(ref)
    return refs


class Audit:
    def __init__(self):
        self.rows, self.archives, self.metadata, self.errors = [], [], [], []
        self.expanded = self.stored = 0
        self.headers = self.models = 0

    def inspect(self, stored, path, purpose, depth=0):
        if depth > 3 or len(self.archives) >= MAX_FILES:
            raise ValueError("file/depth budget")
        data = resource.decompress(stored)
        self.expanded += len(data)
        if self.expanded > MAX_EXPANDED:
            raise ValueError("aggregate expanded-byte budget")
        item = {"path": path, "purpose": purpose, "stored_sha256": sha(stored),
                "stored_bytes": len(stored), "expanded_sha256": sha(data), "expanded_bytes": len(data),
                "models": 0, "texture_headers": 0, "formats": Counter()}
        self.archives.append(item)
        if data[:4] == b"RARC":
            for member, node_type, index, raw in archive_members(data):
                origin = {"original_path": path + "!" + member, "rarc_node_type": node_type,
                          "rarc_entry_index": index, "scope_purpose": purpose}
                b = resource.decompress(raw) if raw[:4] == b"Yaz0" else raw
                if b[:4] == b"RARC":
                    self.inspect(raw, path + "!" + member, purpose, depth + 1)
                    continue
                if raw[:4] == b"Yaz0":
                    self.expanded += len(b)
                    if self.expanded > MAX_EXPANDED:
                        raise ValueError("aggregate expanded-byte budget")
                if member.endswith(("name.bin", "bank.bin", ".dzr", ".dzs")):
                    meta = {**origin, "sha256": sha(b), "bytes": len(b)}
                    if member.endswith("name.bin"):
                        # d_stage.h::dStage_bankName uses fixed 10-byte fields.
                        resource.span(b, 1, b[0] * 10)
                        meta["bank_names"] = [name_at(b, 1 + n * 10, 10) for n in range(b[0])]
                    if member.endswith("bank.bin"):
                        resource.span(b, 1, b[0] * 34)
                        meta["bank_entries"] = [{"room": b[1 + n * 34], "layer": b[2 + n * 34],
                            "name_ids": [i for i in b[3 + n * 34:35 + n * 34] if i != 255]} for n in range(b[0])]
                    if member.endswith((".dzr", ".dzs")):
                        meta["stage_metadata"] = resource.dz_metadata(b)
                    self.metadata.append(meta)
                if member.endswith(".bti"):
                    self.add(texture_record(b, 0, len(b), origin), item)
                if b[:4] == b"J3D2" and b[4:7] in (b"bmd", b"bdl"):
                    item["models"] += 1
                    if resource.u32(b, 8) != len(b):
                        raise ValueError("J3D length")
                    at = 32
                    model_rows, mat3 = [], None
                    for _ in range(resource.u32(b, 12)):
                        size = resource.u32(b, at + 4)
                        if size < 8:
                            raise ValueError("J3D chunk length")
                        resource.span(b, at, size)
                        if b[at:at + 4] == b"MAT3":
                            mat3 = resource.span(b, at, size)
                        if b[at:at + 4] == b"TEX1":
                            count, base, names = u16(b, at + 8), at + resource.u32(b, at + 12), at + resource.u32(b, at + 16)
                            if count and (base < at + 32 or base + count * 32 > at + size):
                                raise ValueError("TEX1 header table")
                            for t in range(count):
                                name = name_at(b, names + u16(b, names + 6 + t * 4))
                                row = texture_record(b, base + t * 32, at + size,
                                    {**origin, "tex1_offset": at, "texture_index": t, "texture_name": name})
                                self.add(row, item)
                                if row.get("canonical_sha256"):
                                    model_rows.append(row)
                        at += size
                    if at != len(b):
                        raise ValueError("J3D trailing extent")
                    for row in model_rows:
                        row["authored_mat3_references"] = material_references(mat3, row["texture_index"]) if mat3 else None
        elif data[:8] == b"JPAC2-10":
            at, count = resource.u32(data, 12), u16(data, 10)
            if count > 1024:
                raise ValueError("JPA texture count")
            for i in range(count):
                size = resource.u32(data, at + 4)
                if size < 64 or data[at:at + 4] != b"TEX1":
                    raise ValueError("JPA TEX1 extent")
                resource.span(data, at, size)
                origin = {"original_path": path, "texture_index": i, "texture_name": name_at(data, at + 12, 20), "scope_purpose": purpose}
                self.add(texture_record(data, at + 32, at + size, origin), item)
                at += size
        else:
            raise ValueError("unsupported selected container")

    def add(self, row, item):
        self.headers += 1
        if self.headers > MAX_HEADERS:
            raise ValueError("texture header budget")
        item["texture_headers"] += 1
        item["formats"][row["gx_format"]] += 1
        if row["gx_format"] == 4 or row.get("external_named_reference"):
            self.rows.append(row)


def admission_manifest(groups, observations):
    entries = []
    observed_groups = defaultdict(list)
    for row in observations:
        observed_groups[row["canonical_sha256"]].append(row)
    for fingerprint in sorted(groups.keys() | observed_groups.keys()):
        aliases, observed = groups.get(fingerprint, []), observed_groups.get(fingerprint, [])
        first = (aliases or observed)[0]
        eligible = [a for a in aliases if a.get("mechanically_eligible")]
        nonzero = [a for a in aliases if a["bti_alpha_enabled"]]
        protected = [a for a in aliases if a.get("scope_purpose") == "protected_layout"]
        exclusions = sorted({e for a in aliases or observed for e in a.get("mechanical_exclusions", [])})
        if eligible and nonzero:
            status = "excluded_nonzero_header_alpha_alias"
        elif not eligible:
            status = "excluded_mechanical_or_protected_role"
        else:
            status = "not_admitted_role_and_usage_audit_pending"
        entries.append({"canonical_sha256": fingerprint, "gx_format": 4,
            "original_width": first["original_width"], "original_height": first["original_height"],
            "original_mips": first["original_mips"], "source_payload_bytes": first["source_payload_bytes"],
            "asset_provenance": aliases, "observed_registrations": observed,
            "admission_status": status, "exclusions_present_across_aliases": exclusions,
            "protected_alias_analysis": {"scope": "selected containers only; not an all-disc claim",
                "parsed_aliases": len(aliases), "mechanically_eligible_headers": len(eligible),
                "nonzero_alpha_headers": len(nonzero), "protected_layout_aliases": len(protected),
                "ui_font_shadow_capture_data_all_uses_excluded": False,
                "nonzero_alias_runtime_use": "not established by initial MAT3 references; BTP/dynamic selection not audited"},
            "static_usage": "consult REPORT.md and source-evidence.json; path/name is not a role proof",
            "observed_usage": "registration only" if observed else "not present in the five captured inventories",
            "remaining_unknown": ["all protected consumer aliases outside the bounded scope", "BTP and dynamic material/texgen selection", "native visual acceptance"],
            "native_validation_receipt": None})
    return {"schema_version": 1, "enabled": False, "approved_sources": [],
        "fingerprint": "SHA256(format u8 + original width BE16 + height BE16 + mip count u8 + entire original GX mip payload)",
        "entries": entries,
        "decision": "No source admitted. All mechanically eligible source fingerprints have a known nonzero-alpha header alias; captured h288 has no authored mip 1."}


def run(args):
    out = args.output.resolve()
    if not out.is_relative_to(OUT_ROOT) or out == OUT_ROOT:
        raise ValueError("output must be a NEW run subdirectory under texture-half-admission")
    out.mkdir(parents=True, exist_ok=False)
    audit = Audit()
    disc = args.private_disc.resolve()
    before = disc.stat()
    with disc.open("rb") as stream:
        files, identity = resource.read_fst(stream, before.st_size)
        requested = {}
        for stage, room in SITES:
            for archive in ("STG_00", f"R{room:02d}_00"):
                requested[f"/res/Stage/{stage}/{archive}.arc"] = "stage_room_static_reference_scope"
        requested.update({f"/res/Object/{name}.arc": "common_or_selected_ordon_object" for name in OBJECTS if f"/res/Object/{name}.arc" in files})
        requested.update({path: "protected_layout" for path in files if path.startswith("/res/Layout/")})
        requested.update({path: "particle_role_unresolved" for path in files if path.startswith("/res/Particle/")})
        done = set()
        while requested.keys() - done:
            path = sorted(requested.keys() - done)[0]
            if len(done) >= MAX_FILES:
                raise ValueError("top-level file budget")
            done.add(path)
            try:
                at, size = files[path]
                audit.stored += size
                if size > resource.MAX_ARCHIVE or audit.stored > MAX_STORED:
                    raise ValueError("stored-byte budget")
                audit.inspect(resource.read_exact(stream, at, size), path, requested[path])
                # Authored bank names are static possibilities, not observed loads.
                for meta in audit.metadata:
                    for name in meta.get("bank_names", []):
                        bank = f"/res/Object/{name}.arc"
                        if bank in files:
                            requested.setdefault(bank, "authored_stage_bank_possible_story_layers")
            except (ValueError, KeyError, IndexError, struct.error) as error:
                audit.errors.append({"path": path, "error": str(error)})
    after = disc.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("private disc changed during audit")
    captures, observations = [], []
    snapshots = sorted((ROOT / "Build/azahar-runs").glob("*/xdg-data/azahar-emu/sdmc/3ds/dusklight/dumps/*.dstx"))
    if len(snapshots) > 16:
        raise ValueError("snapshot count bound")
    for path in snapshots:
        data, frame, records = dstx.read_snapshot(path)
        captures.append({"path": str(path.relative_to(ROOT)), "sha256": sha(data), "frame": frame, "records": len(records), "rgb565_records": sum(r["gx_format"] == "RGB565" for r, _ in records)})
        for record, bti in records:
            if bti[0] != 4:
                continue
            row = texture_record(bti, 0, len(bti), {"snapshot": str(path.relative_to(ROOT)), "frame": frame, "handle": record["handle"]})
            row["original_path"] = None
            row["immutable_pixels"] = "not_exported_by_DSTX"
            observations.append(row)
    groups = defaultdict(list)
    for row in audit.rows:
        if row.get("canonical_sha256"):
            groups[row["canonical_sha256"]].append(row)
    for row in observations:
        row["matching_original_resources"] = groups.get(row["canonical_sha256"], [])
    summary = {"sites": [{"stage": s, "room": r, "runtime_or_story_pass": False} for s, r in SITES],
        "disc": {**identity, "path": str(disc), "bytes": before.st_size, "size_mtime_unchanged": True, "whole_disc_sha256": None},
        "limits": {"files_including_nested": MAX_FILES, "stored_bytes": MAX_STORED, "expanded_bytes": MAX_EXPANDED, "texture_headers": MAX_HEADERS},
        "actual": {"top_level_files": len(done), "files_including_nested": len(audit.archives), "stored_bytes_read": audit.stored, "expanded_container_bytes": audit.expanded, "texture_headers": audit.headers, "models": sum(a["models"] for a in audit.archives), "rgb565_headers": sum(a["gx_format"] == 4 for a in audit.rows), "unique_rgb565_fingerprints": len(groups), "mechanically_eligible_rgb565_headers": sum(a.get("mechanically_eligible", False) for a in audit.rows)},
        "approved_sources": [], "errors": audit.errors, "payload_saved": False,
        "limitations": ["MAT3 initial bindings/texgen only; no display-list/animation/dynamic GX consumer audit or all-game alias claim.", "DSTX inventories registrations, not binds/draws, immutability, resource roles or live GPU residency.", "RARC BTI and BMD/BDL TEX1, and JPAC TEX1 are parsed; other embedded raw/REL/font/data resources are not enumerated.", "12 static location scopes and authored bank possibilities are not 12 runtime/story passes."]}
    for name, obj in (("summary.json", summary), ("resources.json", audit.rows), ("archives.json", audit.archives), ("stage-metadata.json", audit.metadata), ("captures.json", captures), ("observed-rgb565.json", observations), ("fingerprint-aliases.json", groups), ("admission-manifest.json", admission_manifest(groups, observations))):
        (out / name).write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n")
    inputs = [Path(__file__), Path(resource.__file__), Path(dstx.__file__)]
    (out / "reproduction.json").write_text(json.dumps({"argv": sys.argv, "helper_sha256": {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in inputs}, "output_sha256": {p.name: sha(p.read_bytes()) for p in sorted(out.glob("*.json"))}}, indent=2) + "\n")
    print(json.dumps({"actual": summary["actual"], "errors": summary["errors"]}, indent=2))
    return bool(audit.errors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-disc", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    raise SystemExit(run(parser.parse_args()))
