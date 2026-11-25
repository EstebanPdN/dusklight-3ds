// Host checks of the 3DS software mixer (src/dusk/audio/DuskDsp.cpp, Adpcm.cpp),
// compiled from the game's sources. Synthetic wave data and voices only.
//   equivalence: the ADPCM decoder against the Alpha 13 loop (equal samples);
//     the fixed-point mix against the float path from the same state, every
//     subframe: equal channel table and voice state after it, output within
//     1/32768, 16-bit samples at most one step apart.
//   benchmark: host time of the Alpha 13 mixer (48 kHz float) and the 3DS
//     mixer (32 kHz fixed point) for the voices of the console's ranch
//     minigame dump (Alpha 13 dump 018: 28 ADPCM voices, 10 silent, 2 with a
//     biquad), per second of audio. A host CPU, not the ARM11: a ratio only.
#include "dusk/audio/DuskDsp.hpp"
#include "dusk/audio/Adpcm.hpp"
#include "dusk/audio/DuskAudioSystem.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace dusk::audio;

extern "C" void OSReport(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}
extern "C" void OSPanic(const char* file, int line, const char* fmt, ...) {
    std::fprintf(stderr, "OSPanic %s:%d: ", file, line);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::abort();
}
static std::vector<u8> aram;
extern "C" void* ARGetStorageAddress() { return aram.data(); }
JASDsp::TChannel* JASDsp::CH_BUF = nullptr;
// audio_ndsp.cpp: the DSP level is JASDriver::MAX_MIXERLEVEL (0x2EE0).
f32 dusk::audio::VolumeFromU16(u16 value) { return static_cast<f32>(value) / 12000.0f; }

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

// The Alpha 13 decoder loop, as it was.
static const u16 kCoefficient0[] = {0, 0x0800, 0, 0x0400, 0x1000, 0x0e00, 0x0c00, 0x1200,
                                    0x1068, 0x12c0, 0x1400, 0x0800, 0x0400, 0xfc00, 0xfc00, 0xf800};
static const u16 kCoefficient1[] = {0, 0, 0x0800, 0x0400, 0xf800, 0xfa00, 0xfc00, 0xf600,
                                    0xf738, 0xf704, 0xf400, 0xf800, 0xfc00, 0x0400, 0, 0};
static s16 ReferenceClamp16(s32 value) {
    if (value > 0x7FFF) return 0x7FFF;
    if (value < -0x8000) return -0x8000;
    return value;
}
static void ReferenceAdpcm(const u8* adpcm, size_t adpcmLength, s16* pcm, size_t pcmLength, s16& hist2, s16& hist1) {
    auto endPtr = pcm + pcmLength;
    for (size_t i = 0; i < adpcmLength; i += 9) {
        u8 header = adpcm[i];
        s32 scale = 1 << (header >> 4);
        u8 coefIndex = header & 0xF;
        s16 coef0 = (s16)kCoefficient0[coefIndex];
        s16 coef1 = (s16)kCoefficient1[coefIndex];
        for (int sampleIdx = 0; sampleIdx < 16; sampleIdx++) {
            u8 adpcmValue = adpcm[i + 1 + sampleIdx / 2];
            u8 unsignedNibble = sampleIdx % 2 == 0 ? adpcmValue >> 4 : adpcmValue & 0xF;
            s8 signedNibble = ((s8)(unsignedNibble << 4)) >> 4;
            s16 sample = ReferenceClamp16((((signedNibble * scale) << 11) + (coef0 * hist1 + coef1 * hist2)) >> 11);
            hist2 = hist1;
            hist1 = sample;
            *pcm++ = sample;
            if (endPtr == pcm) return;
        }
    }
}

static void CheckDecoder(std::mt19937& random) {
    std::vector<u8> frames(9 * 64);
    std::vector<s16> expected(16 * 64 + 1), actual(16 * 64 + 1);
    for (int round = 0; round < 20000; round++) {
        for (auto& byte : frames) byte = static_cast<u8>(random());
        const size_t frameCount = 1 + random() % 64;
        const size_t samples = 1 + random() % (frameCount * 16);
        s16 h2a = static_cast<s16>(random()), h1a = static_cast<s16>(random());
        s16 h2b = h2a, h1b = h1a;
        expected.back() = actual.back() = 0x5A5A;
        ReferenceAdpcm(frames.data(), frameCount * 9, expected.data(), samples, h2a, h1a);
        Adpcm4ToPcm16(frames.data(), frameCount * 9, actual.data(), samples, h2b, h1b);
        CHECK(std::memcmp(expected.data(), actual.data(), samples * sizeof(s16)) == 0);
        CHECK(h2a == h2b && h1a == h1b);
        CHECK(actual.back() == 0x5A5A);
    }
    std::puts("PASS: ADPCM decoder: 20000 random runs, equal samples and histories (any length, odd tails)");
}

