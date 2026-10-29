# Native PICA200 renderer

Updated: 2026-10-29. This is a development checkpoint, not a playable release.

The objective is to replace runtime GameCube GX translation with prepared native
resources, explicit game parameters and direct Citro3D submission. The new
`dusk::pica` library has no GX command decoder, GX material planner, JSystem
dependency or CPU vertex-transform path. The surrounding game still contains
compatibility rendering for unmigrated systems. **Zero runtime GX translation
for the complete game has not been achieved.**

## Source map

| Component | Location and responsibility |
| --- | --- |
| Native library | [native/](../Source/platforms/3ds/native/README.md): resource contracts, owner-thread renderer, six PICA vertex shaders, bounded quad/model/scene submission, pose evaluation and camera preparation. |
| Native data readers | Mesh, texture, material, pose, model-plan and asset-envelope readers beside the renderer. Their format/ownership documents are linked below. |
| Offline conversion | `Tools/3ds/j3d_native_*.py`: source geometry, textures, materials, samplers, hierarchy and pose conversion on the host. Unsupported source semantics reject explicitly. |
| Pack writers | `Tools/3ds/native_*_pack.py`: deterministic native bank/envelope creation. These are development formats, not a complete one-click game-data pipeline. |
| Game model entry | `J3DPacket.cpp` and `J3DModel.cpp`: source shape ownership and parameter callbacks select native resident parts without executing the model's source display list. |
| Resource identity | `d_resorce.cpp` captures original model SHA-256 and extent before loading mutates its bytes; `GameAssets` admits matching native data on the render owner. |
| Camera | `m_Do_graphic.cpp` publishes selected high-level game camera/window inputs. Unconverted projection/viewport/list changes invalidate them instead of being translated. |
| Frame lifetime | `runtime/game_startup.cpp` connects native model work to the existing verified shared SDK frame boundary. Deferred resource retirement requires completion. |
| Tests | `Source/platforms/3ds/tests/native_*` and `Tools/3ds/test_native_*.py`: synthetic inputs, SDK substitutes, failure injection, source-body extraction and offline shader algebra. |

Original file paths are retained to avoid breaking source inventories and
upstream relationships. Generated banks, shader binaries, test receipts, builds,
game data and captures belong outside tracked source directories.

## What is implemented

- Immutable native mesh/image resources, generation-checked handles, bounded
  budgets, explicit owner-thread submission and completion-dependent retirement.
- Weighted/rigid GPU vertex processing, position/normal palettes, projection,
  basic directional lighting, affine UVs and an experimental projective variant.
- Native combiner, alpha, blend, depth and sampler contracts; dynamic material
  parameters, ordered parts, shared resources and explicit partial-draw errors.
- Offline source parsers and native binary writers/readers, including original
  model identity, copied shape mappings and instance lifetime management.
- Selected actual model packet, resource-loader, pose-palette and scene-camera
  hooks. Host fixtures exercise extracted production bodies under substitutes.
- Main-target camera orientation, viewport/depth mapping, invalid-input refusal
  and invalidation lifecycle. Physical pixel-edge precision is unverified.

These are implementation and host/compile claims. They do not establish a
rendered original scene, animation fidelity, complete materials or playability
through the new renderer.

## Current integration limits

`DUSK_PICA_GAME_DRAW` defaults **ON** when original game startup is enabled.
Model packets use native dispatch and reject missing bindings/inputs. They have
no automatic fallback to the old model renderer. Full native asset coverage and
camera/material/environment producers are unfinished, so the default game
profile can compile while stopping at an unsupported model draw.

The live source producer currently supplies prepared pose palettes and ordering,
plus an explicitly published main-target camera. Texture generation, animated
material colors, complete lighting/fog, billboards and some deformation modes
remain unsupported. External-image variants lack a verified context key and
are refused. Admission of a previously unseen asset after native draws have
already been emitted is also restricted; a preloading/streaming policy is needed.

The temporary development asset lookup is
`sdmc:/3ds/DuskLight/native/<source-sha256>.dna`. This is not a supported SD
installation contract. Embedding native resources in the single user-prepared
`zelda.twilight` remains pending; the existing bundle does not supply all these
native assets. No generated game bank is included in Git.

