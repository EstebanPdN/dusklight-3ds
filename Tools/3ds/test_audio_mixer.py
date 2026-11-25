#!/usr/bin/env python3
"""Host-only checks of the software mixer: ADPCM decoder and fixed-point mix against the float path.

Compiles src/dusk/audio/DuskDsp.cpp and Adpcm.cpp with the host compiler (synthetic voices and
wave data only). Never builds ARM or starts an emulator; the benchmark is host time, a ratio only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "Source"
TESTS = SOURCE / "platforms/3ds/tests"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=ROOT / "Diagnostics/alpha14/audio-mixer")
    parser.add_argument("--no-benchmark", action="store_true")
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="host-", dir=args.evidence)).resolve()
    commands = []

    def run(command, timeout=600):
        result = subprocess.run([str(part) for part in command], cwd=ROOT, text=True, capture_output=True, timeout=timeout)
        commands.append({"argv": [str(part) for part in command], "returncode": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"Command failed; see {out / 'commands.json'}")
        return result.stdout

    compiler = shutil.which("c++")
    assert compiler, "Host C++ compiler required"
    includes = [SOURCE / "include", SOURCE / "libs/JSystem/include", SOURCE / "extern/aurora/include",
                SOURCE / "extern/aurora/include/dolphin", SOURCE / "sdk/include", SOURCE / "src",
                SOURCE / "libs/freeverb/include", SOURCE / "libs/freeverb/include/freeverb"]
    game = ["-std=gnu++20", "-DAVOID_UB=1", "-DDUSK_BUILDING_GAME=1", "-DDUSK_TARGET_3DS=1", "-DTARGET_PC=1",
            "-DVERSION=0", "-fno-short-enums", "-include", SOURCE / "extern/aurora/include/dolphin/types.h",
            "-include", SOURCE / "include/global.h", "-g", "-fno-omit-frame-pointer"]
    game += [f"-I{path}" for path in includes]
    sources = [TESTS / "audio_mixer_test.cpp", SOURCE / "src/dusk/audio/DuskDsp.cpp", SOURCE / "src/dusk/audio/Adpcm.cpp",
               SOURCE / "libs/freeverb/allpass.cpp", SOURCE / "libs/freeverb/comb.cpp", SOURCE / "libs/freeverb/revmodel.cpp"]
    # The test itself warning-clean; the game's sources as the game builds them.
    run([compiler, "-std=gnu++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-multichar",
         "-fsyntax-only", *game[1:], TESTS / "audio_mixer_test.cpp"])
    sanitized = [compiler, "-w", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", *game]
    run(sanitized + [*sources, "-o", out / "mixer-sanitized"])
    lines = [run([out / "mixer-sanitized", "equivalence"]).strip()]
    if not args.no_benchmark:
        run([compiler, "-w", "-O2", *game, *sources, "-o", out / "mixer-o2"])
        lines.append(run([out / "mixer-o2", "benchmark"]).strip())
    print("\n".join(lines))
    hashed = sources + [Path(__file__).resolve(), SOURCE / "src/dusk/audio/DuskDsp.hpp", SOURCE / "src/dusk/audio/Adpcm.hpp"]
    receipt = {"result": "pass", "native_build": False, "emulator_run": False, "physical_audio_verified": False,
               "output": lines,
               "sources_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in hashed}}
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"PASS: receipt {out / 'receipt.json'}")


if __name__ == "__main__":
    main()
