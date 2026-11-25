#pragma once
#include <cstdint>
namespace dusk::native {
// Bumped by every operation that can change PICA fixed-function state, the
// bound program, bound textures or their contents outside the chained native
// shape replay. The replay compares it to prove that the state it applied for
// the previous shape is still in effect.
inline std::uint32_t gxPicaStateSerial{};
// Bumped when the pixel storage of a cached texture is freed or moved (an
// image retired, a spare released, an image moved into VRAM). Recorded PICA
// words that name texture addresses (the HUD tape) are only run again while
// it stands; an image decoded again in place keeps its address.
inline std::uint32_t gxTextureStorageSerial{};
// Bumped by every framebuffer copy (GXCopyTex, its destroy, GXCopyDisp): work
// the PICA does outside the command words of a draw pass, so a pass that
// contains one cannot be run again from its recorded words.
inline std::uint32_t gxCopySerial{};
}
