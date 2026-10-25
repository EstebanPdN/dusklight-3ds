#!/usr/bin/env python3
"""Private metadata inventory and conservative full-game evidence ledger.

No emulator, compiler, asset extraction, or process control. Disc archives are
decoded in memory and only inventory metadata is serialized. Coverage never
infers gameplay, story, audible output, or performance from boot/exit status.
See Diagnostics/implementation-44/full-game-coverage/REPORT.md for the contract.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
MAX_ARCHIVE = 64 * 1024 * 1024
LAYER_PREFIXES = {"ACT", "SCO", "TRE", "Doo", "SON"}
DIMENSIONS = {
    "smoke": ("entry_observed", "guest_completed", "visible_reviewed"),
    "story_state": ("checkpoint_identified", "flags_items_form_verified"),
    "progression": ("objective_before_after", "next_checkpoint_reached"),
    "control": ("guest_input_observed", "movement_camera_collision_reviewed"),
    "menu": ("open_navigate_close", "gameplay_resumed", "allocation_faults_checked"),
    "audio": ("music_effects_menu_heard", "output_capture_reviewed", "pause_resume_shutdown", "underruns_checked"),
    "graphics": ("fixed_frame_comparison_reviewed", "render_errors_checked", "degradation_reviewed"),
    "reload": ("save_written", "cold_reload", "state_integrity_verified"),
    "transitions": ("source_destination_verified", "streaming_teardown_verified"),
    "soak": ("route_duration_recorded", "repeated_transitions", "memory_trend_reviewed", "audio_stability_reviewed"),
    "performance": ("physical_new3ds", "frame_time_distribution", "cpu_gpu_ram_measured", "target_policy_met"),
}
STORY_DIMENSIONS = {"story_state", "progression", "reload", "transitions"}
FAILURE_RE = re.compile(
    r"fatal:|OSPanic|\bPANIC\b|unmapped\s+(?:read|write)|UnmappedAccess|Native draw rejected:|"
    r"movie.*(?:desync|mismatch)|playback.*mismatch", re.I)
RESOURCE_RE = re.compile(r"allocation\s+(?:failed|failure)|Scene2DHeap.*(?:fail|request)", re.I)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def reference(path):
    path = Path(path).resolve()
    try:
        name = str(path.relative_to(ROOT))
    except ValueError:
        name = str(path)
    return {"path": name, "sha256": file_hash(path), "bytes": path.stat().st_size}


def write_json(path, value):
    path = Path(path).resolve()
    if path.is_relative_to(ROOT / "Source") or ".git" in path.parts:
        raise ValueError("private coverage output must remain outside Source/Git")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def span(data, at, size):
    if at < 0 or size < 0 or at > len(data) or size > len(data) - at:
        raise ValueError("metadata span outside input")
    return data[at:at + size]


def u32(data, at):
    return struct.unpack(">I", span(data, at, 4))[0]


def cstring(data, at):
    if not 0 <= at < len(data):
        raise ValueError("string outside metadata")
    end = data.find(b"\0", at)
    if end < 0:
        raise ValueError("unterminated metadata string")
    return data[at:end].decode("ascii")


def read_exact(stream, at, size):
    stream.seek(at)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("truncated disc input")
    return data


def read_fst(stream, disc_size):
    header = read_exact(stream, 0, 0x440)
    if u32(header, 0x1C) != 0xC2339F3D:
        raise ValueError("not a GameCube disc header")
    at, size = struct.unpack_from(">II", header, 0x424)
    if size > 4 * 1024 * 1024 or at + size > disc_size:
        raise ValueError("FST outside bound")
    fst = read_exact(stream, at, size)
    count = u32(fst, 8)
    if not count or count * 12 > len(fst) or fst[0] != 1:
        raise ValueError("invalid FST root")
    names = fst[count * 12:]
    files, stack = {}, [(count, "")]
    for index in range(1, count):
        while stack and index >= stack[-1][0]:
            stack.pop()
        if not stack:
            raise ValueError("invalid FST directory range")
        word, offset, length = struct.unpack_from(">III", fst, index * 12)
        flag = word >> 24
        name = cstring(names, word & 0xFFFFFF)
        if not name or name in (".", "..") or "/" in name:
            raise ValueError("invalid FST name")
        path = stack[-1][1] + "/" + name
        if flag == 1:
            if not index < length <= stack[-1][0]:
                raise ValueError("invalid FST child range")
            stack.append((length, path))
        elif flag == 0:
            if offset + length > disc_size or path in files:
                raise ValueError("invalid FST file extent/duplicate")
            files[path] = (offset, length)
        else:
            raise ValueError("invalid FST entry kind")
    return files, {"disc_id": header[:6].decode("ascii"), "revision": header[7],
                   "fst_offset": at, "fst_bytes": size, "fst_sha256": digest(fst),
                   "fst_entries": count}


def decompress(data):
    if data[:4] != b"Yaz0":
        if len(data) > MAX_ARCHIVE:
            raise ValueError("archive exceeds bound")
        return data
    size = u32(data, 4)
    if size > MAX_ARCHIVE:
        raise ValueError("expanded archive exceeds bound")
    span(data, 0, 16)
    output, at = bytearray(), 16
    while len(output) < size:
        code = span(data, at, 1)[0]
        at += 1
        for bit in range(8):
            if len(output) >= size:
                break
            if code & (0x80 >> bit):
                output.extend(span(data, at, 1))
                at += 1
            else:
                first, second = span(data, at, 2)
                at += 2
                distance = ((first & 15) << 8 | second) + 1
                length = first >> 4
                if length:
                    length += 2
                else:
                    length = span(data, at, 1)[0] + 18
                    at += 1
                if distance > len(output) or len(output) + length > size:
                    raise ValueError("invalid Yaz0 backreference")
                for _ in range(length):
                    output.append(output[-distance])
    return bytes(output)


def rarc_metadata(data):
    if data[:4] != b"RARC" or u32(data, 4) != len(data):
        raise ValueError("invalid RARC size/signature")
    entries, count = u32(data, 0x2C) + 0x20, u32(data, 0x28)
    strings, string_size = u32(data, 0x34) + 0x20, u32(data, 0x30)
    names = span(data, strings, string_size)
    span(data, entries, count * 20)
    base = u32(data, 12) + 0x20
    for index in range(count):
        at = entries + index * 20
        flags = data[at + 4]
        name = cstring(names, struct.unpack_from(">H", data, at + 6)[0])
        if flags & 2 or not name.endswith((".dzr", ".dzs")):
            continue
        yield name, span(data, base + u32(data, at + 8), u32(data, at + 12))


def dz_metadata(data):
    count = u32(data, 0)
    span(data, 4, count * 12)
    chunks, spawns, layers, environment_layers = [], [], set(), set()
    for index in range(count):
        tag, entries, at = struct.unpack(">4sII", span(data, 4 + index * 12, 12))
        tag = tag.decode("ascii")
        if at > len(data) or (entries and at < 4 + count * 12):
            raise ValueError("invalid DZS/DZR chunk offset")
        chunks.append({"tag": tag, "count": entries})
        if entries and tag[:3] in LAYER_PREFIXES and tag[3] in "0123456789abcde":
            layers.add(int(tag[3], 16))
        if entries and tag[:3] in {"PAL", "Col", "Env", "VRB", "LGT"} and tag[3] in "0123456789abcde":
            environment_layers.add(int(tag[3], 16))
        if tag == "PLYR":
            span(data, at, entries * 32)
            for n in range(entries):
                start = at + n * 32
                spawns.append({"record": n, "point": data[start + 29],
                               "parameters_hex": span(data, start + 8, 4).hex()})
    return {"sha256": digest(data), "bytes": len(data), "chunks": chunks,
            "nonempty_layers": sorted(layers), "nonempty_environment_layers": sorted(environment_layers), "spawns": spawns,
            "transition_records": sum(c["count"] for c in chunks if c["tag"] == "SCLS")}


def source_stages(path):
    text = Path(path).read_text()
    match = re.search(r"allStages\s*\[[^;]*?=\s*\{(.*?)\};", text, re.S)
    if not match:
        raise ValueError("source allStages table missing")
    return sorted(set(re.findall(r'"([A-Za-z0-9_]+)"', match[1])))


def inventory(disc, table):
    source = source_stages(table)
    before = Path(disc).stat()
    stages, errors, archive_counts = {}, [], Counter()
    with Path(disc).open("rb") as stream:
        files, disc_info = read_fst(stream, before.st_size)
        for path, (at, size) in sorted(files.items()):
            match = re.fullmatch(r"/res/Stage/([^/]+)/([^/]+)", path)
            if not match:
                continue
            stage, archive = match.groups()
            row = stages.setdefault(stage, {"stage": stage, "source_table": stage in source,
                "scope": "included_reachability_unknown", "exclusion": None,
                "stage_metadata": [], "rooms": [], "unclassified_archives": []})
            room = re.fullmatch(r"R(\d+)_(\d+)\.arc", archive)
            kind = "room" if room else "stage" if re.fullmatch(r"STG_\d+\.arc", archive) else "unknown"
            archive_counts[kind] += 1
            item = {"archive": path, "stored_bytes": size, "metadata": []}
            if room:
                item.update(room=int(room[1]), archive_variant=int(room[2]))
                row["rooms"].append(item)
            elif kind == "stage":
                row["stage_metadata"].append(item)
            else:
                row["unclassified_archives"].append(item)
            try:
                if size > MAX_ARCHIVE:
                    raise ValueError("stored archive exceeds bound")
                raw = read_exact(stream, at, size)
                item["archive_sha256"] = digest(raw)
                for name, data in rarc_metadata(decompress(raw)):
                    item["metadata"].append({"member": name, **dz_metadata(data)})
                if not item["metadata"]:
                    item["unknown"] = "no DZS/DZR member discovered"
            except (ValueError, UnicodeError, struct.error) as error:
                item["unknown"] = str(error)
                errors.append({"archive": path, "error": str(error)})
    after = Path(disc).stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("disc changed during read")
    for stage in sorted(set(source) - stages.keys()):
        stages[stage] = {"stage": stage, "source_table": True, "scope": "source_only_missing_disc",
                         "exclusion": None, "stage_metadata": [], "rooms": [], "unclassified_archives": []}
    rows = [stages[key] for key in sorted(stages)]
    for stage in rows:
        supplemental = {}
        stage["nonempty_stage_layers"] = sorted({layer for a in stage["stage_metadata"] for m in a["metadata"]
            if m["member"] == "stage.dzs" for layer in m["nonempty_layers"]})
        for archive in stage["stage_metadata"]:
            for member in archive["metadata"]:
                match = re.fullmatch(r"room(\d+)\.dzs", member["member"])
                if match:
                    supplemental.setdefault(int(match[1]), []).append({"archive": archive["archive"], **member})
        for room in stage["rooms"]:
            room["supplemental_stage_metadata"] = supplemental.get(room["room"], [])
            room["nonempty_room_layers"] = sorted({layer for m in room["metadata"] + room["supplemental_stage_metadata"] for layer in m["nonempty_layers"]})
            room["point_ids"] = sorted({p["point"] for m in room["metadata"] for p in m["spawns"]})
        stage["rooms_referenced_without_archive"] = sorted(set(supplemental) - {r["room"] for r in stage["rooms"]})
        stage["metadata_only_rooms"] = [{"room": number, "metadata": supplemental[number],
            "scope": "unknown_no_corresponding_room_archive", "exclusion": None}
            for number in stage["rooms_referenced_without_archive"]]
    rooms = [(s, r) for s in rows for r in s["rooms"]]
    metadata = [m for s in rows for a in s["stage_metadata"] + s["rooms"] for m in a["metadata"]]
    room_layers = sum(len(r["nonempty_room_layers"]) for _, r in rooms)
    points = sum(len({p["point"] for m in r["metadata"] for p in m["spawns"]}) for _, r in rooms)
    return {"schema": "dusklight.full-game-inventory.v1", "origin": "private read-only disc metadata; no asset payload output",
        "disc": {**disc_info, "bytes": before.st_size, "mtime_ns": before.st_mtime_ns,
                 "whole_disc_sha256": None, "identity_limit": "FST and each consumed archive hashed, not entire image"},
        "source_table": reference(table), "stages": rows, "errors": errors,
        "totals": {"source_table_stages": len(source), "disc_stages": sum(bool(s["rooms"] or s["stage_metadata"] or s["unclassified_archives"]) for s in rows),
            "union_stages": len(rows), "source_only_stages": len(set(source) - {p.split('/')[3] for p in files if p.startswith('/res/Stage/')}),
            "disc_only_stages": sum(not s["source_table"] for s in rows), "excluded_stages": 0,
            "room_archives": len(rooms), "distinct_stage_rooms": len({(s["stage"], r["room"]) for s, r in rooms}),
            "stage_archives": archive_counts["stage"], "unclassified_archives": archive_counts["unknown"],
            "rooms_referenced_without_archive": sum(len(s["rooms_referenced_without_archive"]) for s in rows),
            "all_discoverable_stage_rooms": len({(s["stage"], r["room"]) for s, r in rooms}) + sum(len(s["metadata_only_rooms"]) for s in rows),
            "nonempty_stage_layer_pairs": sum(len(s["nonempty_stage_layers"]) for s in rows),
            "metadata_members": len(metadata), "nonempty_room_layer_pairs": room_layers,
            "plyr_records": sum(len(m["spawns"]) for m in metadata), "distinct_room_point_ids": points,
            "rooms_without_plyr": sum(not any(m["spawns"] for m in r["metadata"]) for _, r in rooms),
            "scls_records": sum(m["transition_records"] for m in metadata), "parse_errors": len(errors)},
        "uncertainty": ["All disc stages retained, including disc-only/special entries; no guessed unused/debug exclusions.",
            "Source allStages is a mod table, not proof of vanilla completeness or reachability.",
            "Nonempty layer chunks identify authored data, not every valid runtime/story layer.",
            "PLYR belongs to the common stage/room table; layer/spawn cross products are NOT verified valid entries.",
            "Missing PLYR does not prove an unused room: restart, transition, stage-level, or scripted entry may apply.",
            "SCLS counts are static records, not a verified transition graph; events/warps/doors can add routes.",
            "Archive presence is not story, collectible, boss, minigame, or full-game acceptance."]}


def failures(text):
    # Preserve shutdown faults as failures even after a requested developer exit.
    # This exact historical prefix logs successful approximations as rejections.
    # Retain those as degradation, not an invented guest fatal.
    return list(dict.fromkeys(line.strip() for line in text.splitlines() if FAILURE_RE.search(line)
        and "Native draw rejected: GX visual fallback:" not in line))


def resource_incidents(text):
    # Solid heaps may retry an allocation. An allocation log alone does not prove
    # a fatal, but must block automatic acceptance until its recovery is reviewed.
    return list(dict.fromkeys(line.strip() for line in text.splitlines() if RESOURCE_RE.search(line)))


def case_entry(boot):
    if not boot:
        return {"kind": "normal_boot", "stage": None, "room": None, "spawn": None, "layer": None}
    parts = boot.split(",")
    if len(parts) != 4 or not re.fullmatch(r"[A-Za-z0-9_]+", parts[0]):
        raise ValueError("invalid boot-stage tuple")
    return dict(zip(("kind", "stage", "room", "spawn", "layer"),
                    ("direct_stage_boot", parts[0], *(int(x) for x in parts[1:]))))


def empty_dimensions():
    return {key: "not_run" for key in DIMENSIONS}


def coverage_plan(data):
    """An inventory-linked backlog, not an asserted valid boot cross product."""
    cases = []
    for stage in data["stages"]:
        for room in stage["rooms"]:
            cases.append({"id": f'{stage["stage"]}/room-{room["room"]}/archive-{room["archive_variant"]}',
                "stage": stage["stage"], "room": room["room"], "archive": room["archive"],
                "scope": stage["scope"], "spawn_ids_discovered": room["point_ids"],
                "room_layers_authored": room["nonempty_room_layers"],
                "stage_layers_authored": stage["nonempty_stage_layers"],
                "validated_entry_tuples": [], "story_checkpoints": [], "exclusion": None,
                "dimensions": empty_dimensions()})
        for room in stage["metadata_only_rooms"]:
            cases.append({"id": f'{stage["stage"]}/room-{room["room"]}/metadata-only',
                "stage": stage["stage"], "room": room["room"], "scope": room["scope"],
                "exclusion": None, "validated_entry_tuples": [], "story_checkpoints": [],
                "dimensions": empty_dimensions()})
    return {"schema": "dusklight.full-game-plan.v1", "cases": cases,
        "totals": {"stage_room_backlog_rows": len(cases), "verified_entry_tuples": 0,
                   "verified_story_checkpoints": 0},
        "required_dimensions": {key: list(value) for key, value in DIMENSIONS.items()},
        "situations_to_expand_from_walkthrough_and_event_evidence": {
            "story": ["normal title/file select/new game", "each main-quest objective and prerequisite", "every dungeon item/puzzle/boss/exit", "final encounter and ending", "optional quests, collectibles and minigames"],
            "control": ["human/wolf and transformation", "wolf senses/digging", "mounted travel/combat", "each item and targeting mode", "swimming/diving/boats", "camera/collision/interaction"],
            "menu": ["file/name entry/save confirmation", "inventory/collection/map/dungeon map", "shops/dialogue/pause", "cancel/reopen/resume under heap pressure"],
            "audio": ["BGM/streams/stingers/effects/menu/voices", "pause/resume/transitions", "device output versus pre-device PCM", "underruns and shutdown"],
            "graphics": ["day/night/weather/twilight", "crowds/foliage/particles", "water/reflections/indirect TEV", "shadows/depth/EFB", "UI/text/cutscene readability"],
            "reload": ["save/cold-load/state consistency", "death/continue/retry", "cancelled/failed save and interrupted load"],
            "transitions": ["doors/room streaming/overworld borders", "warps/dungeon entrance/exit", "cutscene handoffs/scripted travel"],
            "soak_performance": ["repeated routes >=30 minutes", "peak and trend of every heap/cache", "audio during worst streaming/renderer load", "physical 30Hz frame-time distribution and simulation cadence"]},
        "limits": ["Situation families are a seed checklist, not an exhaustive event/story inventory.",
            "Discover exact story flags/items/forms/event branches and map them to hashed save fixtures before claiming full-game coverage.",
            "Metadata-only rooms remain unknown and included until owner-reviewed unused/debug exclusions have evidence.",
            "An archive-backed room with no PLYR still needs a legitimate entry/restart/transition route.",
            "Stage/room/layer/spawn and story-state coverage are independent; never count a guessed Cartesian product as valid."]}


def safe_run(name):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
        raise ValueError("invalid run basename")
    return name


def import_evidence(coverage, corrections, evidence_root, variant):
    old = json.loads(Path(coverage).read_text())
    rows = old.get("results", old.get("scenes", []))
    records, counts, refs = [], Counter(), [reference(coverage)]
    for row in rows:
        name = safe_run(row["run"])
        fatal = row.get("status") == "guest_failure" or bool(failures("\n".join(row.get("problems", []))))
        visual = row.get("visual_status", "missing_or_unreadable")
        classification = "hard_failure" if fatal else "all_black_inconclusive" if visual == "all_black" else "visible_clean_historical" if visual == "nonuniform_capture_needs_review" and row.get("exit_reached") else "inconclusive"
        counts[classification] += 1
        dims = empty_dimensions()
        dims["smoke"] = "fail" if fatal else "observed_visible" if classification == "visible_clean_historical" else "inconclusive"
        dims["graphics"] = "fail" if fatal else "inconclusive"
        records.append({"id": name, "cohort": "historical_12", "entry": case_entry(row.get("boot_stage")),
            "classification": classification, "candidate_sha256": row.get("candidate_sha256"),
            "input_sha256": None, "save_sha256": None, "dimensions": dims,
            "hard_failures": row.get("problems", []) if fatal else [], "source_row": row,
            "acceptance": False, "limit": "Historical visibility is not a new visual review, control, or story receipt."})
    if corrections:
        refs.append(reference(corrections))
        for correction in json.loads(Path(corrections).read_text()):
            run = safe_run(correction["run"])
            if correction.get("status") != "guest_failure":
                continue
            dims = empty_dimensions()
            dims["menu"] = dims["smoke"] = "fail"
            dims["reload"] = "inconclusive"
            record = {"id": run + ":acceptance-correction", "cohort": "later_menu_shutdown_correction",
                "classification": "hard_failure", "dimensions": dims, "hard_failures": correction["problems"],
                "acceptance": False, "source_row": correction}
            process = Path(evidence_root) / run / "process.json"
            if process.exists():
                receipt = json.loads(process.read_text())
                record.update(entry=case_entry(receipt.get("boot_stage")), candidate_sha256=receipt.get("cxi_sha256"))
                refs.append(reference(process))
            records.append(record)
    if variant:
        process_path, analysis_path = Path(variant) / "process.json", Path(variant) / "analysis.json"
        process, analysis = json.loads(process_path.read_text()), json.loads(analysis_path.read_text())
        refs.extend([reference(process_path), reference(analysis_path)])
        log_path = Path(variant) / "guest.log"
        bad, resources = list(analysis.get("problems", [])), []
        if log_path.exists():
            refs.append(reference(log_path))
            text = log_path.read_text(errors="replace")
            bad += failures(text)
            resources = resource_incidents(text)
        bad = list(dict.fromkeys(bad))
        degraded = analysis.get("degraded_reasons", [])
        dims = empty_dimensions()
        dims["smoke"] = "fail" if bad else "observed_degraded" if degraded else "inconclusive"
        dims["graphics"] = "fail" if bad else "inconclusive"
        records.append({"id": Path(variant).name, "cohort": "separate_diagnostic_variant",
            "entry": case_entry(process.get("boot_stage")), "candidate_sha256": process.get("cxi_sha256"),
            "classification": "hard_failure" if bad else "diagnostic_variant_with_degradation" if degraded else "inconclusive",
            "dimensions": dims, "hard_failures": bad, "degraded_reasons": degraded, "source_row": analysis,
            "resource_incidents": resources,
            "acceptance": False, "limit": "Requested point/layer variant, no verified story/save/control; screenshots require review."})
    return {"schema": "dusklight.full-game-ledger.v1", "records": records, "inputs": refs,
            "historical_12_counts": dict(counts), "verified_story_checkpoints": 0, "acceptance_complete": False,
            "objective": "Entire game and situations; >=10 scenes is an initial floor only.",
            "hard_failure_records": sum(bool(r["hard_failures"]) for r in records),
            "policy": "Append new immutable run IDs. A later variant/pass never erases a historical failure."}


def check_ref(item, root):
    if not isinstance(item, dict) or not re.fullmatch(r"[0-9a-f]{64}", str(item.get("sha256", ""))):
        return False
    path = item.get("path")
    if not isinstance(path, str) or not path:
        return False
    try:
        return file_hash(Path(root) / path) == item["sha256"]
    except OSError:
        return False


def capture_visibility(ref, root):
    """Check original DSKCAP01 bytes, independent of any visual attestation.

    A nonuniform capture only permits further review. It cannot prove correct
    geometry or a playable scene. Black/uniform/missing captures never pass.
    """
    if not check_ref(ref, root):
        return "missing_or_hash_mismatch"
    path = Path(root) / ref["path"]
    if path.stat().st_size > 4 * 1024 * 1024 + 16:
        return "outside_capture_bound"
    try:
        data = path.read_bytes()
        magic, version, width, height = struct.unpack("<4I", span(data, 0, 16))
        if (magic, version) != (0x43534B44, 0x31305041) or not width or not height or len(data) != 16 + width * height * 4:
            return "unreadable_capture"
        colors = {data[at + 1:at + 4] for at in range(16, len(data), 4)}
        if colors == {b"\0\0\0"}:
            return "all_black"
        return "uniform" if len(colors) == 1 else "nonuniform_needs_review"
    except (ValueError, OSError, struct.error):
        return "unreadable_capture"


def evaluate(receipt, root=ROOT):
    """Validate reviewer attestations; unknown or missing proof stays inconclusive.

    Every positive attestation needs hashed build/input and hashed evidence, plus
    its dimension's explicit criteria. Story/load/transition need a hashed save
    and verified checkpoint state entered through normal boot or saved game.
    This validates receipts, not the semantic truth of a human review.
    """
    if receipt.get("schema") != "dusklight.full-game-receipt.v1":
        raise ValueError("unsupported receipt schema")
    identity = receipt.get("id")
    if not isinstance(identity, str) or not identity.strip():
        raise ValueError("receipt id required")
    entry = receipt.get("entry", {})
    if not isinstance(entry, dict):
        raise ValueError("entry must be an object")
    artifacts = receipt.get("artifacts", {})
    if not isinstance(artifacts, dict):
        raise ValueError("artifacts must be an object")
    for key in ("hard_failures", "logs", "captures"):
        if not isinstance(receipt.get(key, []), list):
            raise ValueError(key + " must be an array")
    if not all(isinstance(value, str) for value in receipt.get("hard_failures", [])):
        raise ValueError("hard_failures must contain strings")
    verified = {key: check_ref(artifacts.get(key), root) for key in ("build", "input", "save_before", "save_after")}
    problems = list(receipt.get("hard_failures", []))
    logs_checked, log_text = 0, ""
    for ref in receipt.get("logs", []):
        if check_ref(ref, root):
            logs_checked += 1
            text = (Path(root) / ref["path"]).read_text(errors="replace")
            log_text += text + "\n"
            problems.extend(failures(text))
        else:
            problems.append("missing or hash-mismatched required log")
    problems = list(dict.fromkeys(problems))
    resources = resource_incidents(log_text)
    visibility = [capture_visibility(ref, root) for ref in receipt.get("captures", [])]
    statuses, reasons = empty_dimensions(), {}
    checks = receipt.get("checks", {})
    if not isinstance(checks, dict):
        raise ValueError("checks must be an object")
    unknown = set(checks) - DIMENSIONS.keys()
    if unknown:
        raise ValueError("unknown coverage dimensions: " + ", ".join(sorted(unknown)))
    for dimension, check in checks.items():
        if not isinstance(check, dict) or not isinstance(check.get("criteria", {}), dict) or not isinstance(check.get("evidence", []), list):
            raise ValueError("dimension checks need object criteria and array evidence")
        requested = check.get("outcome")
        if requested not in ("pass", "fail", "inconclusive", "not_run"):
            raise ValueError("invalid dimension outcome")
        if requested != "pass":
            statuses[dimension] = requested
            continue
        missing = []
        if not (verified["build"] and verified["input"]):
            missing.append("exact build/input artifacts not verified")
        if not logs_checked:
            missing.append("no hash-verified runtime log")
        if resources:
            missing.append("allocation/resource incidents require recovery review")
        evidence = check.get("evidence", [])
        if not evidence or not all(check_ref(ref, root) for ref in evidence):
            missing.append("missing/hash-mismatched review evidence")
        criteria = check.get("criteria", {})
        missing += ["criterion:" + key for key in DIMENSIONS[dimension] if criteria.get(key) is not True]
        if dimension in STORY_DIMENSIONS:
            state = receipt.get("story_state", {})
            if not verified["save_before"] or not state.get("checkpoint") or state.get("verified") is not True:
                missing.append("unverified save/checkpoint state")
            if entry.get("kind") not in ("normal_boot", "saved_game"):
                missing.append("direct diagnostic entry is not story acceptance")
        if dimension in ("progression", "reload") and not verified["save_after"]:
            missing.append("post-progress/save artifact not verified")
        if dimension == "performance" and receipt.get("platform") != "physical_new3ds":
            missing.append("emulator timing is not physical performance")
        if dimension in ("smoke", "graphics") and (not visibility or any(v != "nonuniform_needs_review" for v in visibility)):
            missing.append("missing/black/uniform/unreadable raw capture")
        if dimension == "audio" and re.search(r"audio muted|component unavailable|outputState=componentUnavailable", log_text, re.I):
            missing.append("runtime reports muted/unavailable device audio")
        if dimension == "smoke" and (any(entry.get(k) is None for k in ("stage", "room", "layer", "spawn")) or entry.get("layer") == -1):
            missing.append("exact observed stage/room/layer/spawn missing (auto layer unresolved)")
        if problems:
            statuses[dimension] = "fail"
        else:
            statuses[dimension] = "inconclusive" if missing else "pass"
        reasons[dimension] = missing
    if problems:
        statuses["smoke"] = "fail"
    return {"id": identity, "entry": entry, "dimensions": statuses,
        "platform": receipt.get("platform"), "artifacts": artifacts,
        "story_state": receipt.get("story_state"), "measurements": receipt.get("measurements", {}),
        "verified_artifacts": verified, "hard_failures": problems, "inconclusive_reasons": reasons,
        "capture_visibility": visibility,
        "resource_incidents": resources,
        "acceptance_complete": False, "limit": "Pass validates supplied review receipts only, never entire-game completion."}


def summarize(ledger, receipt_paths, root):
    records = list(ledger.get("records", []))
    ids = {r["id"] for r in records}
    for path in receipt_paths:
        result = evaluate(json.loads(Path(path).read_text()), root)
        if result["id"] in ids:
            raise ValueError("duplicate immutable run id: " + result["id"])
        result["receipt"] = reference(path)
        records.append(result)
        ids.add(result["id"])
    return {"schema": "dusklight.full-game-ledger.v1", "records": records,
        "inputs": list(ledger.get("inputs", [])),
        "historical_12_counts": ledger.get("historical_12_counts", {}),
        "dimension_counts": {key: dict(Counter(r.get("dimensions", {}).get(key, "not_run") for r in records)) for key in DIMENSIONS},
        "hard_failure_records": sum(bool(r.get("hard_failures")) or "fail" in r.get("dimensions", {}).values() for r in records),
        "acceptance_complete": False, "limit": "Historical failures retained; run receipts are not a full-game denominator."}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inv = sub.add_parser("inventory", help="read-only disc/FST and source table inventory")
    inv.add_argument("--private-disc", type=Path, required=True)
    inv.add_argument("--stage-table", type=Path, default=ROOT / "Source/mods/randomizer/src/stages.cpp")
    plan = sub.add_parser("plan", help="inventory-linked room backlog and situation checklist")
    plan.add_argument("--inventory", type=Path, required=True)
    imp = sub.add_parser("import-evidence", help="retain historical scene results and later corrections")
    imp.add_argument("--coverage", type=Path, required=True)
    imp.add_argument("--corrections", type=Path)
    imp.add_argument("--evidence-root", type=Path, default=ROOT / "Diagnostics/implementation-44/emulator")
    imp.add_argument("--variant-run", type=Path)
    summary = sub.add_parser("evaluate", help="append/evaluate exact hashed run receipts")
    summary.add_argument("--ledger", type=Path)
    summary.add_argument("--receipt", type=Path, action="append", required=True)
    summary.add_argument("--root", type=Path, default=ROOT)
    for p in (inv, plan, imp, summary):
        p.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "inventory":
        result = inventory(args.private_disc, args.stage_table)
    elif args.command == "plan":
        result = coverage_plan(json.loads(args.inventory.read_text()))
        result["inventory"] = reference(args.inventory)
    elif args.command == "import-evidence":
        result = import_evidence(args.coverage, args.corrections, args.evidence_root, args.variant_run)
    else:
        ledger = json.loads(args.ledger.read_text()) if args.ledger else {}
        result = summarize(ledger, args.receipt, args.root)
    write_json(args.output, result)
    print(json.dumps(result.get("totals", {"records": len(result.get("records", [])), "acceptance_complete": False}), sort_keys=True))
    return 1 if result.get("errors") else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, TypeError, struct.error) as error:
        print("coverage error: " + str(error), file=sys.stderr)
        sys.exit(2)
