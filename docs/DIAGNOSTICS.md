# Diagnostics and reporting

The native source includes developer telemetry, application-memory dumps and
bounded PCM capture. These are local investigation tools. They are not a public
crash-upload service and this publication does not distribute captures.

## Source ownership

| Area | Files |
| --- | --- |
| Diagnostic display and counters | `Source/platforms/3ds/diagnostics.*` |
| Application dump orchestration | `runtime/application_dump*` |
| Bounded file writes | `runtime/dump_io.*` |
| PCM capture and NDSP lifecycle | `runtime/audio_control.*`, `runtime/audio_ndsp.cpp` |
| Dump verifier | `Tools/3ds/verify_application_dump.py` |
| Emulator inspection tools | `Tools/3ds/azahar_debug.py`, `run_azahar_smoke.py`, `azahar_run_report.py` |

Runtime paths in the table are relative to `Source/platforms/3ds`.
Use the scripts' `--help` before a local run; historical runners may require
private baselines or a separately prepared emulator environment.

## Capture limits

The application dump path can capture readable application ranges and selected
LCD/VRAM supplements. It cannot provide kernel memory or memory from unrelated
processes. Chunked live reads are not an atomic whole-process snapshot. A
`COMPLETE` marker is useful only when the inventory, sizes and hashes verify.
Partial or timed-out captures remain partial.

The current original-game integration recognizes the `L+R+A` developer dump
shortcut. Input is suppressed while the capture owns the application flow.
Check the implementation for the active configuration before relying on a
shortcut; older diagnostic contracts do not prove a current runtime path.

PCM receipts describe pre-device mixer output, range validity, queue submissions
and observed completions. A varying waveform does not prove that physical
speakers were audible or that every game sound is correct.

## Useful public report

Include the commit, profile and overrides, SDK versions, console model or
emulator version, data revision identifier, scene and short reproduction steps.
Describe the expected result and actual failure, and include the first relevant
sanitized error or assertion. Synthetic reproduction code is particularly useful.

Keep raw application RAM, VRAM/LCD captures, savedata and game-content recordings
out of public issues and pull requests. A minimal text summary can describe
those observations without distributing the captured contents. Never include
credentials or console system files.
