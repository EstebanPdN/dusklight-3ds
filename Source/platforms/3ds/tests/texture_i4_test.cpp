#include "../runtime/texture_i4.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    using namespace dusk::native;
    // Odd dimensions cross both GX/PICA 8x8 tile axes and require two paddings.
    std::array<std::uint8_t, 32 + 4 * 32> input{};
    input[3] = 9; input[5] = 9; input[24] = 1; input[31] = 32;
    for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 9; ++x) {
        const auto at = 32 + ((y / 8) * 2 + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2;
        const unsigned nibble = (x + 3 * y) % 16;
        input[at] |= nibble << ((x & 1) ? 0 : 4);
    }
    TextureExtent e;
    assert(inspectBtiI4(input, e));
    assert(e.width == 9 && e.height == 9 && e.paddedWidth == 16 && e.paddedHeight == 16);
    assert(e.outputBytes == 16 * 16 * 4 && e.dataOffset == 32);
    std::array<std::uint8_t, 16 * 16 * 4 + 8> guarded;
    guarded.fill(0xee);
    auto output = std::span(guarded).subspan(4, e.outputBytes);
    assert(decodeBtiI4(input, e, output));
    // Independent deinterleave visits every GPU texel, including transparent padding.
    for (unsigned tile = 0; tile < 4; ++tile) for (unsigned m = 0; m < 64; ++m) {
        const unsigned x = (tile % 2) * 8 + (m & 1) + ((m >> 1) & 2) + ((m >> 2) & 4);
        const unsigned y = (tile / 2) * 8 + ((m >> 1) & 1) + ((m >> 2) & 2) + ((m >> 3) & 4);
        const auto expected = x < 9 && y < 9 ? ((x + 3 * y) % 16) * 17 : 0;
        for (unsigned c = 0; c < 4; ++c) assert(output[(tile * 64 + m) * 4 + c] == expected);
    }
    for (unsigned i = 0; i < 4; ++i) {
        assert(guarded[i] == 0xee && guarded[guarded.size() - 1 - i] == 0xee);
    }
    const auto decoded = guarded;
    // alphaEnabled is draw/material metadata: it must not force I4 alpha opaque.
    assert(output[0] == 0 && output[4] == 17);
    input[1] = 2;
    assert(decodeBtiI4(input, e, output) && guarded == decoded);
    input[1] = 0;
    assert(!decodeBtiI4(input, e, output.first(e.outputBytes - 1)) && guarded == decoded);
    auto wrong = e; ++wrong.outputBytes;
    assert(!decodeBtiI4(input, wrong, output) && guarded == decoded);
    // All missing header/data tails must be refused without touching output.
    for (std::size_t prefix = 0; prefix < input.size(); ++prefix) {
        assert(!inspectBtiI4(std::span(input).first(prefix), wrong));
        assert(wrong.outputBytes == 0 && wrong.width == 0);
        assert(!decodeBtiI4(std::span(input).first(prefix), e, output) && guarded == decoded);
    }
    // Input/output equality and partial overlap in both directions.
    for (unsigned inputOffset : {0u, 4u, 8u}) {
        guarded.fill(0xee);
        std::copy(input.begin(), input.end(), guarded.begin() + inputOffset);
        auto original = guarded;
        assert(!decodeBtiI4(std::span(guarded).subspan(inputOffset, input.size()), e, output));
        assert(guarded == original);
    }
    auto reject = [&](std::size_t at, std::uint8_t value) {
        auto changed = input; changed[at] = value;
        assert(!inspectBtiI4(changed, wrong));
        assert(!decodeBtiI4(changed, e, output));
    };
    reject(0, 3); reject(8, 1); reject(11, 1); reject(24, 0); reject(24, 2);
    reject(31, 31); reject(28, 0xff); reject(3, 0); reject(5, 0);
    reject(2, 4); reject(4, 4); // 1033 exceeds the 1024-dimensional cap.
    // Smallest logical image still allocates one 8x8 GPU tile; high nibble wins.
    input[3] = input[5] = 1; input[32] = 0xa3;
    assert(inspectBtiI4(input, e) && e.outputBytes == 256);
    std::array<std::uint8_t, 256> tiny;
    tiny.fill(0xee);
    assert(decodeBtiI4(input, e, tiny));
    for (unsigned c = 0; c < 4; ++c) assert(tiny[c] == 170);
    assert(std::all_of(tiny.begin() + 4, tiny.end(), [](auto v) { return v == 0; }));
    std::puts("PASS I4 nibble order, RGBA intensity/alpha, both tile axes, padding, canaries, alias and bounded refusals");
}
