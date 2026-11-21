# Diagnostics and reporting

The native source includes developer telemetry, application-memory dumps and
bounded PCM capture. These are local investigation tools. They are not a public
crash-upload service and this publication does not distribute captures.

## Source ownership

| Area | Files |
| --- | --- |
| Diagnostic display and counters | `Source/platforms/3ds/diagnostics.*` |
| Bottom-screen developer console and automatic test | `runtime/dev_console.*`, `runtime/zone_table.inc` |
| Public Beta bottom screen | `runtime/beta_screen.*`, `native/beta_face.zst` |
| Story steps and chapter checks of the automatic test | `runtime/story_*`, `Tools/3ds/analyze_story.py` |
| Picture cadence of a test (`cadence.bin`) | `runtime/tour_cadence.*`, `Tools/3ds/analyze_cadence.py` |
| Frame telemetry (`frames.csv`, `perf-summary.csv`) | `runtime/frame_telemetry.*` |
| Measurements per process and per draw | `runtime/measured_dump.hpp`, `Tools/3ds/measure_report.py` |
| Improvement switches (`features.txt`) | `runtime/perf_features.hpp` |
| Console GPU guard and hang records | `runtime/gpu_canary*`, `runtime/gpu_sync_ctru.cpp`, `runtime/ctru_gxqueue.c` |
| Power-safe SD writes | `runtime/durable_file*` |
| Application dump orchestration | `runtime/application_dump*` |
| Bounded file writes | `runtime/dump_io.*` |
| PCM capture and NDSP lifecycle | `runtime/audio_control.*`, `runtime/audio_ndsp.cpp` |
| Dump verifier | `Tools/3ds/verify_application_dump.py` |
| GPU hang decoding | `Tools/3ds/gpu_hang_report.py`, `pica_list_decode.py` |
| GPU time of a dumped picture | `Tools/3ds/gpu_jobs_report.py`, `mip_register_check.py` |
| Emulator inspection tools | `Tools/3ds/azahar_debug.py`, `run_azahar_smoke.py`, `azahar_run_report.py` |

Runtime paths in the table are relative to `Source/platforms/3ds`.
Use the scripts' `--help` before a local run; historical runners may require
private baselines or a separately prepared emulator environment.

## Two build flavours

