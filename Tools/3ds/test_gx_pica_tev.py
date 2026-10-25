#!/usr/bin/env python3
"""Verify bounded GX->PICA TEV planning under host sanitizers; compile for ARM.

The independent evaluators use real normalized arithmetic. Passing does not
establish fixed-point rounding equivalence, PICA execution or physical hardware.
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
OUT = ROOT / "Diagnostics/implementation-05/gx-pica-tev"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    receipt_path = OUT / "results.json"
    receipt_path.write_text(json.dumps({"status": "running; prior result superseded"}) + "\n")
    compiler = shutil.which("clang++")
    arm_compiler = Path("/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++")
    if not compiler or not arm_compiler.is_file():
        receipt_path.write_text(json.dumps({"status": "fail", "error": "Host clang++ and devkitARM required"}) + "\n")
        return 1
    inputs = [SOURCE / "runtime/gx_pica_tev.cpp", SOURCE / "runtime/gx_pica_tev.hpp",
              SOURCE / "runtime/gx_state.hpp", SOURCE / "tests/gx_pica_tev_test.cpp",
              Path(__file__).resolve()]
    before = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    binary = OUT / "gx-pica-tev-asan-ubsan-test"
    arm_object = OUT / "gx-pica-tev-arm.o"
    commands = [
        [compiler, "-std=c++20", "-O1", "-g", "-fno-omit-frame-pointer",
         "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
         str(inputs[0]), str(inputs[3]), "-o", str(binary)],
        [str(arm_compiler), "-std=gnu++20", "-O2", "-g", "-march=armv6k", "-mtune=mpcore",
         "-mfloat-abi=hard", "-mfpu=vfp", "-mtp=soft", "-D__3DS__", "-DTARGET_PC=1",
         "-DDUSK_TARGET_3DS=1", "-Wall", "-Wextra", "-Werror", "-fno-strict-aliasing",
         "-fstack-usage", "-c", str(inputs[0]), "-o", str(arm_object)],
    ]
    run_command = [str(binary)]
    options = {"ASAN_OPTIONS": "halt_on_error=1:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    primary_paths = [ROOT / "Source/libs/JSystem/src/J2DGraph/J2DPicture.cpp",
                     ROOT / "Upstream/extracted/citro3d-1.7.1/include/c3d/texenv.h",
                     ROOT / "Upstream/extracted/citro3d-1.7.1/source/texenv.c"]
    receipt = {
        "status": "running", "origin": "synthetic host execution and ARM compile only",
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler": subprocess.check_output([compiler, "--version"], text=True).strip(),
        "arm_compiler": subprocess.check_output([str(arm_compiler), "--version"], text=True).strip(),
        "build_commands": commands, "run_command": run_command, "sanitizer_environment": options,
        "input_sha256": before,
        "primary_source_sha256": {str(p.relative_to(ROOT)): digest(p) for p in primary_paths},
        "primary_sources": ["https://github.com/devkitPro/citro3d/blob/v1.7.1/include/c3d/texenv.h",
                            "https://github.com/devkitPro/citro3d/blob/v1.7.1/source/texenv.c",
                            "https://gist.github.com/profi200/e31df7d31b8c2cdccc0608aaa71681fc"],
        "admitted": ["untextured raster RGBA replace", "one texture RGB with texture or arbitrary REG2 alpha",
                     "optional raster RGBA modulation", "optional zero-black tint by REG1 RGBA"],
        "limits": ["at most two GX and PICA stages", "one shared RGBA constant per stage",
                   "nonzero REG0 black tint rejected", "no previous-buffer use or update latency dependence",
                   "only ADD, zero bias, scale one, clamped PREV output"],
        "coverage": ["independent GX selector interpreter versus portable PICA evaluator",
                     "3584 edge-value and 28672 seeded random RGBA cases plus distinct register fixture",
                     "every color/alpha input, operation field and order field rejected after mutation",
                     "unsupported valid GX equations rejected", "failure clears previously successful plan",
                     "unused konst state does not affect admitted equations"],
        "not_covered": ["GX/PICA fixed-point rounding equivalence", "Citro3D backend mapping/execution",
                        "GPU or physical 3DS hardware", "sampler, geometry, blend and other non-TEV state",
                        "full J2D materials or gameplay"],
    }
    (OUT / "commands.json").write_text(json.dumps({"build": commands, "run": run_command,
        "environment_overrides": options}, indent=2) + "\n")
    status = 1
    try:
        with (OUT / "build.log").open("w") as log:
            for command in commands:
                subprocess.run(command, text=True, stdout=log, stderr=subprocess.STDOUT, timeout=60, check=True)
        receipt["binary_sha256"] = digest(binary)
        receipt["arm_object_sha256"] = digest(arm_object)
        result = subprocess.run(run_command, env={**os.environ, **options}, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (OUT / "test.log").write_text(result.stdout)
        receipt["test_exit"] = result.returncode
        print(result.stdout, end="")
        result.check_returncode()
        after = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
        receipt["input_unchanged"] = before == after
        if before != after:
            raise RuntimeError("A planner or test input changed during verification")
        receipt["status"] = "pass"
        status = 0
    except (subprocess.SubprocessError, RuntimeError) as error:
        receipt["status"] = "fail"
        receipt["error"] = str(error)
        print((OUT / "build.log").read_text(), file=sys.stderr)
        print(str(error), file=sys.stderr)
    finally:
        receipt["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
