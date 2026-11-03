# Testing and verification

## Public checks without game data

```sh
python3 Tools/3ds/check_publication.py
python3 Tools/3ds/test_public_host.py
```

The host runner builds existing synthetic native fixtures with C++20,
AddressSanitizer and UndefinedBehaviorSanitizer. It checks disc-structure bounds,
I4 texture decoding, PICA math/alpha/clip planning, bootstrap state and controller
mapping. It also runs synthetic USA/PAL bundle round trips and dump-verifier
tests. Generated binaries and fixtures stay in `Build/public-host/`; no original
disc is opened.

Use `--compiler PATH` to choose a host compiler and `--output PATH` to choose
an output directory. The runner fails on compilation, assertion, sanitizer or
Python test failure. It records commands and outcomes in `results.json`.

## GitHub Actions

The `Source checks` workflow runs on pull requests, pushes to `main`, merge
queues and manual dispatch, without secrets or game data:

| Check | Scope |
| --- | --- |
| `Lint` | Ruff correctness rules and Python 3.10 syntax for `Tools/3ds`, ShellCheck for the maintained shell helper, actionlint for workflow syntax/expressions/shell. |
| `C++ static analysis` | Clang's path-sensitive analyzer on SDK-independent native contracts and mesh/texture/material bank readers; analyzer findings fail the job. |
| `Publication policy` | Tracked-file policy, maintained documentation links and the pinned OpenDSP hash. |
| `Host (g++, Python 3.10)` / `Host (clang++, Python 3.14)` | Existing synthetic host fixtures under ASan/UBSan, plus five offline geometry/material and mesh/texture/material-bank suites. |
| `3DS (diagnostic)` / `3DS (game)` | Configure, compile and link both public profiles using the official devkitPro image pinned by digest. The game job also checks all six assembled native shader ABIs, including affine/projective variants. |

Jobs have timeouts, independent matrix failures, read-only repository permissions
and no persisted checkout credentials. Superseded runs are cancelled. No workflow
uploads build directories, game data, binaries or private receipts, creates tags,
or publishes releases. A green cross-build does not prove playability, PICA
execution or hardware performance. The full SDK-substitute engine suite remains
a separate local gate.

Dependabot proposes weekly grouped updates for action pins and the development
linters; it does not auto-merge them. The SDK image digest and actionlint module
version are maintained manually. Validate both native profiles before changing
the SDK pin. Ruff (MIT), ShellCheck (GPL-3.0) and actionlint (MIT) are development
tools only, not linked or bundled into the 3DS executable.

### Local lint commands

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r .github/requirements-lint.txt
.venv/bin/ruff check --config .github/ruff.toml Tools/3ds
.venv/bin/python -m compileall -q Tools/3ds
.venv/bin/shellcheck Tools/fetch-public-git-checkouts.sh
go install github.com/rhysd/actionlint/cmd/actionlint@v1.7.12
PATH="$PWD/.venv/bin:$PATH" "$(go env GOPATH)/bin/actionlint"
clang-tidy-18 --quiet --checks='-*,clang-analyzer-*' --warnings-as-errors='*' \
  Source/platforms/3ds/native/contracts.cpp \
  Source/platforms/3ds/native/pack.cpp \
  Source/platforms/3ds/native/texture_pack.cpp \
  Source/platforms/3ds/native/material_pack.cpp -- -std=c++20
