#include "gx_stream.hpp"
#include <dolphin/os.h>
#include <bit>
#include <cstring>
#include <type_traits>

namespace {
[[noreturn]] void fail(const char* reason) {
    OSPanic(__FILE__, __LINE__, "Native GX texture cache region: %s", reason);
}
}
extern "C" void GXInitTexCacheRegion(GXTexRegion* region, GXBool mipmap32,
    u32 evenAddress, GXTexCacheSize evenSize, u32 oddAddress, GXTexCacheSize oddSize) {
    // This opaque value describes original cached TMEM regions. PICA keeps
    // actual converted images in its bounded cache, keyed by validated image
    // identities; these GC addresses are never dereferenced on ARM.
    if (auto* stream = dusk::native::gx::commandStream(); stream && !stream->finish())
        fail("incomplete command stream");
    const auto even = std::bit_cast<std::underlying_type_t<GXTexCacheSize>>(evenSize);
    const auto odd = std::bit_cast<std::underlying_type_t<GXTexCacheSize>>(oddSize);
    if (!region || (evenAddress & 31) || (oddAddress & 31) ||
        evenAddress >= 0x100000 || oddAddress >= 0x100000 || u32(even) > 2 || u32(odd) > 3)
        fail("invalid address, alignment or size");
    const u32 evenExponent = u32(even) + 3;
    const u32 oddExponent = odd == GX_TEXCACHE_NONE ? 0 : u32(odd) + 3;
    const u32 image1 = (evenAddress >> 5) | (evenExponent << 15) | (evenExponent << 18);
    const u32 image2 = (oddAddress >> 5) | (oddExponent << 15) | (oddExponent << 18);
    static_assert(sizeof(GXTexRegion) == 16);
    // Match the exact original 16-byte region layout without type punning.
    // Original cached initialization leaves sizeEven/sizeOdd/padding unchanged.
    std::memcpy(&region->dummy[0], &image1, sizeof(image1));
    std::memcpy(&region->dummy[1], &image2, sizeof(image2));
    auto* bytes = reinterpret_cast<u8*>(region);
    bytes[12] = mipmap32;
    bytes[13] = 1;
}
