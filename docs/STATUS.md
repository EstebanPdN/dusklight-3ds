# Source publication verification

## Native input mapping and GPU verification — 2026-10-30

The original experimental checkout now sets the complete nine-attribute input
permutation explicitly. The installed SDK truncated its upper word, sending
skin weights to the position input and leaving the weight input zero. Captured
SDK commands and real Azahar interpreter/JIT replay identify and verify the fix.
The pixel probe also now accounts for raw-row Y orientation and acquires complete
ABGR/LCD snapshots before filesystem writes.

| Check | Observed result |
| --- | --- |
| Actual native Renderer GPU matrix | Baseline plus all 297 cases pass; zero failed comparisons, missing interior pixels, changed far-exterior pixels or ordinary-restoration differences. |
| Maximum channel error | R/G/B/A = 1/2/2/1 bytes across the complete matrix, within unchanged tolerances. |
| Renderer coverage | Eight lights, four independent color channels, dual-raster lookup products, all seven fog carriers and range cases, weighted/rigid and affine/projective variants; exact full-image LCD/readback RGB agreement. Texture-backed fog also passes alongside both raster lookup images. |
| Native host regressions | ASan/UBSan suite and all 27 compiled negative controls pass. A constrained GPU-byte-budget case now detects loss of model resource sharing without relying on allocation-count assertions. |
| Complete native game build | `cmake --build Build/public-game --target dusklight-native -j 4` completes the original-game ARM compile/link with `DUSK_PICA_GAME_DRAW=ON`; this build is not a native-scene playability result. |
| Runner | Upstream Azahar `01623d2fb707317410225c0eced82e3e0c84edc1`, Vulkan, explicit New3DS model, 100% CPU; unsandboxed isolated run with silent output. Upstream HEAD still matched this revision when checked. |
| Focused compatibility PR | [PR #2](https://github.com/EstebanPdN/dusklight-3ds/pull/2) remains separate from these experiments. Its sanitizer-scope documentation correction is commit `e9e7824766207a67892558f9f82ac42ea8e59036`; source CI passes for that commit. |

These are asset-free emulator comparisons, not native gameplay or physical
30-FPS acceptance. The current native model limit is 1024 parts; the PAL casual
Link body converts to 384 source-ordered parts. Selected Link, house and sky
banks were prepared from original PAL data and staged in an isolated Azahar run.
After adding concat-view palette support, the run progressed to house
`model3.bmd`, whose live framebuffer image is rejected as mutable. Its authored
dummy is not an acceptable replacement. Copy filtering, capture ordering,
sampling and GPU lifetime require further work before that model can render.
The source-derived cull conversion also passes twelve two-winding Azahar
comparisons against the compatibility projection/cull helper proxy; that is not
a comparison against the full GX renderer or physical hardware.

## Previous native-renderer checkpoint — 2026-10-29

The new renderer, actual model/resource/frame/camera hooks, offline converters,
format contracts and synthetic tests are included in this source update.
Its [implementation and ten-step roadmap](NATIVE_RENDERER.md) describe the
remaining work. This is not a playable native-renderer release.

| Check | Current result and scope |
| --- | --- |
| Existing public host suite | Nine enumerated groups pass under host ASan/UBSan and Python checks, including USA/PAL bundle round trips; no original disc or emulator. |
| Native engine mutation controls | All 27 enumerated mutations are detected; each control compiles before its expected failing execution. |
| Native conversion/format checks | Nine individual geometry/material/texture/mesh/pose suites pass with synthetic inputs. Optional external-codec compression was not rerun. |
| Native engine host suite | Passes with Citro3D substitutes, including actual packet/archive/palette/camera helper bodies and 243 camera point-mapping cases. |
| Native dependency/ARM audit | Passes for 21 optimized translation units (15 C++ units and six embedded shader units); shader bytes match fresh Picasso assembly. |
| Shader checks | Weighted/rigid, affine-UV and projective algebra/ABI suites pass; not physical PICA precision. |
| Model-plan suite | Passes hierarchy/producer admission and all 65,536 modeled source bias values; filtering on hardware remains unverified. |
| Private fixture boundary | Six tests pass for ignored root-repository data, tracked-file rejection, nested repositories, symlinks and Git-check failure. |
| New native game rendering | Local Azahar 2126.0 EU startup/UI ran without a detected fatal error; direct Ordon entry created Link, then refused the first model because its source-identified `.dna` asset was missing. No complete original scene, gameplay or physical acceptance. |
| Current game target | Complete `dusklight-native` ARM compile/link passes with `DUSK_PICA_GAME_DRAW=ON`; local EU execution is scoped by the preceding row, not a playability claim. |
| Isolated source copy | All 20,695 public files match the intended Git tree. Publication/public-host/native-host checks and a fresh 21-unit ARM library build/audit pass without private research inputs. |
| Historical full game link | The October 25 result below predates the new model renderer and remains separate historical evidence. |
| EU data and save identity | USA/PAL synthetic disc/DVD/version/bundle/save checks pass. The local `GZ2P01` ISO packs with full file readback verification, and actual PAL logo/title archives open through the original JKR path under host ASan/UBSan. |
| Retained compatibility renderer, EU Ordon | Separate `DUSK_PICA_GAME_DRAW=OFF` build creates Link, then rejects a 384-vertex material that needs two raster-color channels. The existing opt-in dual-raster diagnostic advances past it but rejects a subsequent non-grayscale raster0 material. No complete Ordon scene or gameplay acceptance. |

Detailed compiler commands and generated receipts remain local. The published
commands in [Testing](TESTING.md) and [Building](BUILDING.md) reproduce source
checks without those private records. Current GitHub Actions results must be
read for the exact pushed commit; this document does not assume they passed.

## Historical first publication — 2026-10-25

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

No physical console run is claimed for this source organization/publication.
The local EU emulator checks above validate disc/resource integration and
identify renderer blockers; they do not establish playability. Earlier scene
investigations remain historical observations, not public reproduction artifacts.
Full-game natural progression, reliable story save/reload, complete graphical
fidelity, sustained physical 30 FPS and whole-game physical audio remain unverified.

The root CI repeats the enumerated synthetic checks on a clean Linux checkout.
Its actual run status is available in the repository Actions tab. Passing it
does not change the hardware or progression limits above.