UI, fonts, custom draws, effects, render-target/copy behavior and original
display-list production are still being migrated. The compatibility sources
remain in the tree. Native shaders and materials also need physical PICA
precision, sampling, fog, overlap and depth validation. No performance or memory
saving is inferred from host execution or configured budgets.

## Ten implementation steps

Work advances through these steps with shared fixes and brief regressions;
finishing a library component does not mark a whole step accepted.

1. **Audit and stabilize the integration boundary.** Reconcile pending changes,
   verify startup and allocation ownership, and inventory remaining GX consumers.
2. **Complete offline resource preparation.** Convert original models/materials/
   textures into native data with provenance and embed them in `zelda.twilight`.
3. **Complete native loading and residency.** Preload, share and retire geometry,
   textures and metadata with bounded, explicit lifetimes across transitions.
4. **Finish GPU vertex and camera inputs.** Connect actual animated palettes,
   billboards, camera/window/scissor and deformation cases without CPU vertices.
5. **Complete native materials.** Supply animated colors, transparency, blending,
   depth, draw ordering and uncommon source operations with documented precision.
6. **Complete lighting, fog and texture generation.** Feed explicit game inputs
   into native shaders/materials and validate supported effects and ranges.
7. **Optimize asset and memory working sets.** Measure compression, mip selection,
   streaming, batching and joint RAM/VRAM budgets without losing gameplay/UI.
8. **Migrate effects and targets.** Implement shadows, water, particles, feedback,
   copies and synchronization with native render targets and safe failure paths.
9. **Migrate the remaining draw producers.** Replace HUD/fonts/menus/custom draws,
   then remove their GX translation and obsolete CPU vertex-rendering paths.
10. **Validate the game on New3DS.** Test the fixed 15 diverse scenarios in order,
    transitions, opening/progression, audio, saves and long sessions; profile on
    hardware and complete a reproducible beginning-to-end playthrough.

The current focus is step 1, with partial implementations supporting later steps.
No percentage or completion date is supported by the existing evidence.

## Build and test

Use the installed SDK versions documented in [Building](BUILDING.md); no toolchain
upgrade or game data is needed for this source verification. Configure the game
build without producing a package, then build just the new library:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
python3 Tools/3ds/build_public.py --profile game --configure-only
cmake --build Build/public-game --target dusk_pica_engine --parallel 4
python3 Tools/3ds/audit_native_engine.py \
  --build-dir Build/public-game --output Build/native-audit
python3 Tools/3ds/test_native_engine.py --output Build/native-host
```

The host engine runner requires `clang++` and installed libctru GPU enum headers;
Citro3D behavior is substituted. It does not execute PICA code. Add
`--negative-controls` to verify that the enumerated mutations are detected.
An optional local Citro3D source oracle is used only when present; the receipt
records availability. The source-matrix and point-projection oracles always run.

Run the individual conversion/format suites described in [Testing](TESTING.md).
Outputs must use fresh ignored directories. The native renderer development
checkpoint has not been run in Azahar or on a console; historical compatibility
renderer routes are not its acceptance evidence.

## Format and API references

- [Mesh format](../Source/platforms/3ds/native/PACK_FORMAT.md)
- [Texture format](../Source/platforms/3ds/native/TEXTURE_PACK_FORMAT.md)
- [Material format](../Source/platforms/3ds/native/MATERIAL_PACK_FORMAT.md)
- [Model ownership](../Source/platforms/3ds/native/MODEL_CONTRACT.md)
- [Model plan](../Source/platforms/3ds/native/MODEL_PLAN_FORMAT.md)
- [Pose contract](../Source/platforms/3ds/native/POSE_CONTRACT.md)
- [Asset envelope](../Source/platforms/3ds/native/ASSET_PACK_FORMAT.md)
- [Game integration](../Source/platforms/3ds/native/GAME_DRAW_CONTRACT.md)
- [Ordered scene submission](../Source/platforms/3ds/native/SCENE_CONTRACT.md)

Public evidence and remaining verification gates are recorded in
[Status](STATUS.md). No binary, release, QR, game data or private diagnostic
archive accompanies this source update.