```

Use Python 3.10 for the minimum-version syntax check and Go 1.26 for actionlint.
Ruff checks syntax, undefined names and invalid control flow/format expressions;
it deliberately does not enforce a new formatting style across historical tools.
Native library compilation already treats `-Wall -Wextra` warnings as errors.
The static analysis job uses Ubuntu's Clang 18; it does not claim coverage of
SDK-dependent renderer code or the full reconstructed game.

### Required repository settings

After these jobs have run on GitHub, a repository administrator should configure
a ruleset for `main` requiring the checks named in the table, a pull request with
at least one approving review, dismissal of stale approvals, resolved review
conversations, and blocked force-pushes/deletions. Keep fork workflow approval
enabled and the default Actions token read-only. These are GitHub settings, not
enforced merely by committing a workflow; no remote settings are changed here.

### CodeRabbit pull-request reviews

The root `.coderabbit.yaml` configures the CodeRabbit GitHub App alongside
Actions, not as a job with a stored API key. It reviews ready-for-review PRs and
subsequent pushes, posts a `CodeRabbit` check, fails that check on review errors,
and keeps automatic approvals and docstring/test generation disabled. Its review
instructions preserve the experimental, source-only and private-data boundaries.

Activation requires a repository owner to [install and authorize the GitHub App](https://docs.coderabbit.ai/platforms/github-com)
for **only this repository**. Review the requested access carefully: the app
requests read/write code, workflow, PR and check permissions independently of
Actions' read-only token. No app installation or permission grant is performed
by this configuration.

After a real PR has received a review, add the `CodeRabbit` check (expected source:
the CodeRabbit app) to the `main` ruleset if review completion should gate merges.
Keep human approval and resolved-conversation requirements: [the CodeRabbit check](https://docs.coderabbit.ai/pr-reviews/status-check)
can pass for skipped/rate-limited reviews and does not prove all findings were
resolved. Do not treat it as a substitute for tests or human review.

## New native renderer checks

The new library's host tests are separate from the compatibility host suite
above. Start with the SDK prerequisites and library build in
[Native renderer](NATIVE_RENDERER.md#build-and-test). With `clang`, `clang++`,
Python 3.10+ and the installed libctru headers, run:

```sh
python3 Tools/3ds/test_native_engine.py --output Build/native-engine-host
python3 Tools/3ds/test_native_model_plan.py --output Build/native-model-plan
python3 Tools/3ds/test_native_pose.py --output Build/native-pose
python3 Tools/3ds/test_private_fixture_git.py
```

The main native runner compiles actual library code with explicit Citro3D
substitutes under ASan/UBSan. It extracts selected actual game packet, archive,
palette and camera helper bodies. It also exercises the real shared-queue
completion wrapper with modeled SDK events, including command-arena reclamation
and failed-completion refusal. Generated original-format inputs are synthetic,
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

### Actual native GPU probe

```sh
python3 Tools/3ds/build_native_scene_probe.py \
  --build-dir Build/native-scene-gpu-check
```

Run the resulting `native-scene-probe.3dsx` on a labelled emulator or console.
This asset-free program exercises the actual Renderer and six assembled shader
variants, comparing a baseline and 297 cases against independent CPU lighting,
fog and interpolation references. It also runs 12 cull cases using synthetic J3D
geometry and MAT3 materials processed by the production converters and pack
readers: both triangle windings, GX none/front/back, and orthographic/perspective
projection. The reference draw uses the actual compatibility projection and
cull-mapping helpers with the native Renderer as a proxy, not the full GX
compatibility backend. Independent CW-front visibility and exact whole-image
candidate/proxy parity are both required. GX cull-all remains an explicit
source-converter refusal, not a native draw mode.

A build alone is not a GPU pass: inspect
`sdmc:/3ds/dusklight-native-scene/run-*/receipt.txt`, `metrics.csv` and
`cull_metrics.csv`. All 297 lighting/fog and 12 cull cases must complete with
zero failed comparisons. Regenerate source-derived material banks after changing
the offline converter; previously built banks retain their old cull values.

Raw readback rows have the opposite Y origin from the physical viewport.
Complete ABGR and LCD surfaces are scalar-acquired into CPU-owned snapshots
before filesystem writes; every LCD RGB pixel must match its ABGR counterpart.
Coverage, exterior-clear pixels and following ordinary-state restoration remain
checked independently of shader-upload replay. The native vertex ABI has nine
attributes, so both buffer fetch and shader-input permutations must retain the
upper nibble for skin weights. SDK substitutes cannot certify that real upload.
Neither this bounded matrix nor an emulator pass establishes full-game coverage,
physical PICA equivalence or hardware 30 FPS.

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
