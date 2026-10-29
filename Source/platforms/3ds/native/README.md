# Native PICA engine

This is a new standalone Citro3D renderer, not a wrapper around the game's
existing compatibility renderer. It has no GameCube command interpreter,
register bank, runtime material translation or CPU vertex transform. The game model-packet entrypoints now have a native dispatch path, selected by
`DUSK_PICA_GAME_DRAW` when game startup is enabled. The source-identified native asset factory is connected to those entrypoints;
complete material/environment input producers are still incomplete: this is not a working migrated
game. UI/effects and display-list production still use compatibility code, and
full-game/physical acceptance remains absent. See GAME_DRAW_CONTRACT.md.

Configure the toolchain as described in [the public guide](../../../../docs/NATIVE_RENDERER.md#build-and-test), then build only this library:

```sh
cmake --build Build/public-game --target dusk_pica_engine -j 4
```

This command builds the library alone. Native game builds link it as a
dependency; this command does not create a game executable/package. No emulator
is launched. The library is compiled with optimization, strict warnings, no C++
exceptions/RTTI and compiler stack reports. Six vertex variants are assembled
with the installed Picasso and embedded in the library: general/rigid geometry,
each with passthrough, native affine UV or position-based projective processing.

## Native data flow

Prepared native assets -> immutable linear mesh/texture allocations -> native
material handles -> ordered draw calls -> Citro3D -> PICA200.

- `Vertex` is a fixed 60-byte native record: position, normal, three UV pairs,
  RGBA bytes, four matrix row offsets and four UNORM8 weights summing to 255.
  Mesh chunks contain at most 65,536 vertices and 65,535 triangle indices.
- The general shader performs four-influence skinning from eight position/normal
  matrices, projection, normal normalization, ambient plus two directional
  Lambert lights, color scaling and three UV outputs. Unlit draws avoid the
  lighting work. A proved first-weight-only mesh uses a specialized shader with
  the same arithmetic and fewer inactive influence branches. The CPU supplies joint matrices and validates asset contracts;
  it never transforms mesh vertices for drawing.
- `PosePack` supplies native hierarchy, inverse-bind/blended matrices and explicit
  normal policies using caller-owned workspaces. Its palette gather connects to
  native model-plan IDs. The Mac resolves original rest-pose conventions; no
  source joint/command interpreter enters the renderer. Animation curves,
  procedural/camera producers and game integration remain unfinished. See
  POSE_CONTRACT.md for numeric, borrowing and conversion limits.
- `DrawUniforms::uvTransforms` provides a native two-row affine matrix and one
  of three input UV channels for each sampler. PICA performs scale, rotation and
  translation. Identity/unconsumed transforms retain the original passthrough
  shader binaries and do not upload the extra UV uniforms. This supports native
  animated parameter updates; it does not yet convert original texture animation
  curves.
- `DrawUniforms::projective` provides a complete native position-to-STQ matrix
  for texture unit0, from object or GPU-posed positions. PICA performs projection
  in its sampler. It is disabled by default; signed/near-zero Q sampling and
  physical precision remain unverified. Native source material binding is still
  unconverted. Normal/reflection texgen remains missing. Unit0 affine/source
  transforms must be identity when its projective transform is consumed; compose
  the complete native projection matrix before drawing.
- A stable borrowed sampler descriptor enables projection on unit0 without
  changing the owned image's ordinary2D descriptor used by units1/2. No pixel
  allocation/copy is added. Passthrough draws do not copy this descriptor.
- Six native combiner stages are prepared once at material creation. There
  is no source-platform material plan on the draw path. Blending, alpha/depth
  tests, culling and an authored native fog LUT belong to the material.
- Textures arrive already tiled/compressed for PICA: RGBA8, RGB565, ETC1 or
  ETC1A4, with explicit authored mip payloads. This module never decodes source
  textures, resizes them or generates runtime mipmaps.
- `QuadBatch` emits up to 32 ordered quads per draw into the same native arena.
  It refuses pending geometry across a frame, target, scissor, clear or
  interleaved draw boundary. It never sorts transparent geometry or silently
  skips a failed batch. Already emitted batches cannot be rolled back.
- `Model::drawParts` selects explicit native parts; `Scene::draw` interleaves
  parts/instances across models using stable explicit integer ordering keys.
  It validates the complete bounded list before submission and allocates no
  extra resident images. It does not infer visibility, source draw-buffer order
  or camera depth. Native scene/game producers remain unfinished. See
  SCENE_CONTRACT.md for the ownership and ordering contract.

## Ownership and memory

One thread owns the entire Citro3D context. Give `Renderer` and `QuadBatch`
stable storage; workers may prepare plain asset data but may not call them.
The ARM object sizes measured for this revision are 122,488 and 9,160 bytes;
do not put the renderer on a small application stack.

The application calls `C3D_Init(limits.commandBytes)` first. Default admission
is 4 MiB resident meshes, 4 MiB textures (including the 256-byte unused sampler),
256 KiB transient geometry, 512 KiB command storage and 64 draws per frame. These
are independent ceilings, not guaranteed free memory or measured full-game
requirements. Allocation/publication failure refuses a resource; it never
evicts a consumed image or substitutes for a missing referenced texture.

`begin()` uses only `C3D_FRAME_NONBLOCK`. A busy return does not recycle the
transient arena or release retired resources. A successful begin establishes
completion of the previous queue before collection. Release invalidates a
handle immediately, retains its storage and budget charge, then frees it at
that completion boundary. Slot generations never wrap into reusable IDs.

The application retains valid native render targets through completion.
`target()` installs the full native viewport/scissor; clear is explicit and
must precede draws in that pass. Materials requiring depth refuse a depthless
target. All uploads check cache publication. Frame end asks the native 3DS
queue to flush its command list rather than the entire linear heap. The SDK's
`GX_CMDLIST_FLUSH` name refers to the **3DS GSP queue**, not GameCube GX.

The declared command-buffer size must match the application's actual
`C3D_Init` allocation. The admission heuristic reserves 32 KiB plus 4 KiB per
draw; it is not a proof of every future SDK command cost. Clear, frame end,
APT hooks and context destruction still inherit SDK behavior; NONBLOCK begin
does not make those operations universally bounded against hardware faults.

Citro3D retains shader, texture and fog pointers. The renderer must outlive its
exclusive context. End its frame, complete/destroy that context, and only then
call `releaseAfterContextDestroyed()`. This function performs no GPU wait and
cannot verify a caller's external context lifetime. Do not mix this context
with the existing renderer, custom command queue or other Citro3D clients.

## Verification and missing work

The host checks substitute Citro3D/GSP calls and model queue completion; they
exercise actual native backend bodies, resource failures and lifetime rules.
The shader algebra interpreter uses host float32 and checks the actual source,
not PICA float24, clipping, pixels or hardware performance. Compilation and
these checks establish neither physical execution nor visual equivalence.
The dependency audit also reassembles shader sources and compares them with
both generated binaries and bytes extracted from the ARM archive. Incremental
timestamps alone cannot establish that an embedded shader matches its source.

Native mesh pack version1 is now implemented; see `PACK_FORMAT.md`. It is
prepared from explicit native JSON. The separate Mac-only
`Tools/3ds/j3d_native_geometry.py` can convert bounded original geometry into
that ABI; it does not make the complete original model rendering-ready.

`TexturePack` now supplies allocation-free native image views; see
`TEXTURE_PACK_FORMAT.md`. Original image decoding and named stage-image lookup
are confined to the Mac asset compiler. The console receives only PICA storage.
Native samplers/material bindings remain separate and unconverted. The initial
RGBA8 expansion preserves decoded texels but can increase memory substantially.

`MaterialPack` supplies bounded native fragment descriptors, custom color/alpha
blend state and explicit instantiation requirements; see
`MATERIAL_PACK_FORMAT.md`. Native fixed component bindings select eight RGBA8
color parameters supplied independently for each draw. Missing producer values
reject; templates remain immutable and draws allocate no parameter storage.
The Mac MAT3 compiler can preserve unsigned color dependencies in admitted
expressions, or explicitly specialize initial values into gated snapshots.
An additional default-off Mac planner can lower branched expressions and
conflicting constants into the six native stages using a delayed buffer model;
physical buffer/precision acceptance remains pending. Runtime descriptors,
allocation and the renderer binary are unchanged by that planner. Actual
game/animation/environment color producers and complete
source lighting/texgen/fog/framebuffer bindings remain unconverted.

Still required: complete native model/material/sampler/animation conversion;
native scene and UI callers; point/specular and
other lighting models; original projected-material conversion/acceptance and
indirect sampling; source fog equivalence,
complex material lowering, all effects/particles, target/VRAM pooling, scene
streaming, gameplay-wide integration, and real hardware acceptance. Unsupported
content must be converted or implemented explicitly, never hidden by a success
skip. The complete-game zero-runtime-translation goal remains open.

`Model` now links native mesh/material/image packs through owned, bounded ordered
part bindings. It shares referenced resources within a resident model and accepts
external instance pose/color blocks, with complete input/budget preflight and
explicit partial-submission errors. Native per-unit samplers use shared image
pixels with independent filtering, wrap and mip/bias ranges. The live game model entrypoints call this API through the native factory;
complete original-game parameter production remains unfinished. See `MODEL_CONTRACT.md` for required
sampler/fog producers, ownership, retirement and unfinished source integration.

`ModelPlan` adds a fully owned native metadata reader for resource/part
associations, sampler controls, uniform palettes and explicit producer
requirements. The Mac assembler resolves original shape/material attachment
and enqueue order into this format. It does not produce final scene sorting,
pose/animation matrices, texgen or lighting behavior. Native resource loading
checks are distinct from rendering acceptance. See `MODEL_PLAN_FORMAT.md`.

The game-facing model bridge preserves original model/shape order, then submits native resident parts and uniforms without interpreting their source display lists. Model entrypoints and shared SDK frame lifecycle are wired in game source; the source-identified native asset factory is wired, and complete live parameter producers remain the next integration work. Missing/expired model bindings refuse, with no model-packet fallback. The default game-build option selects this unfinished native model route; no new executable or package is delivered by the standalone build command above.

`AssetPack` joins native banks and copied source-shape ownership under an original-file SHA256. The archive resource loader captures identity before byte swapping; native file/GPU admission happens on the render owner. `GameAssets` shares one resident asset across instances and unloads it after the final instance is forgotten, retaining completion-dependent GPU retirement. The actual model can copy prepared draw/normal matrices into native palettes without per-vertex work; selected main-target camera publication is now connected, with host-only math/lifecycle coverage; physical precision and all pass coverage remain unverified. External-image context variants explicitly refuse pending a verified variant key. See `ASSET_PACK_FORMAT.md` and `GAME_DRAW_CONTRACT.md`.
