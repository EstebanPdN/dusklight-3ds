# Source publication verification

Date: 2026-10-25. Scope: the first public current-source snapshot.
No release, prerelease, tag, installation QR or downloadable binary is published.

## Checks for this publication

| Check | Result and scope |
| --- | --- |
| Source inventory | Current original-engine modifications, native platform, test/tool source and modified Aurora are included. Dependencies are ordinary files, with upstream pins documented. |
| Synthetic host suite | Passed on macOS with AddressSanitizer and UndefinedBehaviorSanitizer: disc bounds, I4/IA8 conversion, PICA math/alpha/clip planning, boot-stage parsing, JKR/bootstrap and original JUT/native controller integration. Synthetic dump-verifier tests also passed. |
| Publication and navigation | Checked before push: tracked-file boundaries, required documentation, local guide/image destinations and the exact OpenDSP hash. |
| Historical audio PCM host runner | Blocked at compilation: its test substitutes lack `threadCreate` used by the current NDSP initialization. This runner is not included in the passing public host suite; production audio compiles in the full native build. |
| GitHub Actions clean Linux checkout | Publication checker and all eight enumerated host cases passed. The workflow uses an explicit Ubuntu 24.04 runner and pinned Node 24 actions; see Actions for the current commit result. |
| Native SDK | GCC 16.1.0, existing libctru/Citro3D installation; see the build result below. |

## Native build result

The complete game profile configured, compiled and linked successfully from the
prepared public source tree on macOS using devkitARM GCC 16.1.0. CMake produced
`dusklight-native.elf`, `.smdh` and `.3dsx` under ignored `Build/public-game/`.
The public helper used all four original-game integration gates, with inspection
and optional CMake experiments disabled. No game input was needed or embedded.
These outputs are local verification artifacts and are not uploaded.

## Acceptance limits

No new emulator route or physical console run is claimed for this source
organization/publication. Earlier local scene investigations are historical
observations, not public reproduction artifacts. Full-game natural progression,
reliable story save/reload, complete graphical fidelity, sustained physical
30 FPS and whole-game physical audio remain unverified.

The root CI repeats the enumerated synthetic checks on a clean Linux checkout.
Its actual run status is available in the repository Actions tab. Passing it
does not change the hardware or progression limits above.
