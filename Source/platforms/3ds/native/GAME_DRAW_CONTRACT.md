# Game model-packet integration

The real J3DMatPacket::draw, J3DShapePacket::draw and drawFast entrypoints select
native model dispatch when DUSK_GAME_STARTUP and DUSK_PICA_GAME_DRAW are enabled.
The latter option defaults ON for game builds. In that configuration their old
material load, vertex-array preparation and shape/display-list execution are
compiled out. J3D display-list execution and unconverted custom draw packets
explicitly reject; they do not fall back to the source renderer.

This is code integration, not a working migrated game. The source-identified native factory is now wired through J3DModel; its live
parameter producer is deliberately incomplete. Original draw/normal matrices
can be gathered into native palettes; selected main-target camera inputs are
published directly, but full camera/pass coverage, texgen, color/lighting and
environment production are not yet complete. Missing/expired registrations fail
at the first visible model draw. UI/effects and display-list production still
use compatibility code. No complete-game zero-GX claim is made. No new game
executable/package or graphics/hardware execution is part of this integration.

## Resource/instance binding

GameDraw associates an opaque live game instance with one resident native Model,
a copied shape-to-part ownership map and a native uniform producer. Original
high-level packet order and hidden-shape decisions are retained. The bridge does
not interpret command bytes, source materials or registers. A visible shape
selects all its precompiled native parts, preserving their native order.

At most64 live instance bindings are stored; each model contains at most1024 parts.
Each mapping entry names an original shape0..4095, matching the game shape object's
index. The Mac asset factory must verify model identity/ownership and construct
that correspondence from its native model plan/import receipts. It must not guess
IDs from coincidental resource addresses or mix unrelated native asset banks.

A producer receives the opaque game instance and native uniform-slot number and
fills a reset DrawUniforms block. It must be noexcept and make no GPU, registry,
object-destruction or reentrant calls. It must supply actual native game inputs,
not infer success from a default identity camera/unlit pose. The bridge gathers
all selected slots before native model preflight and any geometry submission.
Failed producers and reentry reject rather than reuse a previous instance's data.

Model and producer/user storage remain borrowed and alive until forget. Only the
mapping is copied. Successful native Model loads increment a64-bit resource
revision; exhausted revisions refuse. GameDraw captures that revision and rejects
an unloaded/reloaded resident model before invoking its producer. This protects
resource identity, not the lifetime of a destroyed C++ object. Keep Model storage
alive and stable. Actual J3DModel destruction calls forget for its game identity;
direct GameDraw::forget does not unload the separately owned resident asset.
The global game::forget wrapper also notifies GameAssets: the last bound
instance retires that asset and releases its CPU metadata.

GameDraw checks each selected shape against the remaining frame draw budget and
preflights all of its parts before submission. Backend failure may follow a
submitted prefix. Submission reports that count; the game boundary includes it in
its fatal diagnostic and never selects the old model renderer afterwards. A
material packet can have earlier successful shapes before a later failure; no
transaction rollback is promised. Packet-chain traversal is bounded1024 to
reject corrupt/cyclic hidden chains as well as visible chains.

## Shared SDK frame and state boundary

The existing game GPU queue remains the sole SDK FrameBegin/End owner. The actual
platformBeginGpuFrame first completes prior shared GPU/GSP work, then opens a
nonblocking SDK frame under its existing IRQ gate. Only after success and main
target binding does the game call game::beginFrame. GameDraw then collects retired
native resources and adopts that already-open frame. It never issues a second
SDK FrameBegin or End. Merely flushing a command list is not completion evidence;
calling this API without the external completion guarantee is forbidden.

The source packet boundary explicitly binds the actual active native render
target before native drawing, verifies the existing unsafe-alpha-pass guard,
and invalidates the transitional renderer's applied state, uniform ownership
and texture descriptor bindings afterwards, including failure. Native model
draws prepare their own shader, native material, samplers and uniforms. Source
framebuffer clears already split previous PICA commands before GSP memory fills.
No existing waits/fences are removed or replaced by a cache-only operation.

At frame end the game releases the native frame bookkeeping, then its original
queue owner submits SDK FrameEnd with command-list flushing. The reset path
also closes an adopted active frame before normal shutdown, rejecting incomplete
stream/target state. The next successful external frame admission supplies the
completion barrier required for native retirement/reuse.

