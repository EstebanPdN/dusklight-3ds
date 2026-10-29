# Native asset envelope v1

`DUSKNA01` is a private authoring/runtime container for exactly one source-model
identity and its already-native banks. It contains no source commands or
runtime source-material expressions. It does not make a model rendering-ready.
`Tools/3ds/native_asset_pack.py` writes it on the Mac; AssetPack validates it
before native resource admission. All integer fields are little-endian.

The fixed128-byte header is:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | ASCII DUSKNA01 |
| 8 | 4 | Version1 |
| 12 | 4 | Exact total file length,128..16MiB |
| 16 | 4 | Exact original expanded model length |
| 20 | 4 | Part count1..64 |
| 24 | 32 | SHA256 of all original model bytes before J3D mutation |
| 56 | 40 | Five pairs of section offset/length |
| 96 | 4 | External-image context required:0 or1 |
| 100 | 24 | Reserved, zero |
| 124 | 4 | CRC32 of header[0,124) followed by all bytes[128,end) |

Sections occur in this exact order: MeshPack, TexturePack, MaterialPack,
ModelPlan, source-shape ownership. They are contiguous apart from zero16-byte
alignment padding. The image bank alone may have length0, at the current cursor,
for an untextured asset. Every other section is nonempty. Ownership is exactly
partCount unsigned32-bit shape indices0..4095; it names the logical shape index
stored by the original shape factory. Repeated shape indices are intentional
for multiple native groups belonging to one shape. No trailing data is allowed.

All four bank readers retain their independent bounds/checksums/layout rules.
Envelope CRC covers every payload byte, including unreferenced resources. A
ModelPlan must have exactly the envelope part count. Cross-bank references,
sampler/mip coverage and geometry palette admission are checked by the existing
native Model loader before allocation. No bank address or coincidental numeric
ID supplies conversion provenance.

The writer receives immutable source bytes and matching native outputs. Its
generic signature/length checks do not independently prove that a caller chose
the right conversion outputs. The asset-preparation pipeline must retain source
hash receipts, exact native hashes, source-shape correspondence and any external
image dependency identities. The checkpoint155 real probe regenerates geometry
and material banks from the original bytes, checks the existing image-bank hash,
and verifies plan/shape ownership before assembling the envelope.

Original file extent is supplied by the resource archive, never inferred from an
unbounded pointer. SourceIdentity verifies the declared big-endian file length
against that actual extent before hashing. Known J3D2 model header signatures
are identity guards only; runtime geometry/material conversion does not occur.
Only fully known expanded files32B..16MiB can be identified. Unsupported loader
routes retain an empty identity and refuse native dispatch.

Source identity alone cannot distinguish identical BMDs whose external images
resolve differently in another stage/archive. Such converted banks MUST set
the context-required field. GameAssets currently refuses them even when all
uniform inputs are declared available. A verified live dependency/variant key
is a later integration requirement; clearing the flag to bypass it is forbidden.

The development factory looks up lowercase SHA256 filenames under a native
authoring directory. This is not a delivered installation layout. Embedding the
native index/payloads into a newly prepared single zelda.twilight, while keeping
the original bundle intact, remains separate extractor/packaging work.

The envelope has no source/GPU compatibility acceptance bit. Input availability,
current camera/pose/material/environment producers, memory admission and all
graphical/physical validation are independent gates.
