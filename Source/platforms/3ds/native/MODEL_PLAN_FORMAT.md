# Native model plan version 4

DUSKML01 contains native resource associations, uniform-slot requirements and
samplers. No original hierarchy commands, source material enums, texture
headers or GPU packet stream cross this boundary. The Mac source assembler
emits original **enqueue order**, before draw-buffer sorting. This is not a
render-ready scene or a substitute for native pose/texgen/lighting producers.

## File structure

All integers are little-endian. The maximum size is 147,504 bytes: 1–1024
parts and 1–1024 uniform slots. The 48-byte header contains magic[8], version 4,
total file size, part count, uniform count, required-input mask, part offset 48,
uniform-table offset, uniform-table bytes, a zero reserved word and CRC32.
CRC is IEEE reflected with initial/final XOR 0xffffffff, covering header bytes
0–43 followed by all bytes from 48 to EOF. Earlier versions are rejected; native
banks must be regenerated. CRC is an integrity check, not authentication.

Parts immediately follow,64B each. Each has six32-bit words: mesh ID, material
ID, uniform slot, sampler mask, pass and entry ordinal. IDs are nonzero; slot
indices are0-based; pass0 means opaque/texture-edge queue and1 means alpha
queue; ordinal must equal the record index. Three12-byte native samplers follow,
then one zero word. A sampler contains S/T wrap, magnify/minify/mipmap filters,
first/last mip levels, zero byte, signed16-bit LOD bias and two zero bytes.
Enum domains match contracts.hpp. Actual resident mip validation occurs when
Model loads the corresponding image, not in this metadata-only decoder.

Uniform records are 80 bytes each. They contain material ID, anchor node ID,
up to four native pose-matrix IDs, texture-slot and generator mappings, source
attribute and channel masks, parameter read/wrap/signed masks, source shape,
billboard mode, and source TEV-stage and texgen counts. Bytes 72 and 73 hold
those counts; 0/0 denotes a library-authored binding without source proof.
Source-derived game bindings require the counts and reject a live count mismatch
before submitting the affected shape. This catches warp-stage activation but
does not establish equivalence of program changes with unchanged counts.
Every slot must be referenced and its material must match each referring part.
Pose IDs are opaque native table references; animation remains a producer input.

No gaps, noncanonical offsets, trailing data, unknown versions/reserved fields
or invalid native sampler values are accepted. ModelPlan copies all validated
metadata, including requirements, pass labels, bindings and palettes. Input file
storage may be released after open. Failed opens expose zero records/slots.

## Producer requirements and order

Required input bits: poses1, texgen2, lighting4, fog8, ordering16 and colors32.
Unknown bits reject. bindings(providedInputs) returns an empty span until all
required producers have been declared supplied. entry()/uniform() permit
inspection and resource preparation without that declaration. A supplied bit
is a caller contract, **not proof of correct original behavior or GPU fidelity**.
Model draw validation still checks actual native parameters and resource limits.

The returned bindings retain enqueue order. An ordering declaration does not
sort them. A native scene queue must supply final pass/camera ordering before
actual game integration. Initial source hierarchy order alone is insufficient
for translucent sorting or cross-model material queues. Other unresolved
framebuffer/render-pass, precision, animation and callback policies remain
outside this metadata format; provided flags do not certify them.

Resource IDs are local to the companion mesh/material/image packs. The source
preparation receipt pins their source/content hashes. The asset loader must
keep the correct companions together; independent valid CRCs do not establish
that arbitrary packs belong to the same model. A full native asset container
and runtime manifest binding remain future integration work.

## Source compiler scope

j3d_native_model.py resolves original parent-joint material attachment, material
prepend order and shape/group correspondence only on the Mac. It assigns native
uniform slots using material, anchor and local pose-palette identity. Source
correspondence, transform style and unresolved pose recipes stay in the private
receipt. They are not original commands interpreted by the native reader.

j3d_native_samplers.py requires explicit allow_sdk_sampler_model selection.
It maps the modeled SDK's float32 LOD scaling/clamping/quantization into exactly
representable native control constants. Edge LOD, bias clamp, anisotropy,
fractional mip clamps and nonresident ranges reject until an explicit native
policy exists. This does not prove physical derivative/filtering equivalence.
Source fragment compilation retains its own explicit normalized-arithmetic
selection and existing refusals. All source receipts keep rendering_ready=false.
