<h1 align="center">DuskLight 3DS</h1>

<p align="center">
  <strong>Twilight Princess. A native New Nintendo 3DS project.</strong><br>
  Experimental source port based on Dusklight.
</p>

<p align="center">
  <a href="https://github.com/EstebanPdN/dusklight-3ds/releases/tag/beta-01">Download beta-01</a> ·
  <a href="#project-status">Status</a> ·
  <a href="#features">Features</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="CONTRIBUTING.md">Contribute</a> ·
  <a href="#community-and-bug-reports">Community</a>
</p>

<p align="center">
  <a href="docs/STATUS.md"><img src="docs/badges/status.svg" alt="Status: public beta" height="22"></a>
  <a href="docs/COMPATIBILITY.md"><img src="docs/badges/platform.svg" alt="Target: New Nintendo 3DS" height="22"></a>
  <a href="docs/STATUS.md"><img src="docs/badges/development.svg" alt="Experimental development" height="22"></a>
  <a href="docs/legal/THIRD_PARTY_NOTICES.md"><img src="docs/badges/license.svg" alt="Component-specific licenses" height="22"></a>
</p>

<p align="center">
  <img src="assets/images/logo.png" alt="DuskLight project wordmark" width="760">
</p>

<p align="center">
  <a href="https://estebanpdn.github.io/dusklight.html"><img src="docs/images/website.svg" alt="Visit Esteban’s Forge" width="142" height="46"></a>
  <a href="https://discord.com/servers/esteban-s-forge-1533248895663210506"><img src="docs/images/discord-official.svg" alt="Join our Discord community" width="166" height="46"></a>
  <!-- <a href="https://ko-fi.com/estebanpdn"><img src="docs/images/kofi.svg" alt="Support EstebanPdN on Ko-fi" width="190" height="46"></a> -->
  <a href="https://www.patreon.com/c/EstebanPdN"><img src="docs/images/patreon.svg" alt="Support EstebanPdN on Patreon" width="216" height="46"></a>
</p>

