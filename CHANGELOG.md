# Changelog

This file records public source milestones. It does not announce a binary release.

## Unreleased — Alpha 21 (measured per process and per draw; six improvements behind switches)

Every figure below is from the built-in emulator (instruction time: CPU work
only, the PICA's time is not in it) unless it says console. Nothing of this
has run on a console yet.

Added
- **Measured dump.** A full dump (L+R+A) first measures a burst of 26
  ordinary pictures, 2 to 3 seconds, the game going on as it was
  (`runtime/measured_dump.hpp`): 4 not measured, 12 with the process timers
  alone (every process profile: its execute method, its draw method, the game
  thread painting its models, the GPU-owning thread drawing them; exclusive
  times), 6 with every named section timer, 3 with one draw per GPU command
  list, 1 to let those lists complete. `info.txt` gains the `[measure ...]`
  sections and `dump.dpk` the `gpu-job-owners` blob: the process that owns
  the model of each timed draw (`J3DModel::mNativeOwner`, carried by the
  queued shape and the remembered shadow caster to the GSP job record).
- **The automatic test (START+SELECT) measures every zone seven times** while
  it plays: the first with the named sections, then two groups of three that
  start right after each other: an ordinary measurement, one with
  improvements off, an ordinary one. The first group turns every improvement
  off (the reference), the second a single one, the next in every zone. Each
  measurement is 4 pictures not measured, 8 with the process timers, 2 with
  one draw per GPU list (time, draws and indices summed by owner on the
  console itself, `ctru_gxqueue.c`) and 1 to let them complete. Only the
  phases that count in a zone's figures are measured. The second-core
  worker's own trials do not start inside a measurement and a measurement
  waits for one that runs (`gx_worker_runtime.hpp`). The results wait in RAM
  and are written to `<zone>/measure.txt` where the zone writes its results;
  what a stopped test leaves goes to `measure-unfinished.txt`.
- `measure-at.txt` on the SD takes the same measurements in scripted runs
  (`"600 900r 1200 1500-lod"`: `r` a reference, `-name` without that
  improvement) and appends them to `measure.txt`.
- `runtime/perf_features.hpp`: one switch per improvement, asked at the moment
  of each decision so that it can change between two pictures.
  `features.txt` (`all`, `none`, `mips=1 lod=0 ...`) chooses them for a
  session; a STATS-page line on the bottom screen turns them all off and on
  (with an emulator save state: the same game frame with and without them).
- `Tools/3ds/measure_report.py <dump folder | measure.txt | test folder>`:
  time by process, named sections (also inside each process), the GPU's time
  by owner and painter phase; for a zone one line per measurement and each
  measurement with improvements off against the ordinary ones around it; for
  a test folder a table of what the improvements are worth together and each
  by itself over the zones, with the standard error. `--extracted` adds the
  per-draw view of a dump.
- **Levels of detail** (switches `lod`, `shadow`, on). `Tools/3ds/model_lod.py`
  makes `zelda.lod` (14.5 MiB) from the owner's own `zelda.twilight`: up to
  three reduced index lists per shape over the same vertex records, cut into
  the runtime's own draws (proven against the runtime's command processor over
  the 23,113 display lists of the disc, deterministic). A far shape is drawn
  with the coarsest level whose error stays under 0.75 pixel (never Link,
  never an actor in an event), every shadow caster with the coarsest under
  0.75 pixel of its shadow tile, and a small caster's shadow is a blob. A
  level is drawn only where it is shorter than the list the off-screen cull
  would have left. Indices: village and Hidden Village -18 to -21 %, Snowpeak
  -18 %, spring path -8 %, nothing in the field and the mines (their shapes
  hold the camera). Without `zelda.lod` on the SD nothing changes.
- **Vertex programs per kind of material** (switch `vertex`, on). Four variants
  of the multi-texture vertex program in the same shader binary (490 of the
  512 words) run only what lit draws with fog need; a change of variant sends
  no shader code. 22 to 24 % fewer vertex instructions per picture in five
  scenes, the same picture byte for byte. A GPU stop learned for a variant
  turns the variants off at the next start. `DVLE_s` is read through
  `runtime/ctru_shader_abi.hpp` (its layout depends on the enum size).
- **Fewer draws for skinned models** (switch `skin`, on). The matrix groups of
  a skinned shape merge into runs that load up to 20 matrices at once. Exact
  for unweighted bones; a weighted bone takes its normal from its position
  matrix only while that is within 4 degrees of J3D's (bound in
  `skin_palette.hpp`, checked against 40,000 random blends), otherwise the
  shape is drawn the original way that picture. About 30 draws fewer where
  villagers or Bulblins are in view (village 295 -> 263, Hidden Village
  395 -> 360), at most 19 of 96,000 pixels by 4 levels. A shape drawn at a
  reduced level of detail that picture keeps its own groups (the levels are
  cut for them; `J3DShape::drawMtxGroups`), so with `zelda.lod` on the SD the
  merge is left to the near shapes. Hidden Village, per picture: reference
  393.5 draws and 141.5k indices, everything on 389 and 111.1k, without `lod`
  358 and 140.1k (30 draws against 29,000 indices: the console's table per
  improvement says which is worth more there).
- **Cheaper material packets** (switch `paint`, on). Display-list replays
  answer held registers before the call, a vertex-setup list whose values and
  bindings are all in place is not run (it was 10 ms of the village's paint
  that no timer named), and J3D's current matrix goes as typed writes. Exact
  (48,894 replays and 11,355 held lists verified, 0 mismatches). Paint -6 to
  -12 % (Hidden Village 45.7 -> 40.3 ms, village 34.3 -> 30.8, pond
  32.9 -> 30.8).
- **Fish out of view rest** (switch `logic`, on). A fish that the last picture
  culled, swims calmly, is over 3000 units from the player and has no rod or
  frog near does not run, as the game's own NOEXEC actors. Fishing pond:
  update 5.9 -> 3.55 ms per logic tick. It changes which random numbers the
  game draws afterwards.
- **Mip levels for world textures** (switch `mips`, off). Levels made at
  conversion for J3D world textures authored without them (768 KiB cap, never
  under 3 MiB of free linear memory). Off: once the levels reached the draws
  the picture changed where it can be seen (30 to 39 % of the pixels, sign
  lettering goes soft) and what the PICA saves by it is not measured.
  `Tools/3ds/mip_register_check.py` reads a dump's texture registers.
- `boot-loadout.txt` (`SEED [wolf]`): a scripted zone starts with the automatic
  test's random state for that seed.

Changed
- Display-list commands written by J3D and GD go out as one run of bytes per
  command with one read of the thread's current list, instead of one read (a
  call on this target, `-mtp=soft`) per byte. Same bytes (host test, 1,386
  cases); the draw phase is 35 % cheaper (village 16.2 -> 10.3 ms).
  `logic.txt gdwrite=0` restores the old writer.
- The collision skip of Alpha 16 (`crrskip`) also keeps barrels, pumpkins and
  items on the ground (verify mode, per profile: 45,674, 94,090 and 30,641
  checks, 0 mismatches).
- Process timers are exclusive: a scene that runs its children inside its own
  method is no longer charged for them.

Fixed
- Host tests the batch had left behind, found by running every runner that
  compiles a changed file (`Build/alpha21/host_tests.py`): old stub
  signatures, a texture header that pulled in the command processor, a
  program counter that read through a null pointer.
  `test_gx_rigid_texgen` and `test_gx_indirect` pass again (their shader
  interpreter did not know `ifu` blocks).

Checked
- Emulator, the merged build with every switch on against its reference in the
  same run, the player standing: village 72.8 -> 62 ms of game-thread time
  per picture, fishing pond 48.6 -> 43.1, Hidden Village 86.1 -> 77.1 (the
  exported package); a measurement without an improvement the emulator cannot
  time reads zero.
