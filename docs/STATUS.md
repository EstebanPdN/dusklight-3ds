# Source publication verification

## PAL data and retained GX correctness — 2026-10-29

This update targets the retained compatibility route with
`DUSK_PICA_GAME_DRAW=OFF`. It does not change the unfinished native-model
renderer default or include its separate migration/performance work.

| Check | Executed result and scope |
| --- | --- |
| Public host suite | All ten enumerated cases pass with host ASan/UBSan, including regional packing and card-permission consumer recovery. |
| Save safety | USA/PAL create/write/read/reopen checks pass. Foreign game/maker access and ownership changes are refused with complete container-byte preservation. |
| Card-menu recovery | Actual controller/file-select methods reach the existing non-formatting error state. The old controller is an expected failing busy-state control; matching ownership still dispatches loading. |
| DVD/version/JKR archives | Synthetic USA/PAL checks and read-only PAL-bundle integration pass, including original PAL logo and title resource lookup. No game payload is published. |
| GX planning/backend | Independent normalized material equations, ARM planner compilation, actual SDK register packing and prepared-state restoration pass. |
| GX adapter safety | Host sanitizer checks pass for automatic full-RGBA lookup, separate third-sampler fog, unobserved-alpha recovery, observable-alpha refusal and resource/submit failure paths. |
| Asset-free GPU probe | Nine cases pass in Azahar: affine/perspective comparisons differ by at most one channel byte; byte-atlas and following ordinary-state comparisons are exact. Incorrect vertex-premultiplied products are detected. |
| ARM game target | The complete compatibility game profile compiles/links/packages with devkitARM GCC 16.1.0 and the installed libctru/Citro3D SDK. |
| Normal PAL startup | The original 50/60-Hz selection screen renders and responds to scripted input without forced-stage entry; the run exits at requested frame 360 with completed guest shutdown and no detected fatal. |
| Original scene routes | PAL `F_SP103`, room 1, and Link's house `R_SP01`, room 4, render through requested frame 480 and complete guest shutdown. Reported rejected/simplified/skipped counters remain zero. |
| Controls and visible output | Actual RPC captures are scheduled against the native frame counter. After the house-entry animation, scripted Circle Pad movement changes Link's position in free-control mode; running and subsequent stopping are visible. The A edge is recorded by original PAD code; Ordon dialogue also advances. |
| Existing CI converter/format commands | All five SDK-independent geometry/material/mesh/texture/fragment suites pass locally without importing the separate native-renderer changes. |

Emulator checks use upstream Azahar commit `01623d2fb707317410225c0eced82e3e0c84edc1`,
Vulkan, explicit New3DS selection and a 100% CPU-clock setting. They run with
isolated XDG/SD directories and `--no-sandbox`, not network/write sandbox protection.
Audio output is silent; NDSP status does not establish audible playback.

Direct-stage entries initialize fresh state. These observations are not a natural
playthrough, complete save/progression acceptance, a full-fidelity GX proof or
physical console validation. Stable hardware 30 FPS remains unverified. The
native-renderer checkpoint results below are previously published evidence,
not additional acceptance supplied by this compatibility fix.

## Current native-renderer checkpoint — 2026-10-29

The new renderer, actual model/resource/frame/camera hooks, offline converters,
format contracts and synthetic tests are included in this source update.
Its [implementation and ten-step roadmap](NATIVE_RENDERER.md) describe the
remaining work. This is not a playable native-renderer release.

| Check | Current result and scope |
| --- | --- |
| Existing public host suite | Eight enumerated groups pass under host ASan/UBSan; no disc or emulator. |
| Native engine mutation controls | All 27 enumerated mutations are detected; each control compiles before its expected failing execution. |
| Native conversion/format checks | Nine individual geometry/material/texture/mesh/pose suites pass with synthetic inputs. Optional external-codec compression was not rerun. |
| Native engine host suite | Passes with Citro3D substitutes, including actual packet/archive/palette/camera helper bodies and 243 camera point-mapping cases. |
| Native dependency/ARM audit | Passes for 21 optimized translation units (15 C++ units and six embedded shader units); shader bytes match fresh Picasso assembly. |
| Shader checks | Weighted/rigid, affine-UV and projective algebra/ABI suites pass; not physical PICA precision. |
| Model-plan suite | Passes hierarchy/producer admission and all 65,536 modeled source bias values; filtering on hardware remains unverified. |
| Private fixture boundary | Six tests pass for ignored root-repository data, tracked-file rejection, nested repositories, symlinks and Git-check failure. |
| New native game rendering | Not executed. No original scene, full opening, gameplay or physical acceptance is claimed. |
| Current game target | Complete `dusklight-native` ARM compile/link passes with `DUSK_PICA_GAME_DRAW=ON`; the ELF contains native model-dispatch and camera symbols. No execution or playability claim. |
| Isolated source copy | All 20,695 public files match the intended Git tree. Publication/public-host/native-host checks and a fresh 21-unit ARM library build/audit pass without private research inputs. |
| Historical full game link | The October 25 result below predates the new model renderer and remains separate historical evidence. |

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

No new emulator route or physical console run is claimed for this source
organization/publication. Earlier local scene investigations are historical
observations, not public reproduction artifacts. Full-game natural progression,
reliable story save/reload, complete graphical fidelity, sustained physical
30 FPS and whole-game physical audio remain unverified.

The root CI repeats the enumerated synthetic checks on a clean Linux checkout.
Its actual run status is available in the repository Actions tab. Passing it
does not change the hardware or progression limits above.
