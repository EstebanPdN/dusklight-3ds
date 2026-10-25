#include "../runtime/texture_ia8.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    using namespace dusk::native;
    // 9x9 covers horizontal/vertical GX and PICA boundaries plus both paddings.
    std::array<std::uint8_t, 32 + 3 * 3 * 32> input{};
    input[0] = 3; input[3] = 9; input[5] = 9; input[24] = 1; input[31] = 32;
    for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 9; ++x) {
        const auto at = 32 + ((y / 4) * 3 + x / 4) * 32 + (y % 4 * 4 + x % 4) * 2;
        input[at] = 100 + x; input[at + 1] = 10 * y + x;
    }
    TextureExtent e;
    assert(inspectBtiIa8(input, e));
    assert(e.width == 9 && e.height == 9 && e.paddedWidth == 16 && e.paddedHeight == 16);
    std::array<std::uint8_t, 16 * 16 * 4 + 4> out{};
    out.fill(0xee);
    assert(decodeBtiIa8(input, e, out));
    // Independent bit-mask deinterleave checks every texel, including padding.
    for (unsigned tile = 0; tile < 4; ++tile) for (unsigned m = 0; m < 64; ++m) {
        const unsigned x = (tile % 2) * 8 + (m & 1) + ((m >> 1) & 2) + ((m >> 2) & 4);
        const unsigned y = (tile / 2) * 8 + ((m >> 1) & 1) + ((m >> 2) & 2) + ((m >> 3) & 4);
        const auto at = (tile * 64 + m) * 4;
        const bool logical = x < 9 && y < 9;
        assert(out[at] == (logical ? 100 + x : 0));
        for (unsigned c = 1; c < 4; ++c) assert(out[at + c] == (logical ? 10 * y + x : 0));
    }
    assert(out[e.outputBytes] == 0xee);
    assert(!decodeBtiIa8(input, e, std::span(out).first(e.outputBytes - 1)));
    std::copy(input.begin(), input.end(), out.begin());
    assert(!decodeBtiIa8(std::span(out).first(input.size()), e, out));
    auto wrong = e; ++wrong.width; assert(!decodeBtiIa8(input, wrong, out));
    assert(!inspectBtiIa8(std::span(input).first(input.size() - 1), wrong));
    input[0] = 2; assert(!inspectBtiIa8(input, wrong)); input[0] = 3;
    input[24] = 2; assert(!inspectBtiIa8(input, wrong)); input[24] = 1;
    input[28] = 0xff; assert(!inspectBtiIa8(input, wrong)); input[28] = 0;
    input[2] = input[3] = 0; assert(!inspectBtiIa8(input, wrong));
    std::puts("PASS IA8 alpha/intensity, 4x4->8x8 tiling, edges/padding and bounded refusals");
}
