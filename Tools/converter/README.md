# Forge 3DS Converter

Open the app, choose a port, and drop the compatible game image onto its screen. You can also use the file picker. Each converter verifies the output and saves it beside the input file. Move an existing output before starting a new conversion in the same folder.

| Port | Input | Output | SD card folder |
| --- | --- | --- | --- |
| DuskLight | GameCube Twilight Princess ISO, USA or PAL revision 0 (GZ2E01 or GZ2P01) | zelda.twilight | /3ds/DuskLight/ |
| Mario Kart 64 | Unmodified USA Nintendo 64 ROM, .z64, .n64 or .v64 | mk64.o2r | /3ds/MK64/ alongside the port's required mk64.z64 |
| Super Mario 64 DS | European Nintendo DS ROM, .nds, ASMP revision 0 | mario.sm64ds | /3ds/sm64ds/ |

The converter prepares companion data for an installed 3DS port. Each desktop package runs offline and includes Python and the game-specific conversion engines. Extract the full package before running it. Open Forge 3DS Converter.app on Mac or the Forge 3DS Converter executable on Windows or Linux.

## Build on macOS

Run the following commands in this directory using Node 22.12+, Python 3.12+ and Xcode Command Line Tools. CMake is needed for the Intel Mac and Windows extractors. For Windows, put the LLVM-MinGW x86_64 cross tools on PATH (tested with 20261117 UCRT).

```sh
npm ci
npm run build
```

Packages are written to `Build/forge-converter/`. To build one, pass `mac-arm64`, `mac-intel`, `linux-x64` or `windows-x64` after `npm run build --`.

Builds use this repository's Twilight packer, the sibling SM64DS project's packer core and recipes, and the sibling MARIO KART 64 project's desktop extractor. Linux uses the native engine from the local v2.1-E2 release package. The Intel Mac and Windows engines are built from the extractor’s CMake project. The Windows build normalizes YAML dependency keys before extraction. Converter credits and licenses are included beside the packaged backends.

The launcher uses Forge Store branding, catalog icons and game artwork. The DuskLight screen uses the supplied Link and wolf illustration.

## Artwork

The game logos and illustrations of the port screens (`ui/art/dusklight-art.jpg`,
`ui/art/mk64*` and `ui/art/sm64ds*`) belong to their owners and are not in the
repository. A local build expects them in `ui/art/`. The Forge branding, the
Nunito font (SIL Open Font License, license text beside it in
`ui/art/nunito-OFL.txt`) and the forest background are included.
