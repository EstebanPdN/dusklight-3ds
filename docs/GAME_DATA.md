# Local game data, saves and controls

This repository contains source and project artwork. Runtime content must come
from your own compatible original game. Never upload the disc, prepared bundle,
saves or extracted content to this repository or its issues.

## Compatible input and preparation

The native adapter validates **GameCube USA (`GZ2E01`) or European/PAL
(`GZ2P01`) revision 0**. Packing preserves the original disc ID, DOL, FST and
resource bytes; the existing game version selection chooses PAL resources and
offsets without relabeling the disc as USA. Other revisions and Wii images
remain unsupported by this native adapter.

On a POSIX host, inspect the tool and then prepare a new local output:

```sh
python3 Tools/3ds/twilight_pack.py --help
python3 Tools/3ds/twilight_pack.py --input /path/to/your/twilight.iso \
  --output /path/to/private/zelda.twilight
```

The tool verifies the source structure, writes an indexed `DLTWPK01` container,
checks payload readback and refuses to replace an existing output. It preserves
the source input. The generated file contains game data and must remain private.
Both regions use the same bundle format and command. Validate the synthetic
regional packing regression with `python3 Tools/3ds/test_twilight_pack.py`.
The tool's signal handling and publication mechanics use POSIX facilities;
Windows compatibility is not claimed by this guide.

This is a developer data-preparation route. It does not install an application
and no ready-to-install package accompanies this source publication.

## SD paths

| Purpose | Source-defined path |
| --- | --- |
| Native game data | `sdmc:/3ds/DuskLight/zelda.twilight` |
| Save container | `sdmc:/3ds/dusklight/saves/native-card0.bin` |
| Pending save write | `sdmc:/3ds/dusklight/saves/native-card0.pending` |

Preserve the exact spellings. Data and save paths currently differ in case in
the source. The save container uses `DSVCARD1`, preserves the original game save
payload and is neither a raw GameCube memory card nor a Dolphin GCI file.
New save metadata uses the game/maker ID passed by the original memory-card
initialization, including `GZ2P` for PAL. A container with a different game/maker
is rejected without exposing or overwriting its payload; metadata updates cannot
change its ownership. The 3DS card-menu controller reaches an existing terminal
error/no-save flow instead of remaining busy or offering to format that container.

This adapter holds one save container at the path above, not separate regional
slots. Keep independent backups when switching between USA and PAL. Existing
saves are not relabeled, converted or automatically migrated. Do not rename a
raw card or GCI file into this path. Keep backups outside Git.
Full progression save/reload has not been accepted across the game.

## Input mapping

| New3DS input | Original controller input |
| --- | --- |
| Circle Pad | Main stick |
| C-Stick | C-stick |
| A / B / X / Y | Corresponding original face buttons |
| D-pad | Corresponding original directional buttons |
| L / R | Digital shoulder triggers |
| ZL or ZR | Z trigger |
| START | START |

The source maps the controller API; the game's current context assigns actions.
Analog trigger pressure is synthesized from digital shoulder presses. The port
does not provide a rumble motor. Calibration and physical gameplay acceptance
remain separate from host mapping tests.

For developer dump controls and handling, see [Diagnostics](DIAGNOSTICS.md).