- Ten slow zones (village, spring path, field, Hidden Village, Snowpeak,
  fishing pond, Zora's Domain, Arbiter's Grounds, Lakebed Temple, Goron
  Mines) run with everything on: no stop, no draw left undrawn.
- The exported package, the automatic test over 12 zones from a scripted
  start (seed 24da0b9b, `zelda.lod` on the SD): complete, no failure, no draw
  left undrawn, no held-list mismatch in 1.6 million replays, seven
  measurements in every zone's folder. There the view moves between two
  measurements: everything on against the reference is 21 draws and 4,755
  indices fewer per picture and -1.0 +- 1.6 ms, which twelve zones cannot
  tell from zero; the rows of one improvement have two zones each and say
  nothing yet.
- The same game picture with and without the improvements (the game's own
  capture from one save state; two passes with them on give the same bytes).
  The exported package: Hidden Village 101 of 96,000 pixels (4 by more than
  16 levels), village 1,069 (175 by more than 16: small things far away that
  the fish rule's random numbers move). Before the rule for skinned shapes
  with a level: Forest Temple 0, Snowpeak 175 (none by more than 16).
- START+SELECT from the title runs the story steps and the room zones, writes
  each zone's seven measurements into its folder; a full dump is written and
  verified.
- Not run on a console. What the console must say: the automatic test's table
  per improvement.

## Unreleased — Alpha 18 to Alpha 20 (automatic tours, minimap, story drivers)

Test builds between Alpha 17 and Alpha 21. Their figures are from the
built-in emulator unless they say console; Alpha 20 has not run on a
console.

Added
- Alpha 18: an automatic tour of 81 steps with loads, fights and
  cutscenes (`tour-auto.txt`). A pressed during a tour writes a diagnostic
  marked as manual while the script goes on; a tour stopped by a crash
  records the failed step and continues from the next one on the next
  start. Console: the first 191 cases of the full route took about
  2 h 17 min.
- Alpha 19: **Test everything** (START+SELECT), 399 cases over the game's
  rooms and story. Both screens are captured about every ten seconds of
  play and at each room's entry, exit and milestones; each room entry
  keeps two GPU command samples (`gpu-camera.bin`, `gpu-still.bin`); up to
  24 automatic RAM and VRAM dumps per run (`RAM+VRAM/ZONE` asks for one
  per case). An interrupted case is retried once on the next start.
- Alpha 20: story drivers for the two fights that stopped the story test:
  the west portal (case 26) holds B until the last two enemies are tied,
  and the Darknut of the Temple of Time (case 61) locks on, circles the
  shield and strikes through the opening with the game's own buttons.
  Both now finish with no draw left undrawn.

Changed (Alpha 20)
- Minimap: the paths of every visited room were redrawn every 15 pictures
  even when nothing changed. The background is reused; doors, icons and
  Link's position are still drawn at each refresh. Outside the Citadel
  with 18 rooms visited, the median refresh went from 1,030 draws and
  477.76 ms to 4 draws and 7.10 ms; the 46,656 pixel indices and 408
  palette bytes matched in all 348 refreshes compared.
- A synchronous primitive replay reads the live register bank instead of
  copying its 19,960 bytes per list; compatible sky and foliage geometry
  is packed in one pass.
- Terrain and fog passes share one selection of visible indices: 33,034
  triangles fewer over 10 paired pictures, the same pixels.
- Stars wholly outside the view are not projected or sent (81-90 % fewer
  star vertices in four night replays).
- Actors: animation tracks are prepared at load and redundant collision
  checks are skipped. The 20 archers took 11.37 % fewer ARM instructions,
  the 20 cats 4.0-5.0 %, the 24 Snowpeak bats 8.33 %; movements, events,
  random sequences and 12,546 sphere collision decisions matched.
- HUD panes without rotation build their local matrix directly (weighted
  instructions per matrix 695.84 -> 102.90 on the console's recovered
  data, the same 48 bytes in all 15,574 entries).
- GPU list captures are written by the SD thread; the next picture goes on
  while they are written.

Fixed (Alpha 20)
- Three draws left out in Alpha 19: the Lakebed slime's depth pass now
  follows the model's own entry decision, and the ice room keeps projected
  coordinates whose divisor is negative over the whole primitive.
- End of a tour: the input and SD threads could exit before the main
  thread asked for their handles, which lost the last capture and the
  timing CSVs. Both handles are taken before the stop signal.

Measured
- Emulator, the same 18 field rooms as Alpha 19: 13.01 -> 18.24 FPS
  (p95 293.00 -> 83.76 ms); outside the Citadel 11.71 -> 23.33 FPS.
- Console, Alpha 19: 253 of 399 cases complete; 14,369 of 30,329
  capture-free pictures over 50 ms; manual play in Ordon 15.03 FPS.

## Unreleased — Alpha 17 (repeated models drawn together)

Changed
- Instance batches: the rope bridge (OBJ_BRG: 23 planks, 46 knots, one model
  data per kind) and the monkey cage bars (OBJ_SO) run `calc()` per model but
  enter one carrier per kind with the light set once
  (`mDoExt_nativeInstancesUpdateDL`). The carrier draws up to 8 models per
  draw: their matrices in GX slots 0..7 (made as J3D makes them) and a merged
  list with a matrix index in front of every vertex (`J3DNativeInstances.h`,
  `J3DShape.cpp`). The 24 rope line materials with the same texture, colour
  and tevStr are one strip. The census on F_SP104 shows the bridge's matrix
  groups going from 69 to 2 per picture.
  - Refusals (the original per-model code runs instead):
    - a material animation track that is on (dRes gives every loaded
      material an empty `J3DMaterialAnm`, which does not refuse);
    - light types 9/10;
    - the parts rule;
    - skinned, deformed or callback models;
    - z-sorted lists.
  - Switch: `batch-instances.txt` with `0` (original) or `v` (verify: every
    model also builds its own differed lists and compares them byte for byte).
- Measured in the built-in emulator, real speed, pictures 300-900:

  | Scene | Alpha 15 | Alpha 17 |
  |---|---|---|
  | F_SP104 spawn | 56.5 ms (17.7 FPS) | 40.2 ms (24.9 FPS) |
  | Epona ride | 50.7 ms (19.7 FPS) | 38.0 ms (26.3 FPS) |
  | Epona/Ilia | 47.7 ms (21.0 FPS) | 36.6 ms (27.3 FPS) |
  | village | 84.0 ms | 86.2 ms (no change) |
  | goats | 53.5 ms | 53.9 ms (no change) |

- Checks:
  - verify: 32,629 model comparisons, 0 differences;
  - bridge view (F_SP104 1 5): on against off differs less than off against
    off (run-to-run sway);
  - 8-zone gate complete.

## Unreleased — Alpha 16 (flicker, exit crash, console GPU time)

From the owner's 15 Alpha 15 dumps and a Luma crash dump.

Fixed
- 2D and water flicker: with the overlap, `CommandProcessor::adopt` refused on
  the worker while the game thread recorded a J2D material (the material memo),
  so queued shapes and runs were dropped silently (dump 014: 7,272). `adopt`
  now refuses only on the recorded processor; log `queued draws not drawn 0`.
- Exit abort and kernel crash at power-off/HOME: devkitARM's libstdc++ keeps one
  process-wide `__cxa_eh_globals` (no TLS keys), so the six workers' concurrent
  `ThreadExit` unwinds in the shutdown barrier corrupted each other and
  `unwind_phase2` aborted; `_exit` then unmapped the heap under live workers.
  `__cxa_get_globals` is wrapped with a per-thread block (`runtime/eh_globals.cpp`,
  build check `Tools/3ds/check_eh_globals.py`); audio stops and joins first; the
  SD writer is joined.

Changed
- GPU (console): off-screen 64-triangle blocks of resident rigid draws are not
  submitted (`gpu-cull-off.txt`), and a picture's first submissions are 8/16/32
  draws (`gpu-first-part.txt`). The owner's dumps showed the PICA busy 66-77 ms
  of 80-86 ms pictures, ~0.38 ms per 1000 indices even off screen.
- GPU time per picture in frames.csv (`gpu_busy_ms` ... `gpu_jobs`), a periodic
  log line, `gpu-phases.txt` and `gpu-draw-times.txt` diagnostics and
  `Tools/3ds/gpu_jobs_report.py`.
- Dungeon logic: CrrPos is replayed for unchanged doors/pots when the input
  image and every relevant collision slot version match (`logic.txt` crrskip);
  30-zone verify tour 259,763 comparisons, 0 mismatches.

## Unreleased — Alpha 15 (drawing overlaps the next picture's logic, audio on core 3, exact logic cuts)

Built-in Azahar: 30 zones at real speed (seed `54737253`) Alpha 14 73.6 ->
about 54 ms per picture (zones under 20 FPS 23 -> 16); 8 zones (one tick per
picture) 54.8 -> 41.6 ms; F_SP104 r1 13 -> 25 FPS; Ordon village at real speed
145.6 (Alpha 13) -> 84.1 ms.

Changed
- The GX worker draws picture N while the game thread runs picture N+1's
  logic and draw phase: the painter ends without a sync, the picture's close
  syncs. A 1 MiB ring, a full ring draws only until the record fits, `retain()`
  of a held texture, font pages and the minimap palette no longer take the GPU,
  the 2D passes, HUD tape and retained screens queue as jobs, particle tables
  are const (immutable range), and `configureMainFramebuffer` returns at once
  when nothing changes. Targeted syncs before heap frees/resets, process
  deletion/creation and CPU-skinned vertex swaps. Village: 89.0 -> 67.3 ms
  (real-speed-off), 0 differing pixels against `gx-overlap.txt` 0.
- Audio mixer on core 3 when a thread there provably runs beside the caller,
  else core 2 (`audio-core.txt`: auto/3/2/1 [percent]); 0 underruns in the
  village and the ranch minigame.
- Logic, all exact (verify modes 0 mismatches; exhaustive host proofs; the
  game is not run-to-run deterministic from a boot stage because loading is
  asynchronous, so position replays cannot judge exactness):
  - actor searches by id and by profile name through tables kept where the
    actor list changes (~380 walks of ~30k nodes per village picture -> 0);
  - skin matrices: one joint x inverse-bind product per joint, not per weight;
  - integer `frsqrte` for normal floats, inline `fmaf`, exact `cM_rnd` fraction;
  - twilight mist (KYTAG12): per-tick lookups out of the per-particle loop.
  `logic.txt` switches every part (`none`, `verify`, `key=0|1|2`).
- The bottom screen shows the right release (Alpha 14 still said Alpha 13).

## Unreleased — Alpha 14 (second core, audio mixer, file-select glow)

From the owner's 18 Alpha 13 dumps: paint on the console grows by about 175 us
per draw (least squares over the session: F_SP104 r1 172 us, Ordon village
181 us), while core 2 was busy 12-30 %. Built-in Azahar, 8 zones (seed
`54737253`, one logic tick per picture): Alpha 13 59.3 ms -> Alpha 14 54.8 ms
(-7.6 %; F_SP108 -16 %); Ordon village 94.6 -> 89.7 ms.

Fixed
- File select: the selected slot's light was a hard white rectangle. Its J2D
  material warps an IA4 mask through an indirect texture, and the warp read the
  texture size from BP 0x30-0x3F, which the port's typed GX does not refresh
  for J2D (they held 304x224 from an earlier list, the mask is 72x96). The
  warp now uses the sampled texture's own size. Host test with the console's
  stale values. Same fix for the copy/erase effects and the details screens.
- An old screen-size actor rule set the "not drawn" flag, which game code
  reads (goats deleted themselves, rooms deleted actors, a door's collision
  move stopped): tiny actors now skip only their model entry.

Changed
- Audio mixer at 32 kHz, the GameCube's own rate (7 subframes, 17.5 ms
  blocks), fixed-point resampling and volume ramps, faster ADPCM decode, silent
  voices only advanced. Ranch, same scene: 23 % -> 15 % of core 2; verify
  against the float mixer: largest difference 0.027/32768, no sample over one
  step; no underruns.
