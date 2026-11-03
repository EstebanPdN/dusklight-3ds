# Changelog

This file records public source milestones. It does not announce a binary release.

## Unreleased — GitHub Actions checks (2026-10-30)

- Raise the native game frame ceiling to 4096 draws without changing the
  1024-part model limit or enlarging the command arena and heaps; reclaim command
  storage through the existing shared-queue completion guard when needed.
- Draw mirror model packets natively with validated reflected-base, ambient and
  cull overrides, and republish the mirror and restored main camera/scissors;
  depth and mask quads remain on the compatibility path.
- Prefer VRAM for native immutable model textures, transferring complete mip
  chains through completion-owned linear staging and falling back to linear
  texture storage when VRAM is exhausted.
- Bump native texture packs to version 3 and add tiled, two-byte RGBA5551
  images; version 1 and 2 banks must be regenerated.
- Convert CMPR across all stored mips to RGB565 when every texel is opaque,
  otherwise RGBA5551, with rounded color channels and exact one-bit alpha.
- Accept concat-view envelope matrices whose index exceeds the joint scale
  table (Link `al_face`: 5 joints, 35 envelopes) using the inverse-transpose
  normal; the original read past that array. Report palette-refusal inputs,
  native asset load errors and linear/VRAM/malloc headroom in guest logs.

- Add Python correctness/minimum-version checks, ShellCheck, actionlint and
  path-sensitive C++ analysis without reformatting upstream source.
- Exercise GCC/Clang host sanitizers, both 3DS build profiles and assembled
  native shader checks in read-only, pinned GitHub Actions jobs.
- Add weekly Dependabot updates, cancellation/timeouts and merge-queue triggers;
  document local lint commands and administrator-managed required checks.
- Configure CodeRabbit PR reviews with project-specific guidance and review-error
  reporting; leave GitHub App authorization and merge rules to the repository owner.
- Link the shared JKR thread owner in the diagnostic profile and update the
  geometry rejection fixture to use a genuinely unsupported shape mode.
- Preserve source-only publication: no binary uploads, tags or release workflow.

## Unreleased — native renderer checkpoint (2026-10-29)

- Preserve `fbtex_dummy`/`dummy` as typed live framebuffer-color roles in
  version-2 native texture banks; regenerate older banks. Match the registered
  capture's stable role identity and borrow its current compatibility texture
  at draw time, without accepting unknown mutable or depth providers.
- Record source TEV-stage and texgen counts in native model-plan version 4 and
  refuse live count mismatches, including Link's unsupported warp-on state.
  Regenerate older native banks; equal counts do not prove program equivalence.
- Report the missing bank identity and the failing native shape/producer stage
  to help diagnose incomplete game integration without enabling a fallback.
- Gather concat-view shape palettes from live remapped animation/envelope matrices
  and the packet's view matrix instead of the unused J3D draw-matrix sentinels;
  unsupported matrix modes still reject.
- Convert GX front/back culling to the native PICA winding convention; twelve
  two-winding Azahar comparisons cover orthographic and perspective projections.

- Accept European/PAL revision-0 (`GZ2P01`) data alongside USA revision-0
  (`GZ2E01`) in the packer and native disc reader, preserving original identity
  and existing region-aware resource selection.
- Preserve the original game's region in newly created save metadata.
- Cover both regions in disc, DVD/version, bundle and save regressions; make
  the archive integration harness link its real worker dependencies on Linux.
- Add the standalone direct Citro3D/PICA200 engine, six vertex shader variants,
  resource lifetime/budget contracts and native model/scene submission.
- Set the complete nine-attribute native vertex mapping explicitly; the SDK's
  truncated upper permutation otherwise overwrites position with skin weights
  and collapses submitted triangles.
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