Ordinary Renderer use remains exclusive. GameDraw is a narrowly defined shared
context adapter, not permission to interleave arbitrary public Renderer/QuadBatch
calls with compatibility drawing. New native UI/camera/region producers must
establish their own explicit boundaries; the current model boundary uses the
full active native target. Original viewport/scissor/camera semantics are not
automatically converted from registers. The factory/scene producer must provide
those semantics before a graphics acceptance claim.

## Bootstrap, limits and teardown

Game startup passes the same 4 MiB + 32 KiB command size to `C3D_Init` and the
native renderer. The game frame permits at most4096 native draws, independently
of the1024-part per-model limit. Before every native draw the shared queue owner
reserves 32 KiB of command space, splitting and waiting for reported queue
completion before reusing the arena when needed. Failure refuses the draw;
splitting alone does not authorize reuse. The existing libctru unrelated-IRQ
accounting limitation can report completion early and remains unresolved. This
keeps the existing 16 MiB linear and 80 MiB libc heap allocations unchanged;
neither heap's real-game headroom is proven.
Existing UI/effects setup remains transitional. Normal frame and model dispatch
code is selected by the same build definition across all game object/static-library
targets. There is no runtime toggle to silently enable old model-packet fallback.

The global session is one-context-lifetime only; repeated initialization after
teardown rejects before allocation. Models must be forgotten and resident assets
unloaded by their owners before normal context destruction. After the external
GPU owner has completed work and destroyed Citro3D, native resources/shaders are
released. Incomplete registry/frame ownership prevents a normal clean-release
claim and main uses direct kernel containment. Any remaining native asset/instance ownership also prevents normal release;
there is no implicit destructor-time GPU cleanup.

Earlier checkpoint ARM object sizes predate framebuffer-role metadata and must be remeasured. The bootstrap retains
one renderer and one bridge. GameDraw has64 fixed instance records and one shared
uniform workspace bounded to512KiB, not a full workspace per instance. These CPU
bytes, asset payloads and renderer allocations must be included in eventual real
hardware admission/telemetry. The helper's largest local compiler frame is1456B;
this is not a full call-chain/SDK/interrupt stack bound. No hardware RAM/FPS
saving or complete memory suitability has been demonstrated.

Actual game ARM objects and native library compilation, preprocessor/disassembly
inspection and host SDK-substitute fixtures verify the call boundary. They do
not establish original-model graphics, successful live source asset admission, gameplay,
full native UI/effects or physical behavior. Azahar remains deferred by the owner.


## Source-identified factory checkpoint155

The real dRes_info_c::loadResource captures the archive's expanded BMD/BDL extent
and SHA256 before invoking a mutating J3D loader, then attaches the owned36-byte
SourceIdentity to J3DModelData. Clear resets it. The recognized release resource
types are BMDP/BMDR/BMDV/BMDE/BMWR/BMWE/BMDG/BMDA; DEBUG also covers BMDL. Other
custom/direct loader routes remain unidentified and refuse native draws until
explicitly wired. This adds36 native-only CPU bytes per model-data object;
all game libraries must use the same DUSK_PICA_GAME_DRAW definition.

The actual source shape boundary calls J3DModel::drawNativeShape, which reaches
GameAssets via game::drawSourceShape. Native asset keys contain original bytes,
not addresses or display-list identities. The provisional development lookup is
sdmc:/3ds/DuskLight/native/<source-sha256>-<image-context-sha256>.dna. These are private
authoring banks, not a delivered SD layout or a replacement for the owner's
one-file extractor requirement. Native payload/index embedding into a separately
prepared zelda.twilight remains packaging work; the original bundle is unchanged.

GameAssets only performs file I/O/GPU work on the Renderer owner. Initial loads
require a writable native boundary (outside a frame or before native geometry
has emitted). Already-resident asset instances can bind without new GPU storage.
There is not yet a complete preload/stream queue for assets first requested after
native emission. This conservative refusal must not be relaxed into resource
reuse while previous GPU work is pending.

