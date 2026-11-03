# Native image pack version 3

`DUSKTX01` stores PICA image bytes or a typed live-framebuffer role prepared on
the Mac. It contains no original texture headers, source sampler enums,
material programs or command packets.
All integers are unsigned little-endian. The file is at most 64 MiB.

The 32-byte header contains magic[8], version=3, complete file size, image count
(1..64), directory offset=32, directory byte size=count*32, and CRC32. The CRC
uses IEEE polynomial `0xedb88320`, initial/final XOR `0xffffffff`, over the first
28 header bytes followed by the directory.

Each 32-byte directory entry has eight words: nonzero image ID, width, height,
format, mip count, payload offset, payload byte count, and payload CRC32.
IDs increase strictly. Formats 0=RGBA8, 1=RGB565, 2=ETC1, 3=ETC1A4 and
5=RGBA5551 are owned images: width/height are powers of two from 8 through
1024, and every authored mip remains at least 8 texels on both axes. Payloads
concatenate levels from largest to smallest; sizes match `textureBytes()` and
Citro3D allocation. RGBA5551 uses 2 bytes per texel, tiled in 8×8 Morton order;
each little-endian word stores R in bits 15–11, G in 10–6, B in 5–1 and A in
bit 0. Compressed image encoding correctness is the offline encoder's
responsibility; this reader validates storage, not ETC codec grammar.

Format 4 is the `framebuffer-color` role, not an image encoding. Its width,
height, payload size and CRC are zero, mip count is one, and offset equals the
current payload cursor. It retains the original one-based TEX1 image ID. Version
1 and 2 packs are rejected and must be regenerated.

The first payload immediately follows the directory. Each next payload follows
16-byte-aligned zero padding. Gaps, overlaps, unknown formats, invalid extents,
noncanonical offsets, nonzero padding and trailing bytes are rejected. Layout
metadata is copied into a fixed 64-entry table; subsequent source-directory
mutation cannot enlarge a returned span. Each selected payload is checked again
against its CRC. CRC is an integrity check, not cryptographic authentication.

`TexturePack` allocates nothing and borrows the immutable file buffer. Retain
that buffer until owned payloads have been copied into renderer-owned native
storage. A framebuffer role has no payload and is resolved on submission, not
uploaded at load. Do not mutate the buffer concurrently.
`TextureView` supplies layout and payload; an explicit native material/caller supplies sampler behavior:

```cpp
TextureView image;
if (pack.image(id, image) != PackError::none || image.image.framebufferColor) return false;
TextureInfo info{image.image.width, image.image.height, image.image.format,
                 Wrap::repeat, Wrap::clamp, true, image.image.mipLevels};
auto handle = renderer.createTexture(info, image.payload);
if (!handle) return false;
```

The sample sampler is illustrative, not an original-game sampler conversion.
Create resources before drawing in a frame. Resident upload checks byte count,
budget, allocation and cache publication. Normal completion-dependent retirement
rules apply; the pack reader neither evicts images nor submits rendering.

## Offline source preparation

`Tools/3ds/native_texture_pack.py` deterministically writes native image packs.
`j3d_native_textures.py` reads bounded TEX1 images and optionally resolves named
external BTIs from an explicitly supplied stage-resource dictionary. ASCII
case normalization follows the original archive lookup; ambiguous keys refuse.
The original loader's `fbtex_dummy` and `dummy` names select framebuffer-color
even if their TEX1 entries contain dummy pixels; `Zbuffer` explicitly refuses.
Other external resources replace the complete placeholder descriptor.

`compile_native_texture.cpp` reuses the existing bounded image decoder as a
host-only asset compiler. RGB565 retains packed words. CMPR is decoded across
all authored mips and stored in tiled RGB565 if every texel is opaque, or
RGBA5551 otherwise. Color channels are rounded to the nearest 5- or 6-bit value,
and the one-bit alpha is exact. Other supported source formats expand to RGBA8.
No resizing, half-size selection, mip regeneration or lossy compression is
performed. Decoder code is never linked into the standalone ARM native engine.

Zero-texture source banks produce no image pack. Missing external images,
non-power-of-two sizes, small authored mip tails and banks exceeding 64 native
images/64 MiB refuse conversion rather than substitute images. The converter's
receipt explicitly leaves sampler, material, texgen and rendering acceptance
false. Native color precision, filtering, UV conventions, visual equivalence,
full-model integration and physical PICA execution remain unverified.