The CMake option `DUSK_GAME_DIAGNOSTICS` (default `OFF`, see
[Building](BUILDING.md#two-build-flavours)) decides what the lower screen shows.
The release name of each comes from `runtime/release.hpp`; it is shown on the
lower screen and written into every dump.

| Flavour | Lower screen |
| --- | --- |
| `OFF`: **Public Beta 01** | `runtime/beta_screen.cpp`: the DuskLight wordmark, the frame rate, the game speed and the frame time, refreshed four times a second. Touching the bar at the bottom writes a full dump (its progress, then DONE or FAILED, in the bar); the eye icon at the top right hides and shows the screen. There is no automatic test in this flavour. An empty `dev-console.txt` on the SD leaves the screen off. |
| `ON`: the test build (**Alpha 21** in this source) | The developer console of `runtime/dev_console.cpp`: the STATS, ZONES and TEST pages, START+SELECT to start or stop the automatic test, A during a test for a snapshot. |

`L+R+A` requests a full dump in both flavours.

## Capture limits

The application dump path can capture readable application ranges and selected
LCD/VRAM supplements. It cannot provide kernel memory or memory from unrelated
processes. Chunked live reads are not an atomic whole-process snapshot. A
`COMPLETE` marker is useful only when the inventory, sizes and hashes verify.
Partial or timed-out captures remain partial.

Use `START+SELECT` to start or stop the automatic benchmark. Press physical
`A` during the benchmark to request a diagnostic snapshot without sending
that press to the game. The tour keeps its reports under
`/3ds/DuskLight/tour/`. Other TEST modes also request these snapshots at route
milestones. They use `/3ds/DuskLight/dumps/benchmark-*` and identify
`source=auto` or `source=manual` in `info.txt`. Two requests for one picture
can share render-owner data and retain separate trigger records.

**Test everything** writes chronological visual pairs under
`/3ds/DuskLight/tour/NNN/visual/`. Each pair contains native-resolution
`top-screen.bmp` and `bottom-screen.bmp` plus `info.txt`. Sample the first
drawn picture after verified zone entry and the loading curtain, the zone
exit, route evidence points, and the first completed picture at least
**10 elapsed seconds** after the entry sample or previous periodic
attempt. The same interval applies to long cinematics and slow rendering.
Time without a completed picture cannot produce a sample. A delay earns one
attempt and starts the next interval from that attempt. Simultaneous reasons
share a pair. The reason bits are entry 1, periodic 2, evidence 4, and exit 8.
The visual track replaces the automatic rich snapshots and legacy room BMPs
in this mode. Manual snapshots still use the rich format.

In the room portion of **Test everything**, `gpu-camera.bin` and `gpu-still.bin`
in each eligible room directory sample one ordinary picture halfway through the
camera and still phases. Each file pairs the submitted PICA words with the
completed GSP jobs for that picture. A stage change, allocation failure,
overflow, missing completion or inconsistent job discards that sample. Read
the files with `python3 Tools/3ds/analyze_gpu_lists.py PATH`. The capture and
validation boundaries carry diagnostic and capture-owner flags in the cadence trace.
The SD writer owns the validated buffer through close, rename and the log receipt.
It writes the original command words and GPU submit and completion ticks in DLPC001
format. The next scheduled sidecar waits if that allocation is still in use.
Room log export, full RAM dump and clean shutdown wait for pending sidecar receipts. The command
payload limit is 512 KiB across the recorder and writer. A failed writer startup
uses synchronous output. The `Native GPU sidecar wall us` line records admission,
submit-lock acquisition, list copy, retirement, capacity wait, queue delay and file
I/O in elapsed microseconds.

The top image comes from the identified LCD transfer destination at the next
ordinary GPU completion boundary. The bottom image comes from the console
buffer before the harness redraws it. The manifest records the captured
frame, submit tick, wall timestamp, actual stage and room, and the harness
zone and phase. Sort by frame within a launch and use wall timestamps across
resumes. The FIFO holds two pairs including any write in progress, with
about 863 KiB of fixed pixel storage. If both slots are full, the owner holds
the sampled picture at admission until a slot frees and logs the wait. An
unavailable LCD or storage error leaves an omission record. The render thread copies only on
sample attempts. The SD writer saves both images, with synchronous fallback if its
thread could not start. Each successful pair uses
518,508 bytes of BMP data plus its manifest and filesystem allocation
overhead, about 178 MiB per hour of periodic samples at maximum rate plus
boundary and evidence pairs.

Cadence uses flag value 2 for capture work on the render owner, 4 for an
interval overlapping a visual, benchmark or GPU-sidecar write, and 8 for an interval containing
a full-dump service. A row can carry several flags. A sample requested after
frame N submits can mark the interval ending
at N+1. A writer span can also cross a zone boundary. The raw submission
interval stays intact and the strict cadence report counts every row. The
writer flag measures overlap, including time waiting on I/O. The visual
manifest and native log report elapsed owner and writer costs separately.

The benchmark's sole HID scanner polls about every 4 ms, including while the
render thread is loading. It queues observed A edges and keeps a captured
hold masked through release, even after the benchmark stops. The manifest
records the request's system tick and last render-observed frame, stage, and
room separately from the completed picture's frame and submission tick.
An early request can have an unknown stage until the first render observation.
A press that HID never publishes or that falls between scanner samples can
still be missed. The render thread owns the actual diagnostic snapshot.

Snapshots include the regular state report, route and story context, recent
frame telemetry, PICA shadow state, available GPU jobs, and LCD images when
the render owner can copy them at an ordinary GPU boundary. Images show the
latest presented buffers and can lag the recorded game state. File writes run
on the SD writer. The manifest reports render-owner capture time and attachment
availability. When the attachment slot is occupied, up to 32 queued records
retain the trigger's frame and location with explicit omissions. Queue
overflow or storage failure leaves an omission entry in the native log.

Select **Story** on the TEST page or set `mode=story` in `tour-auto.txt`
for automatic launch. `first=0` starts at the opening and `limit=3` runs
three steps from the chosen start. Story steps restore in-memory quest
checkpoints, then run tick-indexed controller input through the original game.
The native plan and observed outcomes are in `story-plan.csv` and `story.csv`
inside the tour folder. Run `python3 Tools/3ds/analyze_story.py <tour-folder>`
to check the results. Route passes require the authored exit and a rendered
arrival in a new play scene. The other TEST modes are still available.

Full forensic captures remain available through `full=1` in
`tour-options.txt` or `tour-auto.txt`, or through the TEST page RAM+VRAM
toggle. Test everything with the toggle off attempts at most one automatic
full dump per sixteenth of the itinerary, on a zone failure or loop p95
above 50 ms. Up to eight further dumps capture unusually slow rooms during
the still phase. At least 12 eligible pictures must show a median loop of
100 ms or more, or a mean GPU busy time of 80 ms or more. The trigger
excludes all diagnostic, capture and SD-writer intervals. If fewer than 12
eligible pictures remain in the still phase, it also considers the earlier
ordinary play phases from the same room. A room with a
still-phase dump consumes no second attempt at exit. Both budgets persist
across relaunches. Automatic dumps resume the tour when the file closes.
Outside a benchmark, `L+R+A`
requests a standalone forensic dump. Each dump gets its own folder,
`/3ds/DuskLight/dumps/NNN-dump-YYYYMMDD-HHMMSS/`, holding `dump.dpk` (packed
RAM, VRAM, both LCD buffers, the PICA state sent, shared system pages, recent
frame times and the last GPU jobs), `top-screen.bmp`, `bottom-screen.bmp` and
`info.txt` (build, stage, camera, cutscene, player, heaps and actors). Read it
on a computer with `python3 -I Tools/3ds/verify_application_dump.py dump.dpk
--extract <new folder>` (Python 3.14 or later: the blocks are zstd-compressed
and read with the standard `compression.zstd` module). Input is suppressed while the capture owns the
application flow. Keep ample SD space for repeated full dumps, and do not save
a game after the tour has replaced in-memory story state with its test loadout;
exit and relaunch to resume normal play.
Check the implementation for the active configuration before relying on a
shortcut; older diagnostic contracts do not prove a current runtime path.

## Measurements

From Alpha 21 a full dump (`L+R+A`) first measures a burst of 26 ordinary
pictures, 2 to 3 seconds of the game going on as it was
(`runtime/measured_dump.hpp`): 4 not measured, 12 with the process timers
alone, 6 with every named section timer, 3 with one draw per GPU command list
and 1 that lets those lists complete. `info.txt` gains `[measure ...]`
sections (time per process profile: execute, draw, paint on the game thread,
replay on the drawing thread; named sections; renderer parts) and `dump.dpk`
gains a `gpu-job-owners` blob: the process that owns the model of each timed
draw.

The automatic test measures every zone seven times while it plays: first with
the named sections, then two groups of three back to back (an ordinary
measurement, one with improvements off, an ordinary one). The first group turns
every improvement of `runtime/perf_features.hpp` off (the reference), the
second only one, a different one in every zone. Each measurement is 4 pictures
not measured, 8 with the process timers, 2 with one draw per GPU list and 1 to
let them complete. The results wait in RAM and are written to `<zone>/measure.txt`
when the zone writes its results; a stopped test leaves what it had in
`measure-unfinished.txt` in the test folder.

Read any of them with `python3 -I Tools/3ds/measure_report.py` and a dump
folder, a `measure.txt` or a test folder (`tour/NNN`); `--extracted` adds the
per-draw GPU view of a dump extracted by `verify_application_dump.py`. A
console figure needs a console run; in the emulator the GPU's time is not
measured and a measurement without an improvement the emulator cannot time
reads zero.

`frames.csv` also carries the GPU's time per picture since Alpha 16
(`gpu_busy_ms`, `gpu_lists_ms`, `gpu_copies_ms`, `gpu_idle_ms`,
`gpu_longest_ms`, `gpu_jobs`). `Tools/3ds/gpu_jobs_report.py` reads the last
GSP jobs and the command lists of the last complete picture from an extracted
dump.

## Console GPU guard

Up to Alpha 10 the first draw of every new kind (vertex format, shader,
textures and raster state) ran alone and had to complete before later draws of
that kind were batched. Since Alpha 11 these canaries are off by default
(`gpu-canary-on.txt` brings them back): on the console they were the
first-time stutter, and no P3D draw has stopped the PICA since Alpha 0. The
rest of the guard stays. A job silent for six seconds is a hang. The guard writes the evidence before
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
| `picture-rate.txt` | Experimental, incomplete picture rate lock: the pictures per second (5 to 30). The start of each picture is held to a whole number of display refreshes after the previous start; pictures are still presented when their painting ends, so the rate on screen is not steady yet. The game speed is the same with or without it. Without the file the rate is unlocked. |
| `draw-every-tick.txt` | Rebuild draw lists on every logic step. |
| `gx-sync-wait.txt` | GX sync calls wait for the whole GPU again. |
| `readback-transfer.txt` | Read render targets back with the transfer engine. |
| `readback-check.txt` | Read back with both paths and log whether they agree. |
| `gpu-canary-off.txt` | Switch the GPU guard off. |
| `gpu-canary-on.txt` | Run the first draw of each new kind alone again (the canary; off since Alpha 11). Without it the guard keeps the stopped-job watchdog, the stop classes and the learned skips. |
| `loader-core0.txt` | Keep the DVD thread (archive reads and decompression) on the game's core instead of the second core (Alpha 11). |
| `strict-draws.txt` | Make an unsupported draw fatal instead of left out and counted. |
| `rigid-single-on.txt` | Use the single-texture rigid shader route (A/B). |
| `camera-log.txt` | Log cutscene camera values (slow). |
| `tour-options.txt` | Settings of a test started with START+SELECT: `mode= scale= limit= full= exit= seed=`, `hold=` seconds of the TESTS FINISHED screen before the console powers off (default 900, `0` no screen and no power-off), `menu=0` leaves out the menu phase, `probe=0` the probe of the other cores at the end (`core_probe_*` lines of `tour.txt`: a busy thread on cores 1 to 3 against core 0, GSP request and cache flush times). |
| `dump-level.txt` | zstd level of the packed dump (default 1). |

Alpha 7 comparison switches. Each new path is on without its file; the file's
first character selects the older behaviour.

| File | Effect |
| --- | --- |
| `hud-tape.txt` | Pictures per recording of the 2D pass: `1` draws it in every picture, `2` (default) to `8` run the recorded PICA commands again in between. In a recorded picture the retained screens replay what did not change. |
| `gpu-arena-256k.txt` | GPU command arena of 256 KiB instead of 1 MiB. |
| `gpu-chunk-draws.txt` | Draws per incremental submission (default 64). |
| `material-memo.txt` | J2D material memo: `0` off, `v` verify every replay. |
| `state-hash.txt` | Native state key: `0` rehash per state epoch, `v` verify the incremental hash. |
| `immediate-shortcuts.txt` | `0`: no unchanged-draw copies or key memo for immediate draws. |
| `native-lean-replay.txt` | `0`: plan every replayed shape the complete way. |
| `fallback-records.txt` | `0`: stand-in and relaxed-alpha draws are not recorded. |
| `palette-split.txt` | `0`: meshes with an overflowing matrix palette take the CPU vertex route. |
| `mt-single-attribute.txt` | `0`: position-only formats take the CPU vertex route. |
| `mt-normal-texgen-off.txt` | Texture coordinates from the normal on the CPU vertex route. |
| `mt-paired-texgen-off.txt` | Environment maps on skinned models (one texture matrix per bone) on the CPU vertex route, as before Alpha 8. |
| `mt-third-coordinate-off.txt` | Materials whose texgens read three GX texture coordinates on the CPU vertex route (plan reject 61), as before Alpha 8, instead of 36-byte GPU records. |
| `swapped-arrays-off.txt` | Indexed vertex arrays stored in the console's byte order (tables built into the game, such as the simple shadow's volume) refused by the multi-texture decoder (decode reject 5, CPU vertex route), as before Alpha 8. |
| `fog-alpha-ramp-off.txt` | A fog curve other than the linear one on a material that leaves the vertex alpha unread is carried in that alpha (CPU vertex route, plan reject 20), as before Alpha 8, instead of the ramp texture. |
| `alpha-union.txt` | `0` one pair of draws per triangle, `1` two mesh passes on the CPU route, `2` (default) two mesh passes on the GPU. |
| `tev-sum-rules.txt` | `0`: the first TEV planner only (three- and four-term sums take the stand-in). |
| `indirect-constant.txt` | Largest warp, in texels, drawn on the GPU vertex route (its mean as a constant offset, and since Alpha 9 the rest per vertex, see `vertex-warp-off.txt`). No file: every warp (Alpha 8). `0`: exact plans only, the rest on the CPU vertex route (Alpha 7). `-1`: off. |
| `vertex-warp-off.txt` | A warp on the GPU route is its constant offset alone, as in Alpha 8 (the distortion no longer changes from vertex to vertex nor moves), instead of the map sampled at every vertex. |
| `warp-repeat-off.txt` | Every warped draw samples its warp map at every vertex in every picture, instead of copying the numbers kept from an earlier draw of the same mesh under the same warp. |
| `arena-shrink-off.txt` | A draw whose compact records are decoded into the frame's geometry arena keeps its whole reservation (60 bytes per vertex for records of 32 or 36), as before Alpha 9, instead of returning the part they do not fill. |
| `camera-divisor-off.txt` | A warped unit projected through the camera divides per vertex with a clamped divisor when vertices lie behind the camera plane, as in Alpha 8 (the image's border over those triangles), instead of dividing per pixel. |
| `refraction.txt`, `particle-mask.txt` | Particle effect levels (see `developer_script.cpp`). |

Alpha 8 comparison switches, same convention.

| File | Effect |
| --- | --- |
| `screen-tape.txt` | Retained 2D screens: `0` every J2D screen is drawn in every picture (Alpha 7), `1` (default) an unchanged screen replays the PICA commands of its last recording, `v` it is drawn as well and the two command runs are compared (`verified=`, `mismatches=` in the log). |
| `tick-logic-off.txt` | The play scene's collision pass, moving backgrounds, particles and frame counter run once per picture again instead of on every logic tick (Alpha 7; leaves colliders of deleted actors registered between ticks). |

Alpha 10 switches (the second-core worker, see
[Architecture](ARCHITECTURE.md#second-core-worker)).

| File | Effect |
| --- | --- |
| `gx-worker.txt` | First character: `0` every draw on the game thread, as in Alpha 9. `w` the worker on core 2 (the default without a file; also turns it on again after it was learned off). `d` the same queue with no second thread: the game thread draws what is queued at each sync, which proves the queue's records apart from the threading. A `v` behind `w` or `d` adds the verify checks: every bank delta is applied to a copy and compared with the bank, the sum of the bank travels with each shape, and a GPU call reached with shapes queued logs the code addresses on its stack. Verify costs about 40 % of a picture. |
| `gx-worker-learned.txt` | Written by the game, not a switch to create: `off build=<label>; ...`. The start before had the worker on and ended on a fatal error, a CPU exception or a GPU stop, so this build leaves it off from then on. Delete the file (or put `w` in `gx-worker.txt`) to turn it on again. Another build ignores and removes it. |
| `mixer-plain.txt` | The audio mixer renders every voice into its own cleared buffer and then adds it, as in Alpha 9, instead of adding it straight to the stereo mix. |
| `mixer-verify.txt` | Every audio subframe is rendered both ways from the same state and compared; the log counts subframes and mismatches. |
| `mixer-verify.txt` (Alpha 14) | With the fixed-point mixer, each subframe is also rendered by the float path; the log prints the largest difference and the samples over 1/32768 (must be 0). |
| `mixer-old.txt` | The Alpha 13 mixer: 48 kHz, float. `mixer-48k.txt` keeps 48 kHz only, `mixer-float.txt` keeps the float voices only. |
| `gx-late-runs-off.txt`, `gx-sections-off.txt`, `gx-bloom-job-off.txt`, `gx-shadow-arm-job-off.txt`, `gx-palette-fast-off.txt`, `gx-rest-measured-off.txt` | Turn off one Alpha 14 core-2 change each: particles after the translucent lists queued, the refraction check as a queued section, bloom as a job, the shadow-caster start as a job, the palette refresh without the GPU, the measured rest trial (back to the Alpha 13 rest rule). |
| `compiled-lists.txt` | Display-list templates: `0` the Alpha 13 compiled-list cache, `v` every template replay is checked against the interpreter (`mismatches=` must stay 0). |
| `lod.txt` | Level of detail, read once at boot: `none` the original game, `alpha13` Alpha 13, then `key=value` words (`far bg actor parts shadow grass skin census`); the defaults and their meaning are in `include/d/d_native_lod.h`. |
| `batch-instances.txt` | Instance batches (Alpha 17): `0` the original per-model draws of the rope bridge and monkey cage, `v` verify (every model also builds its own differed lists, compared byte for byte; log `verify models N differed 0`); no file: on. Log lines `Native instance batches ...` every 300 pictures. |
| `gpu-trace.txt` | Lists one picture's draws in the log (`Native GPU trace N: sig= indices= uniforms= ... pixels=`): a picture in the middle of the test's still phase, or with `e` five frames before the end of the last phase. Two logs of the same zone show which draw differs between two modes. |

Alpha 11 to Alpha 17 switches. Each path is on without its file unless the
row says otherwise.

| File | Effect |
| --- | --- |
| `transition-hold-off.txt` | Zone transitions reveal the new zone as soon as the play scene's create is done, as the original, instead of holding the curtain until the zone is ready (Alpha 12). |
| `texture-compact-off.txt` | Textures keep their 32-bit storage instead of the 16-bit plans that hold the same picture (Alpha 12). |
| `native-packets.txt` | Native packets, off by default. Letters: `0` off, `b` room models, `a` all models, `d` direct records, `c` captured records, `v` verify. Where the worker shares the game thread's core, packets stay on the original path unless `f` is given. |
| `pica-blocks.txt` | Kept PICA command words for unchanged recorded shapes, off by default: `0` off, `1` on, `v` verify (the normal path draws every shape and its words are compared with the block's). |
| `immediate-fog.txt` | Fogged immediate draws recorded, off by default: `1` on, `v` verify, `0` off. |
| `particle-runs.txt` | Particle emitter loop: `0` every emitter draws on its own (Alpha 11), `1` emitters with nothing on screen set no GX state, `2` (default) emitters of one resource also share one batch where that cannot change the picture. `particle-size-cull.txt` (pixels, off by default) also leaves out emitters whose whole footprint is smaller. |
| `skin-matrix-runs-off.txt` | J3D loads the matrices of a skinned shape slot by slot instead of in contiguous runs. |
| `audio-core.txt` | Where the audio mixer runs: `auto` (no file) or `3` core 3 when a thread there provably runs beside the caller, else core 2; `2` core 2 without a check (up to Alpha 14); `1 [percent]` core 1 under the system's CPU time limit (40 % if not given), else core 2. |
| `logic.txt` | Exact game-logic rules (`include/d/d_native_logic.h`), read once: `none` every rule off (Alpha 14), `verify` every rule on in verify mode, then `key=N` (`0` off, `1` on, `2` verify) for `procindex actorindex envelope frsqrte fmaf rnd math kytag12 ccpre crrskip gdwrite`; `census=1` logs a line every 300 pictures. Verify modes run the original next to each rule and count differences. |
| `gx-overlap.txt` | The worker's share of a picture (Alpha 15): `0` Alpha 14 as it was, `1` within the painter only, `2` (default) the painter ends without a sync and the worker draws on during the next logic tick. |
| `gpu-cull-off.txt` | Off-screen blocks of 64 triangles of resident rigid draws are submitted too (Alpha 16). `gpu-cull-refine.txt` uses smaller bounds for large draws instead. |
| `gpu-first-part.txt` | Draws in the first submission of a picture (default 8, then 16, 32, 64); `0` restores Alpha 15. |
| `gpu-phases.txt` | Command lists split at every painter phase, so the GPU's time is known per phase. |
| `gpu-draw-times.txt` | One draw per command list in every picture, so each draw is timed (a measurement does this for its own pictures only). |

Alpha 21 switches.

| File | Effect |
| --- | --- |
| `features.txt` | The performance improvements of the session (`runtime/perf_features.hpp`), read once at boot: `all`, `none`, or words such as `lod=0 mips=1` for `lod shadow skin vertex mips logic paint`. Without the file every one is on except `mips`. The log names the session's choice (`Native improvements: ...`). |
| `zelda.lod` | Not a switch: the level-of-detail store made by `Tools/3ds/model_lod.py` ([Game data](GAME_DATA.md)). Without it `lod` and `shadow` do nothing. |
| `measure-at.txt` | Picture numbers at which a measurement is taken in a scripted run, such as `600 900r 1200 1500-lod` (`r` a reference with the improvements off, `-name` without that improvement alone); each is appended to `measure.txt` in `/3ds/DuskLight/`. |
| `boot-loadout.txt` | `SEED [wolf]`: a scripted zone start (boot stage) gets the automatic test's random state for that hexadecimal seed, the three twilights cleared for a human zone; without it the stage starts with an empty save. |
| `dev-console.txt` | Public Beta flavour only: keep its lower screen off. |

Emulator-only test switches: `gpu-canary-test.txt` (report the Nth new draw
as a hang), `gpu-transfer-test.txt` (`frame=N`: report the next texture
copy as a stopped transfer) and `gx-worker-test.txt` (`fatal=N`: a fatal
error on the Nth shape the worker draws). Further investigation switches are
read where they are implemented; search the runtime for `sdmc:/3ds/`.

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
