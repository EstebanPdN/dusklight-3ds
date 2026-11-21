# Testing and verification

## Public checks without game data

```sh
python3 Tools/3ds/check_publication.py
python3 Tools/3ds/test_public_host.py
```

The host runner enumerates eleven synthetic checks. Its C++ checks use C++20,
AddressSanitizer and UndefinedBehaviorSanitizer; `disc-pack` and `dump-verifier`
execute Python tests. It covers disc bounds, I4/IA8 decoding, PICA math,
boot-stage parsing, particle emitter culling (`particle-cull`: the screen
bounds of every particle quad shape and the order of merged emitter runs),
bootstrap/controller integration, USA/PAL bundle round trips, dump
verification and card-permission consumer recovery. Generated binaries and
fixtures stay in `Build/public-host/`; no original disc is opened.

The packed-dump cases of `dump-verifier` need Python 3.14 or later (the
standard `compression.zstd` module); older versions report them as skipped.
CI runs Python 3.14.

Use `--compiler PATH` to choose a host compiler and `--output PATH` to choose
an output directory. The runner fails on compilation, assertion, sanitizer or
Python test failure. It records commands and outcomes in `results.json`.

Root GitHub Actions runs the publication checker and these synthetic host checks
on pushes and pull requests. It has read-only repository permissions. It does
not build a release, create tags, upload game data or publish binaries.
It also runs five SDK-independent new-native suites: offline geometry/material
conversion and native mesh/texture/material bank formats. The complete new
engine mock suite and ARM/shader checks require the additional prerequisites
below; they are not implied by the CI job.

## Regional data and GX compatibility checks

The public suite includes synthetic USA/PAL packing and permission-error recovery
through actual card-controller and file-select methods. Save ownership and
original DVD/JKR integration have additional host sanitizer checks:

```sh
python3 Tools/3ds/test_framebuffer_execution.py --suite storage \
  --output Build/regional-save-check
python3 Tools/3ds/test_native_archive.py --output Build/regional-archive-check
```

The save fixture creates disposable data, checks both regions and rejects
foreign game/maker access while preserving the complete original container.
The consumer regression additionally verifies that permission denial leaves the
busy state and reaches a non-formatting terminal error, while matching ownership
still dispatches the ordinary load path.
Use new output directories; the storage and renderer-safety runners refuse
to reuse an existing result directory.

With `clang++`, devkitARM and `DEVKITPRO` pointing at the installed SDK:

```sh
python3 Tools/3ds/test_gx_pica_material.py --output Build/gx-material-check
python3 Tools/3ds/test_gx_material_backend.py --output Build/gx-register-check
python3 Tools/3ds/test_renderer_safety.py --output Build/gx-adapter-check
python3 Tools/3ds/build_color_lookup_probe.py --build-dir Build/color-lookup-probe
```

These host checks exercise original material equations, independent raster RGBA,
buffer-alpha lifetimes, actual SDK register packing and adapter failure paths.
The default full-color adapter case also checks two lookup resources plus a
third fog resource without overwriting either color's coordinates.

Run the generated `color-lookup-probe.3dsx` separately on a console or emulator.
It needs no game data. Its SD directory
`/3ds/dusklight-color-lookup/run-<tick>/` contains a completion receipt and
pixel metrics for affine, perspective and byte-atlas inputs. It compares identity,
fragment products and textured products, verifies subsequent ordinary-state
restoration, and rejects the incorrect vertex-premultiplication alternative.
Compilation or the absence of an emulator crash is not a passing GPU result.
Require `status=pass`, all nine cases, and passing metrics.

The probe validates a nonuniform reference and acquires synchronized readbacks
on the CPU before filesystem writes; a uniform clear alone cannot detect a
corrupted readback. Comparisons allow bounded UNORM rounding, not a blanket claim
of exact GX integer arithmetic or physical PICA behavior.

