#!/usr/bin/env python3
"""Inventory ARM32 game objects without modifying or linking the native target.

The native kernel's compile command is the authority for ABI/feature flags.
Only the source/output/dependency paths and diagnostic options are replaced.
Undefined-symbol reports are object-level candidate dependencies, not evidence
that every function survives a real --gc-sections executable link.
"""
import argparse
import collections
import concurrent.futures
import hashlib
import json
import re
import shlex
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "Source"
MANIFEST = SOURCE / "platforms/3ds/compile/game_core_sources.json"
DB = ROOT / "Build/new3ds-native/compile_commands.json"
OUTPUT = ROOT / "Diagnostics/implementation-04/core-inventory"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def category(message):
    checks = [
        ("desktop profiling", r"tracy[/\.]|Tracy"),
        ("desktop graphics/math dependency", r"webgpu|dawn|glm/|aurora/math"),
        ("desktop settings/mod/UI dependency", r"absl/|nlohmann|borealis|RmlUi|imgui|dusk/|fmt/|spdlog|magic_enum|cxxopts|lua"),
        ("original console MMIO/FIFO", r"WGPIPE|__GXData|MMIO|GX.*unavailable|hardware register"),
        ("PPC assembly/intrinsics", r"assembler|opcode|register name|__cntlzw|__rlwimi|asm"),
        ("ARM32 layout/ABI", r"static assertion|alignment|precision|cast.*pointer|sizeof"),
        ("C++ type/overload portability", r"ambiguous|no matching function|conversion|invalid.*operand|cannot convert|operator"),
        ("OS/threading", r"SDL|pthread|thread|mutex|atomic|condition_variable"),
        ("file I/O", r"DVD|CARD|fopen|fseek"),
        ("graphics API", r"GX|J2D|J3D"),
    ]
    for name, pattern in checks:
        if re.search(pattern, message, re.I):
            return name
    return "other (manual review required)"