Bounds:16 resident assets,64 instances,256KiB resident asset metadata,16MiB maximum
single-file staging. Resident/retired GPU storage remains subject to Renderer
budgets. CPU metadata uses bounded malloc/placement construction, bypassing a
short-lived current JKR solid heap. Banks and file staging are released after
successful upload; ModelPlan and shape maps are copied. A successful asset is
shared across instances. Failure retains ownership if an unexpected unload fails;
empty()/normal shutdown must not report success in that state.

The producer receives a native UniformBinding, not source registers. Original
animated joint/envelope matrices are already calculated by gameplay; gathering
at most four of them does not CPU-transform vertices. For concat-view shapes it
concatenates the packet base with the remapped joint/envelope animation matrix;
ordinary SMTX/SMML shapes consume their calculated draw and normal banks.
Supported concat-view billboard loaders use the J3D view, as in the source.
Unsupported shared/no-matrix banks (flag 0x100), CPU skinning, custom deformation,
invalid matrix groups and unsupported numeric values reject.

The source adapter advertises the pose, ordering, color, lighting, texgen and
fog producers; individual material, camera, geometry and parameter proofs can
still reject. The factory checks the ModelPlan's declared inputs before GPU
allocation. This is not proof of complete animated-character behavior.

An external image can differ across contexts even for identical BMD source bytes.
The ordered `DUSKCTX1` digest selects and validates the actual variant.
Only the registered framebuffer-color provider gets a stable `DUSKROLE1` role
digest; unknown mutable images and depth captures reject. The v2 texture pack
retains that slot's original ID without dummy pixels or owned GPU storage.
After every producer callback completes, submission materializes a matching
deferred copy and borrows the compatibility cache's current RGBA8 texture.
Clamp/single-mip sampling and padded affine/projective UVs match compatibility;
borrowed slots are generation checked and never flushed, deleted or charged as
native-owned texture bytes. Synthetic fog/lookup units remain independent.
Stable optional fog environments must also match exactly when sharing an asset.
This targets parity with the existing compatibility capture, not proof of exact
GX copy-filter coefficients or half-scale tap phase. Existing libctru
unrelated-IRQ queue accounting and physical hardware remain independent limits.

Latest ARM factory local frame:11,768 bytes, excluding callees/SDK/interrupts.
All successful paths explicitly forget/unload before context destruction. No
native game execution, scene acceptance, full memory fit or FPS gain is claimed.

## Main-target camera contract

`publishCamera` accepts the authored clip-depth `[-W,0]` projection and continuous
logical canvas/window values on the render owner during an active shared frame.
`prepareCamera` composes the sideways display orientation and viewport/depth
range once per pass. It does not transform mesh vertices or decode GX state.
`target` applies the prepared physical scissor when the main target is bound.
`applyCamera` copies the prepared matrix into a producer's native uniform block.

Frame begin/end, unconverted camera setters and raw display-list execution
invalidate the publication. Offscreen/custom targets are not admitted. Failed
publication clears prior validity; producer-time invalidation poisons the
current preflight. A fresh main-target publication after an offscreen pass
requires a subsequent main-target bind.

The current J3D producer supplies pose, live material inputs and this camera.
Unsupported matrix calculation modes, CPU skin paths, custom deformations and
unproved producer inputs reject. Host point mapping, rejection/reset and actual
helper/producer fixtures are distinct from physical raster validation.

### Mirror pass

The mirror preserves the current perspective/viewport; only its reflected packet
base and logical scissor change. Its game-facing publisher verifies that the
saved projection matches the canonical authored scene matrix, uses the actual
saved viewport and integer mirror bounds, and intersects the physical scissor
with the viewport. A mismatched projection or empty/unsupported region fails
closed. After the compatibility mirror depth/mask quads restore GX projection
and scissor, the original native camera and scissor are explicitly republished.

Each native mirror shape uses the normal source-identified packet boundary,
never J3D display lists. Scoped overrides bind the exact model/shape/material,
write all four ambient-0 channels from the environment, and map GX front-cull
to native back-cull without mutating the material. The packet's reflected base
is used by concat-view palettes; ordinary SMTX/SMML and billboard loaders retain
their source drawFast semantics. Mixed packet ownership and unsupported matrix
producers reject. Compatibility quads remain ordered after the reflected models.
Host SDK-substitute tests cover publication, restoration and the overrides; they
do not establish emulator or hardware raster equivalence.