- Core 2 keeps more of the drawing: the refraction particles' "frame copy
  made?" check is a conditional section in the queue instead of a sync; bloom
  and the shadow-caster start are queued jobs; the palette refresh takes the
  GPU only for indexed copy images; particles after the translucent lists are
  queued; the worker rests only after a measured 30-picture trial says it is
  faster without it.
- Display lists are parsed once into templates and replayed with live values
  (interpreted lists in the village 55k -> 10k); 0 mismatches against the
  interpreter.
- LOD: skin (envelope) matrices are made only for ticks whose picture is drawn
  (same picture). `lod.txt` holds every rule; defaults keep the Alpha 13 look.

Measured, not used (pictures exact, but slower in Azahar)
- Native packets (J3D records replayed on core 2): +14 ms per village picture.
- One draw per skinned shape (merged matrix groups): only ~8 draws saved per
  picture (each light takes palette rows), +2.8 ms.
- Cached PICA blocks with patched projection/light/texture rows: exact (95k
  checks, 0 bad) but a hit cost 134 us; +12 ms, and +12 ms again after the
  uniform/state re-send fix (p3).

## Unreleased — Alpha 13 (the owner's slow scenes)

From the owner's eight Alpha 12 console dumps (spring path 12-14 FPS, Ordon
village 8-11, ranch goat minigame 4.7, pause/save screen 5.5) and Azahar
reproductions with profiling.

Fixed
- Ranch minigame and its pause screen: the minigame timer reloaded its 12
  digit textures by name every logic tick (a lookup across every mounted
  archive plus a 2,048-record texture release under the global lock, which the
  audio mixer also waited on). The ten digit images are looked up once, an
  unchanged digit is not re-stored, texture records are chained by owner
  (O(1) release), and the DVD drive status is read without the DVD lock.
  Azahar, ranch layer 5: TIMER 5.3 ms per picture -> gone; logic 19 -> 7.7 ms.
- Spring-path sand (F_SP104 r1) far too dark: its 4-stage material (light
  texture + konst + vertex alpha, x4) was refused by the planner and drawn as a
  one-stage stand-in. New exact planner rule (register chain), host-proven
  against the GX evaluator.
- Ordon village logic: fish searched every process three times each per tick
  (now once per tick, same results); fish, pumpkins, chickens and frogs no
  longer run a water check whose result they never read. Azahar village logic
  90 -> 48 ms per picture at 4 ticks.

Changed
- Collision walks skip an object when its own root box test would reject the
  query (same test, same live box, before the virtual call): 822,525 query
  results identical on 305 scenes of real collision data.
- Per-tick profile scopes no longer carry a thread-safe static guard.

Measured, not used
- Offline merging of room geometry: the ROM's 5,486 room shapes each have
  their own material, so merging saves 14 draws game-wide
  (`Tools/3ds/model_prep.py`).

## Unreleased — Alpha 12 (zone transitions, cheaper textures and logic, native-path groundwork)

Read of the owner's Alpha 11 console run (91 zones, no crash, mean 75 ms) and
their ask: 30 FPS, native PICA rendering instead of per-frame GX translation, no
freeze after a fade, shadows kept. Measured in the built-in Azahar on 8 zones
(seed `54737253`, one logic tick per picture): Alpha 11 63.8 ms, Alpha 12 59.8
ms with the default switches (logic per tick -2 to -4 ms per zone).

