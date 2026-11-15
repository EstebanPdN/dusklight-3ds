# ADR 0002: Build the game with int-sized enums

Date: 2026-11-14. Status: adopted. Verified in Azahar; console confirmation
of the cutscene camera is pending.

## Context

devkitARM compiles with `-fshort-enums`: an enumeration whose values fit in
one byte occupies one byte. The reconstructed game, like the GameCube original
and the desktop port, assumes four-byte enums. It reads them from big-endian
file data (for example `BSWAP32(*(TFunctionValue::TEInterpolate*)p)` in
JStudio) and lays structures with enum members over resources.

With one-byte enums such a read took the first byte of a big-endian word,
which is zero. The visible symptom was the cutscene camera: every key frame
was read as "no interpolation", so each shot held still and then jumped.
Nothing failed loudly; the same wrong read can affect any file-backed enum.

## Decision

Build the whole tree with `-fno-short-enums` (and `-Wl,--no-enum-size-warning`
for the mixed link) in `Source/platforms/3ds/CMakeLists.txt`.

The prebuilt libctru, Citro3D and zstd libraries keep short enums. Every SDK
structure was compiled both ways and compared. Only `C3D_FrameBuf`,
`C3D_RenderTarget` and `ERRF_ExceptionInfo` (plus SDK-internal structures the
port never touches) change layout; everything passed by value is identical.
Fields of those three structures from the first enum member onward are read
only through `Source/platforms/3ds/runtime/ctru_abi.hpp`, which mirrors the
SDK layout with static assertions. SDK inline helpers that touch those fields
(`C3D_RenderTargetDetachOutput`, `C3D_FrameBufAttrib`, `C3D_FrameBufColor`,
`C3D_FrameBufDepth`) are not used.

`Tools/3ds/check_ctru_abi.py` runs as part of the native build and fails it
when a platform source names one of those members or inline helpers directly.

## Alternatives

- Keep short enums and patch each file-backed read: the affected reads are
  spread across the reconstructed game and cannot be found reliably.
- Rebuild libctru and Citro3D with int enums: removes the boundary, but adds a
  private SDK fork that every contributor would have to build.

## Consequences

- File-backed enums read their full value; cutscene cameras interpolate again.
- Save structures have the same size under both settings
  (`dSv_save_c` is 0x958 bytes), so saves made by earlier builds stay valid.
- The first mirror missed three `C3D_GetFrameBuf()` reads and switched GPU
  triangle fog off; the build check exists because a careful search was not
  enough.

## Verification and reconsideration

Azahar: the cutscene camera value changes on every logic tick and dense
captures match the reference intro. Host: the boundary check rejects a planted
direct member access. Reconsider when the SDK is updated: compile its
structures with both settings again and update the mirrors and their
assertions.
