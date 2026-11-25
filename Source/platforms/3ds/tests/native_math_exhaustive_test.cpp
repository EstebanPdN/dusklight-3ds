// Exhaustive host proof of the logic.txt "math" rule (dolphin/ppc_math.h,
// SSystem/SComponent/c_math.cpp):
//
// 1. dusk_frsqrte_f32_fast(x) == frsqrte((double)x), bit for bit, for every
//    positive normal float (2,130,706,432 inputs); RSQRTE_TABLE24 is
//    RSQRTE_TABLE >> 24 with nothing lost. Pure integer work on both sides,
//    so the host result is the device result.
// 2. fabsf(x - (float)(int)x) == fabsf(fmodf(x, 1.0f)) for every float with
//    |x| < 2^31 (the random generators' sums are under 3). fmodf is exact by
//    definition, so any conforming libm (host or newlib) gives the same bits.
// 3. cM_rnd's three generators run 200 million steps from many seeds with
//    either fraction: the same sequence.
// The fmaf rule needs no search: dusk_fmaf_device is the body of the device's
// newlib fmaf (vcvt.f64.f32 x3, vmla.f64, vcvt.f32.f64; VFPv2 multiply-add is
// not fused). The device verify mode (logic.txt math=2) compares all three
// against the device's own libm on live inputs.
#include <dolphin/ppc_math.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

uint64_t bitsOf(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}
uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float floatOf(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void testTable() {
    for (int i = 0; i < 32; i++) {
        assert((RSQRTE_TABLE[i].base & 0xffffffull) == 0);
        assert((RSQRTE_TABLE[i].dec & 0xffffffll) == 0);
        assert((int64_t)RSQRTE_TABLE24[i][0] == (int64_t)(RSQRTE_TABLE[i].base >> 24));
        assert((int64_t)RSQRTE_TABLE24[i][1] == (RSQRTE_TABLE[i].dec >> 24));
    }
    std::printf("RSQRTE_TABLE24 == RSQRTE_TABLE >> 24, all 32 entries exact\n");
}

void testFrsqrte() {
    uint64_t checked = 0;
    for (uint32_t exponent = 1; exponent <= 254; exponent++) {
        for (uint32_t mantissa = 0; mantissa < (1u << 23); mantissa++) {
            const float x = floatOf((exponent << 23) | mantissa);
            const uint64_t fast = bitsOf(dusk_frsqrte_f32_fast(x));
            const uint64_t original = bitsOf(frsqrte((double)x));
            if (fast != original) {
                std::printf("frsqrte mismatch x=%08x fast=%016llx original=%016llx\n", bitsOf(x),
                            (unsigned long long)fast, (unsigned long long)original);
                assert(false);
            }
            checked++;
        }
    }
    std::printf("frsqrte: %llu positive normal floats, fast == original bit for bit\n",
                (unsigned long long)checked);
}

void testFraction() {
    uint64_t checked = 0;
    for (uint32_t sign = 0; sign < 2; sign++) {
        // Exponent field 0..157: |x| < 2^31 (and every subnormal and zero).
        for (uint32_t exponent = 0; exponent <= 157; exponent++) {
            for (uint32_t mantissa = 0; mantissa < (1u << 23); mantissa++) {
                const float x = floatOf((sign << 31) | (exponent << 23) | mantissa);
                const float fast = std::fabs(x - (float)(int32_t)x);
                const float original = std::fabs(fmodf(x, 1.0f));
                if (bitsOf(fast) != bitsOf(original)) {
                    std::printf("fraction mismatch x=%08x fast=%08x original=%08x\n", bitsOf(x),
                                bitsOf(fast), bitsOf(original));
                    assert(false);
                }
                checked++;
            }
        }
    }
    std::printf("random fraction: %llu floats with |x| < 2^31, x - trunc(x) == fmodf(x, 1) after fabsf\n",
                (unsigned long long)checked);
}

void testGenerator() {
    uint64_t steps = 0;
    const int32_t seeds[][3] = {{100, 100, 100}, {1, 2, 3}, {30268, 30306, 30322}, {12345, 2345, 345},
                                {-77, 901, 30000}, {0, 0, 1}};
    for (const auto& seed : seeds) {
        int32_t a0 = seed[0], a1 = seed[1], a2 = seed[2];
        int32_t b0 = a0, b1 = a1, b2 = a2;
        for (int i = 0; i < 33333334; i++) {
            a0 = (a0 * 171) % 30269;
            a1 = (a1 * 172) % 30307;
            a2 = (a2 * 170) % 30323;
            const float sa = a0 / 30269.0f + a1 / 30307.0f + a2 / 30323.0f;
            const float ra = std::fabs(fmodf(sa, 1.0f));
            b0 = (b0 * 171) % 30269;
            b1 = (b1 * 172) % 30307;
            b2 = (b2 * 170) % 30323;
            const float sb = b0 / 30269.0f + b1 / 30307.0f + b2 / 30323.0f;
            const float rb = std::fabs(sb - (float)(int32_t)sb);
            assert(bitsOf(ra) == bitsOf(rb));
            steps++;
        }
    }
    std::printf("cM_rnd: %llu steps from 6 seeds, the same sequence\n", (unsigned long long)steps);
}

}  // namespace

int main() {
    testTable();
    testFraction();
    testGenerator();
    testFrsqrte();
    std::printf("native math exhaustive tests passed\n");
    return 0;
}