Added
- Zone transitions keep the curtain closed until the zone is ready
  (`runtime/scene_transition.*`): the overlap does not send its out request
  while anything is being created or the DVD thread has work (1 s stall rule,
  10 s timeout), then the scene runs one tick per picture behind an opaque,
  frozen fader with the sound paused until two pictures in a row are steady
  (45 pictures / 4 s at most), and the game clock is reset before the original
  fade-in plays. Console Alpha 11 showed 354 + 161 + 655 ms right after the
  F_SP104 fade. `transition-hold-off.txt` restores the original reveal.
- 16-bit texture storage where it holds the same picture (`texture_gx`
  compact plans): all-opaque RGB5A3 -> RGBA5551 and full-table palettes exact,
  CMPR -> RGB565/RGBA5551 within one 5/6-bit step. Halves those images in the
  12 MiB cache; 256 slots instead of 128. `texture-compact-off.txt`.
- CMPR interior tiles decode through a per-block palette in the output format
  (exact against the pinned Aurora decoder on 2.29 M disc texels).
- Native draw groundwork, all exact under verify and OFF by default because
  they do not yet beat the original path in Azahar:
  `native-packets.txt` (J3D packets captured or built directly from J3D data and
  replayed on core 2; verify: 38,927 direct records and 272,507 shapes with 0
  mismatches), `pica-blocks.txt` (recorded shapes replayed as cached PICA words
  with patched matrix/uniform rows; verify ok 243k, bad 0),
  `immediate-fog.txt` (fogged immediate records).
- Exact TEV planner rules (`gx_pica_material.cpp`): ADDHALF, clamp-off
  intermediates, register lifetimes; operation refusals on Alpha 11 materials
  148 -> 42. Refusals now logged per rule.
- Particles: emitters with nothing on screen set no GX state; emitters of one
  resource share one setup and batch when their footprints do not overlap
  (`particle-runs.txt`).

Changed
- Game logic, exact: KCol collision data converted to native byte order at
  room load (305/305 disc files byte-identical on 2,800 queries each); small
  vector helpers inlined (no FMA on the 3DS FPU); collision walks stop at the
  last used slot; `C_MTXConcat` reads its inputs once (4 M cases identical);
  KYTAG12 compares the stage name once per tick.
- Depth peeks (sun / lens flare) only on the tick that paints: later ticks
  waited for the picture just submitted (F_SP104: 104-147 waits per 300
  pictures, 8-16 ms each).
- Geometry arena 1 MiB (512 KiB): a full arena waited for the whole GPU.
- Twilight mist: quads with the same TEV register merge into one GXBegin
  (identical pixels); the second mist pass carries C0 in the vertex colour and
  C1 as C0 x konst (red and green exact, blue within 8/255) in one draw.
- Per-tick profile scopes for collision, camera and J3D calc.

Found
- F_SP104's darkness is the original 18:00 palette (dump read byte for byte).
- Pre-converted textures in zelda.twilight would load slower on the console
  (3.5x the bytes at 0.63 MB/s); built and tested, not used.

## Unreleased — Alpha 11 (no first-time stutter)

The owner's report: on the console an action stutters the first time and
not the second, and zones stutter while they start. The Alpha 10 console run
(13 zones, seed `45704950`) named the causes; none of them was a missing
preload.

Fixed
- The test ended in zone 14 (R_SP107 room 3): the room places an actor
  whose release profile has no methods (NPC_KDK, NPC_HENNA0, PATH_LINE and
  Obj_DamCps carry `Actor SubMtd = NULL`) and creation read address 0. Such
  an actor is no longer created (`f_op_actor.cpp`), as when the map tool
  removes an enemy group; the log names it once.
- First-time stutter: the GPU guard ran the first draw of every new kind
  alone, after a GPU drain and two SD writes with flush. On the console that
  was up to 3.7 s per 300 pictures, 600 ms pictures with two new draws, and
  400-650 ms when the collection menu opened; the known-good table started
  empty with every new build, so every CIA stuttered again. Canaries are off
  by default (`gpu-canary-on.txt` brings them back); the stopped-job watchdog,
  its evidence, the transfer/shadow/HUD-tape stop classes and the learned
  skips stay. No P3D draw has stopped the PICA since Alpha 0.
- Periodic stalls: the log was written to the SD on the game thread every
  5 s (console: 27-110 ms per flush on average, up to 5 s, outside the
  picture telemetry) and the guard's session record every 60 pictures.
  Both are written by a low-priority SD writer thread now; fatal paths still
  write synchronously, and a final session record can never be overwritten
  by a late "running" one.

Changed
- The DVD thread (archive reads, Yaz0 decompression, archive parsing) runs
  on the New 3DS's second core, below the audio mixer and the GX worker. On
  the game's core it ran above the game (priority main - 2, as on the
  GameCube), so an archive loaded during play held the picture while it was
  decompressed. Completion goes through a release/acquire pair on
  `mIsDone`. `loader-core0.txt` keeps the old place.
- The log reports disc reads every 300 pictures, game thread against loader
  threads (`Native disc reads, last N pictures`), so the next console run
  shows whether anything is still read on the game thread.

## Unreleased — Alpha 10 (the second core)

The owner asked for two things: cut the per-draw cost of the 3D material and
shape packets, and run the GX translation on the New 3DS's second
application core. Alpha 10 does both. Pictures and times are Azahar's, on the
14 zones Alpha 9 was measured on (seed `54737253`, one logic tick per
picture); no console has run Alpha 8, 9 or 10.

Added
- Second-core worker (`runtime/gx_worker*`, `runtime/gx_bank_delta.hpp`,
  docs/ARCHITECTURE.md). The game thread walks the draw lists and runs the GX
  side of every packet on its command processor; each 3D shape (a J3D
  display list, or a run of primitive packets copied into the queue) is
  queued with the words of the register bank that changed since the last
  one. A thread on core 2 keeps a copy of the bank and draws the shapes with
  the same code. A sync does not wait for the queue to empty: the worker
  stops between two records and the game thread draws the rest itself.
- Work between two shapes that the game does not wait for is queued as a
  job instead of a sync: the shadow caster pass, a frame copy into an
  existing image, `GXInvalidateTexAll`.
- On by default, with guards. The worker runs below the audio mixer, which
  shares core 2, and the game thread raises it only while it waits for it;
  where the mixer leaves it under half of the shapes (Death Mountain in
  Azahar) the worker rests for a while and tries again. A fatal error or a
  GPU stop met on the worker is handed to the main thread, which ends the
  program as for its own; a start that had the worker on and ended on a
  fatal error, a CPU exception or a GPU stop leaves it off from then on
  (`gx-worker-learned.txt`). SD switch `gx-worker.txt`: `0` off, `w` on,
  `d` the same records drawn by the game thread, a `v` behind them adds the
  verify checks (docs/DIAGNOSTICS.md).
- The automatic test measures every zone once more with the worker off
  (phase `onecore`; `onecore_ms - still_ms` in `summary.csv`, printed by
  `analyze_tour.py`); the `still` phase before it keeps the worker on even
  where it would rest. The test's second and third captures come 60
  pictures later than in Alpha 9 because of it.
- Build check `Tools/3ds/audit_shared_state.py`: from the image's object
  files, every writable variable the game thread and the drawing side can
  both reach without a sync in between; one without a written verdict
  (`Tools/3ds/shared_state_judged.tsv`) fails the build. Its first run
  listed 298; 63 remain, each judged.
- `gpu-trace.txt` lists one picture's draws in the log, to find which draw
  differs between two modes; `mixer-plain.txt` and `mixer-verify.txt`
  select and check the audio mixer's paths.

Changed
- The texture registry is read by two threads: its lookups use exact
  indexes kept up to date by every registration and release (they no longer
  walk the tables, and no lookup rebuilds anything), and a registration
  becomes visible in one step.
