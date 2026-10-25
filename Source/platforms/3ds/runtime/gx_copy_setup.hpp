#pragma once
#include <dolphin/gx.h>
#include <array>

namespace dusk::native::gx {
// Original GX keeps display and texture copy setup independently and emits
// their shared BP source/size/stride registers only at the actual copy command.
// This is real pending command state; no EFB transfer/completion is implied.
struct CopySurface {
    std::array<u16, 4> source{}; // left, top, width, height in logical EFB pixels
    u16 width{}, height{};
    GXTexFmt format{GX_TF_RGBA8};
    u32 stride{}, control{};
    bool sourceReady{}, destinationReady{}, mipmap{}, depth{};
};
struct CopySetup {
    CopySurface display{}, texture{};
    u64 textureDestination{}; // Original full-width Aurora packet address.
    bool textureDestinationReady{};
};
// Match the original GC tile layout for legal copy destination formats.
bool copyTileLayout(u32 format, unsigned& xShift, unsigned& yShift, unsigned& components);
}