Native **New Nintendo 3DS** port work for *The Legend of Zelda: Twilight Princess*,
based on [Dusklight](https://github.com/TwilitRealm/dusklight) and the
[Twilight Princess decompilation](https://github.com/zeldaret/tp).

This project was developed with the help of [OpenAI Codex](https://openai.com/codex/). Learn more about the development process [here](AI_DEVELOPMENT.md).

The [public beta](https://github.com/EstebanPdN/dusklight-3ds/releases/tag/beta-01) is available for New Nintendo 3DS. Expect frame rate drops and occasional graphical glitches. I'm still working on performance, graphics, and reliable save/reload.

## Requirements

- **New Nintendo 3DS, New Nintendo 3DS XL, or New Nintendo 2DS XL**.
- Your own *Twilight Princess* GameCube disc, USA (`GZ2E01`) or European/PAL (`GZ2P01`), revision 0.
- For audio, a local DSP component at `sdmc:/3ds/dspfirm.cdc`. The beta also runs muted without it.

## Installation

1. Open [Forge 3DS Converter 1.2.0](https://github.com/EstebanPdN/estebanpdn.github.io/releases/tag/forge-3ds-converter-v1.2.0), select **DuskLight**, and choose the ISO dumped from your own disc.
2. Copy the generated `zelda.twilight` to `sdmc:/3ds/DuskLight/zelda.twilight`.
3. For a HOME Menu install, use FBI to install the CIA from the SD card, or go to **Remote Install → Scan QR Code** in FBI and scan the release QR.

[FBI installation QR](https://github.com/EstebanPdN/dusklight-3ds/releases/download/beta-01/QR-beta-01-github.png)

## Verify the download

Compare the SHA-256 of your download with [`SHA256SUMS.txt`](https://github.com/EstebanPdN/dusklight-3ds/releases/download/beta-01/SHA256SUMS.txt).

## Beta feedback

If the problem is visible, tap **DUMP** on the lower screen and wait for **DONE**.
Keep the completed folder from `sdmc:/3ds/DuskLight/dumps/` for private follow-up. Include your console model, region, beta version and reproduction steps in a GitHub issue. Keep game files, saves and dumps out of public issues.


## Features

<table>
  <tr>
    <td width="50%" valign="top"><strong>Native ARM game integration</strong><br>Original game and JSystem code linked through a dedicated 3DS platform target.</td>
    <td width="50%" valign="top"><strong>Native PICA200 renderer in development</strong><br>Prepared geometry, shaders, materials and textures submitted directly through Citro3D; whole-game migration remains incomplete.</td>
  </tr>
  <tr>
    <td width="50%" valign="top"><strong>3DS platform services</strong><br>Circle Pad and C-Stick mapping, NDSP audio, SD data access and application diagnostics.</td>
    <td width="50%" valign="top"><strong>Inspectable development</strong><br>Source, synthetic host tests, pinned dependencies and maintained build and architecture guides.</td>
  </tr>
</table>

These are implemented development systems, with the validation limits described
in [Compatibility](docs/COMPATIBILITY.md). The 400×240, 2D, 30 Hz target is a goal;
sustained 30 FPS on physical hardware has not been established.

## Project status

The [public beta](https://github.com/EstebanPdN/dusklight-3ds/releases/tag/beta-01) uses the retained GX compatibility renderer. The native model renderer is still in development. See the [native renderer roadmap](docs/NATIVE_RENDERER.md).

[Building](docs/BUILDING.md) covers the development builds. [Verification status](docs/STATUS.md) has dated results, and [Game data](docs/GAME_DATA.md) covers compatible discs, SD paths, saves and controls.

## Documentation

| Guide | Purpose |
| --- | --- |
| [Documentation index](docs/README.md) | Start with the guide for your task. |
| [Repository map](docs/REPOSITORY.md) | Source ownership, directory layout and maintenance rules. |
| [Building](docs/BUILDING.md) | Toolchain, clean checkout, diagnostic and full integration builds. |
| [Native renderer](docs/NATIVE_RENDERER.md) | Current implementation, ten-step roadmap, build/test commands and open gates. |
| [Architecture](docs/ARCHITECTURE.md) | Engine boundary, rendering, audio, storage and lifecycle. |
| [Compatibility](docs/COMPATIBILITY.md) | Target systems, implemented paths and remaining acceptance gates. |
| [Game data and controls](docs/GAME_DATA.md) | Local data preparation, SD paths, save format and input mapping. |
| [Configuration](docs/CONFIGURATION.md) | CMake options, profiles and experiment boundaries. |
| [Testing](docs/TESTING.md) | Synthetic host checks, target compilation and runtime evidence. |
| [Diagnostics](docs/DIAGNOSTICS.md) | Local reports, dump verification and useful bug reports. |
| [Verification status](docs/STATUS.md) | Checks performed for this source publication. |
| [Source provenance](docs/UPSTREAM.md) | Upstream pins, vendored changes and attribution. |

## Development

```sh
git clone https://github.com/EstebanPdN/dusklight-3ds.git
cd dusklight-3ds
python3 Tools/3ds/check_publication.py
python3 Tools/3ds/test_public_host.py
```

The source and pinned dependencies are included as ordinary files; a recursive
submodule download is not required. Host checks use Python 3 and a C++20 compiler
with AddressSanitizer and UndefinedBehaviorSanitizer. A target build also needs
devkitARM, libctru, Citro3D, CMake and the devkitPro 3DS tools.

<details>
<summary><strong>Build the native integration locally</strong></summary>

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
python3 Tools/3ds/build_public.py --profile game
```

Outputs stay in `Build/public-game/`. Building does not upload anything or create
a release. See [Building](docs/BUILDING.md) for the diagnostic profile, exact
configuration, prerequisites and current limitations.

</details>

## Community and bug reports

[Join the project Discord](https://discord.com/servers/esteban-s-forge-1533248895663210506) for discussion and
[open a GitHub issue](https://github.com/EstebanPdN/dusklight-3ds/issues) for a
reproducible source problem. Include the commit, build profile, toolchain,
console or emulator version and concise reproduction steps.
See [Diagnostics](docs/DIAGNOSTICS.md) before sharing captures.

## Credits

- [Twilit Realm and Dusklight contributors](https://github.com/TwilitRealm/dusklight) — upstream reimplementation.
- [zeldaret/tp](https://github.com/zeldaret/tp) and the GameCube/Wii decompilation community — reconstructed engine and research.
- [Aurora contributors](https://github.com/encounter/aurora) — GX compatibility and native integration foundations.
- [devkitPro](https://devkitpro.org/) — devkitARM, libctru and Citro3D.
- [Cellenseres / OpenDSP](https://github.com/Cellenseres/OpenDSP) — open-source NDSP component.
- Esteban PDN — New Nintendo 3DS adaptation, project artwork and maintenance, with help from Codex.
- Nintendo — the original game and its associated names and content.

Full dependency scope is in [Third-party notices](docs/legal/THIRD_PARTY_NOTICES.md).
This is an independent fan project and is not affiliated with or endorsed by
Nintendo or Twilit Realm. Upstream project links identify the original authors;
the 3DS adaptation and its development process are maintained independently.

## License and legal notice

Dusklight and original port contributions follow the upstream
[CC0 1.0 dedication](LICENSE.md). Dependencies retain their own licenses and
notices; the root dedication does not relicense them or Nintendo game content.
See [Third-party notices](docs/legal/THIRD_PARTY_NOTICES.md) and
[Artwork](docs/images/README.md).

---

<p align="center">
  <a href="https://estebanpdn.github.io/">Esteban’s Forge</a> ·
  <a href="https://discord.com/servers/esteban-s-forge-1533248895663210506">Discord</a> ·
  <a href="docs/README.md">Documentation</a> ·
  <a href="docs/legal/THIRD_PARTY_NOTICES.md">Credits and notices</a><br>
  <sub>Twilight Princess belongs to Nintendo. This is an independent fan project.</sub>
</p>
