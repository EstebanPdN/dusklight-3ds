# Source publication verification

Sections are newest first. Each one says where its figures come from: the
owner's New 3DS (console), the emulator built into Code Atlas (Azahar), or
host runners. Emulator times are emulator times, not console times. No binary
is published; the CHANGELOG has the detail behind each section.

## Alpha 21: measured per process and per draw, six improvements behind switches — 2026-11-20

Every figure is from the built-in emulator and is instruction time (CPU work
only; the PICA's time is not in it). Alpha 21 has not run on a console.

| Check | Result and scope |
| --- | --- |
| Azahar: the merged build against its own reference in the same run, player standing | Game-thread time per picture with every switch on against all off: Ordon Village 72.8 to 62 ms, fishing pond 48.6 to 43.1, Hidden Village 86.1 to 77.1 (the exported package). A measurement without an improvement the emulator cannot time reads zero. |
| Azahar: per improvement | Levels of detail: indices -18 to -21 % in the village and Hidden Village, -18 % Snowpeak, -8 % spring path, none in the field and the mines. Vertex program variants: 22 to 24 % fewer vertex instructions per picture in five scenes, same picture byte for byte. Skinned matrix runs: about 30 draws fewer where villagers or Bulblins are in view (village 295 to 263, Hidden Village 395 to 360). Packet paint: -6 to -12 % (Hidden Village 45.7 to 40.3 ms). Fish rest: fishing pond update 5.9 to 3.55 ms per logic tick. Run-of-bytes display-list writer: draw phase 35 % cheaper (village 16.2 to 10.3 ms). |
| Exactness checks | Packet paint: 48,894 replays and 11,355 held lists verified, 0 mismatches. Display-list writer: same bytes in 1,386 host cases. Collision skip extended to barrels, pumpkins and items: 45,674, 94,090 and 30,641 verify checks, 0 mismatches. Skinned weighted bones: normal within 4 degrees of J3D's, bound checked against 40,000 random blends. |
| Azahar: ten slow zones, everything on | Village, spring path, field, Hidden Village, Snowpeak, fishing pond, Zora's Domain, Arbiter's Grounds, Lakebed Temple, Goron Mines: no stop, no draw left undrawn. |
| Azahar: the exported package, automatic test over 12 zones from a scripted start (seed `24da0b9b`, `zelda.lod` on the SD) | Complete, no failure, no draw left undrawn, no held-list mismatch in 1.6 million replays, seven measurements in every zone's folder. Everything on against the reference: 21 draws and 4,755 indices fewer per picture and -1.0 +- 1.6 ms, which twelve zones cannot tell from zero; one improvement's rows have two zones each and say nothing yet. |
| Azahar: the same game picture with and without the improvements (one save state) | Hidden Village 101 of 96,000 pixels differ (4 by more than 16 levels); village 1,069 (175 by more than 16: small far objects the fish rule's random numbers move). World mip levels stay off: with them 30 to 39 % of the pixels changed. |
| Azahar: START+SELECT from the title | Runs the story steps and the room zones, writes each zone's seven measurements into its folder; a full dump is written and verified. |
| Host research runners | Runners the batch had left behind build again; `test_gx_rigid_texgen` and `test_gx_indirect` pass again. |

Not verified on a console: everything above. The automatic test's table per
improvement (`measure_report.py` over a console test folder) is what a console
run has to give.

## Alpha 18 to Alpha 20 — 2026-11-19 to 2026-11-20

Test builds between Alpha 17 and Alpha 21: the automatic tour, Test
everything, the minimap reuse and the story drivers (CHANGELOG). No console
run of Alpha 20 exists.

| Check | Result and scope |
| --- | --- |
| Console: Alpha 18 | The first 191 cases of the full route in about 2 h 17 min. |
| Console: Alpha 19 Test everything | 253 of 399 cases complete; 14,369 of 30,329 capture-free pictures over 50 ms; manual play in Ordon 15.03 FPS. |
| Azahar: Alpha 20 minimap | Outside the Citadel, 18 rooms visited: median refresh 1,030 draws and 477.76 ms to 4 draws and 7.10 ms; 46,656 pixel indices and 408 palette bytes identical in 348 refreshes. |
| Azahar: Alpha 20, 18 field rooms | 13.01 to 18.24 FPS (p95 293.00 to 83.76 ms); outside the Citadel 11.71 to 23.33 FPS. |
| Azahar: Alpha 20 story drivers | West portal (case 26) and Darknut (case 61) finish, no draw left undrawn. |

## Alpha 17: repeated models drawn together — 2026-11-19

| Check | Result and scope |
| --- | --- |
| Azahar, real speed, pictures 300-900 | F_SP104 spawn 56.5 to 40.2 ms (17.7 to 24.9 FPS), Epona ride 50.7 to 38.0 ms, Epona and Ilia 47.7 to 36.6 ms; Ordon Village 84.0 to 86.2 ms and the goats 53.5 to 53.9 ms (no change). |
| Instance batches in verify mode | 32,629 model comparisons, 0 differences. The census on F_SP104 shows the rope bridge's matrix groups going from 69 to 2 per picture. |
| Pictures and gate | Bridge view (F_SP104 room 1, point 5): on against off differs less than off against off (the bridge's own sway between runs). 8-zone gate complete. |

Not verified on a console.

## Alpha 16: flicker, exit crash, console GPU time — 2026-11-19

Read of the owner's 15 Alpha 15 console dumps and a Luma crash dump.

| Check | Result and scope |
| --- | --- |
| Console: Alpha 15 dumps | The PICA was busy 66 to 77 ms of 80 to 86 ms pictures, about 0.38 ms per 1000 indices even off screen. One dump (014) had 7,272 queued shapes dropped without a trace: the 2D and water flicker. |
| Fixes | The queue drop: `adopt` refuses only on the recorded processor (log `queued draws not drawn 0`). The exit abort and kernel crash at power-off/HOME (six workers unwinding at once through one shared exception block): a per-thread C++ exception block (`runtime/eh_globals.cpp`) with the build check `check_eh_globals.py`; audio and the SD writer are joined before exit. |
| Verify tour, 30 zones | Door and pot collision replays (`logic.txt` `crrskip`): 259,763 comparisons, 0 mismatches. |

Not verified on a console: the off-screen block cull of resident draws and the
smaller first submissions of a picture, both aimed at the console GPU time
above.

## Alpha 15: drawing overlaps the next picture's logic — 2026-11-18

| Check | Result and scope |
| --- | --- |
| Azahar: 30 zones at real speed (seed `54737253`) | Alpha 14 73.6 to about 54 ms per picture; zones under 20 FPS 23 to 16. |
| Azahar: 8 zones, one logic tick per picture | 54.8 to 41.6 ms. F_SP104 room 1: 13 to 25 FPS. Ordon Village at real speed: 145.6 ms (Alpha 13) to 84.1 ms. |
| Azahar: the overlap alone | Village 89.0 to 67.3 ms (`real-speed-off.txt`), 0 differing pixels against `gx-overlap.txt` `0`. |
| Azahar: audio on core 3 | 0 underruns in the village and the ranch minigame. |
| Logic rules (`logic.txt`) | Verify modes 0 mismatches; exhaustive host proofs for the maths rules. Actor searches by id and name: about 380 walks of about 30,000 nodes per village picture to 0. |

The owner ran Alpha 15 on a console; the Alpha 16 section reads those dumps.

## Alpha 14: second core, audio mixer, file-select glow — 2026-11-18

| Check | Result and scope |
| --- | --- |
| Console: 18 Alpha 13 dumps | Paint grows by about 175 us per draw (F_SP104 room 1 172 us, Ordon Village 181 us) while core 2 was busy 12 to 30 %. |
| Azahar: 8 zones (seed `54737253`, one logic tick per picture) | Alpha 13 59.3 to Alpha 14 54.8 ms (-7.6 %; F_SP108 -16 %); Ordon Village 94.6 to 89.7 ms. |
| Azahar: audio mixer at 32 kHz, fixed point | Ranch, same scene: 23 to 15 % of core 2; against the float mixer the largest difference is 0.027/32768, no sample over one step; no underruns. |
| Display-list templates | Interpreted lists in the village 55,000 to 10,000; 0 mismatches against the interpreter. |
| File select glow | Host test with the console's stale register values. |
| Measured, not used (pictures exact, slower in Azahar) | Native packets +14 ms per village picture; one draw per skinned shape +2.8 ms (about 8 draws saved); cached PICA blocks exact (95,000 checks, 0 bad) but +12 ms. |

## Alpha 13: the owner's slow scenes — 2026-11-18

| Check | Result and scope |
| --- | --- |
| Console: eight Alpha 12 dumps | Spring path 12 to 14 FPS, Ordon Village 8 to 11, ranch goat minigame 4.7, pause/save screen 5.5. |
| Azahar: ranch layer 5 | The minigame timer's 5.3 ms per picture is gone; logic 19 to 7.7 ms. |
| Azahar: Ordon Village logic | 90 to 48 ms per picture at 4 ticks. |
| Collision walks | 822,525 query results identical on 305 scenes of real collision data. |
| Spring-path sand | New exact planner rule, host-proven against the GX evaluator. |
| Measured, not used | Offline merging of room geometry would save 14 draws game-wide (`Tools/3ds/model_prep.py`). |

## Alpha 12: zone transitions, cheaper textures and logic — 2026-11-18

| Check | Result and scope |
| --- | --- |
| Console: the Alpha 11 run | 91 zones, no crash, mean 75 ms per picture; 354, 161 and at least 655 ms pictures right after the F_SP103 to F_SP104 fade (the reason for the transition hold). |
| Azahar: 8 zones (seed `54737253`, one logic tick per picture) | Alpha 11 63.8 ms, Alpha 12 59.8 ms with the default switches (logic per tick -2 to -4 ms per zone). |
| Exactness | KCol in native byte order: 305 of 305 disc files byte-identical on 2,800 queries each. `C_MTXConcat`: 4 million cases identical. CMPR interior tiles: exact against the pinned Aurora decoder on 2.29 million disc texels. TEV planner: operation refusals on Alpha 11 materials 148 to 42. |
| Off by default, verified | Native packets: 38,927 direct records and 272,507 shapes, 0 mismatches. PICA blocks: 243,000 ok, 0 bad. Both slower than the original path in Azahar. |
| Found | F_SP104's darkness is the original 18:00 palette (dump read byte for byte). Pre-converted textures in `zelda.twilight` would load slower on the console (3.5 times the bytes at 0.63 MB/s): built and tested, not used. |

## Alpha 11: no first-time stutter — 2026-11-18

| Check | Result and scope |
| --- | --- |
| Console: the Alpha 10 run (13 zones, seed `45704950`) | The test stopped in zone 14 (R_SP107 room 3): an actor whose release profile has no methods. First-draw canaries cost up to 3.7 s per 300 pictures (600 ms pictures with two new draws, 400 to 650 ms when the collection menu opened); log flushes on the game thread 27 to 110 ms on average, up to 5 s. |
| Changes | Such actors are not created; canaries off by default (`gpu-canary-on.txt`); the log and the guard's session record written by a low-priority SD writer thread; the DVD thread on the second core (`loader-core0.txt`); disc reads reported every 300 pictures. |

The Alpha 11 run itself is read in the Alpha 12 section.

## Alpha 10: the second core — 2026-11-17

The 3D shapes of a picture are drawn on the New 3DS's second application
core while the game thread issues the next ones, and each draw costs less
(docs/ARCHITECTURE.md, "Second-core worker"). No binary is published and no
console run of Alpha 8, 9 or 10 exists: every figure is Azahar 2126.2,
emulator time.

| Check | Result and scope |
| --- | --- |
| Azahar: pictures | Worker on and off: the three captures of each of the 14 comparison zones are identical pixel for pixel. The first capture of each zone is identical to Alpha 9's; the second and third are taken 60 pictures later than in Alpha 9 (the new `onecore` test phase), so they show another moment of the game: three of them were looked at with labels. File select: the 7 captures identical to Alpha 9's. |
| Azahar: 14 zones of the console's plan, one logic step per picture | Alpha 9 80.4 ms per picture, Alpha 10 with the worker off 78.1 (-3.0 %), Alpha 10 71.1 (-11.6 %). Eleven zones -7.2 to -19.9 %; Death Mountain, Zora's Domain and the Bulblin camp -2.8 to -3.9 % (the audio mixer shares core 2 and the worker rests there). |
| Azahar: verify modes | `gx-worker.txt` = `wv` in three zones: bank deltas, bank sums, damaged records, refused bindings, list failures and shadow job failures all zero, pictures identical. `mixer-verify.txt`: 30,000 audio subframes rendered both ways, none differs. |
| Azahar: guards of the worker | A fatal error forced on the worker's 300th shape reaches the main thread; the next start learns the worker off (`gx-worker-learned.txt`), a third keeps it off. |
| Azahar: the Alpha 7 gate plan, 35 zones at real speed | 31 end normally, 4 as "moved" (the same four as in Alpha 9), no rejected draw, no fault, no unmapped read. Mean of the 31: Alpha 9 83.9 to Alpha 10 78.5 ms per picture (-6.5 %): at real speed game logic is a larger share and the worker rests where the mixer needs core 2. |
| Azahar: real speed, worker off to on | Ordon Village 67.6 to 56.3 ms (-16.7 %), Faron Woods 78.1 to 63.8 (-18.2 %), Ordon Spring at night 100.7 to 86.9 (-13.8 %), Lakebed: Morpheel 132.7 to 116.8 (-12.0 %), Death Mountain 100.0 to 98.1 (-1.9 %, the worker rests). |
| Azahar: the zones the Alpha 7 console run ended in | All three end normally, no stale collider, no rejected draw. |
| Azahar: menus, title, end of a test | File select: the 7 captures identical to Alpha 9's. Title to the opening cutscene at real speed: no fault; the captures that differ show the same shots at another moment (looked at with labels). TESTS FINISHED, power-off request and core probe lines written. |
| Build checks | The shared-state audit (63 variables reached by both threads, each with a verdict) and the inlining check pass on the release image. |
| Host research runners | 74 of 114 pass (the new queue test among them); the same 40 fail on the Alpha 7 tree. Three runners that compile the draw adapter or the audio mixer were brought up to date for the new files. |

Not verified on a console: everything above. The worker's share of core 2
beside the audio mixer, cross-core memory and caches and the GPU used from
two threads are what Azahar cannot show; the `onecore` phase of the
automatic test and the guards exist for that
([Diagnostics](DIAGNOSTICS.md) for the switches).

Later: the owner ran Alpha 10 on a New 3DS (13 zones, seed `45704950`); the
Alpha 11 section reads that run.

## Alpha 9: the Alpha 8 water, corrected — 2026-11-17

The owner compared an Alpha 8 picture of Ordon Spring with the Alpha 7 one
and found the water wrong (flat beside the camera, a hard edge). Alpha 9
corrects it and changes nothing else; the Alpha 8 section below still
describes the rest. No binary is published and no console run of Alpha 8 or
Alpha 9 exists: every figure is Azahar 2126.2, emulator time.

| Check | Result and scope |
| --- | --- |
| Azahar: pictures against Alpha 7 | First picture of each of the 14 comparison zones (same game frame): 0 pixels differ in 12 zones, 5 and 8 of 96,000 in Faron Woods and Ordon Spring. Alpha 8: 18.2 %, 2.6 %, 2.0 % and 0.2 % in four zones. Labelled Alpha 7, 8 and 9 pictures of four water zones were looked at. |
| Azahar: 14 zones of the console's plan, one logic step per picture | Alpha 7 145.4 ms per picture, Alpha 8 79.9, Alpha 9 80.4. Water zones +0.3 to +1.7 ms over Alpha 8, the others +0.1 to +0.3 ms. All end normally, no rejected draw. |
| Azahar: the Alpha 7 gate plan, 35 zones at real speed | 31 end normally, 4 as "moved" (the test's walking reached an exit; two of them as in Alpha 7 and 8), no rejected draw, no fault, no unmapped read. Mean of the 31: 105.2 to 83.9 ms per picture against Alpha 7. |
| Azahar: the zones the Alpha 7 console run ended in | Two end normally; in the Bulblin camp the wolf was killed in the test's last phase ("died", the test's own status; life 13 of 15 quarters at the end of the Alpha 8 run). No stale collider. |
| Azahar: menus and title | File select 33.0 ms per picture, captures identical to Alpha 8's. Title to new game plays to frame 3700 with 42 captures and no unmapped read. |
| Azahar: end of a test | TESTS FINISHED, power-off request, core probe lines. Not run on a console. |
| Build check | The linked image holds no per-vertex reader or field pass of the vertex decoder as a function of its own (`Tools/3ds/check_hot_inlining.py`). |
| Host research runners | 73 of 113 pass; the same 40 fail on the Alpha 7 tree. |

Not verified on a console: the projection divided per pixel with vertices
behind the camera, the per-vertex warp, the arena return, and every time
above ([Diagnostics](DIAGNOSTICS.md) for the switches).

## Alpha 8: the Alpha 7 crash and a second performance batch — 2026-11-17

This update covers the changes made after the owner's Alpha 7 run on a New
3DS (56 random zones before a crash; 52 ended normally at 102.1 ms per
picture). No binary is published. Every Alpha 8 figure below comes from
Azahar 2126.2 and is emulator time; no console run of Alpha 8 exists yet.

Console runs so far (owner's New 3DS, automatic test where it existed):

| Build | Zones | ms per picture | Pictures per second | Note |
| --- | --- | --- | --- | --- |
| Alpha 0 | 1 (F_SP102 r0) | 70.9 | 14.1 | One logic step per picture: the game ran at 47 % of its speed. |
| Alpha 5 | 1 (F_SP104 r1) | 606.7 | 1.6 | The GPU guard ran every draw alone after an unexplained stop. |
| Alpha 6 | 85 ended normally | 119.9 | 8.3 | Paint 82.0, update 24.6, draw 10.0; 3.1 logic steps per picture. |
| Alpha 7 | 52 ended normally | 102.1 | 9.8 | Paint 62.9, update 25.8, draw 9.8; 2.6 logic steps per picture. |
| Alpha 6 to 7, the 12 zones both runs share | 12 | 121.4 to 85.5 | 8.2 to 11.7 | -29.6 %: paint -38.6 %, update -10.4 %, draw -5.3 %, draws -32.4 %. |

| Check | Result and scope |
| --- | --- |
| Azahar: the zones the console run ended in | Zones 56 to 58 of the console's plan complete; the stale-collider check added with the fix reports 0 (131 per run before it). |
| Azahar: 14 zones of the console's plan, one logic step per picture | Alpha 7 release 145.4 ms per picture, Alpha 8 79.9 ms (-45 %; paint -53 %). Typical zones gain 6 to 10 %, the zones with CPU-route draws 31 to 77 %. All end normally, no rejected draw. |
| Azahar: the Alpha 7 gate plan, 35 zones at real speed | 33 end normally, 2 as "moved" (as in Alpha 7), no rejected draw, no fault, no unmapped read. Mean of the 33: 106.9 to 84.6 ms per picture (-21 %). |
| Azahar: file select | 103.6 to 33.0 ms per picture with the retained 2D screens; 7 captures identical pixel for pixel. |
| Azahar: title to new game | Plays to frame 3700 with 42 captures and no unmapped read. In verify mode 97 of 18,529 screen comparisons differ, none with a picture difference found. |
| Azahar: pictures against Alpha 7 | First picture of each of the 14 zones: identical in 7, within 5/255 in 2, small in 2, 2 to 3 % of the pixels in 2 (flowers, warped water) and 18 % in Ordon Spring, where the water is calmer. |
| Azahar: end of a test | TESTS FINISHED, power-off request after the hold time, core probe lines written. The power-off and the probe have not run on a console. |
| Picture rate lock | Started, not finished: `picture-rate.txt` spaces the start of the pictures; they are still presented when their painting ends. Off without the file. |
| Host research runners | 73 of 113 pass; the other 40 fail on the Alpha 7 tree as well. |

Not reached: 30 pictures per second. Painting a typical zone takes 50 to 60
ms in Azahar. The remaining steps are GX translation on a second core (the
test now ends with a probe of the other cores for it), J3D material packets
that skip the register walk, and the game logic (up to four steps per
picture, 22 ms per step in one Lakebed Temple room). Physical speed,
full-game progression and console stability of Alpha 8 remain unverified.

## Alpha 7: performance from the Alpha 6 console run — 2026-11-16

This update covers the changes made after the owner's Alpha 6 run on a New
3DS (94 random zones, 72,726 measured frames: 117.5 ms per picture). No
binary is published. Every Alpha 7 figure below comes from Azahar 2126.2 and
is emulator time; no console run of Alpha 7 exists yet.

| Check | Result and scope |
| --- | --- |
| Azahar: the console's plan, zones 1-8 | Mean of the 7 zones that end normally in every build: 159.3 ms per picture in Alpha 6, 111.5 ms in Alpha 7. No rejected draws (Alpha 6: 208 and 260 in two zones). |
| Azahar: 35 zones of the plan | 33 end normally, 2 end as "moved" (as in Alpha 6), no rejected draw, no fault, no unmapped read. Mean 106.9 ms per picture, from 37 ms to 190 ms. |
| Azahar: zone 84 (night-sky stars) | Completes; it ended the game on the console in Alpha 6. |
| Azahar: zone 32 (Darkhammer's room) | Drawn (it was 83 % black), 49 ms per picture against 95. |
| Azahar: water shine | The black shore band of Alpha 6 is gone in the zones checked. No reference picture was compared. |
| Azahar: title to new game | Plays to frame 3690 with 42 captures and no unmapped read. The first release build failed this check (a null read at the first scene change, introduced in this cycle) and was rebuilt. |
| HUD tape | The 2D pass is translated every second picture and replayed as PICA command words in between: 3,413 recordings and 3,280 replays in the 8-zone run, same HUD in the screenshots. Never run on a real PICA; `hud-tape.txt` = `1` turns it off, and a stop in one of its lists turns it off on the next start. |
| Host research runners | 74 of 113 pass. The other 39 fail on the Alpha 6 tree as well (hardcoded toolchain path, private inputs, stale fixtures). |

Not reached: 30 pictures per second. The guest profile is flat after these
changes (no function above 4 % of the samples). The remaining steps are
architectural: GX translation on a second core, moving water on the GPU with
its per-vertex warp, and J3D material packets that skip the register walk.
Physical speed, full-game progression and console stability of Alpha 7 remain
unverified.

## Console test builds and cleanup — 2026-11-15

This update covers five private console test builds (Alpha 0 to Alpha 4) of
the GX compatibility route on the owner's New 3DS, a source cleanup (Alpha 4:
Alpha 3 plus the cleanup) and the fixes from the Alpha 4 run (Alpha 5). No
binary is published. Emulator results come from Azahar 2126.2; console
results come from the owner's logs, GPU guard records and dumps.

| Check | Result and scope |
| --- | --- |
| Console: title and intro | Runs at about 10-16 pictures per second with real-speed logic (30 logic steps per second). Cutscene cameras held and jumped until Alpha 2 ([ADR 0002](decisions/0002-int-sized-enums.md)); the Alpha 2 fix is emulator-verified, console confirmation pending. |
| Console: START+SELECT test | Starts from the title and travels to a stage (Alpha 2). |
| Console: entering gameplay | Stopped on the first gameplay picture in every Alpha 0 and Alpha 2 session (transfer engine after the minimap read-back). With CPU read-backs ([ADR 0003](decisions/0003-cpu-readbacks-and-transfer-stops.md)) Alpha 4 entered the opening cutscene and played about 220 pictures of it. |
| Console: model shadows | Alpha 4 and Alpha 5 (with its own position-only caster shader) stopped in the model-shadow pass; drawn one at a time the same pass ran for ten minutes. In Azahar the pass replayed a draw from reclaimed arena geometry nearly once per pass in the opening cutscene; Alpha 6 leaves those out, and a stop in the pass makes the next start use blob shadows. |
| Console: GPU guard isolate mode | After an unexplained stop, Alpha 5 ran its next start with every draw alone: 1.4 pictures per second. Alpha 6 records the stop only. |
| Console: HUD minimap | Visible during the Alpha 4 cutscene: its alpha was never written on 3DS. Fixed in Alpha 5 with five menus that had the same gap; checked in Azahar. |
| Console: saves | Creation and writing work on the SD card since Alpha 1 (whole-file replace without rename-over). |
| Azahar: title to new game | Intro, file select, name entry and the opening cutscene play; L+R+A dump verifies. |
| Azahar: read-back equivalence | CPU read-backs equal the transfer-engine read-backs byte for byte (240x400 frame, 216x216 minimap). |
| Azahar: automatic test | START+SELECT from the title intro completes six zones with no failure. |
| Azahar: random test | Random plans (rooms, clothes, equipment, items, form, time of day) found three game faults in their first eight zones: item models in a solid heap (slingshot), the test changing the save under a live Link, and leaked GX texture object slots (models lost their textures after seven zones). All fixed; two 8-zone plans then complete with no exception and no unmapped read. |
| Azahar: GPU guard paths | Both transfer-stop fallback levels play; a simulated transfer stop is recorded and classified. |
| Cleanup equivalence | Removing three compile-time probes leaves the preprocessed output of all eleven affected game and startup files byte-identical. |
| Public host suite and CI commands | All ten public host checks and the five conversion/format suites pass locally. |
| Host research runners | Seven runners that no longer built or ran (audio PCM, GPU queue, indexed geometry, framebuffer viewport, depth probe, both alpha-interval runners) and the vertex-decode runner build and pass again; see [Testing](TESTING.md#existing-research-runners). |

Menus with many 2D panels (file select, name entry) run at about 6-8 pictures
per second on both the console and Azahar. Physical 30 FPS, full-game
progression and whole-game console stability remain unverified.

## PAL data and retained GX correctness — 2026-10-29

This update targets the retained compatibility route with
`DUSK_PICA_GAME_DRAW=OFF`. It does not change the unfinished native-model
renderer default or include its separate migration/performance work.

| Check | Executed result and scope |
| --- | --- |
| Public host suite | All ten enumerated cases pass with host ASan/UBSan, including regional packing and card-permission consumer recovery. |
| Save safety | USA/PAL create/write/read/reopen checks pass. Foreign game/maker access and ownership changes are refused with complete container-byte preservation. |
| Card-menu recovery | Actual controller/file-select methods reach the existing non-formatting error state. The old controller is an expected failing busy-state control; matching ownership still dispatches loading. |
| DVD/version/JKR archives | Synthetic USA/PAL checks and read-only PAL-bundle integration pass, including original PAL logo and title resource lookup. No game payload is published. |
| GX planning/backend | Independent normalized material equations, ARM planner compilation, actual SDK register packing and prepared-state restoration pass. |
| GX adapter safety | Host sanitizer checks pass for automatic full-RGBA lookup, separate third-sampler fog, unobserved-alpha recovery, observable-alpha refusal and resource/submit failure paths. |
| Asset-free GPU probe | Nine cases pass in Azahar: affine/perspective comparisons differ by at most one channel byte; byte-atlas and following ordinary-state comparisons are exact. Incorrect vertex-premultiplied products are detected. |
| ARM game target | The complete compatibility game profile compiles/links/packages with devkitARM GCC 16.1.0 and the installed libctru/Citro3D SDK. |
| Normal PAL startup | The original 50/60-Hz selection screen renders and responds to scripted input without forced-stage entry; the run exits at requested frame 360 with completed guest shutdown and no detected fatal. |
| Original scene routes | PAL `F_SP103`, room 1, and Link's house `R_SP01`, room 4, render through requested frame 480 and complete guest shutdown. Reported rejected/simplified/skipped counters remain zero. |
| Controls and visible output | Actual RPC captures are scheduled against the native frame counter. After the house-entry animation, scripted Circle Pad movement changes Link's position in free-control mode; running and subsequent stopping are visible. The A edge is recorded by original PAD code; Ordon dialogue also advances. |
| Existing CI converter/format commands | All five SDK-independent geometry/material/mesh/texture/fragment suites pass locally without importing the separate native-renderer changes. |

Emulator checks use upstream Azahar commit `01623d2fb707317410225c0eced82e3e0c84edc1`,
Vulkan, explicit New3DS selection and a 100% CPU-clock setting. They run with
isolated XDG/SD directories and `--no-sandbox`, not network/write sandbox protection.
Audio output is silent; NDSP status does not establish audible playback.

Direct-stage entries initialize fresh state. These observations are not a natural
playthrough, complete save/progression acceptance, a full-fidelity GX proof or
physical console validation. Stable hardware 30 FPS remains unverified. The
native-renderer checkpoint results below are previously published evidence,
not additional acceptance supplied by this compatibility fix.

## Current native-renderer checkpoint — 2026-10-29

The new renderer, actual model/resource/frame/camera hooks, offline converters,
format contracts and synthetic tests are included in this source update.
Its [implementation and ten-step roadmap](NATIVE_RENDERER.md) describe the
remaining work. This is not a playable native-renderer release.

| Check | Current result and scope |
| --- | --- |
| Existing public host suite | Eight enumerated groups pass under host ASan/UBSan; no disc or emulator. |
| Native engine mutation controls | All 27 enumerated mutations are detected; each control compiles before its expected failing execution. |
| Native conversion/format checks | Nine individual geometry/material/texture/mesh/pose suites pass with synthetic inputs. Optional external-codec compression was not rerun. |
| Native engine host suite | Passes with Citro3D substitutes, including actual packet/archive/palette/camera helper bodies and 243 camera point-mapping cases. |
| Native dependency/ARM audit | Passes for 21 optimized translation units (15 C++ units and six embedded shader units); shader bytes match fresh Picasso assembly. |
| Shader checks | Weighted/rigid, affine-UV and projective algebra/ABI suites pass; not physical PICA precision. |
| Model-plan suite | Passes hierarchy/producer admission and all 65,536 modeled source bias values; filtering on hardware remains unverified. |
| Private fixture boundary | Six tests pass for ignored root-repository data, tracked-file rejection, nested repositories, symlinks and Git-check failure. |
| New native game rendering | Not executed. No original scene, full opening, gameplay or physical acceptance is claimed. |
| Current game target | Complete `dusklight-native` ARM compile/link passes with `DUSK_PICA_GAME_DRAW=ON`; the ELF contains native model-dispatch and camera symbols. No execution or playability claim. |
| Isolated source copy | All 20,695 public files match the intended Git tree. Publication/public-host/native-host checks and a fresh 21-unit ARM library build/audit pass without private research inputs. |
| Historical full game link | The October 25 result below predates the new model renderer and remains separate historical evidence. |

Detailed compiler commands and generated receipts remain local. The published
commands in [Testing](TESTING.md) and [Building](BUILDING.md) reproduce source
checks without those private records. Current GitHub Actions results must be
read for the exact pushed commit; this document does not assume they passed.

## Historical first publication — 2026-10-25

Date: 2026-10-25. Scope: the first public current-source snapshot.
No release, prerelease, tag, installation QR or downloadable binary is published.

## Checks for this publication

| Check | Result and scope |
| --- | --- |
| Source inventory | Current original-engine modifications, native platform, test/tool source and modified Aurora are included. Dependencies are ordinary files, with upstream pins documented. |
| Synthetic host suite | Passed on macOS with AddressSanitizer and UndefinedBehaviorSanitizer: disc bounds, I4/IA8 conversion, PICA math/alpha/clip planning, boot-stage parsing, JKR/bootstrap and original JUT/native controller integration. Synthetic dump-verifier tests also passed. |
| Publication and navigation | Checked before push: tracked-file boundaries, required documentation, local guide/image destinations and the exact OpenDSP hash. |
| Historical audio PCM host runner | Blocked at compilation: its test substitutes lack `threadCreate` used by the current NDSP initialization. This runner is not included in the passing public host suite; production audio compiles in the full native build. |
| GitHub Actions clean Linux checkout | Publication checker and all eight enumerated host cases passed. The workflow uses an explicit Ubuntu 24.04 runner and pinned Node 24 actions; see Actions for the current commit result. |
| Native SDK | GCC 16.1.0, existing libctru/Citro3D installation; see the build result below. |

## Native build result

The complete game profile configured, compiled and linked successfully from the
prepared public source tree on macOS using devkitARM GCC 16.1.0. CMake produced
`dusklight-native.elf`, `.smdh` and `.3dsx` under ignored `Build/public-game/`.
The public helper used all four original-game integration gates, with inspection
and optional CMake experiments disabled. No game input was needed or embedded.
These outputs are local verification artifacts and are not uploaded.

## Acceptance limits

No new emulator route or physical console run is claimed for this source
organization/publication. Earlier local scene investigations are historical
observations, not public reproduction artifacts. Full-game natural progression,
reliable story save/reload, complete graphical fidelity, sustained physical
30 FPS and whole-game physical audio remain unverified.

The root CI repeats the enumerated synthetic checks on a clean Linux checkout.
Its actual run status is available in the repository Actions tab. Passing it
does not change the hardware or progression limits above.
