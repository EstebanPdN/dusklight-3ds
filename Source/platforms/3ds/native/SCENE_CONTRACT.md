# Native scene submission

`Scene::draw(renderer, items)` synchronously submits native model parts from
multiple instances in explicit caller-supplied order. It decodes no source
packets, materials, hierarchy, GPU registers or texture formats. It performs
no CPU vertex transformation. The current game does not call this API yet.

Each `SceneItem` references a resident `Model`, a zero-based native part and
one native `DrawUniforms` block. Its parameters are direct per-instance input,
independent of the model plan's uniform-slot numbers. Multiple items may share
a model, a part or a parameter block. Different resident models keep their
separate handles; this is not cross-model image/resource deduplication.

## Explicit ordering

Items are submitted in ascending `(pass, order)` order. Equal keys retain input
order. Both fields are unsigned integer keys, not source commands, addresses,
floating-point depths or an implicit camera/material policy. `pass` is only an
ordering group: it does not change target, clear buffers or choose blend state.
Material state remains part of the already-native resident resource.

The caller must produce appropriate keys from actual native scene/camera and
gameplay decisions. A back-to-front transparency policy can supply smaller keys
for farther instances, or a producer can supply already-final draw ordinals.
Depth is not guessed from object origins or baked into this API. No automatic
opaque regrouping, triangle sorting, occlusion culling or alpha correctness is
claimed. Sorting is a bounded comparison of indices, not vertex work.

The original J3D queues have different material, animation, depth-bucket and
head/tail traversal modes. Native `ModelPlan` enqueue order does not establish
their final order. Connecting the native game producers, including safe
visibility/LOD and order semantics, remains unfinished. Declaring the plan's
ordering input does not prove this conversion or perform sorting itself.

## Validation and failures

Capacity is1024 items. The renderer's remaining per-frame draw budget applies
to the complete list. Every referenced model must be loaded on that same
renderer, on its exclusive owner thread, during an active frame. Each selected
part validates its own palette coverage, required color inputs, native depth
availability, sampler ownership and UV/projective constraints. Unselected parts
do not impose their parameter/depth requirements. Missing or invalid input in
any entry rejects the entire list before the first native backend change.

The queue does not promise GPU transaction rollback. Backend failure after a
prefix returns the exact number already submitted, with a non-success error;
no further entries submit. The count refers to the sorted sequence, not a
prefix of the unsorted input array. Invalid input is never silently omitted.

Use separate calls at target, clear, scissor or other externally controlled pass
boundaries. The queue calls the renderer's normal checked native draw path,
preserving state/version, frame counters and resource retirement behavior.

## Ownership and bounds

Items, models and parameter blocks must remain alive and immutable for this
synchronous call. No pointers are retained afterwards. Keep renderer ownership
and resource lifetimes as specified in MODEL_CONTRACT.md. Worker threads may
prepare ordinary data with caller-managed synchronization, but must not mutate
borrowed blocks during submission or call the renderer.

Ordering uses a fixed2048-byte index array and the installed standard library's
in-place comparison sort; it adds no GPU/resident image allocation or parameter
snapshot arena. Caller item storage is separate. The installed ARM ABI gives
20 bytes per item, so a full1024-item caller list alone is20480 bytes. Prefer
explicitly budgeted storage rather than a large automatic caller array.
The default64-draw budget remains unchanged. Reaching the API maximum requires
an appropriately provisioned command buffer; maximum capacity is not a
recommendation for a hardware frame or a proven feasible game workload.

ARM local-stack reports, host SDK-substitute checks and dependency auditing
establish code contracts only. Original scene fidelity, PICA precision,
performance, real-console memory peaks and full-game compatibility remain
unverified. No emulator is required or launched by this module.