Use the explicit [compatibility configuration](BUILDING.md#retained-gx-compatibility-renderer)
for original-game route testing. The native-model renderer default is unchanged.

## New native renderer checks

The new library's host tests are separate from the compatibility host suite
above. Start with the SDK prerequisites and library build in
[Native renderer](NATIVE_RENDERER.md#build-and-test). With `clang++`, Python 3.10+
and the installed libctru enum headers, run:

```sh
python3 Tools/3ds/test_native_engine.py --output Build/native-engine-host
python3 Tools/3ds/test_native_model_plan.py --output Build/native-model-plan
python3 Tools/3ds/test_native_pose.py --output Build/native-pose
python3 Tools/3ds/test_private_fixture_git.py
```

The main native runner compiles actual library code with explicit Citro3D
substitutes under ASan/UBSan. It extracts selected actual game packet, archive,
palette and camera helper bodies. Generated original-format inputs are synthetic,
not captured game data. Native resources, instance lifetimes, failure paths,
retirement, ordered submission and camera contracts are exercised. Add
`--negative-controls` to the engine or pose runner for their mutation checks.

Each of these additional format/conversion suites takes `--output PATH`:

| Area | Command filename under `Tools/3ds/` |
| --- | --- |
| Native mesh bank | `test_native_pack.py` |
| Native texture bank | `test_native_texture_pack.py` |
| Native material bank | `test_native_material_pack.py` |
| Dynamic material parameters | `test_native_material_parameters.py` |
| Material temporary/buffer rules | `test_native_material_temporaries.py` |
| Offline original geometry | `test_j3d_native_geometry.py` |
| Offline original materials | `test_j3d_native_materials.py` |
| Offline original textures | `test_j3d_native_textures.py` |

The assembled shader ABI checks need a completed native library build:

```sh
python3 Tools/3ds/test_native_shader.py \
  --shader-binary Build/public-game/native_scene.shbin \
  --rigid-binary Build/public-game/native_scene_rigid.shbin \
  --output Build/native-shader-host
python3 Tools/3ds/test_native_uv_shader.py \
  --build-dir Build/public-game --output Build/native-uv-host
python3 Tools/3ds/test_native_projective_shader.py \
  --build-dir Build/public-game --output Build/native-projective-host
```

Shader algebra interpreters and binary ABI inspection do not execute PICA or
model its float24 precision. Optional compression checks additionally require
an explicitly pinned, locally supplied codec source; they are not part of the
SDK-only commands above. No native game rendering, emulator route, physical
acceptance or speedup follows from these host results.

## Existing research runners

`Tools/3ds/test_*.py` and `Source/platforms/3ds/tests/` preserve the current test
source. They are not one uniformly runnable public suite. Some older runners
expect `/opt/devkitpro`, a user-provided disc, fetched public references or
historical private baselines under `Diagnostics/`. Such tests need those local
inputs and should not be represented as passing in a clean checkout.

These research runners were brought up to date with the current runtime on
2026-11-15 and pass on macOS with Apple Clang (ASan/UBSan):

| Runner | Notes |
| --- | --- |
| `test_audio_pcm.py --evidence PATH` | NDSP lifecycle, PCM capture and WAV export |
| `test_gpu_queue_lifecycle.py --output PATH` | GPU queue, GPU guard timeouts and hang records |
| `test_gx_indexed_geometry.py --output PATH` | Geometry arena, flush and clip-guard contracts |
| `test_framebuffer_viewport.py --output PATH` | Framebuffer raster-state scope |
| `test_vertex_decode.py --output PATH` | Vertex decoders, compact records and their caches |
| `test_depth_retained_probe.py --output PATH` | Developer depth probe on the transfer read-back path |
| `test_alpha_interval.py --output PATH` | Alpha-interval backend, negative control, command-draw adapter |
| `test_alpha_interval_ownership.py --output PATH` | Target ownership, guarded copies and clears |

The two alpha-interval runners compile against the libctru 2.7.0 and Citro3D
1.7.1 sources extracted under `Upstream/extracted/` (not tracked). The depth
probe and ownership fixtures model transfer-engine read-backs and select them
with the SD switch `readback-transfer.txt` in their working directory; the
CPU read-back the game uses by default is compared byte for byte in Azahar
([ADR 0003](decisions/0003-cpu-readbacks-and-transfer-stops.md)). The
command-draw adapter cases check the single-texture rigid shader with a host
model; the multi-texture shader, the default route, has no host model and is
checked in Azahar.

### Runners added in Alpha 10 to Alpha 21

Each runner compiles the production source it names together with a test under
`Source/platforms/3ds/tests/`. They are research runners like the ones above,
not part of CI. Most take `--output PATH`; the ones marked ARM also need
devkitARM, and those marked picasso the devkitPro shader assembler. A runner
whose default reference lives under `Build/` or `Diagnostics/` (both untracked)
needs that local snapshot or an explicit argument. Dated results are in
[Verification status](STATUS.md).

| Runner | Compiles | Checks |
| --- | --- | --- |
| `test_audio_mixer.py` | `audio_mixer_test.cpp` | ADPCM decoder and fixed-point mix against the float path |
| `test_eh_globals.py` | `eh_globals_test.cpp` | One C++ exception block per thread (ASan/UBSan and TSan); with `--image`, the linked-image gate |
| `test_font_pane_lifecycle.py` | `font_pane_lifecycle_test.cpp` | Original font owner with the Alpha 17 pane budget |
| `test_gpu_canary.py` | `gpu_canary_test.cpp` | Console GPU guard with stand-ins for GSP, the GX queue and the SD |
| `test_gx_indirect.py` | `gx_indirect_gpu_test.cpp`, `gx_indirect_hookup_test.cpp` | Indirect warps on the GPU route (ARM, picasso) |
| `test_gx_rigid_texgen.py` | `gx_rigid_texgen_test.cpp`, `native_replay_plan_test.cpp` | Multi-texture GPU texgen against the CPU vertex path (ARM, picasso) |
| `test_gx_worker.py` | `gx_worker_test.cpp` | Register bank deltas and the second-core shape queue, threaded part under TSan (ARM) |
| `test_invisible_model_entry.py` | its own fixture | Native model and dependent Z-sort packet entry |
| `test_j3d_skin_matrix_runs.py` | `j3d_skin_matrix_runs_test.cpp` | Skin matrix runs and the worker's bank parity |
| `test_material_memo.py` | `material_memo_test.cpp` | J2D material memo against the typed GX setters (ARM) |
| `test_native_logic.py` | `native_logic_test.cpp`, `native_math_exhaustive_test.cpp` | `logic.txt` rules: process id table, shared skin products, maths rules without FMA |
| `test_native_material_identity.py` | `native_material_identity_test.cpp`, `native_material_identity_reference.cpp` | Material planner against a reference planner (local reference snapshot) |
| `test_native_material_operation_rules.py` | `native_material_operation_rules_test.cpp`, `native_material_identity_reference.cpp` | Alpha 12 operation rules against the Alpha 11 planner (local reference snapshot) |
| `test_native_packets.py` | `native_packets_test.cpp` | Native packets against the original path, verify modes included (ARM) |
| `test_pica_blocks.py` | `pica_blocks_test.cpp` | Kept PICA words against the normal replay path (ARM) |
| `test_skin_palette.py` | `skin_palette_test.cpp` | Merged matrix groups of skinned shapes |
| `test_static_compact_cache.py` | `gx_static_compact_cache_test.cpp` | Static geometry cache against the uncached decode (ARM) |
| `test_story_benchmark.py`, `test_story_chapters.py` | their own fixtures | Story route rules, chapter itinerary and result evaluators |
| `test_texture_compact.py` | `texture_compact_test.cpp` | 16-bit texture plans against the RGBA8 picture |
| `test_tour_cadence.py` | `tour_cadence_test.cpp` | Cadence interchange, every-picture accounting and the strict gate |
| `test_vertex_variants.py` | `vertex_variant_test.cpp` | Vertex program variants against the general program, by execution (ARM, picasso) |
| `test_wolf_door.py` | its own fixture | Fishing Pond door cut; optional disc check |
| `test_world_mips.py` | `world_mips_test.cpp` | Generated mip levels, coverage per level and register rules |
| `test_zone_harness.py` | (Python and a small host build) | Zone table, start points, zone statuses and restarted-test timing |

Existing runners also gained cases: `test_native_bootstrap.py` compiles
`boot_options_test.cpp`, `test_gx_uniforms.py` compiles `gx_uniform_row_test.cpp`
and `test_application_dump_verifier.py` compiles `packed_dump_index_test.cpp`.
`native_lod_test.cpp` (the `lod.txt` rules) carries its own build command in
its header. `benchmark_capture_test.cpp`,
`frame_telemetry_csv_test.cpp`, `gx_resident_cull_test.cpp` and
`tour_visual_capture_test.cpp` have no runner yet; they include their runtime
headers directly (`-I Source/platforms/3ds/runtime`).

The Mac-only proofs of the offline tools need the owner's own disc data and are
run through their tools: `model_lod.py --test` and `--unit`, `model_prep.py`,
`pretexture_pack.py` (with `pretexture_store_test.cpp`) and
`survey_material_refusals.py` ([tool index](TOOL_REFERENCE.md)).

Do not run every historical `test_*.py` through discovery: several files are
executable research workflows with different prerequisites and evidence paths.
The public runner deliberately enumerates reproducible synthetic checks.

## Evidence levels

| Level | Establishes | Does not establish |
| --- | --- | --- |
| Publication checks | Tracked-file policy, links and firmware integrity | Code behavior or legal ownership of arbitrary added content |
| Synthetic host tests | Exercised API contracts under host sanitizers | ARM precision, PICA output or hardware behavior |
| Native configure/compile/link | Build closure with the selected SDK and flags | Correct boot, playable progression or frame rate |
| Azahar route tests | The exact visible/input/audio-callback route observed | Physical console timing, speaker output or complete story progression |
| Physical New3DS checks | Recorded behavior on that model and build | Untested scenes or all-game stability |

## Runtime changes

Keep exact build options, commit, input revision, scene/room, actions and duration.
Use normal title/new-game progression as well as controlled direct-stage entry.
Check several locations, menus, cutscenes, transitions, combat and save/reload
when a shared engine change can affect them. Preserve failing cases and describe
skipped or blocked gates explicitly.

Compare captures at equivalent frames and game states. A clean process exit,
zero rejected draws, high assertion count or seeded card is insufficient to
prove a complete playable route. Physical performance needs physical timing;
keep emulator observations separate.
