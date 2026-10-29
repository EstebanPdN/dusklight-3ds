# ADR 0001: Prepared native resources and explicit game inputs

Date: 2026-10-29. Status: adopted development direction; whole-game migration
and physical acceptance remain incomplete.

## Context

The retained renderer decodes GameCube commands, plans materials and performs
some vertex work at runtime. The project objective is a fully native PICA200
renderer with bounded memory and minimal CPU overhead. Replacing its backend
alone would preserve that translation cost and would not meet the objective.

## Decision

Convert supported geometry, textures, materials and model metadata offline.
Give the native renderer only validated native banks, resource handles and
explicit game parameters. Keep original game model/shape ownership and packet
ordering while replacing the actual draw entrypoints incrementally.

The `native/` module does not import GX, Aurora or JSystem. Game-facing hooks
supply source identity and already-calculated joint/camera data through narrow
interfaces. The GPU processes mesh vertices. Pose graph and per-pass matrix
preparation on the CPU are separate from CPU vertex rendering.

Missing resources, unsupported inputs and lifetime violations reject explicitly.
The current native model route has no automatic compatibility-renderer fallback.
The public game build selects this unfinished route by default; it is a source
checkpoint, not a supported playable build.

## Alternatives

- Continue optimizing GX translation: useful for historical compatibility work,
  but would retain the runtime interpreter the current objective removes.
- Replace every subsystem at once: fewer temporary boundaries, but no bounded
  way to verify resource, ordering and lifetime changes independently.
- Silently fall back for unsupported native draws: would obscure coverage and
  make native performance and correctness claims unreliable.

## Consequences

Native resource formats and their conversion provenance must be maintained.
The game needs complete producers for camera, animation, texgen, material colors,
lighting and fog. Existing UI/effects still need migration. Temporary `.dna`
lookup and model-cache bounds are development constraints, not final packaging
or residency design. Until those inputs and assets exist, game draws can refuse.

No source-move campaign, global toolchain upgrade or loss of original saves/data
is required. Existing upstream paths and licenses remain intact. Generated game
banks stay private and out of Git.

## Verification and reconsideration

Use the [native contract/format tests](../TESTING.md), ARM compilation, shader
ABI checks and the native dependency audit. Track original scenes, progression,
audio and physical precision/performance separately. A passing host assertion
or compiled archive cannot establish a playable scene.

Revisit resource limits and preloading when representative workloads demonstrate
pressure; keep admission failures visible until a verified replacement exists.
Remove a GX producer only after its native replacement covers its actual users
and passes the applicable regression and hardware gates. Keep this record if a
later decision supersedes it, linking that replacement explicitly.