// ---- Voices ----

static constexpr u32 kWaveBytes = 1 << 20;

struct VoiceSetup {
    bool adpcm = true;
    u32 pitch = 4096;
    bool loop = true;
    u32 endSample = 30000;
    bool autoMixer = true;
    u16 volume = 6000, initVolume = 6000, pan = 64;
    u16 busLeft = 0, busRight = 0;
    bool biquad = false;
    int oscillator = -1;
};

static void StartVoice(int index, const VoiceSetup& setup, std::mt19937& random) {
    auto& voice = JASDsp::CH_BUF[index];
    std::memset(static_cast<void*>(&voice), 0, sizeof(voice));
    voice.mIsActive = 1;
    voice.mResetFlag = 1;
    voice.mPitch = static_cast<u16>(setup.pitch);
    if (setup.oscillator >= 0) {
        voice.mBytesPerBlock = static_cast<u16>(setup.oscillator);
        voice.mWaveAramAddress = 0;
    } else {
        voice.mSamplesPerBlock = setup.adpcm ? 16 : 1;
        voice.mBytesPerBlock = setup.adpcm ? 9 : 16;
        const u32 bytes = setup.adpcm ? (setup.endSample / 16 + 1) * 9 : setup.endSample * 2;
        voice.mWaveAramAddress = 64 + (random() % (kWaveBytes - bytes - 128)) / 32 * 32;
    }
    voice.mLoopFlag = setup.loop ? 0xFF : 0;
    voice.mEndSample = setup.endSample;
    voice.mLoopStartSample = setup.loop ? (random() % (setup.endSample / 2)) / 16 * 16 + (random() % 3 == 0 ? 5 : 0) : 0;
    voice.mpLast = static_cast<s16>(random());
    voice.mpPenult = static_cast<s16>(random());
    voice.mSamplePosition = random() % 4 == 0 ? (random() % 2000) : 0;
    voice.mAramBaseAddress = nullptr;
    voice.mAutoMixerBeenSet = setup.autoMixer ? 1 : 0;
    voice.mAutoMixerVolume = setup.volume;
    voice.mAutoMixerInitVolume = setup.initVolume;
    voice.mAutoMixerPanDolby = static_cast<u16>(setup.pan << 8);
    voice.mOutputChannels[0] = {0x0D00, setup.busLeft, setup.busLeft, 0};
    voice.mOutputChannels[1] = {0x0D60, setup.busRight, setup.busRight, 0};
    if (setup.biquad) {
        voice.mFilterMode = 0x20;
        // A gentle low-pass: b1 0.3, b2 0.2, a1 0.4, a2 -0.1 (stable).
        const s16 iir[4] = {9830, 6554, 13107, -3277};
        std::memcpy(voice.iir_filter_params, iir, sizeof(iir));
    }
}

static VoiceSetup RandomSetup(std::mt19937& random) {
    static const u32 pitches[] = {4096, 2730, 1716, 1881, 2112, 3247, 6000, 8192, 12000, 30000, 0x7FFF, 1, 100, 4095, 4097};
    VoiceSetup setup;
    setup.adpcm = random() % 5 != 0;
    setup.pitch = pitches[random() % (sizeof(pitches) / sizeof(pitches[0]))];
    setup.loop = random() % 3 != 0;
    setup.endSample = 200 + random() % 40000;
    setup.autoMixer = random() % 2 == 0;
    setup.volume = random() % 4 == 0 ? 0 : static_cast<u16>(random() % 12001);
    setup.initVolume = random() % 3 == 0 ? setup.volume : static_cast<u16>(random() % 12001);
    setup.pan = static_cast<u16>(random() % 128);
    setup.busLeft = random() % 4 == 0 ? 0 : static_cast<u16>(random() % 12001);
    setup.busRight = random() % 4 == 0 ? 0 : static_cast<u16>(random() % 12001);
    setup.biquad = random() % 8 == 0;
    if (random() % 16 == 0) {
        // Oscillators take the float path in both mixers. Type 11 (evolving
        // harmonic) reads a table each render advances, which only the
        // mixer's own verify mode saves; it is left to that check.
        static const int types[] = {0, 1, 3, 4, 7, 12};
        setup.oscillator = types[random() % 6];
    }
    return setup;
}

