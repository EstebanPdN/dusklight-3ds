# Changelog

This file records public source milestones. It does not announce a binary release.

## Unreleased — console test fixes and source cleanup

Fixed
- Cutscene cameras and other file-backed enums: the tree is built with
  int-sized enums; SDK structures whose layout depends on it are read through
  `runtime/ctru_abi.hpp` and checked at build time.
- First gameplay picture on the console: render targets are read back by the
  CPU from VRAM instead of the transfer engine.
- START+SELECT from the title no longer clears the title scene's memory pools.
- Saves on the SD card: the container is replaced as a whole file; the empty
  save left by earlier builds is removed.
- The L+R+A dump no longer reads GPU hardware registers (that read froze the
  console).
- Black presentation between scenes no longer uses an SDK inline that reads
  render-target fields at the wrong offsets.
- Host test runners: the substitutes and fixtures of eight research runners
  follow the current runtime again (six-attribute compact vertices, the
  hardware clip guard, the GPU guard, the multi-texture default route, F32
  texture coordinates as fixed point); two no longer need private baselines.

Added
- Real-speed game logic: 30 logic steps per second of real time, up to four
  per picture.
- Console GPU guard: new draw kinds run alone once, hang records with the last
  GPU jobs, transfer-stop fallback levels, per-build learned state.
- Bottom-screen developer console, START+SELECT automatic multi-zone test,
  frame telemetry, and one folder per L+R+A dump with screenshots and a
  readable state report.
- Native bloom and model shadows drawn with PICA-native passes.
- Tools: `check_ctru_abi.py`, `check_pica_shader.py`, `gpu_hang_report.py`,
  `pica_list_decode.py`, `pica_shader_disasm.py`, `analyze_tour.py`,
  `generate_zone_table.py`.

Changed
- GX sync calls are ordered on the GPU instead of waiting for it.
- An oversized real-shadow request (the title's wall of light) is skipped.

Removed
- Closed investigations: the capture94 and FRAME76 draw captures, the
  VITAL80 log, the strict material74 mode, and the scene-creation,
  ground-creation and background-budget compile-time probes.
- Four functions with no caller and an orphan test source no build compiled.

No release, tag or binary is published. Console results are recorded in
[Verification status](docs/STATUS.md).

## Unreleased — PAL input and GX compatibility fixes

- Accept European/PAL revision-0 (`GZ2P01`) data alongside USA revision-0
  (`GZ2E01`) in the packer and disc reader without relabeling original data.
- Preserve game/maker IDs in new save metadata and reject foreign ownership
  without reading, overwriting or converting the existing save container.
- Route native card permission errors through the existing non-formatting menu
  error state instead of leaving file selection busy.
- Translate supported dual-raster GX materials using independently interpolated
  RGBA colors, keeping their multiplication in the fragment combiner.
- Preserve both lookup bindings when an untextured dual-raster draw appends a
  third fog sampler, including its separate texture-coordinate output.
- Retain ordinary two-texture rendering when raster-1 alpha is unobserved;
  alpha tests, blending and alpha writes keep their real dependencies.
- Add regional data/save regressions, full-color adapter checks and an asset-free
  GPU probe; repair the archive test harness's Linux worker linkage.
- Keep the unfinished native-model renderer default and performance work separate.
  These fixes target `DUSK_PICA_GAME_DRAW=OFF`; no hardware 30-FPS claim is made.

## Native renderer checkpoint — 2026-10-29

- Add the standalone direct Citro3D/PICA200 engine, six vertex shader variants,
  resource lifetime/budget contracts and native model/scene submission.
- Include offline geometry, texture, material, sampler and pose conversion tools,
  native bank/envelope readers and original source identity checks.
- Connect actual game model packets, resource identity, shared frame lifecycle,
  palette gather and selected high-level camera publication. Complete live
  material/environment input, UI/effects and full-game migration remain pending.
- Add synthetic renderer/converter/format/shader tests and complete the missing
  camera test dependency. Replace a machine-specific model-plan SDK include path.
- Document implementation boundaries, build/test commands, source ownership,
  the ten-step roadmap and the native architecture decision using Code Atlas.
- Extend read-only source CI with five SDK-independent new-native conversion
  and bank-format suites.
- Keep prepared native game banks ignored and retain GitHub's existing owner
  documentation changes while reconciling the source histories.

This is unfinished source work. No playable binary, hardware validation or
performance improvement is claimed.

## Historical initial source publication — 2026-10-25

- Publish the current New3DS platform layer, shared engine changes and Aurora
  adaptations based on Dusklight v2.0.1.
- Include pinned dependency source, native fixtures, PICA programs, open-source
  audio component and the current development tools.
- Add a consistent project README, documentation index, architecture, build,
  configuration, data, testing, compatibility, diagnostic and provenance guides.
- Add an explicit public build helper, reproducible synthetic host checks and
  a tracked-file publication check.
- Replace five machine-specific SDK paths in research tools with explicit
  `DEVKITPRO` / `DEVKITARM` configuration.
- Keep generated packages, game inputs, saves, captures and private engineering
  records outside public Git.

No release, prerelease, version tag, installation QR or end-user binary is created
for this milestone. Full-game and physical performance acceptance remain pending.
