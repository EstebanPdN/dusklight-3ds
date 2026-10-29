# Native mesh pack version 1

This format feeds the native renderer directly. It contains native vertex and
triangle-index bytes, not display lists or source-platform register state.
It does **not** yet contain textures, materials, animations, scene objects or
an entire original model. The separate offline converter
`Tools/3ds/j3d_native_geometry.py` imports supported J3D/BMD geometry; see
[the migration guide](../../../../docs/NATIVE_RENDERER.md) for its limits.

All integers and float32 values are little-endian. The maximum file size is
64 MiB, with 1–64 meshes. Entries have strictly increasing nonzero 32-bit asset
IDs; these are asset identities, not the renderer's generation handles.

| Header offset | Type | Meaning |
| --- | --- | --- |
| 0 | 8 bytes | ASCII `DUSKPN01` |
| 8 | u32 | Version = 1 |
| 12 | u32 | Exact file size |
| 16 | u32 | Mesh count |
| 20 | u32 | Directory offset = 32 |
| 24 | u32 | Directory byte size = mesh count × 32 |
| 28 | u32 | IEEE CRC32 of header bytes 0–27 followed by the directory |

Each directory entry is eight u32 words:

| Word | Meaning |
| --- | --- |
| 0 | Asset ID |
| 1 | Vertex count, at most 65,536 |
| 2 | Index count, at most 65,535, divisible by three |
| 3 | Vertex payload offset |
| 4 | Index payload offset |
| 5 | Vertex bytes = count × 60 |
| 6 | Index bytes = count × 2 |
| 7 | IEEE CRC32 of vertex bytes followed by index bytes |

Payloads follow directory order. Vertices start immediately after the directory
or the preceding aligned index payload. Indices start at the next 16-byte
boundary after vertices. The next mesh/file end is the next 16-byte boundary
after indices. All alignment bytes must be zero. Gaps, overlaps, duplicate IDs,
unexpected trailing bytes, contradictory sizes and unsupported versions refuse.

The fixed 60-byte vertex contains:

| Offset | Data |
| --- | --- |
| 0 | Position: three float32 values |
| 12 | Normal: three float32 values |
| 24 | UV0, UV1, UV2: six float32 values |
| 48 | R, G, B, A: four unsigned bytes |
| 52 | Four palette row offsets: 0, 3, …, 21 |
| 56 | Four unsigned weights, sum exactly 255 |

Indices are native u16 triangle indices. `contracts.hpp` asserts the same
record size/offsets on ARM. `createMeshBytes()` validates copied native records
without treating arbitrary file bytes as pre-existing aligned C++ Vertex
objects, then publishes one renderer-owned linear allocation. Validation is
performed at asset admission, not on every resident draw.

`MeshPack::open()` validates the bounded directory, metadata CRC and canonical
layout. It copies metadata into its own fixed directory so later header edits
cannot change the stored bounds. `mesh(id)` binary-searches that directory and
checks the selected payload CRC. The caller owns and retains the file buffer
until upload; no concurrent mutation or free is permitted. This view does not
perform file I/O, stream the SD card, authenticate content or own a full-file
allocation. CRC detects accidental changes, not adversarial forgery. Full-file
SHA-256 provenance is recorded by the Mac writer.

## Mac preparation

`Tools/3ds/native_mesh_pack.py` accepts the following **already native** JSON:

```json
{
  "schema": "dusklight-native-mesh/1",
  "meshes": [{
    "id": 1,
    "vertices": [
      {"position": [0, 0, 0]},
      {"position": [1, 0, 0]},
      {"position": [0, 1, 0]}
    ],
    "indices": [0, 1, 2]
  }]
}
```

Optional vertex members: `normal` (three numbers, default +Z), `uv` (three
native pairs, default zero), `color` (four bytes, default white), `bones`
(four indices 0–7, default zero), and `weights` (four bytes, default
255/0/0/0). Bone indices become row offsets offline. Unknown fields and invalid
numbers, domains, lengths or sums refuse; the writer does not guess conversion
rules or silently discard attributes.

```sh
python3 -B Tools/3ds/native_mesh_pack.py Build/native-assets/scene.json \
  --output Build/native-assets/scene.dnp
```

Output must be project-private, ignored and untracked under Build, Diagnostics
or Saves. Directory traversal is pinned without following symlinks; output is
exclusive mode0600, flushed and never overwritten. This tool is infrastructure
for offline native preparation, not the owner's requested ISO extractor or a
claim that original-game assets have been converted.