// What JAS changes between subframes: volumes, pan, pitch, pause, starts and stops.
static void ChangeVoices(std::mt19937& random) {
    for (int i = 0; i < DSP_CHANNELS; i++) {
        auto& voice = JASDsp::CH_BUF[i];
        const u32 roll = random() % 1000;
        if (!voice.mIsActive) {
            if (roll < 20) StartVoice(i, RandomSetup(random), random);
            continue;
        }
        if (roll < 5) { voice.mIsActive = 0; continue; }
        if (roll < 8) { voice.mForcedStop = 1; continue; }
        if (roll < 12) { voice.mPauseFlag = voice.mPauseFlag ? 0 : 1; continue; }
        if (roll < 15) { voice.mResetFlag = 1; continue; }
        if (roll < 300) {
            voice.mAutoMixerVolume = random() % 5 == 0 ? 0 : static_cast<u16>(random() % 12001);
            voice.mOutputChannels[0].mTargetVolume = random() % 5 == 0 ? 0 : static_cast<u16>(random() % 12001);
            voice.mOutputChannels[1].mTargetVolume = random() % 5 == 0 ? 0 : static_cast<u16>(random() % 12001);
        }
        if (roll < 100) voice.mAutoMixerPanDolby = static_cast<u16>((random() % 128) << 8);
        if (roll < 60 && voice.mWaveAramAddress != 0) {
            const int bend = static_cast<int>(voice.mPitch) + static_cast<int>(random() % 401) - 200;
            voice.mPitch = static_cast<u16>(std::clamp(bend, 1, 0x7FFF));
        }
    }
}

struct MixerSnapshot {
    std::vector<JASDsp::TChannel> voices;
    std::vector<ChannelAuxData> aux;
    f32 prevMaster;
};
static void Save(MixerSnapshot& snapshot) {
    snapshot.voices.assign(JASDsp::CH_BUF, JASDsp::CH_BUF + DSP_CHANNELS);
    snapshot.aux.assign(ChannelAux, ChannelAux + DSP_CHANNELS);
    snapshot.prevMaster = PrevMasterVolume;
}
static void Restore(const MixerSnapshot& snapshot) {
    std::memcpy(static_cast<void*>(JASDsp::CH_BUF), snapshot.voices.data(), sizeof(JASDsp::TChannel) * DSP_CHANNELS);
    std::memcpy(static_cast<void*>(ChannelAux), snapshot.aux.data(), sizeof(ChannelAuxData) * DSP_CHANNELS);
    PrevMasterVolume = snapshot.prevMaster;
}
// The voice state both paths must leave the same: the channel table and every
// aux field (the decode buffer up to its count).
static bool SameState(const MixerSnapshot& a, const MixerSnapshot& b, std::string& where) {
    for (int i = 0; i < DSP_CHANNELS; i++) {
        if (std::memcmp(&a.voices[i], &b.voices[i], sizeof(JASDsp::TChannel)) != 0) {
            where = "channel table " + std::to_string(i);
            return false;
        }
        const auto& x = a.aux[i];
        const auto& y = b.aux[i];
        const size_t head = offsetof(ChannelAuxData, decodeBuf), tail = offsetof(ChannelAuxData, decodeBufCount);
        if (std::memcmp(&x, &y, head) != 0 ||
            std::memcmp(reinterpret_cast<const u8*>(&x) + tail, reinterpret_cast<const u8*>(&y) + tail, sizeof(ChannelAuxData) - tail) != 0 ||
            std::memcmp(x.decodeBuf, y.decodeBuf, std::clamp(x.decodeBufCount, 0, ChannelAuxData::DECODE_BUF_SIZE) * sizeof(s16)) != 0) {
            where = "voice state " + std::to_string(i);
            return false;
        }
    }
    return true;
}

static s16 Pcm16(f32 value) {
    return static_cast<s16>(std::clamp(value, -1.0f, 32767.0f / 32768) * 32768);
}

static void Fill(std::mt19937& random) {
    aram.resize(kWaveBytes);
    // ADPCM frames with the scales and predictors of real data (shift 0..11);
    // the same bytes read as big-endian PCM16 by the PCM voices.
    for (u32 at = 0; at < kWaveBytes; at++) {
        aram[at] = static_cast<u8>(random());
        if (at % 9 == 0) aram[at] = static_cast<u8>(((random() % 12) << 4) | (random() % 16));
    }
}

