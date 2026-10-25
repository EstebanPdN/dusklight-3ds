# New Nintendo 3DS platform

This is the native CMake entry point. It is independent of the desktop build
at `Source/CMakeLists.txt`. Start with the repository's [build guide](../../../docs/BUILDING.md).

- `main.cpp`: application initialization and frame loop.
- `CMakeLists.txt`: kernel, GX, scene/environment and original-game link gates.
- `compile/`: devkitARM toolchain and explicit original-source inventories.
- `runtime/`: native platform services and GX translation; see its [map](runtime/README.md).
- `gx_*_pica.cpp` and `*.v.pica`: GPU-facing renderer and vertex programs.
- `tests/`: synthetic fixtures and platform substitutes; see [test guidance](tests/README.md).
- `vendor/opendsp/`: exact open-source audio component and redistribution notices.

The default CMake configuration is diagnostic. The full integration needs
explicit scene/environment, gameplay-link and game-startup options. A successful
link is separate from playable progression or physical New3DS acceptance.
