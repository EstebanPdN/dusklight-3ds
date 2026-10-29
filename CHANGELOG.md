# Changelog

This file records public source milestones. It does not announce a binary release.

## Unreleased — native renderer checkpoint (2026-10-29)

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