static void CheckMix(std::mt19937& random, int rate, int subframes) {
    RenderRate = rate;
    std::vector<JASDsp::TChannel> table(DSP_CHANNELS);
    JASDsp::CH_BUF = table.data();
    std::memset(static_cast<void*>(ChannelAux), 0, sizeof(ChannelAuxData) * DSP_CHANNELS);
    for (int i = 0; i < 40; i++) StartVoice(i, RandomSetup(random), random);
    MasterVolume = PrevMasterVolume = 1.0f;

    MixerSnapshot before, floatState, fixedState;
    f32 largest = 0;
    u64 samples = 0, overStep = 0, pcmDiffer = 0, silentAdvances = 0, mixedVoices = 0;
    int pcmLargest = 0;
    for (int frame = 0; frame < subframes; frame++) {
        ChangeVoices(random);
        if (frame % 97 == 0) MasterVolume = (random() % 101) / 100.0f;
        for (int i = 0; i < DSP_CHANNELS; i++) {
            const auto& voice = JASDsp::CH_BUF[i];
            if (voice.mIsActive && !voice.mPauseFlag && !voice.mForcedStop && voice.mWaveAramAddress) {
                const bool silent = voice.mAutoMixerBeenSet ? voice.mAutoMixerVolume == 0 && voice.mAutoMixerInitVolume == 0
                                                            : voice.mOutputChannels[0].mTargetVolume == 0 && voice.mOutputChannels[1].mTargetVolume == 0;
                (silent ? silentAdvances : mixedVoices)++;
            }
        }
        Save(before);
        OutputSubframe floatOut{}, fixedOut{};
        DspFixedMix = false;
        DspRender(floatOut);
        Save(floatState);
        Restore(before);
        DspFixedMix = true;
        DspRender(fixedOut);
        Save(fixedState);
        std::string where;
        if (!SameState(floatState, fixedState, where)) {
            std::fprintf(stderr, "FAIL: %d Hz subframe %d: %s differs after the float and fixed renders\n", rate, frame, where.c_str());
            std::exit(1);
        }
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
                const f32 a = floatOut.channels[c][i], b = fixedOut.channels[c][i];
                CHECK(std::isfinite(b));
                const f32 steps = std::fabs(a - b) * 32768.0f;
                largest = std::max(largest, steps);
                overStep += steps > 1.0f;
                const int difference = std::abs(Pcm16(a) - Pcm16(b));
                pcmDiffer += difference != 0;
                pcmLargest = std::max(pcmLargest, difference);
                samples++;
            }
        }
    }
    // The mixer's own verify mode (mixer-verify.txt) from the same state as a
    // plain fixed render: the same samples and state, and a bounded report.
    DspVerifyLargestStep = 0;
    DspVerifySamplesOverStep = 0;
    DspVerifiedSubframes = DspVerifyMismatches = 0;
    for (int frame = 0; frame < 300; frame++) {
        ChangeVoices(random);
        Save(before);
        OutputSubframe plain{}, verified{};
        DspFixedMix = true;
        DspVerifyMix = false;
        DspRender(plain);
        Save(fixedState);
        Restore(before);
        DspVerifyMix = true;
        DspRender(verified);
        DspVerifyMix = false;
        Save(floatState);
        std::string where;
        CHECK(SameState(fixedState, floatState, where));
        CHECK(std::memcmp(&plain, &verified, sizeof(plain)) == 0);
    }
    CHECK(DspVerifiedSubframes == 300 && DspVerifyLargestStep <= 1.0f && DspVerifySamplesOverStep == 0);
    std::printf("PASS: %d Hz verify mode: 300 subframes equal to the plain fixed render; it reports %llu differing, largest %.5f/32768\n",
                rate, static_cast<unsigned long long>(DspVerifyMismatches), DspVerifyLargestStep);
    std::printf("PASS: %d Hz, %d subframes (%llu voice renders, about %llu silent): equal voice state; output "
                "largest difference %.5f/32768, %llu of %llu samples over 1/32768; 16-bit samples: %llu differ, by at most %d\n",
                rate, subframes, static_cast<unsigned long long>(mixedVoices + silentAdvances),
                static_cast<unsigned long long>(silentAdvances), largest, static_cast<unsigned long long>(overStep),
                static_cast<unsigned long long>(samples), static_cast<unsigned long long>(pcmDiffer), pcmLargest);
    CHECK(largest <= 1.0f && overStep == 0 && pcmLargest <= 1);
    // Rounding at a truncation boundary only: a value a few thousandths of a
    // step from an integer may land on the other side (loud 40-voice mixes).
    CHECK(pcmDiffer * 100 <= samples);
}

