#!/usr/bin/env python3
"""Host-only tests for bounded native PCM capture. Never builds ARM or starts an emulator."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "Source/platforms/3ds/runtime"
TESTS = ROOT / "Source/platforms/3ds/tests"


def fnv(data):
    result = 2166136261
    for value in data:
        result = ((result ^ value) * 16777619) & 0xffffffff
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=ROOT / "Diagnostics/implementation-44/audio-pcm")
    parser.add_argument("--sanitizer", choices=["address,undefined", "thread"], default="address,undefined")
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="host-", dir=args.evidence)).resolve()
    commands = []

    def run(command):
        result = subprocess.run([str(part) for part in command], cwd=ROOT, text=True, capture_output=True, timeout=60)
        commands.append({"argv": [str(part) for part in command], "returncode": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"Command failed; see {out / 'commands.json'}")
        return result.stdout

    compiler = shutil.which("c++")
    assert compiler, "Host C++ compiler required"
    flags = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
             "-fno-omit-frame-pointer", f"-fsanitize={args.sanitizer}"]
    shared = [RUNTIME / "audio_control.cpp", RUNTIME / "dump_io.cpp"]
    component = out / "opendsp_component.cpp"
    run(["cmake", f"-DINPUT={RUNTIME.parent / 'vendor/opendsp/opendsp.cdc'}",
         f"-DOUTPUT={component}", "-P", RUNTIME.parent / "embed_audio.cmake"])
    run(flags + [f"-I{RUNTIME}", TESTS / "audio_pcm_test.cpp", *shared, "-o", out / "capture-test"])
    print(run([out / "capture-test", out / "capture"]).strip())
    run(flags + ["-Wno-sign-compare", "-DDUSK_TARGET_3DS=1", f"-I{TESTS / 'audio_pcm_stubs'}",
                 f"-I{RUNTIME}", TESTS / "audio_pcm_lifecycle_test.cpp", RUNTIME / "audio_ndsp.cpp",
                 *shared, component, "-o", out / "lifecycle-test"])
    for mode in ["ndsp", "muted", "disabled", "fatal"]:
        print(run([out / "lifecycle-test", out / mode, mode]).strip())

    outputs = []
    for path in sorted(out.rglob("*.wav")):
        data = path.read_bytes()
        with wave.open(str(path), "rb") as wav:
            assert wav.getnchannels() == 2 and wav.getsampwidth() == 2 and wav.getframerate() == 48000
            pcm = wav.readframes(wav.getnframes())
            assert len(data) == 44 + len(pcm)
        metadata_path = path.with_suffix(".json")
        if metadata_path.exists():
            metadata = json.loads(metadata_path.read_text())
            assert metadata["wav_bytes"] == len(data) and metadata["wav_fnv1a32"] == fnv(data)
            assert metadata["wav_readback_verified"] is True and metadata["host_playback_verified"] is False
            assert metadata["sample_values"] * 2 == len(pcm)
            assert metadata["captured_blocks"] * metadata["frames_per_block"] * 4 == len(pcm)
            assert metadata["observation"] == "pre_device_mixer"
            if path.name == "capture.wav":
                assert metadata["first_captured_block"] == 1 and metadata["end_block_exclusive"] == 3
                assert metadata["capture_submitted_queue_calls"] == 2 and metadata["capture_observed_completed_blocks"] == 1
                assert metadata["nonzero_samples"] == [3, 3] and metadata["clipped_or_full_scale_samples"] == [1, 1]
            if path.name in ["empty.wav", "max-empty.wav"]:
                assert metadata["range_valid"] is False and metadata["captured_blocks"] == 0
            if path.name == "partial.wav":
                assert metadata["requested_block_count"] == 3 and metadata["captured_blocks"] == 1
                assert metadata["first_captured_block"] == 2 and metadata["end_block_exclusive"] == 3
                assert metadata["capture_submitted_queue_calls"] == metadata["capture_observed_completed_blocks"] == 0
            if path.name == "audio-pcm.wav":
                assert metadata["first_captured_block"] == 1 and metadata["end_block_exclusive"] == 5
                assert struct.unpack("<" + "hh" * 3200, pcm) == (8192, -16384) * 3200
                if "muted" in path.parts:
                    assert metadata["output"] == "muted_missing_component" and metadata["ndsp_result"] == 0xd880a7fa
                    assert metadata["submitted_queue_calls"] == metadata["observed_completed_blocks"] == 0
                    assert metadata["capture_submitted_queue_calls"] == metadata["capture_observed_completed_blocks"] == 0
                else:
                    assert metadata["output"] == "ndsp" and metadata["capture_submitted_queue_calls"] == 4
        outputs.append({"path": str(path.relative_to(out)), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(), "fnv1a32": fnv(data),
                        "metadata": metadata if metadata_path.exists() else None})
    sources = [RUNTIME / name for name in ["audio_ndsp.cpp", "audio_control.hpp", "audio_control.cpp"]]
    sources += [RUNTIME.parent / "embed_audio.cmake", RUNTIME.parent / "embed_shader.cmake",
                RUNTIME.parent / "vendor/opendsp/opendsp.cdc"]
    sources += [TESTS / "audio_pcm_test.cpp", TESTS / "audio_pcm_lifecycle_test.cpp", Path(__file__).resolve()]
    sources += sorted((TESTS / "audio_pcm_stubs").rglob("*.h")) + sorted((TESTS / "audio_pcm_stubs").rglob("*.hpp"))
    receipt = {"result": "pass", "sanitizer": args.sanitizer, "native_build": False,
               "emulator_run": False, "physical_audio_verified": False,
               "sources_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
               "outputs": outputs}
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"PASS: independent Python wave/JSON/FNV/SHA-256 verification; receipt {out / 'receipt.json'}")


if __name__ == "__main__":
    main()
