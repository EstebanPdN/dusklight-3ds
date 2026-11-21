#include "Adpcm.hpp"

#include <cassert>
#include <fstream>

#include "JSystem/JAudio2/JASAramStream.h"

// https://github.com/magcius/vgmtrans/blob/8e34ddc2fb43948dc1e1a8759c739a0c1c7b62d7/src/main/formats/JaiSeqScanner.cpp#L489-L531
// https://github.com/XAYRGA/JaiSeqX/blob/f29c024ec3663503f506aa02bcd503ada6e7d8aa/JaiSeqXLJA/DSP/JAIDSPADPCM4.cs#L86-L87

static constexpr u16 Coefficient0[] = {
    0,0x0800,0,0x0400,
    0x1000,0x0e00,0x0c00,0x1200,
    0x1068,0x12c0,0x1400,0x0800,
    0x0400,0xfc00,0xfc00,0xf800
};

static constexpr u16 Coefficient1[] = {
    0,0,0x0800,0x0400,0xf800,
    0xfa00,0xfc00,0xf600,0xf738,
    0xf704,0xf400,0xf800,0xfc00,
    0x0400,0,0,
};

constexpr int AdpcmFrameSize = 9;

// One decoded sample: the nibble scaled by 2^(e+11) plus the two-tap
// prediction, >> 11, saturated to 16 bits (ARMv6: SSAT with the shift).
static inline s32 AdpcmSample(s32 nibble, int shift, s32 coef0, s32 coef1, s32 hist1, s32 hist2) {
    const s32 sum = (nibble << shift) + coef0 * hist1 + coef1 * hist2;
#if defined(__arm__) && !defined(__thumb__) && defined(__ARM_ARCH) && __ARM_ARCH >= 6
    s32 value;
    __asm__("ssat %0, #16, %1, asr #11" : "=r"(value) : "r"(sum));
    return value;
#else
    const s32 value = sum >> 11;
    return value < -0x8000 ? -0x8000 : value > 0x7FFF ? 0x7FFF : value;
#endif
}

// The same samples as the original loop (one sample at a time through the
// caller's history references, a branch per nibble): the histories stay in
// registers and each byte gives its two nibbles, high first.
void dusk::audio::Adpcm4ToPcm16(const u8* adpcm, size_t adpcmLength, s16* pcm, size_t pcmLength, s16& hist2, s16& hist1) {
    assert (adpcmLength % AdpcmFrameSize == 0 && "ADPCM must be divisible by frame size");

    s32 h1 = hist1, h2 = hist2;
    size_t left = pcmLength;

    for (size_t i = 0; i < adpcmLength && left != 0; i += AdpcmFrameSize) {
        const u8 header = adpcm[i];
        const int shift = (header >> 4) + 11;
        const u8 coefIndex = header & 0xF;
        const s32 coef0 = (s16)Coefficient0[coefIndex];
        const s32 coef1 = (s16)Coefficient1[coefIndex];
        const u8* bytes = adpcm + i + 1;

        const size_t count = left < 16 ? left : 16;
        left -= count;
        const size_t pairs = count / 2;
        for (size_t b = 0; b < pairs; b++) {
            const u32 byte = bytes[b];
            const s32 first = AdpcmSample(static_cast<s32>(byte << 24) >> 28, shift, coef0, coef1, h1, h2);
            const s32 second = AdpcmSample(static_cast<s32>(byte << 28) >> 28, shift, coef0, coef1, first, h1);
            pcm[0] = static_cast<s16>(first);
            pcm[1] = static_cast<s16>(second);
            pcm += 2;
            h2 = first;
            h1 = second;
        }
        if (count & 1) {
            const u32 byte = bytes[pairs];
            const s32 sample = AdpcmSample(static_cast<s32>(byte << 24) >> 28, shift, coef0, coef1, h1, h2);
            *pcm++ = static_cast<s16>(sample);
            h2 = h1;
            h1 = sample;
        }
    }

    hist2 = static_cast<s16>(h2);
    hist1 = static_cast<s16>(h1);
}