// The minigame's voices (dump 018): pitch at 48 kHz, volumes (0 = silent), biquad.
struct DumpVoice { u16 pitch48; u16 left, right; bool biquad; };
static const DumpVoice kMinigame[] = {
    {1716, 0, 0, false},    {1881, 70, 243, false},  {1676, 422, 1449, false}, {1259, 0, 0, false},
    {2112, 21, 72, false},  {1339, 700, 760, true},  {1071, 540, 480, true},   {2237, 30, 31, false},
    {3247, 0, 0, false},    {1881, 11, 40, false},   {3065, 0, 0, false},      {2112, 597, 190, false},
    {2432, 0, 0, false},    {2112, 125, 428, false}, {2432, 0, 0, false},      {1881, 70, 243, false},
    {2825, 2949, 763, false}, {2577, 0, 0, false},   {3065, 0, 0, false},      {2037, 46, 0, false},
    {2719, 46, 0, false},   {2112, 15, 13, false},   {1676, 422, 1449, false}, {1414, 0, 0, false},
    {1757, 0, 0, false},    {1881, 1805, 1537, false}, {2730, 3749, 969, false}, {2730, 483, 857, false},
};

static double TimeMixer(std::mt19937& random, int rate, bool fixed, int subframes) {
    RenderRate = rate;
    DspFixedMix = fixed;
    std::vector<JASDsp::TChannel> table(DSP_CHANNELS);
    JASDsp::CH_BUF = table.data();
    std::memset(static_cast<void*>(ChannelAux), 0, sizeof(ChannelAuxData) * DSP_CHANNELS);
    int index = 0;
    for (const auto& dump : kMinigame) {
        VoiceSetup setup;
        setup.pitch = rate == 48000 ? dump.pitch48 : static_cast<u32>(dump.pitch48 * 1.5f);
        setup.loop = true;
        setup.endSample = 20000;
        setup.autoMixer = false;
        setup.busLeft = dump.left;
        setup.busRight = dump.right;
        setup.biquad = dump.biquad;
        StartVoice(index++, setup, random);
    }
    MasterVolume = PrevMasterVolume = 1.0f;
    volatile f32 sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < subframes; frame++) {
        // A volume ramp on a third of the voices, as the envelopes give.
        for (int i = frame % 3; i < index; i += 3) {
            auto& out = JASDsp::CH_BUF[i].mOutputChannels;
            if (out[0].mTargetVolume) out[0].mTargetVolume = static_cast<u16>(out[0].mTargetVolume ^ 1);
        }
        OutputSubframe out{};
        DspRender(out);
        sink = sink + out.channels[0][frame % DSP_SUBFRAME_SIZE];
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    (void)sink;
    return seconds / subframes;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "equivalence";
    std::mt19937 random(0x5EED);
    Fill(random);
    OutChannelCount = 2;
    EnableReverb = false;
    EnableHrtf = false;
    DumpAudio = false;
    DspInit();
    if (mode == "equivalence") {
        CheckDecoder(random);
        CheckMix(random, 32000, 6000);
        CheckMix(random, 48000, 3000);
        return 0;
    }
    if (mode == "benchmark") {
        const int subframes = 40000;
        TimeMixer(random, 48000, false, 2000);  // warm-up
        const double old48 = TimeMixer(random, 48000, false, subframes);
        const double float32 = TimeMixer(random, 32000, false, subframes);
        const double fixed48 = TimeMixer(random, 48000, true, subframes);
        const double fixed32 = TimeMixer(random, 32000, true, subframes);
        // Per second of audio: 600 subframes at 48 kHz, 400 at 32 kHz.
        std::printf("BENCHMARK (host CPU, 28 voices of the minigame dump) per second of audio: "
                    "Alpha 13 float 48 kHz %.2f ms, float 32 kHz %.2f ms, fixed 48 kHz %.2f ms, fixed 32 kHz %.2f ms; "
                    "fixed 32 kHz / Alpha 13 = %.3f\n",
                    old48 * 600 * 1000, float32 * 400 * 1000, fixed48 * 600 * 1000, fixed32 * 400 * 1000,
                    (fixed32 * 400) / (old48 * 600));
        return 0;
    }
    std::fprintf(stderr, "usage: audio_mixer_test [equivalence|benchmark]\n");
    return 2;
}
