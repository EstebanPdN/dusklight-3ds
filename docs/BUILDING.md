# Building the New3DS target

Run every command below from the repository root. No game data is needed to
compile the native executable. Running original game code needs local data.

## Prerequisites

| Tool | Purpose | Verification |
| --- | --- | --- |
| Python 3.10+ | Build helper and source checks | `python3 --version` |
| CMake 3.25+ | Native build graph | `cmake --version` |
| devkitARM | ARM11 cross compiler | `"$DEVKITARM/bin/arm-none-eabi-g++" --version` |
| libctru | 3DS services and linker specs | `$DEVKITPRO/libctru/include/3ds.h` |
| Citro3D | PICA200 submission | `$DEVKITPRO/libctru/lib/libcitro3d.a` |
| picasso, smdhtool, 3dsxtool | Shader and homebrew packaging | `$DEVKITPRO/tools/bin/` |
| Host C++20 compiler | Synthetic checks only | `c++ --version` |

Use an existing devkitPro installation. The maintained source was developed with
GCC 16.1.0, libctru 2.7.0 and Citro3D 1.7.1. Record the actual installed versions
when reporting a build; these values are not a claim that every SDK version works.
The vendored queue adapter identifies its libctru 2.7.0 source explicitly.

## Clean checkout

```sh
git clone https://github.com/EstebanPdN/dusklight-3ds.git
cd dusklight-3ds
python3 Tools/3ds/check_publication.py
python3 Tools/3ds/test_public_host.py
```

Dependencies needed by the native target are included. Do not run upstream
checkout-completion scripts against this public snapshot: they were historical
workspace tools and are not the public dependency installation route.

## Native game integration

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
python3 Tools/3ds/build_public.py --profile game --jobs 4
```

On installations outside `/opt/devkitpro`, set `DEVKITPRO` to the real directory.
The helper passes both SDK locations explicitly to the existing CMake target.
It enables the original scene, environment, complete actor link and game startup
paths. It does not enable optional scene experiments.

The current game profile defaults `DUSK_PICA_GAME_DRAW=ON`, selecting the
**unfinished native model renderer**. It is not the earlier compatibility
renderer build and is not playable yet: missing native assets/parameters refuse
at draw time. To compile only the new renderer, use
[the native library instructions](NATIVE_RENDERER.md#build-and-test).

Outputs are `Build/public-game/dusklight-native.elf`, `.3dsx`, `.smdh` and `.map`.
The generated SMDH still uses the historical diagnostic description; it does not
identify a supported consumer build. ELF and map files are for local debugging.
A successful link does not establish full-game or physical hardware behavior.
The October 25 full-link result in the historical status predates the new
renderer. See [current verification](STATUS.md) before attributing that result
to this checkpoint.

## Retained GX compatibility renderer

To exercise the regional data and dual-raster fixes without selecting the
unfinished native-model route, use an explicit separate configuration:

```sh
python3 Tools/3ds/build_public.py --profile game \
  --build-dir Build/compatibility-game \
  --cmake-option=-DDUSK_PICA_GAME_DRAW=OFF
```

The default is unchanged. Compatibility-route observations do not establish
acceptance of the new native renderer, complete gameplay or physical performance.

## Synthetic diagnostic profile

```sh
python3 Tools/3ds/build_public.py --profile diagnostic --jobs 4
```

This leaves original game startup disabled and produces the native diagnostic
bootstrap under `Build/public-diagnostic/`. It is not a playable game.

## Direct CMake equivalent

```sh
cmake -S Source/platforms/3ds -B Build/manual-game \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/Source/platforms/3ds/compile/devkitarm.cmake" \
  -DDEVKITPRO="$DEVKITPRO" -DDEVKITARM="$DEVKITARM" \
  -DDUSK_SCENE_STARTUP=ON -DDUSK_SCENE_ENVIRONMENT_STARTUP=ON \
  -DDUSK_GAMEPLAY_LINK_GATE=ON -DDUSK_GAME_STARTUP=ON
cmake --build Build/manual-game --parallel 4
```

For a different build directory, use `--build-dir PATH`. For an explicit CMake
option, repeat `--cmake-option=-DNAME=VALUE`. Keep experimental configurations
separate and report their exact options; the [configuration guide](CONFIGURATION.md)
explains the distinction.

## Local runtime data

Follow [Game data](GAME_DATA.md) to prepare your own compatible input. The
compiler does not read or embed the disc, bundle or save. The open-source
OpenDSP component is hash-checked and embedded for the full game startup path.
It is separate from Nintendo console firmware.

## CIA packaging and publication

The current native CMake entry point generates a 3DSX. Historical local CIA
packaging helpers and RSF templates are retained for source completeness. They
need extra tools and local inputs and are not a supported release pipeline.
There is no upload, tag or release step in `build_public.py` or the root CI.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Compiler not found | Verify `DEVKITARM`, including its `bin/` directory. |
| picasso or smdhtool missing | Verify the devkitPro `tools/bin` installation. |
| `3dsx.specs` or libctru missing | Use the matching devkitARM and libctru SDK. |
| Missing Aurora headers | Use the complete public source checkout, not a partial archive. |
| Full link requires scene/environment | Use the game profile or all four direct CMake options. |
| Missing runtime input | Prepare data locally; compiling does not create game data. |

Do not delete working saves or replace SDK versions to hide a failure. Include
the first actual compiler/linker error and exact configuration in a report.
