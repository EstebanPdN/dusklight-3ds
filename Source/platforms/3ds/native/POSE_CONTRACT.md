# Native pose graphs and shader palettes

`PosePack` evaluates an immutable native affine graph and native matrix recipes.
It does not parse original model sections, animation commands, joint styles,
GPU registers or source draw packets. Original conventions are resolved by the
Mac tool `j3d_native_pose.py`. No mesh vertex is processed on the CPU. This is a
native CPU producer of small per-joint/per-palette matrices; the PICA shaders
continue to transform/skin vertices. Animation curve sampling is not implemented
or moved to the GPU by this module. The game does not call this API yet.

## Evaluation and native bindings

The graph contains topologically ordered affine nodes. A parent index is0 for
the supplied root or the1-based index of an earlier node. Rest local transforms
are already-native3x4 row-major matrices. Optional nonzero anchor IDs name final
joint outputs; helper nodes can remain unnamed. Resolve `anchorNode` once during
binding rather than searching the graph for every draw.

Each shader-matrix recipe either copies one node's world transform or blends
`world * inverseBind` transforms with authored scalar coefficients. Coefficients
are not normalized. Native terms may use signed coefficients within the format's
domain; the current original-envelope converter accepts source weights0..1.
The blend is per matrix, never per mesh vertex. Compute poses once per instance
update and reuse them across that instance's selected parts.

Normal policy is explicit: either inverse transpose of the resulting3x3 linear
transform or a direct linear copy with zero translation. The latter preserves
original scale-flag semantics even when a blend of rotations is not orthogonal.
Inferring inverse transpose for every weighted matrix would change the original
lighting. Singular inverse-transpose requests fail; no identity/stale-normal
fallback is inserted. Animation cases that depend on original singular retention
are not supported by this policy yet.

`evaluate(root, locals, world, matrices, view, normalModes)` takes caller-owned
workspaces and allocates no memory. Empty locals selects the native authored
rest graph. A nonempty local array must supply every node and explicit normal
modes for every recipe; it cannot accidentally reuse rest scale decisions for
animated transforms. Normal modes may also override an authored rest evaluation.
Root-transform changes, including nonuniform instance scale, require the caller
to supply the appropriate normal policy. No source instance-base-scale rule is
implicitly inferred from the root matrix.

The supplied root defines the output coordinate space. Normally it includes the
native model-instance transform; view/projection stays in DrawUniforms. Light
directions must use the same space as the resulting normals. The Mac converter
only proves its original rest pose with identity model base/scale. Original
nonidentity base-scale inheritance, especially separated Softimage scale state,
needs the native instance/animation producer before claiming equivalent gameplay.

After success, `PoseView::palette(ids, output)` gathers1..8 native recipe IDs into
the existing shader Palette ABI. IDs are1-based recipe ordinals, matching the
native ModelPlan's pose references. Failed gather sets palette count0. The host
inspector verifies all77 scoped existing plan slots, including anchor lookup;
this is data binding, not rendered original-model acceptance.

## Failure, ownership and numeric contract

Pack bytes stay alive and immutable through all graph accesses/evaluations.
`open` clears its output on failure and validates the entire canonical graph,
recipe/term directory, scalar domain and CRC before publishing the view. It reads
unaligned bytes without constructing typed objects over file memory. No repeated
CRC scan is imposed during evaluation under the immutable-file contract.

World and matrix workspaces must be large enough and disjoint from each other,
pack bytes, root, local input and normal-policy input. Invalid inputs, aliases,
sizes and policies reject before workspace writes. Intermediate numeric failure
can leave partially updated workspace, but clears the returned PoseView. Never
consume workspace after failure or retain an old copied view across workspace
reuse. Successful views borrow workspace; they own no snapshot or GPU resource.
Callers synchronize updates and must not modify data while a draw borrows it.

Inputs/outputs use the renderer's normal finite float domain: nonzero magnitude
at least2^-62, linear coefficients at most32768, translation at most2^20 and
normal coefficients at most256. Integer-bit classification rejects NaN, infinity
and tiny values independently of ARM FZ/DN. Composition/blend/inversion use bounded
double intermediates and check range before casting to float. This avoids relying
on gradual float underflow; it is not bit-identical PPC arithmetic or a physical
ARM/PICA precision guarantee. There is no unvalidated FZ-mode manipulation.

## DUSKPS01 version1

All integers and IEEE binary32 words are little-endian. The48-byte header is:
magic8; version; total bytes; node count; recipe count; term count; node offset48;
recipe offset; term offset; reserved0; CRC32 at44. CRC covers header bytes0..43
followed by the entire payload starting48. No trailing bytes or directory gaps.

Node64 bytes: parent index, anchor ID,12 affine floats,8 reserved zero bytes.
Recipe16 bytes: kind0(copy)/1(blend), node ID or first-term index, term count
(zero for copy), normal mode0(inverse transpose)/1(linear). Blend ranges are
contiguous, ordered and collectively cover all terms. Term56 bytes: node ID,
weight,12 inverse-bind floats. Anchor IDs0..4096 are unique when nonzero.
Node/recipe IDs are native mathematical references, not source pointer tokens.

Bounds:1..8192 nodes,1..65535 recipes, at most65536 terms,1..255 terms per blend,
file at most8MiB. No recursive graph traversal or resident allocation is needed.
The ARM ABI gives PosePack28B, PoseView16B, Affine48B and PoseMatrix96B. Required
workspace is48*nodes +96*recipes bytes, plus caller overrides when used. These
limits are parsing bounds, not recommended hardware allocation sizes. Explicitly
budget asset/workspace memory and retained view lifetimes before game integration.

Native banks still require an owning asset container to verify shared model
identity across mesh/material/image/plan/pose packs. IDs alone cannot establish
that relationship. Current preparation/inspection matches source SHA-256 receipts;
no automatic runtime cross-bank authenticity or full scene loader is claimed.

## Current source preparation scope

The Mac compiler resolves original basic, Softimage and Maya rest conventions
into generic affine nodes, including helper nodes for separate scale inheritance.
It preserves logical joints/remaps, hierarchy ownership, authored inverse binds,
envelope coefficients, draw-matrix associations and native normal policies.
It rejects malformed spans, missing joints, unknown styles, billboards and invalid
scale compensation rather than silently dropping work. Source trig is modeled
with its quantized13-bit table angles and float32 arithmetic; original PPC
reciprocal/FMA/libm behavior is not claimed bit-exact.

Animation curves, procedural callbacks, camera billboards, dynamic model-base
scales, original scene lighting/texgen and gameplay integration remain unfinished.
All current checks are host arithmetic/format/binding and ARM compilation. No
Azahar, GPU rendering, hardware memory/performance or full-game acceptance.
