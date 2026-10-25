#include <bit>
#include <cstddef>
#include "helpers/endian.h"
#include "helpers/offset_ptr.h"
#include "SSystem/SComponent/c_phase.h"
#include "SSystem/SComponent/c_node.h"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/ai.h>
#include <dolphin/card.h>

static_assert(sizeof(void*) == 4);
static_assert(sizeof(long) == 4 && sizeof(size_t) == 4);
static_assert(sizeof(u32) == 4 && sizeof(u64) == 8);
static_assert(BIT_64 == 0);
static_assert(std::endian::native == std::endian::little);
static_assert(TARGET_LITTLE_ENDIAN == 1, "ARM32 must use host-resource byte swapping");
static_assert(sizeof(BE<u32>) == 4 && sizeof(OffsetPtr) == 4);
static_assert(sizeof(node_class) == 12);
static_assert(sizeof(request_of_phase_process_class) == 8);
static_assert(sizeof(BOOL) == 4 && sizeof(GXBool) == 1);
static_assert(sizeof(GXTexObj) == 64 && sizeof(GXTlutObj) == 40);
static_assert(sizeof(OSContext) == 0x2c8);
static_assert(__OSBusClock == OS_BUS_CLOCK && __OSCoreClock == OS_CORE_CLOCK);
static_assert(std::is_same_v<decltype(&AIInitDMA), void (*)(uintptr_t, u32)>);
static_assert(std::is_same_v<decltype(&CARDInit), void (*)(const char*, const char*)>);
static_assert(std::is_same_v<decltype(&GXSetArray), void (*)(GXAttr, const void*, u32, u8, bool)>);
static_assert(be16(0x1234) == 0x3412);
static_assert(be32(0x12345678) == 0x78563412);
static_assert(be64(0x0123456789abcdefULL) == 0xefcdab8967452301ULL);

// A non-constant input ensures target code generation exercises runtime swaps.
u32 dusk_arm32_endian_probe(volatile u32* value) {
    return RES_U32(*value);
}
