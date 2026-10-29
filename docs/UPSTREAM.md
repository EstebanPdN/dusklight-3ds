# Source provenance

The public repository begins with a complete current-source snapshot based on
the following pinned public sources. Existing local port changes are included;
the snapshot is not described as byte-identical to those upstream commits.

| Component | Upstream revision | Local path and scope |
| --- | --- | --- |
| [Dusklight](https://github.com/TwilitRealm/dusklight/tree/422d7bb1b6c8d973cccf8b3d0b226a57ac3cc8c7) | `v2.0.1`, `422d7bb1b6c8d973cccf8b3d0b226a57ac3cc8c7` | `Source/`; current ARM/native/shared changes included |
| [Aurora](https://github.com/encounter/aurora/tree/d0933b745abe0eb9815bedcea8047575da18698d) | `d0933b745abe0eb9815bedcea8047575da18698d` | `Source/extern/aurora/`; includes current 3DS adaptations |
| [Borealis](https://github.com/encounter/borealis/tree/0bdba6c50a46409c4862474c72b4a3a631fbe0ec) | `0bdba6c50a46409c4862474c72b4a3a631fbe0ec` | `Source/extern/borealis/`; upstream UI dependency |
| [Dusklight cosmetics](https://github.com/TwilitRealm/dusklight-cosmetics/tree/fd93491089fa95441789244f2b2fd22a451ecbcc) | `fd93491089fa95441789244f2b2fd22a451ecbcc` | `Source/mods/cosmetics/` |
| [Dusklight randomizer](https://github.com/TwilitRealm/dusklight-randomizer/tree/d0ceae8f18bbef4a40287a72a2791a3c06c9aca5) | `d0ceae8f18bbef4a40287a72a2791a3c06c9aca5` | `Source/mods/randomizer/` |
| [OpenDSP](https://github.com/Cellenseres/OpenDSP/tree/781c8a3bf22f3d661b304f309526fb19120caf80) | `781c8a3bf22f3d661b304f309526fb19120caf80` | `Source/platforms/3ds/vendor/opendsp/`; reviewed open firmware |
| [libctru queue](https://github.com/devkitPro/libctru/blob/v2.7.0/libctru/source/gpu/gxqueue.c) | `v2.7.0` | `Source/platforms/3ds/runtime/ctru_gxqueue.c`; provenance and zlib notice retained in source |

Dependencies are vendored as ordinary files so the exact local Aurora changes
are available in a fresh clone. Future updates must identify the new pin, local
changes, license notices and affected regression checks together.

## Native adaptation

`Source/platforms/3ds/` and `Tools/3ds/` contain the native platform layer,
research/verification tools and test sources. Shared changes in game, JSystem,
SDK and Aurora code are preserved in their established locations. The existing
desktop build and resources remain upstream material; the native build has its
own CMake entry point.

The PAL identity/save checks and dual-raster mathematical lookup helpers use the
existing platform adapters, C++ standard library and installed Citro3D/libctru.
They add no external port implementation or dependency. Their generated lookup
images and public regression inputs are mathematical fixtures, not game assets.

## Reference-only projects

[Melee 3DS](https://github.com/2gifts/melee-3ds/tree/81cd9f05de43435c9cc71972c16862234c616749)
was an architectural/behavioral reference. Its GPLv3 integration is not vendored
as this port's implementation. Public reference-fetch scripts may retrieve it
for local research, with its own license. Original upstream credits and
component notices remain authoritative.

The public snapshot excludes local research archives, captured game resources,
private operational documents and generated performance evidence. A maintained
status summary describes validation limits without distributing those inputs.
