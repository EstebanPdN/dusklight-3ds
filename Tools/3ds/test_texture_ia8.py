#!/usr/bin/env python3
"""Run the synthetic IA8 conversion fixture under host ASan/UBSan.

Does not open a disc or execute ARM code. Outputs include exact commands,
sanitizer options, compiler identity, logs, and input/binary hashes.
"""
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "Source/platforms/3ds"
OUT = ROOT / "Diagnostics/implementation-04/texture"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("clang++")
    if not compiler:
        raise RuntimeError("clang++ is required for the host sanitizer fixture")
    inputs = [SOURCE / "runtime/texture_ia8.cpp", SOURCE / "runtime/texture_ia8.hpp",
              SOURCE / "tests/texture_ia8_test.cpp", Path(__file__).resolve()]
    before = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    binary = OUT / "texture-ia8-asan-ubsan-test"
    command = [compiler, "-std=c++20", "-O1", "-g", "-fno-omit-frame-pointer",
               "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
               str(inputs[0]), str(inputs[2]), "-o", str(binary)]
    run_command = [str(binary)]
    options = {"ASAN_OPTIONS": "halt_on_error=1:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    receipt = {
        "status": "running", "origin": "synthetic host execution; no ARM or hardware execution",
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler": subprocess.check_output([compiler, "--version"], text=True).strip(),
        "build_command": command, "run_command": run_command, "sanitizer_environment": options,
        "input_sha256": before,
        "coverage": ["9x9 GX IA8 alpha/intensity conversion", "4x4 source tile boundaries",
                     "8x8 PICA Morton tile boundaries", "16x16 transparent padding",
                     "output sentinel", "short output refusal", "overlapping buffer refusal",
                     "extent mismatch refusal", "truncated input and invalid metadata refusal"],
    }
    (OUT / "commands.json").write_text(json.dumps({"build": command, "run": run_command,
        "environment_overrides": options}, indent=2) + "\n")
    status = 1
    try:
        build = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=60)
        (OUT / "build.log").write_text(build.stdout)
        receipt["build_exit"] = build.returncode
        build.check_returncode()
        receipt["binary_sha256"] = digest(binary)
        result = subprocess.run(run_command, env={**os.environ, **options}, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (OUT / "test.log").write_text(result.stdout)
        receipt["test_exit"] = result.returncode
        print(result.stdout, end="")
        result.check_returncode()
        after = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
        receipt["input_unchanged"] = before == after
        if before != after:
            raise RuntimeError("A test input changed during the sanitizer run")
        receipt["status"] = "pass"
        status = 0
    except (subprocess.SubprocessError, RuntimeError) as error:
        receipt["status"] = "fail"
        receipt["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        receipt["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (OUT / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