- Per-draw costs: the GPU guard parses a single plain register write
  without its general path; texture objects, registrations and image
  identities are found through exact indexes; the checksum of an image the
  game can rewrite reads two words at a time; an archive texture kept for
  the archive's lifetime is answered from a memo.
- The audio mixer adds each voice straight into the stereo mix instead of
  clearing and filling a buffer per voice (`mixer-plain.txt` restores the
  Alpha 9 path, `mixer-verify.txt` renders both and counts differences).

Fixed (in the second core, before it shipped)
- Each of these differed in pictures between the worker on and off, and was
  found that way: the capture state of `GXCopyTex` changed before the shapes
  issued earlier were drawn; the shadow caster list was read before the
  casters were drawn; the particles asked whether the frame copy had been
  made before the refracting water was drawn, and left the ripples around
  Link out (Ordon Spring).
- The learned switch-off did not trigger after a fatal error: the session
  record's word was followed by a colon. The guard's host test now reads
  every record format.
- The first Alpha 10 gate found the `onecore` measurement wrong at real
  speed: the worker had started a rest during the phase before `still`, so
  `still` and `onecore` both measured it off (Ordon Village: -1.3 ms "gained"
  where the worker saves 9 ms). `still` now suspends the rest.

Tests
- `Tools/3ds/test_gx_worker.py`: bank deltas against a real command
  processor, the queue in deferred and threaded mode (ASan/UBSan and
  ThreadSanitizer), jobs, copied runs, a sync after every shape, and a
  watchdog that turns a stuck queue into a failure.
- `test_gx_lookup.py`: 6,000 random registrations, releases and aliases with
  every answer checked against a walk of the table.

Performance (Azahar, 14 zones, ms per picture)
- Alpha 9 80.4; Alpha 10 with the worker off (the per-draw cuts alone) 78.1
  (-3.0 %, every zone -1.7 to -4.1 %); Alpha 10 as shipped 71.1 (-11.6 %,
  12.4 to 14.1 pictures per second). Eleven zones gain 7 to 20 % over
  Alpha 9; in Death Mountain, Zora's Domain and the Bulblin camp the audio
  mixer leaves the worker too little of core 2, it rests, and those zones
  keep the -3 %.
- A draft that left the worker above the mixer reached 63.3 ms over three
  zones but the mixer missed blocks; that was a bookkeeping error and is
  gone. Taking the mixer off core 2 (core 1) is the next step for those
  zones: the core probe at the end of the automatic test measures what core
  1 gives on a console.
- At real speed (as on a console), worker off -> on: Ordon Village 67.6 ->
  56.3 ms, Faron Woods 78.1 -> 63.8, Ordon Spring at night 100.7 -> 86.9,
  Lakebed: Morpheel 132.7 -> 116.8, Death Mountain 100.0 -> 98.1. The
  35-zone plan at real speed: Alpha 9 83.9 -> Alpha 10 78.5 ms (31 zones
  ok in both).
- Pictures: the worker on and off give the same three captures in all 14
  zones, pixel for pixel; the first capture is identical to Alpha 9's in
  all 14.

Not verified on a console: everything above. On a console the mixer's cost
on core 2, cross-core memory and cache behaviour and the GPU from two
threads are what Azahar cannot show; the `onecore` phase and the guards
above exist for that.

## Unreleased — Alpha 9 (the Alpha 8 water, corrected)

The owner put an Alpha 8 picture of Ordon Spring beside the Alpha 7 one: the
water was flat beside the camera, with a hard edge and a grey shape in it.
The Alpha 8 gate had measured an 18 % pixel difference in that zone and it
was written down as "calmer water" without opening the picture. Alpha 9 is
that correction and nothing else. Pictures and times are Azahar's, on the 14
zones Alpha 8 was measured on (seed `54737253`, one logic tick per picture);
no console run of Alpha 8 or Alpha 9 exists.

Fixed
- Water that shows the scene behind it (Ordon Spring, Faron Woods, Zora's
  Domain, the Lakebed Temple). Its texture is a copy of the frame projected
  through the camera. When a water mesh crossed the camera plane, Alpha 8
  divided that projection per vertex with a clamped divisor, which reads the
  border of the copy: one flat colour over every such triangle (all 990
  water draws of the Ordon Spring run). A projection whose divisor is the
  clip w times a positive factor is now proved from the draw's matrices, once
  per draw, and left to the PICA per pixel: the rasteriser clips at the near
  plane before any pixel divides. `camera-divisor-off.txt` restores the
  Alpha 8 division.
- The distortion of warped water changes from point to point and moves again.
  Alpha 8 replaced the warp of a GX indirect stage by one constant per mesh.
  The warp is now that constant plus what the warp map adds at each vertex:
  the CPU reads the map at the vertex (nothing else of the vertex runs on the
  CPU) and the number travels in the record's second texture coordinate,
  which these materials leave unused, so the vertex layout the console
  already draws is unchanged. The shader adds it to the units the stage
  warps, times the projection's divisor for the projected one. Against the
  Alpha 7 pictures (CPU vertex route) the first picture of each of the 14
  zones differs in 0 pixels in twelve and in 5 and 8 pixels of 96,000 in
  Faron Woods and Ordon Spring; Alpha 8 differed in 18.2 %, 2.6 %, 2.0 % and
  0.2 % of the pixels in four of them. `vertex-warp-off.txt` restores the
  constant alone.
- The Alpha 8 record said the 2 % difference in Faron Woods came from the
  flowers lit in the combiner. It was the water as well: with the warp
  corrected that zone is 5 pixels from Alpha 7.

Performance (what the correction costs, and what keeps it small)
- The mean of the 14 zones is 80.4 ms per picture (Alpha 8: 79.9, Alpha 7:
  145.4). The six zones with warped water cost 0.3 to 1.7 ms more than in
  Alpha 8, the other eight 0.1 to 0.3 ms.
- The map read at a vertex is an all-integer bilinear sample (about 110
  instructions; the first version took 530): the coordinate is kept in turns
  of the map, so the wrap is the overflow of a 32-bit sum.
- Warps that stand still are copied. In three Lakebed Temple rooms 95 to
  99 % of the warped vertices get the same numbers picture after picture; a
  static mesh keeps them with the plan they were read under
  (`warp-repeat-off.txt` reads every time).
- A draw whose records are decoded into the frame's geometry arena returns
  the part of its reservation they do not fill (60 bytes a vertex were
  reserved for records of 32). Warped water is decoded every picture and had
  filled the 512 KiB arena in one Lakebed room, which waits for the GPU in
  the middle of the picture (`arena-shrink-off.txt`).
- The vertex decoder's field passes and readers are always inlined. Adding
  the warp pass made the compiler stop inlining them, three times in three
  different places, at 0.5 to 1 % of a picture in every zone.
  `Tools/3ds/check_hot_inlining.py` now fails the build when the linked
  image holds one of them as a function.

Tests
- The test of this path on the real draw adapter
  (`gx_indirect_hookup_test.cpp`) had not linked since Alpha 8. It is
  repaired, covers the camera projection and the per-vertex warp, and runs
  with the standard runner (`Tools/3ds/test_gx_indirect.py`).
- Host research runners: 73 of 113 pass, the same 40 fail as on the Alpha 7
  tree.

Not verified on a console: the projection divided per pixel with vertices
behind the camera, the per-vertex warp, the arena return and every time
above. Each has its switch (docs/DIAGNOSTICS.md).

## Unreleased — Alpha 8 (after the Alpha 7 console run)

