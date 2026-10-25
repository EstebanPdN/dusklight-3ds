#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include "helpers/endian.h"

static_assert(std::endian::native == std::endian::little);
static_assert(TARGET_LITTLE_ENDIAN == 1);
static_assert(sizeof(BE<u16>) == 2 && sizeof(BE<u32>) == 4 && sizeof(BE<u64>) == 8);
static_assert(BE<u32>(0x12345678).host() == 0x12345678);
static_assert(be16(0x1234) == 0x3412);
static_assert(be32(0x12345678) == 0x78563412);
static_assert(be64(0x0123456789abcdefULL) == 0xefcdab8967452301ULL);

template<class T, size_t N>
T read_synthetic(const std::array<u8, N>& bytes) {
    static_assert(sizeof(T) == N);
    T out;
    std::memcpy(&out, bytes.data(), N);
    return out;
}

int main() {
    // Deliberately synthetic, including sign and IEEE float bits. No game data.
    auto word = read_synthetic<BE<u32>>(std::array<u8, 4>{0x12, 0x34, 0x56, 0x78});
    assert(word.host() == 0x12345678);
    auto signedWord = read_synthetic<BE<s32>>(std::array<u8, 4>{0xff, 0xff, 0xff, 0xfe});
    assert(signedWord.host() == -2);
    auto floating = read_synthetic<BE<f32>>(std::array<u8, 4>{0x3f, 0x80, 0, 0});
    assert(floating.host() == 1.0f);
    auto wide = read_synthetic<BE<u64>>(std::array<u8, 8>{1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef});
    assert(wide.host() == 0x0123456789abcdefULL);
    volatile u32 input = 0x12345678;
    assert(RES_U32(input) == 0x78563412);
    volatile u64 large = 0x0123456789abcdefULL;
    assert(RES_U64(large) == 0xefcdab8967452301ULL);
    BE<Vec> vector{1.0f, -2.0f, 3.0f};
    Vec native = vector;
    assert(native.x == 1.0f && native.y == -2.0f && native.z == 3.0f);
    BE<u32> counter{0x100};
    counter += 2;
    assert(counter.host() == 0x102);
    return 0;
}
