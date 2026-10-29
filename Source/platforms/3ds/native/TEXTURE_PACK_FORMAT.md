# Native image pack version 1

`DUSKTX01` stores PICA image bytes prepared on the Mac. It contains no original
texture headers, source sampler enums, material programs or command packets.
All integers are unsigned little-endian. The file is at most 64 MiB.

The 32-byte header contains magic[8], version=1, complete file size, image count
(1..64), directory offset=32, directory byte size=count*32, and CRC32. The CRC
uses IEEE polynomial `0xedb88320`, initial/final XOR `0xffffffff`, over the first
28 header bytes followed by the directory.

Each 32-byte directory entry has eight words: nonzero image ID, width, height,
format, mip count, payload offset, payload byte count, and payload CRC32.
IDs increase strictly. Width/height are powers of two from 8 through 1024.
Every authored mip remains at least 8 texels on both axes; no tail is silently
dropped. Formats are 0=RGBA8, 1=RGB565, 2=ETC1, 3=ETC1A4. Payloads concatenate
levels from largest to smallest. Sizes match `textureBytes()` and Citro3D's
allocation calculation. Compressed image encoding correctness is the offline
encoder's responsibility; this reader validates storage, not ETC codec grammar.

The first payload immediately follows the directory. Each next payload follows
16-byte-aligned zero padding. Gaps, overlaps, unknown formats, invalid extents,
noncanonical offsets, nonzero padding and trailing bytes are rejected. Layout
metadata is copied into a fixed 64-entry table; subsequent source-directory
mutation cannot enlarge a returned span. Each selected payload is checked again
against its CRC. CRC is an integrity check, not cryptographic authentication.

`TexturePack` allocates nothing and borrows the immutable file buffer. Retain
that buffer until selected payloads have been copied into renderer-owned native
storage. Do not mutate it concurrently. `TextureView` supplies image layout and
payload; an explicit native material/caller supplies sampler behavior:

```cpp
TextureView image;
if (pack.image(id, image) != PackError::none) return false;
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
The external resource replaces the complete placeholder descriptor, matching
the source resource loader. Resolution and source decoding happen only on Mac.

`compile_native_texture.cpp` reuses the existing bounded image decoder as a
host-only asset compiler. RGB565 retains packed words. Other supported source
formats expand to RGBA8 while retaining decoded color/alpha and all authored
mips. No resizing, half-size selection, mip regeneration or lossy compression is
performed. This deliberately conservative first conversion can expand image
memory substantially; it is not a RAM optimization. Decoder code is never
linked into the standalone ARM native engine.

Zero-texture source banks produce no image pack. Missing external images,
non-power-of-two sizes, small authored mip tails and banks exceeding 64 native
images/64 MiB refuse conversion rather than substitute images. The converter's
receipt explicitly leaves sampler, material, texgen and rendering acceptance
false. Native color precision, filtering, UV conventions, visual equivalence,
full-model integration and physical PICA execution remain unverified.
