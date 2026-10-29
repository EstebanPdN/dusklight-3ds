# Native resident models and sampling

`Model` combines already-native mesh, fragment and image packs using an ordered
span of `PartBinding` descriptors. It parses no source model, source hierarchy,
GX packet stream or original texture header. Game model packets now dispatch through GameDraw into this API when native
model mode is selected; source-identified asset registration is wired, while complete game parameter
producers remain incomplete. This does not establish working original-game rendering. Original model assembly/pose/animation conversion remains separate.

## Resource and parameter contract

A binding names nonzero native mesh/material IDs, one native uniform slot0..63,
and native samplers. `samplerMask` must exactly match the material's consumed
image units; missing source sampler conversion is not inferred from defaults.
Material image IDs resolve inside the native image bank. Required fog needs an
explicit native environment binding; initial-state snapshots remain refused.

Load preflights the complete dependency list before allocation, then uploads
each distinct referenced mesh/material/image ID once within the resident model.
Different parts can share an image with different native sampler settings.
Descriptor/parameter metadata is owned; file spans and input bindings are
borrowed only through synchronous load and may be released afterwards.

At most64 ordered parts and64 distinct meshes/materials/images are admitted.
Renderer resource-slot, command, resident-byte and allocation limits also apply.
No scene-specific bypass, silent texture fallback or automatic quality reduction
is used. Failed creation retires all already-created handles and preserves the
original error. Retired GPU bytes remain charged until an actual completed
frame boundary; failure does not promise immediate budget recovery.

One resident model can be drawn repeatedly with different external native
parameter/pose blocks, allowing multiple instances to share its GPU resources.
IDs are scoped to its input packs. This is not global cross-model content-hash
deduplication or streaming. Animated texture selection, dynamic fog contexts,
visibility/LOD selection and original animation producers remain unfinished.

`draw(parameters)` validates used uniform slots, palette coverage, required
color producers, projective/UV conflicts, depth availability and whole-model
frame-draw budget before submitting parts. Parts retain input order. Sampler
settings belong to the model; an instance cannot also request conflicting
sampler overrides. No source matrix/vertex processing occurs on the CPU.
Submission still prepares native uniforms/state; there is no performance claim.

`drawParts(indices, parameters)` selects native parts explicitly, preserving the
provided index order and repeats. It validates the entire selection before
submission and consults only selected parts' required slots/inputs/depth.
`drawPart(index, parameters)` instead takes one direct instance block, regardless
of that part's plan uniform-slot number. Neither method changes visibility or
performs source scene traversal. The caller supplies these decisions.

`Scene::draw` can interleave these parts across resident models using explicit
native ordering keys. See SCENE_CONTRACT.md for ordering, borrowing, failure and
bounded-storage semantics. Whole-model `draw` retains its previous path/order.

An SDK/backend failure can occur after earlier parts have been submitted.
`Submission.submitted` reports that exact prefix and `Submission.error` remains
non-success. Queued GPU work is never described as rolled back. The caller must
handle this error rather than continue as though the model rendered completely.

## Native sampler contract

`DrawUniforms` can independently override the samplers of consumed units0..2.
`Sampler` provides native S/T wrap, independent magnification/minification/mip
filters, inclusive first/last resident mip levels and an exact signed fixed
point LOD bias with8 fraction bits (raw domain-4096..4095). Missing resident mip
levels, inverted ranges and invalid enums reject before submission. Level0-only
sampling is explicit; allocated image mip storage is not discarded or changed.

Owned images keep their immutable native storage descriptors. The renderer has
three stable borrowed sampler views; it copies a descriptor only for a native
override or projected unit0. Views share the original image address and contain
no new pixel allocation. Unit0 can project while units1/2 sample that same image
as ordinary2D. Borrowed views are never deleted/flushed as separate images.
Without an override, previous image sampler presets retain their exact path.

The installed Citro3D `C3D_TexBind` marks the unit dirty on every accepted call,
even for the same view address. Updating a stable view between ordered draws
therefore reaches native command preparation; the API does not rely on a
pointer-change heuristic. Physical sampler precision/output remain unvalidated.

## Lifetime and ownership

Use the renderer's exclusive owner thread. Keep its address/context stable for
the complete load/draw/unload lifetime. A model cannot be copied or moved.
Call `unload()` before renderer/context destruction. It retires resource handles;
GPU-dependent storage remains owned by the renderer. Wrong-thread unload leaves
ownership intact. Unload is idempotent once ownership has been released. There
are no implicit GPU/service calls from the model destructor.

Then end/complete the renderer's frames, destroy the Citro3D context and call
`Renderer::releaseAfterContextDestroyed()` as documented by the renderer. Models
and other fixed metadata objects should have deliberately budgeted storage;
ARM object sizes/stack receipts are recorded in the private technical evidence.

Native source assembly still needs the original shape/material hierarchy,
matrix palette/pose mapping, texgen matrices, native sampler conversion,
lighting/environment and animation bindings. Compilation/host SDK substitutes
prove neither real PICA pixels nor original-game or physical-console behavior.

A resident model increments its resource revision on every successful load. GameDraw captures that revision and rejects an unloaded/reloaded binding before invoking its producer. This does not replace the requirement to retain the actual C++ Model object until all bindings are forgotten.
