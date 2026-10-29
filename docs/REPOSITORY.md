# Repository map

## Layout and ownership

```text
README.md                    public project entry point
AI_DEVELOPMENT.md             development-process disclosure
LICENSE.md                   upstream CC0 dedication
CONTRIBUTING.md               contribution and review workflow
CHANGELOG.md                  source-publication history
.github/                     source checks and issue templates
Source/
  CMakeLists.txt              original desktop build entry point
  include/                   original and shared engine declarations
  src/                       reconstructed game, actors and game systems
  libs/                      JSystem, audio and other engine libraries
  sdk/                       existing SDK compatibility declarations
  extern/aurora/             pinned Aurora, including current ARM/GX adaptations
  extern/borealis/           pinned upstream UI dependency
  mods/                      pinned upstream cosmetics and randomizer sources
  platforms/3ds/
    CMakeLists.txt           native 3DS executable and link gates
    compile/                 devkitARM toolchain and explicit source inventories
    native/                  new direct Citro3D renderer, shaders, formats and contracts
    runtime/                 transitional GX, OS, data, audio, save and thread adapters
    tests/                   host fixtures, assertions and platform substitutes
    vendor/opendsp/          open-source firmware, notices and exact hash
    *.v.pica                 PICA vertex programs
  platforms/trace/           host-side GX tracing fixture
  docs/                      original upstream documentation
Tools/
  3ds/                       build helpers, tests, data and diagnostic tools
assets/
  images/                    supplied project wordmark and icon
  3ds/                       small icon and banner packaging inputs
docs/                        maintained public New3DS documentation
  badges/                    local factual README badges
  images/                    community buttons and artwork attribution
  legal/                     component notices
  decisions/                 consequential architecture decisions
Build/                       generated locally; ignored
Diagnostics/                 generated local evidence; ignored
```

## Why the source layout is retained

The original CMake files, explicit source inventories and tooling use `Source/`
and `Tools/`. Keeping those paths preserves the engine's established structure
and avoids changing runtime code merely for appearance. Public guides are kept
in `docs/`; generated products remain outside the source tree.

The first public commit is a complete current-source snapshot. Pinned dependency
contents are ordinary tracked files, including local Aurora adaptations. There
are no required Git submodules in the public checkout. `Source/.gitmodules` is
preserved upstream metadata and is not the root repository's submodule setup.
The original upstream history is linked in [Upstream](UPSTREAM.md).

## Maintenance conventions

- Keep native adapters under `Source/platforms/3ds` and use the existing platform guards.
- Preserve actor, JSystem and SDK paths; review shared changes for desktop impact.
- Put maintained public explanations in `docs/` and executable helpers in `Tools/3ds/`.
- Keep native test fixtures beside the platform source. Distinguish synthetic
  fixtures from tests that require locally prepared data or historical evidence.
- Keep dependency notices with their sources and update provenance when changing a pin.
- Use short English commit subjects. File cleanup uses `Clean project files`.
- Do not commit game images, prepared runtime data, saves, memory captures,
  build products, credentials or personal development records.

The root ignore rules deliberately allow only public source, tools, docs and
approved artwork. [The publication checker](../Tools/3ds/check_publication.py)
checks tracked content, public navigation and firmware provenance before a push.

## Organization and documentation policy

This update applies the relevant structure, C++/3DS and documentation chapters
of [Code Atlas](https://code-atlas.estebanpdn.chatgpt.site/#a05-title). Existing
CMake and upstream source paths remain authoritative; folders are added for
actual responsibilities rather than a new universal layout.

Use the README for project state and navigation, this guide for ownership,
[Architecture](ARCHITECTURE.md) for runtime boundaries,
[Native renderer](NATIVE_RENDERER.md) for migration work, and
[Status](STATUS.md) for dated results. Formats and API lifetime/failure contracts
live beside their owning implementation. Keep consequential choices in
[architecture decisions](decisions/0001-native-renderer.md).

When changing an interface, update its contract, caller guidance and relevant
checks in the same change. State ownership, bounds, thread requirements and
failure behavior. Explain why a constraint exists instead of narrating each
line. Keep historical evidence distinct from current acceptance; generated
reports and private inputs stay outside public Git. Verify links, examples and
source closure from a clean checkout before publishing.
