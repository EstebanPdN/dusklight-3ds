# Testing and verification

## Public checks without game data

```sh
python3 Tools/3ds/check_publication.py
python3 Tools/3ds/test_public_host.py
```

The host runner builds existing synthetic native fixtures with C++20,
AddressSanitizer and UndefinedBehaviorSanitizer. It checks disc-structure bounds,
I4 texture decoding, PICA math/alpha/clip planning, bootstrap state and controller
mapping. It also runs synthetic dump-verifier tests. Generated binaries and
fixtures stay in `Build/public-host/`; no original disc is opened.

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

The historical audio PCM runner currently fails its host lifecycle compile because
its test substitutes do not declare the production `threadCreate` initialization
path. Its source is retained; it is outside the passing public host suite. The
full native game build compiles that production path with the real SDK. Host
PCM lifecycle coverage must be updated before it can be claimed again.

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
