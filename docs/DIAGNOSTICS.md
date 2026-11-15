# Diagnostics and reporting

The native source includes developer telemetry, application-memory dumps and
bounded PCM capture. These are local investigation tools. They are not a public
crash-upload service and this publication does not distribute captures.

## Source ownership

| Area | Files |
| --- | --- |
| Diagnostic display and counters | `Source/platforms/3ds/diagnostics.*` |
| Bottom-screen developer console and automatic test | `runtime/dev_console.*`, `runtime/zone_table.inc` |
| Frame telemetry (`frames.csv`, `perf-summary.csv`) | `runtime/frame_telemetry.*` |
| Console GPU guard and hang records | `runtime/gpu_canary*`, `runtime/gpu_sync_ctru.cpp`, `runtime/ctru_gxqueue.c` |
| Power-safe SD writes | `runtime/durable_file*` |
| Application dump orchestration | `runtime/application_dump*` |
| Bounded file writes | `runtime/dump_io.*` |
| PCM capture and NDSP lifecycle | `runtime/audio_control.*`, `runtime/audio_ndsp.cpp` |
| Dump verifier | `Tools/3ds/verify_application_dump.py` |
| GPU hang decoding | `Tools/3ds/gpu_hang_report.py`, `pica_list_decode.py` |
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

The current original-game integration recognizes `START+SELECT` to start or
stop the automatic stage tour. A fresh interactive tour enables RAM/LCD/VRAM
dumps per zone by default; the bottom-screen TEST page can toggle them. The
tour records timing, memory, renderer/audio reports and screenshots under
`/3ds/DuskLight/tour/`; full application captures go under
`/3ds/DuskLight/dumps/`. Resumed tours retain their recorded settings, and
`tour-auto.txt` automation uses its explicit `full=` setting. The TEST page
also offers main-stage and all-room modes. These scripted probes do not prove
complete story progression, audible speaker output or every hardware feature.

`L+R+A` requests a standalone developer dump. Each dump gets its own folder,
`/3ds/DuskLight/dumps/NNN-dump-YYYYMMDD-HHMMSS/`, holding `dump.dpk` (packed
RAM, VRAM, both LCD buffers, the PICA state sent, shared system pages, recent
frame times and the last GPU jobs), `top-screen.bmp`, `bottom-screen.bmp` and
`info.txt` (build, stage, camera, cutscene, player, heaps and actors). Read it
on a computer with `python3 -I Tools/3ds/verify_application_dump.py dump.dpk
--extract <new folder>`. Input is suppressed while the capture owns the
application flow. Keep ample SD space for repeated full dumps, and do not save
a game after the tour has replaced in-memory story state with its test loadout;
exit and relaunch to resume normal play.
Check the implementation for the active configuration before relying on a
shortcut; older diagnostic contracts do not prove a current runtime path.

## Console GPU guard

The first draw of every new kind (vertex format, shader, textures and raster
state) runs alone and must complete; later draws of that kind are batched.
A job silent for six seconds is a hang. The guard writes the evidence before
any further GPU or GSP call, then asks for a power cycle:

| File in `/3ds/DuskLight/` | Contents |
| --- | --- |
| `gpu-hangs.txt` | Readable history: the silent job, its classification and the last GPU jobs. |
| `gpu-hang-NN.bin` | Binary record: job words, shadow registers, uniforms, command list, last 32 jobs, draw data. Decode with `Tools/3ds/gpu_hang_report.py`. |
| `gpu-skip.txt` | What the next start does differently, per build (`build=` first line). |
| `gpu-verified.bin` | Draw kinds this console has completed; they no longer run alone. |
| `gpu-journal.txt`, `gpu-session.txt` | Power-safe markers that let the next start find a freeze. |

A silent transfer job is a transfer stop, not a draw: the next start avoids
in-frame transfer jobs, and a second stop also skips offscreen targets
([ADR 0003](decisions/0003-cpu-readbacks-and-transfer-stops.md)). An
unexplained stop in a batched list runs every draw alone for one start only.
Entries learned by another build are moved to `gpu-skip-old.txt`.

## SD switches

Empty files in `/3ds/DuskLight/` that change runtime behavior for comparison.
They are developer controls, not settings; remove them for normal play.

| File | Effect |
| --- | --- |
| `real-speed-off.txt` | One logic step per picture (the game slows down with the frame rate). |
| `draw-every-tick.txt` | Rebuild draw lists on every logic step. |
| `gx-sync-wait.txt` | GX sync calls wait for the whole GPU again. |
| `readback-transfer.txt` | Read render targets back with the transfer engine. |
| `readback-check.txt` | Read back with both paths and log whether they agree. |
| `gpu-canary-off.txt` | Switch the GPU guard off. |
| `strict-draws.txt` | Make an unsupported draw fatal instead of left out and counted. |
| `rigid-single-on.txt` | Use the single-texture rigid shader route (A/B). |
| `camera-log.txt` | Log cutscene camera values (slow). |
| `tour-options.txt` | Settings of a test started with START+SELECT: `mode= scale= limit= full= exit=`. |
| `dump-level.txt` | zstd level of the packed dump (default 1). |

Emulator-only test switches: `gpu-canary-test.txt` (report the Nth new draw
as a hang) and `gpu-transfer-test.txt` (`frame=N`: report the next texture
copy as a stopped transfer). Further investigation switches are read where
they are implemented; search the runtime for `sdmc:/3ds/`.

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
