# Local game data, saves and controls

This repository contains source and project artwork. Runtime content must come
from your own compatible original game. Never upload the disc, prepared bundle,
saves or extracted content to this repository or its issues.

## Compatible input and preparation

The native adapter currently validates **GameCube USA revision 0 (`GZ2E01`)**.
The upstream desktop project's wider disc support does not apply automatically
to this native path.

On a POSIX host, inspect the tool and then prepare a new local output:

```sh
python3 Tools/3ds/twilight_pack.py --help
python3 Tools/3ds/twilight_pack.py --input /path/to/your/twilight.iso \
  --output /path/to/private/zelda.twilight
```

The tool verifies the source structure, writes an indexed `DLTWPK01` container,
checks payload readback and refuses to replace an existing output. It preserves
the source input. The generated file contains game data and must remain private.
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
Do not rename another save format into this path. Keep backups outside Git.
A staged native save provider is implemented; full progression save/reload has
not been accepted across the game.

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