The Alpha 7 console run (New 3DS, 56 zones of the random test, seed
`54737253`) ended in zone 57 with a crash and gave the first timing of that
build on hardware: 100 ms per picture on average (10 pictures per second),
62 ms of it painting. Its log also carries the named time parts of 118
profiled pictures, so the costs below are ranked from the console, not from
the emulator. Every "after" figure is Azahar time on the same zones with one
logic tick per picture (`real-speed-off.txt`); no console run of Alpha 8
exists yet.

Fixed
- The crash of the console run. Part of the game logic lives in the scene's
  draw function (collision pass, moving backgrounds, particles, the frame
  counter). With real-time logic a picture carries several logic ticks and
  that function ran once per picture, so colliders of actors deleted between
  two ticks stayed registered and were read after their memory was reused.
  That logic now runs on every tick (`dScnPly_LogicOnlyDraw`), and an actor
  removes its colliders when it is deleted. In the zone that ended the run,
  131 stale colliders per run became 0.

Performance
- CPU vertex route. On the console the draws that the GPU routes refused cost
  14.8 ms of a picture on average and 100 to 190 ms in the worst zones. Most
  of them now stay on the GPU:
  - Warped water: every indirect warp takes the constant-offset GPU route
    (Alpha 7 took it only when the result was exact). Non-linear GX fog goes
    through a ramp texture, positions with two components are accepted, and a
    projected unit crossing the vertices divides per vertex with a clamp
    instead of leaving the route. Lakebed Temple room 2: 317 to 81 ms per
    picture. `indirect-constant.txt` = `0` restores Alpha 7.
  - Environment maps on skinned models (Magic Armor, the Lakebed creatures,
    Morpheel): J3D gives every bone its own texture matrix and each vertex
    selects it. When all of them are one matrix after the bone's normal
    matrix, which is checked for every bone of every draw, the shader reads
    the normal it already transformed. A Lakebed Temple room went from 259 to
    116 ms per picture; `mt-paired-texgen-off.txt` restores the CPU route.
  - Texture coordinates outside the compact range: float coordinates beyond
    32 repeats and U16 ones above 32767 are stored with fewer fraction bits
    (1/256 of a repeat or coarser instead of 1/1024) instead of refusing the
    draw. Ordon Spring had 90 such strips a picture.
  - Vertex formats with attributes no plan reads (TEX2 to TEX7, a second
    colour) are decoded; they were refused.
  - Flowers: the batched draw used two colour channels (the lit colour times
    the colour of the leaf vertex). The PICA rasterizes one, so they were lit
    on the CPU: 46 ms of a picture in a Faron Woods room. On the 3DS the
    batch uses one channel and makes the same product in the combiner; Faron
    Woods rooms went from 120 to 92 and from 170 to 109 ms per picture.
  - 3D lines (reeds, ropes): all the lines of a material are one triangle
    strip, tied by repeated vertices, instead of one strip and one draw per
    line (Ordon Spring: 95 strips a picture became 25).
  - Materials whose texgens read three texture coordinate sets (a lit cellar
    floor in Kakariko, 2,080 vertices: 38 ms of a 65 ms picture on the CPU)
    are drawn from 36-byte records with the third set beside the second;
    that room went from 65 to 41 ms per picture.
    `mt-third-coordinate-off.txt` restores the CPU route.
  - Reverse exponential fog on unlit materials that leave the vertex alpha
    unread (two or three draws a picture in Faron Woods and Ordon Spring)
    takes its curve from the fog ramp texture instead of the CPU vertex
    route (`fog-alpha-ramp-off.txt`).
  - Indexed vertex arrays stored in the console's byte order (the volume of
    the simple shadow, one or two draws in every picture) are converted while
    decoding instead of refusing the draw (`swapped-arrays-off.txt`).
- Vertex decoding of draws that change every picture (flowers, grass, ropes:
  4,000 to 7,000 vertices a picture outdoors, 11 to 20 ms of it). The decoder
  makes one pass over the vertices per attribute, so a format is chosen once
  per draw and not per vertex; it reads each attribute where it lies, in its
  own byte order, rounds without a library call per value, and converts the
  small tables that grass and flowers index (float normals and coordinates)
  once per draw. A grass vertex went from 2.05 to 0.54 microseconds.
- GX vertex calls. Every `GXPosition3f32`, `GXColor` or `GXTexCoord` went
  through the complete command stream write, about 2 microseconds a call. A
  value that only adds to the primitive being collected is now appended
  directly (`CommandStream::writeValue`), for every caller: weather sprites,
  particle trails, 2D pictures and text. The 1,200 stars of a night sky
  (7,200 calls a picture) and the corners of each particle are also written
  in blocks.
- Menus and HUD (file select ran at about 5 pictures per second on the
  console). A 2D screen is fingerprinted from everything its panes read; an
  unchanged screen replays the PICA command words of its last recording and
  the register words that recording changed. A screen that keeps changing is
  split: runs of unchanged sibling panes are recordings of their own and only
  the panes that move are drawn. Images the game can rewrite (item icons, the
  minimap) enter the fingerprint with a checksum of their pixels. File
  select: 105.5 to 33.8 ms per picture, the same picture pixel for pixel in
  seven captures. In play the HUD tape of Alpha 7 (one recording of the 2D
  pass every second picture) stays on and the retained screens work inside
  its recordings. `screen-tape.txt`: `0` off, `v` verify (draws as well and
  compares the command words). See
  `Source/platforms/3ds/runtime/screen_tape.hpp`.
- Vertex data of batched draws (flowers, 3D lines) is written to the GX
  stream in blocks, as the grass batch already did: a vertex call per value
  cost about 8 ms of a picture for 2,100 line vertices.
- Archive textures asked for again get their answer from a memo (1.4 ms of a
  picture in 3D zones); the texture object, image and token lookups probe two
  places instead of scanning the table when the first is taken.
- `-fno-math-errno` for the platform layer and the game libraries.

Added
- A first, incomplete picture rate lock (`picture-rate.txt`, off without the
  file). The game speed has not depended on the picture rate since Alpha 1
  (30 logic steps per second of real time). The file spaces the start of
  the pictures by a whole number of display refreshes; each picture is still
  presented when its painting ends, so the rate on screen is not steady yet
  (Azahar, one zone at 12 per second: starts 83.6 ms apart, pictures 67 to
  117 ms apart). A steady lock needs the wait before the presentation and
  rates that are a whole number of logic steps (30, 15, 10).

Automatic test (START+SELECT)
- When every zone has run, the bottom screen shows TESTS FINISHED with the
  zones run and failed, the test time and the folder of the results. After 15
  minutes without a button or a touch the console powers off (shutdown
  notification, then PTM); any input keeps it on. `tour-options.txt`
  `hold=` sets the seconds (`0`: no screen, no power-off).
- A menu phase: the collection screen is opened and closed in every zone, with
  its own picture (`shot3.bmp`) and times.
- A probe at the end, after the results are on the card: how fast a busy
  thread runs on cores 1 (at 30, 50 and 80 percent), 2 and 3 against core 0,
  what a GSP request costs meanwhile and what a cache flush costs through GSP
  and through the kernel (`core_probe_*` lines of `tour.txt`; `probe=0`
  leaves it out). The next step, GX translation on a second core, needs
  these numbers from a console.
- Zone reports carry the CPU-route gates and reject codes, the retained
  screens and their panes, battery level and charger state.

## Unreleased — Alpha 7 (after the Alpha 6 console run)

Measured in Azahar on the first 8 zones of the console's random plan (seed
`0e7bc814`), mean of the 7 zones that ended normally in every run: 159 ms per
picture in Alpha 6, 133 after the first batch below, 121 after the second and
108 with the HUD tape. This is emulator time; console gains are modelled, not
measured. The numbers and the per-scope profile are in
`Build/alpha7/wave1-azahar-measurements.md` (local, not in the repository).

