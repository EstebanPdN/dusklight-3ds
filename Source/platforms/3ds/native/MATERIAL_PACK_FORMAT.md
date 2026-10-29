# Native fragment material pack versions 1 and 2

`DUSKMT01` stores native fragment programs and render states. It contains no
source material records, original enums, source register bank or draw commands.
The renderer prepares Citro3D environments from these native descriptors once
at material creation. It does not run the Mac compiler on the console.

All integers are unsigned little-endian. File size is at most 81,952 bytes.
There are 1..64 records. IDs are nonzero, strictly increasing and may be sparse.
Empty source banks do not need a material file.

The 32-byte header contains magic[8], version=1 or2, file size, record count,
directory offset=32, directory bytes=count*32 and CRC32. CRC is IEEE reflected
`0xedb88320`, initial/final XOR `0xffffffff`, covering the first 28 header bytes
followed by the directory. It is an integrity check, not authentication.

Each 32-byte directory record has eight words: ID, payload offset, payload size,
payload CRC32, three native image IDs and requirements. Requirements bit0 means
an explicit native fog binding is needed. Bit1 marks an initial-state snapshot,
which normal instantiation rejects. Unknown requirement bits reject. Image ID0
means no binding; consumed texture-mask bits require nonzero IDs and vice versa.
The complete payload region immediately follows the directory. Gaps, overlap,
noncanonical positions and trailing bytes reject. Directory metadata is owned;
later edits to borrowed directory bytes cannot enlarge a span or clear a saved
requirement. The caller retains immutable file bytes until records are decoded.

## Native payload

The 32-byte prefix contains these fields:

| Offset | Field |
| --- | --- |
| 0..5 | stage count1..6, texture mask, blend, cull, depth compare, alpha compare |
| 6 | flags: depth test, depth write, color write, alpha write, fog, fog-depth-flip in bits0..5 |
| 7..9 | alpha reference, RGB buffer-update mask, alpha buffer-update mask |
| 10..11 | zero |
| 12..19 | packed native buffer RGBA8, packed native fog RGBA8 |
| 20..25 | custom blend RGB/alpha equations, RGB source/destination factors, alpha source/destination factors |
| 26..31 | zero |

Enum domains match `contracts.hpp` in declaration order. Native packed RGBA8
has red in its low byte. The three original authored blend presets retain their
existing semantics; custom blend supplies independent native color/alpha state.
It does not imply that framebuffer or original destination-alpha policy has
been converted.

Exactly `stage count` records follow, each32 bytes: three RGB sources, three
alpha sources, three RGB operands, three alpha operands, RGB/alpha function,
RGB/alpha scale, constant RGBA8 word and12 extension bytes. Version1 requires
all12 to be zero. Version2 uses the first4 for RGBA constant component bindings
and requires the remaining8 to be zero. Binding0 retains that literal component;
binding1..32 selects component `(binding-1)%4` of native color parameter
`(binding-1)/4`. Future versions/unknown selectors reject. Functions/operands/scales
use the native descriptor domains. Missing consumed textures, undefined
first-stage PREVIOUS and unsupported enum combinations reject.

If fog is enabled,256 little-endian float32 LUT entries follow. They must be
finite values in[0,1] within the native scalar domain. No source fog conversion
occurs at loading or drawing. Total payload size must equal32+32*stages plus
1024 bytes only when fog is enabled.

`MaterialPack::material()` checks selected payload CRC, reserved bytes, complete
length, native semantic validation and image IDs. It returns an owned
`MaterialView`, not casts or pointers into file structs. Successful `open()`
alone does not establish semantic validity of every payload.

## Instantiation

Call `instantiateMaterial(view, fogParameters, output)`. If fog is required,
the caller must provide an explicit native environment binding, including an
explicit disabled binding when appropriate. Required fog is never silently
removed. Unknown image bindings or invalid native state reject.

Initial snapshots also reject by default. `allowInitialSnapshot=true` is for an
explicitly controlled snapshot/immutable fixture. It is not a way to certify
dynamic original material support. After successful instantiation, create the
renderer material and resolve native image IDs to live texture handles before
submitting native meshes. Normal resource budgets and retirement apply.

For a parameterized material, supply `DrawUniforms::material` with eight packed
native RGBA8 colors and a supplied-mask bit for each required parameter. Values
may change for each draw. Missing required producer values reject before GPU
state submission; they never reuse the last object's colors. Literal component
bytes remain authored. Native fixed component wiring assembles up to six stage
constants; no source register file, expression evaluation, heap allocation or
material recreation occurs per draw. The prepared template stays immutable.
`QuadBatch` captures its parameters at begin, like its other draw uniforms.

## Offline source scope

`native_material_pack.py` writes explicitly authored native descriptors.
`j3d_material_source.py` bounds initial MAT3 declarations. The separate
`j3d_native_materials.py` compiles initial fragment expressions through a
symbolic graph and native accumulator scheduling. It can eliminate dead source
register operations and reorder native texture bindings so one projected input
occupies PICA unit0. Original shader-space/texgen matrices remain separate.

Source compilation always requires `allow_normalized_arithmetic=True`. Choose
exactly one of `specialize_initial_constants=True` or
`preserve_color_parameters=True`; both remain false by default. Snapshot records
are tagged initial snapshots.
They are valid only for those initial register/konst values. Procedural color
updates and original animations are not supplied by that snapshot path. Default
production instantiation refuses those snapshots.

The parameter path preserves unsigned RGBA8 source color dependencies in fixed
native bindings. It does not fold a parameter just because its initial value is
zero/one. The offline receipt maps source colors to native slots; the 3DS file
contains only native bindings. Signed/out-of-byte parameters, live expressions
requiring additional temporary storage and conflicting constants still reject.
The additional default-false `allow_native_temporaries=True` option permits a
bounded Mac-only search for PREVIOUS/delayed-buffer allocation when linear
scheduling cannot retain every live value. It emits the same native sources,
stage constants and RGB/alpha buffer masks; there is no search at load/draw time.
The model makes a stored stage k output available at k+2, with stores limited to
stages0..3. It never reads stage0's buffer or assumes the initial buffer contains
a graph value. Limit:64 candidate nodes,20,000 search states,6 output stages.
Failure to fit rejects. Independent RGB/alpha schedules are merged without
moving their stages. This is experimental until physical buffer timing and
combiner precision are validated. It does not bypass source arithmetic limits.

Native game/animation/environment producers must provide the actual changing
values; this compiler does not implement them. A parameter template is not a
whole source material or a guarantee of complete color-animation support.

Normalized expression checks do not prove integer source TEV equivalence or
PICA precision. Source byte wrapping, compare operations, unsupported bias,
multiple projected bindings, temporary-register pressure, stage limits and
observable pre-alpha depth writes reject rather than silently approximate.
Source dither and framebuffer/destination-alpha requirements remain in the
private receipt and require native render-pass policy/precision validation.
Lighting, texgen, samplers, fog parameters, animations and full model/game
integration remain incomplete. Every source receipt keeps `rendering_ready=false`.