def symbols(nm, path):
    result = subprocess.run([str(nm), "--extern-only", "--format=posix", str(path)],
                            text=True, capture_output=True, check=True)
    defined, strong, weak = set(), set(), set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        symbol, kind = parts[:2]
        if kind == "U":
            strong.add(symbol)
        elif kind in ("w", "v"):
            weak.add(symbol)
        else:
            defined.add(symbol)
    return defined, strong, weak


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--group", action="append", help="Limit run to named manifest groups")
    parser.add_argument("--compile-db", type=Path, default=DB,
                        help="Compile-command database supplying the real native JKRHeap flags")
    parser.add_argument("--output-root", type=Path, default=OUTPUT,
                        help="Evidence directory; the fresh label is created underneath it")
    parser.add_argument("--extra-include", action="append", default=[],
                        help="Explicit additional Source-relative include directory; recorded separately from kernel flags")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.label):
        parser.error("label must be a simple directory component")
    if not 1 <= args.jobs <= 6:
        parser.error("jobs must be between 1 and 6")
    out = args.output_root.resolve() / args.label
    out.mkdir(parents=True, exist_ok=False)
    for name in ("objects", "logs", "deps"):
        (out / name).mkdir()
    manifest = json.loads(args.manifest.read_text())
    selected = [s for s in manifest["sources"] if not args.group or s["group"] in args.group]
    if not selected:
        parser.error("no selected source files")
    db = json.loads(args.compile_db.read_text())
    entry = next(e for e in db if e["file"].endswith("/JKernel/JKRHeap.cpp"))
    original = entry.get("arguments") or shlex.split(entry["command"])
    base, i = [], 0
    while i < len(original):
        if original[i] in ("-o", "-c", "-MF", "-MT", "-MQ"):
            i += 2
        elif original[i] in ("-MD", "-MMD", "-MP"):
            i += 1
        else:
            base.append(original[i])
            i += 1
    compiler = Path(base[0])
    nm = compiler.with_name("arm-none-eabi-nm")
    cxxfilt = compiler.with_name("arm-none-eabi-c++filt")
    extra_includes = []
    for include in args.extra_include:
        path = (SOURCE / include).resolve()
        if not path.is_relative_to(SOURCE) or not path.is_dir():
            parser.error("extra include must be an existing directory inside Source")
        extra_includes += ["-I", str(path)]
    native_objects = []
    for record in db:
        command = record.get("arguments") or shlex.split(record["command"])
        candidate = Path(record["directory"]) / command[command.index("-o") + 1]
        if candidate.is_file():
            native_objects.append(candidate)
    native_definitions = set()
    native_receipts = []
    for obj in native_objects:
        native_definitions.update(symbols(nm, obj)[0])
        native_receipts.append({"path": str(obj.relative_to(ROOT)), "sha256": digest(obj)})

    def compile_one(pair):
        index, source = pair
        path = SOURCE / source["path"]
        stem = f"{index:03d}-{path.stem}"
        obj, log, dep = out/"objects"/(stem+".o"), out/"logs"/(stem+".log"), out/"deps"/(stem+".d")
        cmd = base + extra_includes + ["-fdiagnostics-color=never", "-fmax-errors=8", "-MMD", "-MF", str(dep), "-o", str(obj), "-c", str(path)]
        before = digest(path)
        started = time.monotonic()
        run = subprocess.run(cmd, cwd=entry["directory"], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        elapsed = time.monotonic() - started
        log.write_text(run.stdout)
        errors = [line.replace(str(ROOT)+"/", "") for line in run.stdout.splitlines() if "error:" in line or "Error:" in line]
        defined, undefined, weak = symbols(nm, obj) if run.returncode == 0 else (set(), set(), set())
        result = dict(source, returncode=run.returncode, seconds=round(elapsed, 3), command=cmd,
                      object=str(obj.relative_to(ROOT)) if run.returncode == 0 else None,
                      log=str(log.relative_to(ROOT)), source_sha256=before,
                      source_changed_during_compile=before != digest(path), errors=errors,
                      first_error_category=category(errors[0]) if errors else None,
                      defined=sorted(defined), undefined=sorted(undefined), weak_undefined=sorted(weak))
        print(f"{'PASS' if run.returncode == 0 else 'FAIL'} {source['path']}" + (f" [{result['first_error_category']}]" if errors else ""), flush=True)
        return result

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        units = list(pool.map(compile_one, enumerate(selected)))
    providers = collections.defaultdict(list)
    for unit in units:
        for symbol in unit["defined"]:
            providers[symbol].append(unit["path"])
    all_symbols = sorted({s for unit in units for s in unit["undefined"]})
    demangled_text = subprocess.run([str(cxxfilt)], input="\n".join(all_symbols)+"\n", capture_output=True, text=True, check=True).stdout.splitlines()
    names = dict(zip(all_symbols, demangled_text))
    external = {}
    for symbol in all_symbols:
        if symbol not in providers and symbol not in native_definitions:
            external[symbol] = {"name": names[symbol], "required_by": [u["path"] for u in units if symbol in u["undefined"]]}
    for unit in units:
        unit["external_to_inventory_and_native_objects"] = [s for s in unit["undefined"] if s in external]
        unit["inventory_dependencies"] = {s: providers[s] for s in unit["undefined"] if s in providers}
    group_summary = {}
    for group in sorted({u["group"] for u in units}):
        subset = [u for u in units if u["group"] == group]
        group_summary[group] = {"passed": sum(u["returncode"] == 0 for u in subset), "failed": sum(u["returncode"] != 0 for u in subset)}
    result = {"schema": 1, "origin": "ARM32 cross-compilation and static symbol inventory; no execution or final link",
              "label": args.label, "source_revision": subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip(),
              "manifest_path": str(args.manifest.resolve()), "manifest_sha256": digest(args.manifest),
              "compile_database_path": str(args.compile_db.resolve()),
              "compile_database_sha256": digest(args.compile_db), "kernel_template": entry,
              "additional_source_include_directories": args.extra_include,
              "compiler": subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0],
              "native_object_receipts": native_receipts, "groups": group_summary,
              "passed": sum(u["returncode"] == 0 for u in units), "failed": sum(u["returncode"] != 0 for u in units),
              "first_error_categories": dict(collections.Counter(u["first_error_category"] for u in units if u["returncode"])),
              "units": units, "external_symbols": external,
              "scope_limit": "Undefined symbols include unreachable functions and C/C++ runtime/library symbols. Native object providers are from recorded existing build objects, not a new final link. Failed objects hide deeper errors and dependency edges."}
    (out / "inventory.json").write_text(json.dumps(result, indent=2)+"\n")
    (out / "summary.json").write_text(json.dumps({k: v for k, v in result.items() if k not in ("units", "external_symbols", "kernel_template", "native_object_receipts")}, indent=2)+"\n")
    (out / "external-symbols.json").write_text(json.dumps(external, indent=2)+"\n")
    print(json.dumps({"passed": result["passed"], "failed": result["failed"], "external_symbols": len(external), "groups": group_summary}), flush=True)
    return int(result["failed"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