Performance
- Audio mixer (core 2): a managed thread that finds a mutex taken now waits on
  the lock word and is woken by the unlock, instead of retrying in 1 ms sleeps.
  The mixer holds the JAS lock only around `updateDSP`; voice rendering, PCM
  conversion and the cache flush run outside it. The per-subframe cache
  invalidation of the DSP channel table (CPU-only data, 600 kernel calls per
  second) is skipped.
- GPU command arena 256 KiB → 1 MiB. The hardware build never received the
  512 KiB the code asked for: that request depended on `DUSK_PICA_GAME_DRAW`,
  which is off in that build. Arena reclaims (each one a full GPU drain, half
  of all frames on the console) fell to zero in Azahar. `gpu-arena-256k.txt`
  restores the old size.
- GPU waits poll every 100 µs for their first 2 ms instead of sleeping 1 ms.
- The dynamic textures the game re-decodes every frame (the minimap) no longer
  wait for the whole GPU after the per-frame `GXInvalidateTexAll`. Storage last
  bound in an earlier frame is rewritten in place; storage bound in the
  current frame gets a spare (at most 4 spares, 1 MiB in all).
- Static geometry with a compact matrix palette is cached and replayed like
  other static batches. The static geometry pool is emptied when a stage
  changes, not in the middle of play when it fills up. The resident index pool
  grew from 256 KiB to 1 MiB: it filled up to five times per zone.
- Texture coordinates generated from the normal (environment maps on swords,
  bottles, the bow, Freezards; about 540 disc materials) run on the
  multi-texture GPU shader instead of the CPU vertex route
  (`mt-normal-texgen-off.txt` restores the CPU route). The CPU route itself
  normalizes with one square root and unrolls its matrix products.
- Snow flakes wholly behind the camera or wholly past a scissor edge are
  skipped before `GXBegin` (the GPU discarded them anyway).
- Minimap paths: inside the map's merge scope, line strips, lines and points
  become one line list and one point list.
- HUD: `J2DMaterial::setGX` can be replayed from recorded register writes
  (`material-memo.txt`: `0` off, `v` verify). The hit rate is still low.
- The log is copied out of its ring under the lock and written to the SD
  outside it, at most every 5 seconds, 32 KiB per write.
- HUD tape: the 2D pass over the picture (hearts, buttons, minimap) was more
  than a third of the painter with no single hot spot. It is now translated
  every second picture; the picture in between runs the PICA command words of
  the last recorded pass again, so the PICA draws the same HUD with the same
  blending and the game's 2D lists are not walked. The HUD is then at most one
  picture old. Full-screen menus are too large for the store and are drawn in
  every picture. `hud-tape.txt` holds the pictures per recording (`1` = every
  picture as before, `2` default, up to `8`). See
  `Source/platforms/3ds/runtime/hud_tape.hpp`.
- Immediate draw records: choosing the slot for a new record no longer empties
  it. A draw that ends without a record used to cost another draw its record
  (5 to 12 evictions a picture). A new record replaces the same draw's stale
  record first, then the record unused for longest.
- Native shape replays plan less: the projection and fog plans are kept while
  the registers they read stand, and a replay finishes the record's uniform
  block in place (`native-lean-replay.txt`).
- HUD immediate draws: an unchanged draw copies its previous vertices, and
  the lookup key is kept while the state epoch stands
  (`immediate-shortcuts.txt`).
- Draw submission: uniform rows are compared as words, the attribute layout
  and the vertex buffers are sent only when they differ from Citro3D's copy,
  and the frame is handed to the PICA in parts of 64 draws instead of 24
  (`gpu-chunk-draws.txt`).
- The typed TEV setters make one masked write instead of two, the native state
  key is kept as an incremental hash of the register bank (`state-hash.txt`),
  and the J2D material memo covers more materials and gives up on the ones it
  keeps refusing.
- GPU guard: the command parse is about twice as fast with the same
  signatures; the known-good table holds 12,288 signatures and no longer
  forgets them in a session.
- More draws stay on the GPU: formats with only a position, lit meshes whose
  matrix palette overflowed (drawn as runs of triangles, `palette-split.txt`),
  additive water layers with two alpha tests (`alpha-union.txt`), and draws
  whose only fallback is the TEV stand-in or the relaxed alpha test are
  recorded like other static shapes (`fallback-records.txt`).
- TEV planner: sums of three and four terms are translated with the combiner
  buffer (`tev-sum-rules.txt`). In the 8-zone run the `equation` stand-ins fell
  from 10,726 to 0.

Fixed
- Zone 84 of the Alpha 6 test ended the game: the mirrored night-sky stars
  (F_SP103 and F_SP127 at night) made one triangle list larger than the
  64 KiB immediate scratch. The stars are now drawn in lists of at most 3,600
  vertices, and the command stream splits any fixed-count list of independent
  primitives at a primitive boundary.
- Water shine (material MA09, used in 26 game stages) was drawn black or
  opaque. It now translates
  in 5 PICA stages, which leaves the sixth for its fog. Other TEV forms that
  Alpha 6 replaced by a one-stage stand-in are translated too: divide-by-2
  scale, clamp off on the last stage, signed D, and the GX compare operations.
- Dungeon minimap door, chest, key and boss icons were missing: their GX
  compare pass is translated.
- Darkhammer's room (D_MN11B room 49) was mostly black: 844 draws whose
  channel reads a vertex colour the format does not carry were refused. They
  are drawn with white, as Dolphin does (the console reuses the last colour
  sent, which depends on the draw order).
- Warp maps in RGB565, RGBA8 and CMPR are accepted by the per-vertex indirect
  route (the Snowpeak iron ball was not drawn).
- Frame copies smaller than their texture repeat their last row and column
  into the padding, so bilinear samples at the edge no longer read black.
- A full dump taken on the title screens read the cutscene camera through a
  demo object that exists only inside a play scene (a null read). The check
  is now made first; the same call in this cycle's scene-change log was
  caught by the title gate before any build left the machine.
- Clip-disabled draws on the multi-texture GPU route get the depth clamp
  their single-texture equivalents had (sky, weather, backdrops).
- Particles could show another texture after a room change: the textures of a
  particle scene heap are released before the heap is destroyed.
- Frame-copy ("refraction") particles are drawn in frames where the frame
  copy already exists, with a sway in place of the warp
  (`refraction.txt`: `0` left out as in Alpha 6, `3` always).
- A fatal error now records itself in the GPU session file, so the next start
  no longer reports "switched off or frozen".
- A thread join that timed out counted as finished (the kernel's timeout code
  passes `R_SUCCEEDED`). Latent: never seen in a log.
- `abort`, `exit` and `_exit` ended the process without a word; they now log
  their caller first.
- Random test: the fishing rod is given; Link keeps at least three hearts and
  his health is restored at every phase, and a zone where he dies anyway is
  reported as `died`; start points inside events are avoided; every scene
  change during a zone is logged with its exit and caller; dump folders carry
  the zone number; the elapsed time survives a restart.
- `CalcDivideInfo` log lines are rate-limited per message (9,049 lines in one
  Alpha 6 run).

## Unreleased — after the Alpha 5 console run

Added
- Random automatic test (the default START+SELECT mode): 94 rooms drawn from
  every stage in a shuffled order, outdoor stages as the wolf one time in
  four. Each zone gets its own random state from the plan seed: clothes,
  sword, shield, the items on X and Y, hearts and health, rupees and time of
  day. The seed is written to `tour.txt`, the plan to `zones.csv`, and each
  zone's state to its `report.txt` and to the L+R+A dump note;
  `tour-options.txt` `seed=` replays a plan.

Fixed
- Using an item whose model the game reads from ARAM into Link's item heap
  (the slingshot was the first found, by the random test) ended the game:
  that heap is a solid heap, and J3D texture retention only knew archives
  and expandable heaps. It now takes the live extent in solid heaps; an image
  with no bounded storage is left out (its draws counted) instead of ending
  the game. A new texture also drops owner records left at its address by a
  heap freed without destructors, so an item never shows the previous one's
  image.
