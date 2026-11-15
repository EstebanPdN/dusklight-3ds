#pragma once
#include <cstdint>
namespace dusk::native {
// Bumped by every operation that can change PICA fixed-function state, the
// bound program, bound textures or their contents outside the chained native
// shape replay. The replay compares it to prove that the state it applied for
// the previous shape is still in effect.
inline std::uint32_t gxPicaStateSerial{};
}