- Models lost all their textures after several stage changes (Link drew
  as a floating sword in the eighth zone of a random test): destroying a GX
  texture object left its packed address slot taken, so the table filled
  with stale slots while reporting a fifth in use, and new objects were
  refused without a report. `gx_lookup_test` now destroys a full table and
  fills it again at new addresses.
- A particle texture number past the emitter's list (only asserted by the
  original) read address 0x48 through a null texture entry, a fault on the
  console; the load is now skipped and reported.
- Wolf form was lost when a test zone resumed after a restart.
- A test travel changed the save (empty save, equipment, form, time) while
  the previous scene's Link was still drawn during the fade. With random
  clothes this ended the game: Link created in the Zora Armor drew the Magic
  Armor's animations, which he had never loaded. The state is now applied
  when the next play scene starts, after the previous one is deleted.
- The model-shadow pass replayed pointers into the frame geometry arena even
  when the arena had been reclaimed (and refilled) since the scene draw:
  nearly once per pass in the opening cutscene (measured in Azahar). Such
  replays are now left out. The console stopped in this pass twice.
- The automatic test (START+SELECT) started on the warning screen never
  warped: those scenes wait for A and the test ignores the pad. It now
  presses A once a second until the warp is accepted.

Changed
- A stop in the model-shadow pass makes the next start use blob shadows
  (model shadows stay the default; `shadows.txt` overrides).
- The GPU guard no longer asks the next start to run every draw alone after
  an unexplained stop (that start played at 1.4 pictures per second); the
  stopped list is recorded for offline decoding instead. A hand-written
  `gpu-isolate.txt` still selects that mode.
- The guard recognises every command list queued during the model-shadow
  pass, not only the one that switches the colour target.

## Unreleased — HUD and menus on 3DS, model-shadow pass

Fixed
- HUD minimap visible during cutscenes on the console: its alpha was never
  written on 3DS (the original line is removed for every `TARGET_PC` build
  and the desktop replacement is excluded on 3DS), so it kept heap contents.
- Same gap in the options menu (an uninitialised index used as an array
  index; frozen animations), the letters menu (never opened, closed or slid),
  the explanation window (its open/close counters never advanced), the item
  message box (no animation, no box alpha) and the name-entry cursor.
- Console GPU stop in the first model-shadow pass: casters are drawn with a
  position-only shader (`gx_caster.v.pica`) that reads no scene state, with
  texture units off, in their own command lists.

Added
- GPU guard: a stop in a list that renders into the shadow image is recorded
  as `shadow-stop`; the next start keeps blob shadows instead of running every
  draw alone.

## Unreleased — console test fixes and source cleanup

Fixed
- Cutscene cameras and other file-backed enums: the tree is built with
  int-sized enums; SDK structures whose layout depends on it are read through
  `runtime/ctru_abi.hpp` and checked at build time.
- First gameplay picture on the console: render targets are read back by the
  CPU from VRAM instead of the transfer engine.
- START+SELECT from the title no longer clears the title scene's memory pools.
- Saves on the SD card: the container is replaced as a whole file; the empty
  save left by earlier builds is removed.
- The L+R+A dump no longer reads GPU hardware registers (that read froze the
  console).
- Black presentation between scenes no longer uses an SDK inline that reads
  render-target fields at the wrong offsets.
- Host test runners: the substitutes and fixtures of eight research runners
  follow the current runtime again (six-attribute compact vertices, the
  hardware clip guard, the GPU guard, the multi-texture default route, F32
  texture coordinates as fixed point); two no longer need private baselines.

Added
- Real-speed game logic: 30 logic steps per second of real time, up to four
  per picture.
- Console GPU guard: new draw kinds run alone once, hang records with the last
  GPU jobs, transfer-stop fallback levels, per-build learned state.
- Bottom-screen developer console, START+SELECT automatic multi-zone test,
  frame telemetry, and one folder per L+R+A dump with screenshots and a
  readable state report.
- Native bloom and model shadows drawn with PICA-native passes.
- Tools: `check_ctru_abi.py`, `check_pica_shader.py`, `gpu_hang_report.py`,
  `pica_list_decode.py`, `pica_shader_disasm.py`, `analyze_tour.py`,
  `generate_zone_table.py`.

Changed
- GX sync calls are ordered on the GPU instead of waiting for it.
- An oversized real-shadow request (the title's wall of light) is skipped.

Removed
- Closed investigations: the capture94 and FRAME76 draw captures, the
  VITAL80 log, the strict material74 mode, and the scene-creation,
  ground-creation and background-budget compile-time probes.
- Four functions with no caller and an orphan test source no build compiled.

No release, tag or binary is published. Console results are recorded in
[Verification status](docs/STATUS.md).

## Unreleased — PAL input and GX compatibility fixes

- Accept European/PAL revision-0 (`GZ2P01`) data alongside USA revision-0
  (`GZ2E01`) in the packer and disc reader without relabeling original data.
- Preserve game/maker IDs in new save metadata and reject foreign ownership
  without reading, overwriting or converting the existing save container.
- Route native card permission errors through the existing non-formatting menu
  error state instead of leaving file selection busy.
- Translate supported dual-raster GX materials using independently interpolated
  RGBA colors, keeping their multiplication in the fragment combiner.
- Preserve both lookup bindings when an untextured dual-raster draw appends a
  third fog sampler, including its separate texture-coordinate output.
- Retain ordinary two-texture rendering when raster-1 alpha is unobserved;
  alpha tests, blending and alpha writes keep their real dependencies.
- Add regional data/save regressions, full-color adapter checks and an asset-free
  GPU probe; repair the archive test harness's Linux worker linkage.
- Keep the unfinished native-model renderer default and performance work separate.
  These fixes target `DUSK_PICA_GAME_DRAW=OFF`; no hardware 30-FPS claim is made.

## Native renderer checkpoint — 2026-10-29

- Add the standalone direct Citro3D/PICA200 engine, six vertex shader variants,
  resource lifetime/budget contracts and native model/scene submission.
- Include offline geometry, texture, material, sampler and pose conversion tools,
  native bank/envelope readers and original source identity checks.
- Connect actual game model packets, resource identity, shared frame lifecycle,
  palette gather and selected high-level camera publication. Complete live
  material/environment input, UI/effects and full-game migration remain pending.
- Add synthetic renderer/converter/format/shader tests and complete the missing
  camera test dependency. Replace a machine-specific model-plan SDK include path.
- Document implementation boundaries, build/test commands, source ownership,
  the ten-step roadmap and the native architecture decision using Code Atlas.
- Extend read-only source CI with five SDK-independent new-native conversion
  and bank-format suites.
- Keep prepared native game banks ignored and retain GitHub's existing owner
  documentation changes while reconciling the source histories.

This is unfinished source work. No playable binary, hardware validation or
performance improvement is claimed.

## Historical initial source publication — 2026-10-25

- Publish the current New3DS platform layer, shared engine changes and Aurora
  adaptations based on Dusklight v2.0.1.
- Include pinned dependency source, native fixtures, PICA programs, open-source
  audio component and the current development tools.
- Add a consistent project README, documentation index, architecture, build,
  configuration, data, testing, compatibility, diagnostic and provenance guides.
- Add an explicit public build helper, reproducible synthetic host checks and
  a tracked-file publication check.
- Replace five machine-specific SDK paths in research tools with explicit
  `DEVKITPRO` / `DEVKITARM` configuration.
- Keep generated packages, game inputs, saves, captures and private engineering
  records outside public Git.

No release, prerelease, version tag, installation QR or end-user binary is created
for this milestone. Full-game and physical performance acceptance remain pending.
