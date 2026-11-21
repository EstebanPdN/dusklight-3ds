#include "DuskDsp.hpp"

#include "Adpcm.hpp"
#include "DuskAudioSystem.h"
#include "global.h"

#if !defined(DUSK_TARGET_3DS)
#include "dusk/logging.h"
#endif
#include "helpers/endian.h"

#include <ar.h>
#include <dolphin/os.h>
#include <freeverb/revmodel.hpp>
#if !defined(DUSK_TARGET_3DS)
#include <tracy/Tracy.hpp>
#else
#define ZoneScoped
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <span>
#include <numbers>
#include <optional>
#include <cstddef>
#include <cstring>

using namespace dusk::audio;

ChannelAuxData dusk::audio::ChannelAux[DSP_CHANNELS] = {};

static revmodel SharedReverb;
static bool ReverbHasTail = false;

static bool sDumpWasActive = false;
static FILE* sChannelDumpFiles[DSP_CHANNELS] = {};

static void OpenChannelDumpFiles() {
    char name[32];
    for (int i = 0; i < DSP_CHANNELS; i++) {
        snprintf(name, sizeof(name), "channel_%02d.raw", i);
        sChannelDumpFiles[i] = fopen(name, "wb");
    }
}

static void CloseChannelDumpFiles() {
    for (int i = 0; i < DSP_CHANNELS; i++) {
        if (sChannelDumpFiles[i]) {
            fclose(sChannelDumpFiles[i]);
            sChannelDumpFiles[i] = nullptr;
        }
    }
}

int dusk::audio::RenderRate = SampleRate;
f32 dusk::audio::MasterVolume = 1.0f;
f32 dusk::audio::PrevMasterVolume = 1.0f;
bool dusk::audio::EnableReverb = true;
bool dusk::audio::DumpAudio = false;
bool dusk::audio::EnableHrtf = false;
f32 dusk::audio::HrtfGain = 0.5f;
u8 dusk::audio::OutChannelCount = 0;

// 3dB at 5kHz.
static constexpr f32 HRTF_LP_K      = (SampleRate == 48000) ? 0.6267f : 0.75f;
static constexpr f32 HRTF_ALLPASS_G = (SampleRate == 48000) ? 0.448f : 0.3f;
// Front never drops below (1 - HRTF_EXTRACT_MAX).
static constexpr f32 HRTF_EXTRACT_MAX = 0.6f;

static f32 sHrtfLp1    = 0.0f;
static f32 sHrtfLp2    = 0.0f;
static f32 sHrtfApIn1  = 0.0f;
static f32 sHrtfApOut1 = 0.0f;

/**
 * Validate that a DSP channel's format is actually something we know how to play.
 */
static bool ValidateChannelWaveFormat(const JASDsp::TChannel& channel) {
    if (channel.mSamplesPerBlock == AdpcmSampleCount && channel.mBytesPerBlock == Adpcm4FrameSize)
        return true;
    if (channel.mSamplesPerBlock == 1 && channel.mBytesPerBlock == 16)
        return true;
    /*
    if (channel.mSamplesPerBlock == AdpcmSampleCount && channel.mBytesPerBlock == Adpcm2FrameSize)
        return true;
    if (channel.mSamplesPerBlock == 1 && channel.mBytesPerBlock == 8)
        return true;
    */
    return false;
}

/**
 * Validate that a DSP channel is actually something we know how to play.
 */
static void ValidateChannel(const JASDsp::TChannel& channel) {
    if (!ValidateChannelWaveFormat(channel)) {
#if defined(DUSK_TARGET_3DS)
        OSPanic(__FILE__,__LINE__,"Unsupported native DSP channel format %02x/%02x",channel.mSamplesPerBlock,channel.mBytesPerBlock);
#else
        const auto msg = fmt::format("Unable to handle channel format: {:02x}, {:02x}\n",
            channel.mSamplesPerBlock, channel.mBytesPerBlock);
        CRASH(msg.c_str());
#endif
    }
}

static u32 ConvertSamplesToDataLength(const JASDsp::TChannel& channel, u32 samples) {
    if (samples % channel.mSamplesPerBlock != 0) {
        // Ensure we round up.
        samples += channel.mSamplesPerBlock;
        //CRASH("Indivisible sample count: %d\n", samples);
    }

    return (samples / channel.mSamplesPerBlock) * BlockBytes(channel);
}

/**
 * Reset state for a DSP channel between independent playbacks.
 */
static void ResetChannel(JASDsp::TChannel& channel, ChannelAuxData& aux) {
    aux.resetCount += 1;

    channel.mSamplesLeft = channel.mEndSample - channel.mSamplePosition;

    aux.hist0 = 0;
    aux.hist1 = 0;

    aux.decodeBufCount = 0;
    aux.resamplePos = 0.0;
    aux.resamplePrev = 0;

    aux.oscPhase = 0;

    aux.prev_lp_out = 0.0f;
    aux.prev_lp_in = 0.0f;

    aux.biq_in1 = 0.0f;
    aux.biq_in2 = 0.0f;
    aux.biq_out1 = 0.0f;
    aux.biq_out2 = 0.0f;

    for (auto& volume : aux.prevVolume) {
        volume = NAN;
    }

    channel.mResetFlag = false;
}

/**
 * Mix subframe data from src into dst.
 */
static void MixSubframe(DspSubframe& dst, const DspSubframe& src) {
    for (int i = 0; i < dst.size(); i++) {
        dst[i] += src[i];
    }
}

static void MixOutputSubframe(OutputSubframe& dst, const OutputSubframe& src) {
    for (int i = 0; i < OutChannelCount; i++) {
        MixSubframe(dst.channels[i], src.channels[i]);
    }
}

enum class OscType : u16 {
    SQUARE_WAVE_PW_50        = 0,
    SAW_WAVE                 = 1,
    SQUARE_WAVE_PW_25        = 3,
    TRIANGLE_WAVE            = 4,
    // idk what 5 and 6 are
    SINE_WAVE                = 7,
    // idk what 8 and 9 are
    SINE_WAVE_VAR_STEP       = 10,
    EVOLVING_HARMONIC        = 11,
    EVOLVING_RAMP            = 12,
};

static s16 gEvolvingHarmonic[64];

static void GenerateEvolvingHarmonic() {
    static bool initialized = false;
    if (!initialized) {
        gEvolvingHarmonic[62] = 8191;
        gEvolvingHarmonic[63] = 16383;
        initialized = true;
    }

    u32 prev2 = (u32)gEvolvingHarmonic[62];
    u32 prev1 = (u32)gEvolvingHarmonic[63];

    for (int i = 0; i < 64; i += 2) {
        u32 cur = (u32)gEvolvingHarmonic[i];
        gEvolvingHarmonic[i] = (s16)((s32)(prev2 * prev1 - (cur << 16)) >> 16);
        prev2 = prev1;
        prev1 = cur;

        cur = (u32)gEvolvingHarmonic[i + 1];
        gEvolvingHarmonic[i + 1] = (s16)((s32)(2u * (prev2 * prev1 + (cur << 16))) >> 16);
        prev2 = prev1;
        prev1 = cur;
    }
}

static void RenderOscChannel(
    JASDsp::TChannel& channel,
    ChannelAuxData& channelAux,
    DspSubframe& buf) {
    if (channel.mResetFlag)
        ResetChannel(channel, channelAux);

    const u32 pitch = channel.mPitch;
    const auto oscType = static_cast<OscType>(channel.mBytesPerBlock);

    switch (oscType) {
    case OscType::SQUARE_WAVE_PW_50: {
        std::generate(buf.begin(), buf.end(), [&] {
            f32 s = channelAux.oscPhase < 0x8000u ? 0.5f : -0.5f;
            channelAux.oscPhase += pitch >> 1;
            return s;
        });
        break;
    }
    case OscType::SQUARE_WAVE_PW_25: {
        std::generate(buf.begin(), buf.end(), [&] {
            f32 s = channelAux.oscPhase < 0x4000u ? 0.5f : -0.5f;
            channelAux.oscPhase += pitch >> 1;
            return s;
        });
        break;
    }
    case OscType::SAW_WAVE:
    case OscType::EVOLVING_RAMP: {
        std::generate(buf.begin(), buf.end(), [&] {
            f32 s = (f32)(s16)channelAux.oscPhase / 32768.0f;
            channelAux.oscPhase += pitch >> 1;
            return s;
        });
        break;
    }
    case OscType::SINE_WAVE:
    case OscType::SINE_WAVE_VAR_STEP: {
        std::generate(buf.begin(), buf.end(), [&] {
            f32 s = sinf((f32)channelAux.oscPhase * (2.0f * M_PI / 65536.0f)) * 0.5f;
            channelAux.oscPhase += pitch >> 1;
            return s;
        });
        break;
    }
    case OscType::TRIANGLE_WAVE: {
        std::generate(buf.begin(), buf.end(), [&] {
            f32 s = 0.5f - fabsf((f32)(s16)channelAux.oscPhase / 32768.0f);
            channelAux.oscPhase += pitch >> 1;
            return s;
        });
        break;
    }
    case OscType::EVOLVING_HARMONIC: {
        std::generate(buf.begin(), buf.end(), [&] {
            f32 s = gEvolvingHarmonic[channelAux.oscPhase >> 10] / 32768.0f;
            channelAux.oscPhase += pitch >> 1;
            return s;
        });
        break;
    }
    default:
        // Silence: the caller hands in an uncleared buffer.
        buf.fill(0.0f);
#if defined(DUSK_TARGET_3DS)
        OSReport("RenderOscChannel: unimplemented oscillator type %u\n",channel.mBytesPerBlock);
#else
        DuskLog.error("RenderOscChannel: unimplemented oscillator type {}", channel.mBytesPerBlock);
#endif
        break;
    }
}

/**
 * Actually decode samples from memory for the given audio channel.
 */
static void ReadSampleData(
    const JASDsp::TChannel& channel,
    ChannelAuxData& aux,
    const u8* data,
    size_t dataLength,
    s16* pcm,
    size_t pcmLength) {
    if (channel.mSamplesPerBlock == 1) {
        if (channel.mBytesPerBlock == 0x10) {
            // PCM16
            assert(reinterpret_cast<uintptr_t>(data) % 2 == 0 && "PCM data must be aligned");
            assert(dataLength % 2 == 0 && "Data length must be multiple of 2");
            assert(dataLength * 2 >= pcmLength && "Input too small!");

            auto srcPcm = reinterpret_cast<const BE(s16)*>(data);
            for (size_t i = 0; i < pcmLength; i++) {
                pcm[i] = srcPcm[i];
            }
        } else {
            CRASH("Unsupported format: PCM8");
        }
    } else {
        if (channel.mBytesPerBlock == 9) {
            Adpcm4ToPcm16(data, dataLength, pcm, pcmLength, aux.hist1, aux.hist0);
        } else {
            CRASH("Unsupported format: ADPCM2");
        }
    }
}

/**
 * Read a single *contiguous* chunk of sample data from a channel into outBuf
 *
 * @returns Amount of samples written to outBuf. May be less than desiredSamples
 */
static int ReadChannelSamplesChunk(
    JASDsp::TChannel& channel,
    ChannelAuxData& aux,
    int desiredSamples,
    s16* outBuf,
    int outBufSize) {

    assert(desiredSamples >= 0);

    auto aramBase = static_cast<u8 const*>(channel.mAramBaseAddress ? channel.mAramBaseAddress : ARGetStorageAddress());
    aramBase += channel.mWaveAramAddress;

    auto curSamplePosition = channel.mSamplePosition;
    u32 skipSamples = curSamplePosition % channel.mSamplesPerBlock;
    if (skipSamples != 0) {
        // We need to start reading in the middle of a block. This can happen thanks to loops.
        // So we move back to the start of the block and keep track that those samples should
        // *not* be emitted.
        desiredSamples += static_cast<int>(skipSamples);
        curSamplePosition -= skipSamples;

        channel.mSamplesLeft += skipSamples;
        channel.mSamplePosition -= skipSamples;
    }

    // Pad desiredSamples so that we always leave the channel block-aligned.
    desiredSamples = ALIGN_NEXT(desiredSamples, channel.mSamplesPerBlock);

    assert(curSamplePosition % channel.mSamplesPerBlock == 0);
    auto dataPosition = ConvertSamplesToDataLength(channel, curSamplePosition);

    u32 renderSamples = std::min(channel.mSamplesLeft, static_cast<u32>(desiredSamples));

    if (skipSamples == 0 && renderSamples <= static_cast<u32>(outBufSize)) {
        // The usual case: no samples to drop and room for all of them. The
        // decoders write every sample they are asked for, so decoding in
        // place gives the bytes the cleared scratch buffer and copy gave.
        ReadSampleData(
            channel,
            aux,
            aramBase + dataPosition,
            ConvertSamplesToDataLength(channel, renderSamples),
            outBuf,
            renderSamples);
        channel.mSamplesLeft -= renderSamples;
        channel.mSamplePosition += renderSamples;
        return static_cast<int>(renderSamples);
    }

    int renderSize = static_cast<int>(sizeof(s16) * renderSamples);
    auto renderData = static_cast<s16*>(alloca(renderSize));
    memset(renderData, 0, renderSize);

    ReadSampleData(
        channel,
        aux,
        aramBase + dataPosition,
        ConvertSamplesToDataLength(channel, renderSamples),
        renderData,
        renderSamples);

    channel.mSamplesLeft -= renderSamples;
    channel.mSamplePosition += renderSamples;

    int outputCount = static_cast<int>(renderSamples - skipSamples);

    // this should never be hit with the limits on pitch shift (i think) but just in case!!
    outputCount = std::min(outputCount, outBufSize);
    if (outputCount > 0) {
        memcpy(outBuf, renderData + skipSamples, outputCount * sizeof(s16));
    }

    assert(curSamplePosition % channel.mSamplesPerBlock == 0 || channel.mSamplesLeft == 0);

    return outputCount;
}

/**
 * Fill decodeBuf with at least `needed` samples, fewer may be written if the channel has no loop and its data ends
 */
static void FillDecodeBuf(JASDsp::TChannel& channel, ChannelAuxData& aux, int needed) {
    while (aux.decodeBufCount < needed) {
        if (channel.mSamplesLeft == 0) {
            if (!channel.mLoopFlag) {
                // we aren't a looping channel and there's no samples left, we out of this fuckin loop
                break;
            } else {
                // we are looping, handle loop logic
                channel.mSamplesLeft = channel.mEndSample - channel.mLoopStartSample;
                channel.mSamplePosition = channel.mLoopStartSample;
                aux.hist1 = channel.mpPenult;
                aux.hist0 = channel.mpLast;
            }
        }

        int remainingDecodeSpace = ChannelAuxData::DECODE_BUF_SIZE - aux.decodeBufCount;
        if (remainingDecodeSpace == 0) {
            break;
        }

        aux.decodeBufCount += ReadChannelSamplesChunk(
            channel, aux, std::min(remainingDecodeSpace, needed - aux.decodeBufCount),
            aux.decodeBuf + aux.decodeBufCount, remainingDecodeSpace
        );
    }

    channel.mAramStreamPosition = channel.mWaveAramAddress + ConvertSamplesToDataLength(channel, channel.mSamplePosition);
}

/**
 * Adapts IIR biquad filter coeffs for fs = 32kHz -> RenderRate, Hz-preserving.
 * At 32 kHz (the 3DS) the GameCube's own coefficients are used as they are.
 */
static BiquadCoeffs RemapBiquad32to48(float b1, float b2, float a1, float a2) {
    using cplx = std::complex<f64>;

    if (RenderRate == 32000) {
        return {b1, b2, a1, a2};
    }

    constexpr auto kOldRate = 32000.0;
    const auto kNewRate = f64(RenderRate);
    const auto kGamma = kOldRate / kNewRate;

    if (b1 == 0 && b2 == 0 && a1 == 0 && a2 == 0) {
        return {0, 0, 0, 0};
    }

    auto mapPoint = [kGamma](cplx z) {
        if (std::abs(z.imag()) < 1e-9 * std::max(std::abs(z.real()), 1.0)) {
            f64 r = z.real();
            f64 sign = (r >= 0.0) ? 1.0 : -1.0;
            return cplx(sign * std::pow(std::abs(r), kGamma), 0.0);
        }
        f64 r = std::abs(z);
        f64 theta = std::arg(z);
        return std::polar(std::pow(r, kGamma), theta * kGamma);
    };

    f64 disc = f64(a1) * a1 + 4.0 * a2;
    cplx p1, p2;
    if (disc >= 0.0) {
        f64 sq = std::sqrt(disc);
        p1 = {(a1 + sq) * 0.5, 0.0};
        p2 = {(a1 - sq) * 0.5, 0.0};
    } else {
        f64 sq = std::sqrt(-disc);
        p1 = {a1 * 0.5,  sq * 0.5};
        p2 = {a1 * 0.5, -sq * 0.5};
    }
    cplx p1n = mapPoint(p1);
    cplx p2n = mapPoint(p2);
    f64 a1n = (p1n + p2n).real();
    f64 a2n = -(p1n * p2n).real();

    f64 z0n = (b1 == 0.0f) ? 0.0 : mapPoint(cplx(-f64(b2) / b1, 0.0)).real();

    f64 denOldDc = 1.0 - a1 - a2;
    f64 fRef = (std::abs(denOldDc) > 1e-6) ? 0.0 : 1.0;

    auto H = [](cplx z, cplx p1_, cplx p2_, f64 bb1, f64 bb2) {
        return (bb1 * z + bb2) / (z * (z - p1_) * (z - p2_));
    };

    cplx zOldRef = std::polar(1.0, 2.0 * M_PI * fRef / kOldRate);
    cplx zNewRef = std::polar(1.0, 2.0 * M_PI * fRef / kNewRate);

    cplx hOldRef     = H(zOldRef, p1,  p2,  b1,  b2)   * zOldRef;
    cplx shapeNewRef = H(zNewRef, p1n, p2n, 1.0, -z0n) * zNewRef;

    f64 K = (hOldRef / shapeNewRef).real();

    return {float(K), float(-K * z0n), float(a1n), float(a2n)};
}

/**
 * Render the audio data contributed by a single DSP channel. Reads & decodes new input samples.
 */
static void RenderChannel(
    JASDsp::TChannel& channel,
    ChannelAuxData& aux,
    DspSubframe& buf) {

    if (channel.mResetFlag) {
        ResetChannel(channel, aux);
    }

    // how many input samples we step per output sample, aka the resampling ratio
    auto step = static_cast<f32>(channel.mPitch) / 4096.0f;

    // how many input samples to resample to DSP_SUBFRAME_SIZE output samples
    int needed = static_cast<int>(aux.resamplePos + DSP_SUBFRAME_SIZE * step) + 2;

    FillDecodeBuf(channel, aux, needed);

    // source ran dry, channel is finished
    if(aux.decodeBufCount < needed) {
        channel.mIsFinished = true;
    }

    f32 pos = aux.resamplePos;
    s16 prev = aux.resamplePrev;
    s16 next = aux.decodeBufCount > 0 ? aux.decodeBuf[0] : prev;
    int srcIdx = 0;

    // linear resampling and f32 conversion. The two source samples are
    // converted when the source position moves, not for every output sample
    // (the same values: an s16 and a difference of two are exact in f32).
    f32 prevF = static_cast<f32>(prev);
    f32 diffF = static_cast<f32>(next - prev);
    for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
        buf[i] = (prevF + pos * diffF) / 32768.0f;
        pos += step;
        if (pos >= 1.0f) {
            do {
                pos -= 1.0f;
                prev = next;
                srcIdx++;
                next = srcIdx < aux.decodeBufCount ? aux.decodeBuf[srcIdx] : prev;
            } while (pos >= 1.0f);
            prevF = static_cast<f32>(prev);
            diffF = static_cast<f32>(next - prev);
        }
    }

    // save resampler state for the next subframe, prevents popping on pitch change
    aux.resamplePos = pos;
    aux.resamplePrev = prev;

    // IIR FILTER

    // IIR part 1, low-pass: out[n] = (in[n] - in[n-1]) * (coeff/128) + out[n-1]
    // removed: seems to be dead code that doesn't work as intended anyway

    // IIR part 2, biquad: out[n] = (b1*in[n-1] + b2*in[n-2] + a1*out[n-1] + a2*out[n-2]) / 32768
    if ((channel.mFilterMode & 0x20) != 0) {
        std::span newCoefs{channel.iir_filter_params, 4};
        if (!std::ranges::equal(newCoefs, aux.curBiquadCoefs)) {
            aux.remappedBiquadCoefs = RemapBiquad32to48(
                newCoefs[0] / 32768.0f,
                newCoefs[1] / 32768.0f,
                newCoefs[2] / 32768.0f,
                newCoefs[3] / 32768.0f
            );
            std::ranges::copy(newCoefs, aux.curBiquadCoefs.begin());
        }

        const auto coefs = aux.remappedBiquadCoefs;
        for (f32& sample : buf) {
            f32 out = std::clamp((
                coefs.b1 * aux.biq_in1  +
                coefs.b2 * aux.biq_in2  +
                coefs.a1 * aux.biq_out1 +
                coefs.a2 * aux.biq_out2
            ), -1.0f, 1.0f);

            // shift history, then store new input and output
            aux.biq_in2 = aux.biq_in1;   // in[n-2]  = in[n-1]
            aux.biq_in1 = sample;        // in[n-1]  = in[n]
            aux.biq_out2 = aux.biq_out1; // out[n-2] = out[n-1]
            sample = aux.biq_out1 = out; // out[n-1] = out[n]
        }
    }

    // move any remaining samples in the decode buf to the beginning
    int remainingDecodeBuf = aux.decodeBufCount - srcIdx;
    if (remainingDecodeBuf > 0) {
        memmove(aux.decodeBuf, aux.decodeBuf + srcIdx, remainingDecodeBuf * sizeof(s16));
    }

    aux.decodeBufCount = std::max(0, remainingDecodeBuf);
}

struct VolumeValue {
    f32 Target;
    f32 Init;
};

using VolumeArray = std::array<VolumeValue, OutputSubframe::NUM_CHANNELS>;

static void ApplyVolume(
    std::span<f32> dst,
    std::span<const f32> src,
    const f32 startVolume,
    const f32 endVolume) {
    assert(dst.size() >= src.size());

    if (startVolume == endVolume) {
        for (int i = 0; i < (int)src.size(); i++) {
            dst[i] = src[i] * startVolume;
        }
    } else {
        const f32 step = (endVolume - startVolume) / static_cast<f32>(src.size());
        for (int i = 0; i < (int)src.size(); i++) {
            dst[i] = src[i] * (startVolume + i * step);
        }
    }
}

// The same products added to dst instead of stored: a voice mixed straight
// into the output (plain stereo, DspRender). Each product is rounded to f32
// before the addition, as when it went through a buffer of its own.
static void ApplyVolumeAdd(
    std::span<f32> dst,
    std::span<const f32> src,
    const f32 startVolume,
    const f32 endVolume) {
    assert(dst.size() >= src.size());

    if (startVolume == endVolume) {
        for (int i = 0; i < (int)src.size(); i++) {
            const f32 scaled = src[i] * startVolume;
            dst[i] += scaled;
        }
    } else {
        const f32 step = (endVolume - startVolume) / static_cast<f32>(src.size());
        // The sample number as f32 (exact), without a conversion per sample.
        f32 number = 0.0f;
        for (int i = 0; i < (int)src.size(); i++) {
            const f32 scaled = src[i] * (startVolume + number * step);
            dst[i] += scaled;
            number += 1.0f;
        }
    }
}

struct SpeakerPlacement {
    OutputChannel channel;
    f32 angle;
};

struct SpeakerPair {
    OutputChannel first;
    OutputChannel second;
    f32 lo;
    f32 span;
};

template <std::size_t N>
constexpr auto BuildSpeakerPairs(const std::array<SpeakerPlacement, N>& config) {
    std::array<SpeakerPair, N> pairs = {};
    constexpr f32 kDegToRad = std::numbers::pi_v<f32> / 180.0f;

    for (std::size_t i = 0; i < N; i++) {
        std::size_t j = (i + 1) % N;
        float lo = config[i].angle;
        float hi = config[j].angle;
        float span = hi - lo;
        if (span < 0) span += 360.0f;

        pairs[i] = {config[i].channel, config[j].channel, lo * kDegToRad, span * kDegToRad};
    }

    return pairs;
}

constexpr auto Placement6ch = std::to_array<SpeakerPlacement>({
    // the "rear" channels are actually surround left/right,
    // changed to match SDL order
    {OutputChannel::FRONT_RIGHT, -30},
    {OutputChannel::FRONT_CENTER, 0},
    {OutputChannel::FRONT_LEFT, 30},
    {OutputChannel::REAR_LEFT, 120},
    {OutputChannel::REAR_RIGHT, -120},
});

constexpr auto Placement8ch = std::to_array<SpeakerPlacement>({
    {OutputChannel::SURROUND_RIGHT, -90},
    {OutputChannel::FRONT_RIGHT, -30},
    {OutputChannel::FRONT_CENTER, 0},
    {OutputChannel::FRONT_LEFT, 30},
    {OutputChannel::SURROUND_LEFT, 90},
    {OutputChannel::REAR_LEFT, 150},
    {OutputChannel::REAR_RIGHT, -150},
});

constexpr auto Pairs6ch = BuildSpeakerPairs(Placement6ch);
constexpr auto Pairs8ch = BuildSpeakerPairs(Placement8ch);

static void CalcStereoChannelVolumes(
    const JASDsp::TChannel& voice,
    VolumeArray& volumes)
{
    const auto volume = VolumeFromU16(voice.mAutoMixerVolume);
    const auto initVolume = VolumeFromU16(voice.mAutoMixerInitVolume);

    const auto right = static_cast<f32>(voice.mAutoMixerPanDolby >> 8) / 127.0f;
    const auto left = 1.0f - right;

    volumes[0] = {left * volume, left * initVolume};
    volumes[1] = {right * volume, right * initVolume};
}

static void CalcSurroundChannelVolumes(
    const JASDsp::TChannel& voice,
    VolumeArray& volumes)
{
    constexpr f32 kTurn = 2.0f * std::numbers::pi_v<f32>;

    const auto omniGain = 1.0f / static_cast<f32>(OutChannelCount - 1);
    const auto pan = static_cast<f32>(voice.mAutoMixerPanDolby >> 8) / 63.5f - 1.0f;
    const auto dolby = static_cast<f32>(voice.mAutoMixerPanDolby & 0xFF) / 63.5f - 1.0f;
    const auto focus = std::min(std::sqrt(pan * pan + dolby * dolby), 1.0f);

    f32 angle = std::atan2(-pan, -dolby);
    angle = std::fmod(angle, kTurn);
    if (angle < 0) angle += kTurn;

    std::array<f32, OutputSubframe::NUM_CHANNELS> gains = {};
    gains.fill(omniGain * (1.0f - focus));

    using Pairs = std::span<const SpeakerPair>;
    const auto pairs = OutChannelCount == 6 ? Pairs{Pairs6ch} : Pairs{Pairs8ch};

    for (const auto& pair : pairs) {
        const auto offset = std::fmod(angle - pair.lo + kTurn, kTurn);

        if (offset <= pair.span) {
            const auto first = static_cast<size_t>(pair.first);
            const auto second = static_cast<size_t>(pair.second);
            const auto t = std::clamp(offset / pair.span, 0.0f, 1.0f);

            const auto firstGain = std::cos(t * std::numbers::pi_v<f32> / 2.0f);
            const auto secondGain =  std::sin(t * std::numbers::pi_v<f32> / 2.0f);

            gains[first] += focus * firstGain;
            gains[second] += focus * secondGain;

            break;
        }
    }

    const auto volume = VolumeFromU16(voice.mAutoMixerVolume);
    const auto initVolume = VolumeFromU16(voice.mAutoMixerInitVolume);

    for (size_t i = 0; i < OutChannelCount; i++) {
        volumes[i].Target = volume * gains[i];
        volumes[i].Init = initVolume * gains[i];
    }
}

// The start and target volume of every output channel for this subframe.
static void CalcVoiceVolumes(const JASDsp::TChannel& voice, VolumeArray& volumes) {
    if (voice.mAutoMixerBeenSet) {
        if (OutChannelCount > 2) {
            CalcSurroundChannelVolumes(voice, volumes);
        } else {
            CalcStereoChannelVolumes(voice, volumes);
        }
    } else {
        for (const auto& outChannel : voice.mOutputChannels) {
            std::optional<OutputChannel> ch;

            switch (outChannel.mBusConnect) {
                case 0x0D00:
                    ch = OutputChannel::FRONT_LEFT;
                    break;
                case 0x0D60:
                    ch = OutputChannel::FRONT_RIGHT;
                    break;
                default:
                    break;
            }

            if (ch) {
                auto& v = volumes[static_cast<size_t>(*ch)];
                v.Target = VolumeFromU16(outChannel.mTargetVolume);
                v.Init = VolumeFromU16(outChannel.mCurrentVolume);
            }
        }
    }
}

// `add`: the voice is added to `output` (the mix itself) instead of written
// into it (a cleared buffer of its own).
static void ApplyPanning(
    const JASDsp::TChannel& voice,
    ChannelAuxData& aux,
    const DspSubframe& input,
    OutputSubframe& output,
    bool add = false)
{
    VolumeArray volumes = {};
    CalcVoiceVolumes(voice, volumes);

    for (size_t i = 0; i < OutChannelCount; i++) {
        const auto ch = static_cast<OutputChannel>(i);
        if (ch == OutputChannel::LFE) {
            continue;
        }

        const auto& volume = volumes[i];

        const f32 targetVolume = volume.Target;
        auto& prevVolume = aux.PrevVolume(ch);

        if (std::isnan(prevVolume)) {
            // Initialize previous volume to new volume on first render.
            prevVolume = volume.Init;
        }

        if (prevVolume == 0 && targetVolume == 0) {
            continue;
        }

        if (add) {
            ApplyVolumeAdd(output[ch], input, prevVolume, targetVolume);
        } else {
            ApplyVolume(output[ch], input, prevVolume, targetVolume);
        }
        prevVolume = targetVolume;
    }
}

static void DownmixSurroundToStereo(
    const OutputSubframe& input,
    OutputSubframe& output)
{
    auto& left = output.channels[0];
    auto& right = output.channels[1];
    for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
        const auto fc = input.channels[2][i] * 0.5f;
        left[i] = input.channels[0][i] + fc + input.channels[4][i];
        right[i] = input.channels[1][i] + fc + input.channels[5][i];
        if (OutChannelCount > 6) {
            left[i] += input.channels[6][i];
            right[i] += input.channels[7][i];
            left[i] /= 3.5f;
            right[i] /= 3.5f;
        } else {
            left[i] /= 2.5f;
            right[i] /= 2.5f;
        }
    }
}

static void UpmixStereoToSurroundInplace(OutputSubframe& buf) {
    // pseudoinverse of downmix matrix
    const auto w = OutChannelCount > 6 ? 1.0f / 12.0f : 1.0f / 8.0f;
    for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
        const auto le = buf.channels[0][i] * (1.0f + w) + buf.channels[1][i] * -w;
        const auto re = buf.channels[1][i] * (1.0f + w) + buf.channels[0][i] * -w;
        const auto c = buf.channels[0][i] * 0.5f + buf.channels[1][i] * 0.5f;
        /* FL  */ buf.channels[0][i] = le;
        /* FR  */ buf.channels[1][i] = re;
        /* FC  */ buf.channels[2][i] = c;
        /* LFE */ buf.channels[3][i] = 0.0f;
        /* BL  */ buf.channels[4][i] = le;
        /* BR  */ buf.channels[5][i] = re;
        if (OutChannelCount > 6) {
            /* SL */ buf.channels[6][i] = le;
            /* SR */ buf.channels[7][i] = re;
        }
    }
}

static void AccumulateReverbInput(
    DspSubframe& dstL, DspSubframe& dstR,
    const DspSubframe& srcL, const DspSubframe& srcR,
    f32 gain)
{
    for (int j = 0; j < DSP_SUBFRAME_SIZE; j++) {
        dstL[j] += srcL[j] * gain;
        dstR[j] += srcR[j] * gain;
    }
}

void dusk::audio::DspInit() {
    SharedReverb.setwet(1.0f);
    SharedReverb.setdry(0.0f);
    SharedReverb.setroomsize(0.5f);
    SharedReverb.setdamp(0.7f);
    SharedReverb.setwidth(1.0f);
    SharedReverb.setmode(0.0f);
    SharedReverb.mute();
}

// One subframe of every voice into `subframe`. `direct`: plain stereo with
// no reverb, surround bus or per-voice dump (the 3DS): each voice is added
// straight to the two output channels. Otherwise it is rendered into a
// cleared eight-channel buffer first (2.5 KB) and that buffer is added: the
// same sums, at twice the memory traffic per voice.
static void SyncChannelDumpFiles() {
    if (DumpAudio != sDumpWasActive) {
        sDumpWasActive = DumpAudio;
        if (DumpAudio) {
            OpenChannelDumpFiles();
        } else {
            CloseChannelDumpFiles();
        }
    }
}

static void RenderSubframe(OutputSubframe& subframe, bool direct) {
    ZoneScoped;
    SyncChannelDumpFiles();

    GenerateEvolvingHarmonic();

    std::span voices(JASDsp::CH_BUF, DSP_CHANNELS);

    DspSubframe reverbInputL = {};
    DspSubframe reverbInputR = {};
    bool anyReverbInput = false;

    DspSubframe surroundBus = {};
    bool anySurroundInput = false;

    for (int i = 0; i < voices.size(); i++) {
        auto& voice = voices[i];
        auto& aux = ChannelAux[i];

        if (!voice.mIsActive) {
            continue;
        }
        else if (voice.mPauseFlag) {
            // Not really sure what the practical difference between pause and
            // deactivation is. Either avoids clearing state or allows the DSP to avoid popping?
            continue;
        }
        else if (voice.mForcedStop) {
            voice.mIsFinished = true;
            continue;
        }

        // Both renderers write every sample.
        DspSubframe monoBuf;
        if (voice.mWaveAramAddress == 0 && !voice.mAramBaseAddress) {
            RenderOscChannel(voice, aux, monoBuf);
        } else {
            ValidateChannel(voice);
            RenderChannel(voice, aux, monoBuf);
        }

        if (direct) {
            ApplyPanning(voice, aux, monoBuf, subframe, true);
            continue;
        }

        OutputSubframe buf = {};
        ApplyPanning(voice, aux, monoBuf, buf);

        if (EnableReverb) {
            // scale the input to the reverb rather than using wet/dry on the output.
            // this way the reverb's internal buffers accumulate energy proportional to mAutoMixerFxMix,
            // so any tail always decays at the correct level regardless of mAutoMixerFxMix changes
            // prevents transients when the next sound starts playing with a different reverb level
            // 600.0f was pulled out of my ass and just sounds good enough for console
            f32 inputGain = (voice.mAutoMixerFxMix >> 8) / 600.0f;
            if (inputGain > 0) {
                anyReverbInput = true;
                if (OutChannelCount > 2) {
                    OutputSubframe downmix;
                    DownmixSurroundToStereo(buf, downmix);
                    AccumulateReverbInput(reverbInputL, reverbInputR, downmix.channels[0], downmix.channels[1], inputGain);
                } else {
                    AccumulateReverbInput(reverbInputL, reverbInputR, buf.channels[0], buf.channels[1], inputGain);
                }
            }
        }

        if (EnableHrtf && voice.mAutoMixerBeenSet) {
            f32 dolby = (voice.mAutoMixerPanDolby & 0xFF) / 127.0f;
            if (dolby > 0.0f) {
                anySurroundInput = true;
                f32 extract = dolby * HRTF_EXTRACT_MAX;
                f32 frontScale = 1.0f - extract;
                for (int j = 0; j < DSP_SUBFRAME_SIZE; j++) {
                    f32 mono = (buf.channels[0][j] + buf.channels[1][j]) * 0.5f;
                    surroundBus[j] += mono * extract;
                    buf.channels[0][j] *= frontScale;
                    buf.channels[1][j] *= frontScale;
                }
            }
        }

        if (DumpAudio && sChannelDumpFiles[i]) {
            f32 interleaved[DSP_SUBFRAME_SIZE * 2];
            for (int j = 0; j < DSP_SUBFRAME_SIZE; j++) {
                interleaved[j * 2 + 0] = buf.channels[0][j];
                interleaved[j * 2 + 1] = buf.channels[1][j];
            }
            fwrite(interleaved, sizeof(f32), DSP_SUBFRAME_SIZE * 2, sChannelDumpFiles[i]);
        }

        MixOutputSubframe(subframe, buf);
    }

    if (EnableReverb && (anyReverbInput || ReverbHasTail)) {
        // Equivalent to -80 dBFS: rms = 1e-4, rms^2 = 1e-8, sumSq = 2 * N * 1e-8
        constexpr f32 REVERB_ENERGY_EPSILON = 2.0f * DSP_SUBFRAME_SIZE * 1e-8f;
        f32 wetEnergy = 0.0f;
        if (OutChannelCount > 2) {
            OutputSubframe reverbOut;
            wetEnergy = SharedReverb.processreplace(
                reverbInputL.data(), reverbInputR.data(),
                reverbOut.channels[0].data(), reverbOut.channels[1].data(),
                DSP_SUBFRAME_SIZE, 1, 1.0f
            );
            UpmixStereoToSurroundInplace(reverbOut);
            MixOutputSubframe(subframe, reverbOut);
        } else {
            wetEnergy = SharedReverb.processmix(
                reverbInputL.data(), reverbInputR.data(),
                subframe.channels[0].data(), subframe.channels[1].data(),
                DSP_SUBFRAME_SIZE, 1, 1.0f
            );
        }
        ReverbHasTail = wetEnergy >= REVERB_ENERGY_EPSILON;
    }

    if (EnableHrtf && anySurroundInput) {
        // Two-pole LPF: -12 dB/oct above 3 kHz
        for (int j = 0; j < DSP_SUBFRAME_SIZE; j++) {
            sHrtfLp1 = (1.0f - HRTF_LP_K) * sHrtfLp1 + HRTF_LP_K * surroundBus[j];
            sHrtfLp2 = (1.0f - HRTF_LP_K) * sHrtfLp2 + HRTF_LP_K * sHrtfLp1;
            surroundBus[j] = sHrtfLp2;
        }

        // Mix into L and R
        // L gets the filtered signal directly; R gets it allpass for mild decorrelation
        for (int j = 0; j < DSP_SUBFRAME_SIZE; j++) {
            f32 s = surroundBus[j];

            subframe.channels[0][j] += s * HrtfGain;

            f32 r = -HRTF_ALLPASS_G * s + sHrtfApIn1 + HRTF_ALLPASS_G * sHrtfApOut1;
            sHrtfApIn1  = s;
            sHrtfApOut1 = r;
            subframe.channels[1][j] += r * HrtfGain;
        }
    }

    for (int i = 0; i < OutChannelCount; i++) {
        auto& channel = subframe.channels[i];
        ApplyVolume(channel, channel, PrevMasterVolume, MasterVolume);
    }
    PrevMasterVolume = MasterVolume;
}

// ---- Fixed-point plain stereo mix (DspFixedMix) ----
//
// A volume of 1.0 is 2^26 and an interpolated sample s (-32768..32767, with
// 12 fraction bits from the 1/4096 position) is s * 2^16. The high word of
// their product is s * volume * 2^10: one step of the 16-bit output is 1024
// units, and 64 voices at full volume and full scale stay inside 32 bits.

static constexpr f32 kFixedVolumeOne = 67108864.0f;                       // 2^26
static constexpr f32 kFixedRampScale = kFixedVolumeOne / DSP_SUBFRAME_SIZE;
static constexpr f32 kFixedToFloat = 1.0f / 33554432.0f;                  // 2^-25 = 2^-10 / 32768
// Largest source index one subframe can read: pitch below 0x8000 (8 input
// samples per output sample), a start below one sample, and the next sample.
static constexpr int kFixedSourceSamples = DSP_SUBFRAME_SIZE * 8 + 4;

// acc + round(a * b / 2^32): one ARMv6 SMMLAR (no 64-bit accumulator).
static inline s32 MulHighRoundAdd(s32 acc, s32 a, s32 b) {
#if defined(__arm__) && !defined(__thumb__) && defined(__ARM_ARCH) && __ARM_ARCH >= 6
    s32 result;
    __asm__("smmlar %0, %1, %2, %3" : "=r"(result) : "r"(a), "r"(b), "r"(acc));
    return result;
#else
    return static_cast<s32>(static_cast<u32>(acc) +
                            static_cast<u32>((static_cast<s64>(a) * b + 0x80000000LL) >> 32));
#endif
}

// 80 output samples of one voice added to the interleaved stereo
// accumulator. `source[0]` is the sample before the first decoded one;
// `position` is in 1/4096 of a source sample (below 4096), `pitch` the step.
// The volumes move by `leftStep`/`rightStep` per sample (the float path's
// linear ramp from the previous volume to the target).
static void MixVoiceFixed(s32* mix, const s32* source, u32 position, u32 pitch,
                          s32 left, s32 leftStep, s32 right, s32 rightStep) {
    // The position as a source index and a fraction in the top 12 bits: the
    // carry out of the fraction is the next source sample (ADDS/ADC).
    u32 index = 0;
    u32 fraction = position << 20;
    const u32 fractionStep = pitch << 20;
    const u32 indexStep = pitch >> 12;
    for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
        const s32 first = source[index];
        const u32 diff = static_cast<u32>(source[index + 1] - first);
        // first * 65536 + diff * fraction * 16: the float path's
        // prev + pos * (next - prev), times 65536. The products may wrap in
        // 32 bits; the sum lies between the two samples, so it is exact.
        const s32 sample = static_cast<s32>((static_cast<u32>(first) << 16) + diff * (fraction >> 16));
        const s32 mixedLeft = mix[0];
        const s32 mixedRight = mix[1];
        mix[0] = MulHighRoundAdd(mixedLeft, sample, left);
        mix[1] = MulHighRoundAdd(mixedRight, sample, right);
        mix += 2;
        left += leftStep;
        right += rightStep;
        const u32 next = fraction + fractionStep;
        index += indexStep + (next < fraction ? 1u : 0u);
        fraction = next;
    }
}

// One ADPCM/PCM voice without a biquad: RenderChannel and ApplyPanning in
// fixed point. The decoder calls, finish flag, resampler position, previous
// sample and decode buffer end exactly as in RenderChannel (the float path
// keeps its position in exact multiples of 1/4096 too). `mix` is null for a
// voice whose two volumes stay zero: it only advances.
static void RenderChannelFixed(JASDsp::TChannel& channel, ChannelAuxData& aux, s32* mix,
                               const s32 start[2], const s32 step[2]) {
    const u32 pitch = channel.mPitch;
    const u32 from = static_cast<u32>(aux.resamplePos * 4096.0f);
    const u32 to = from + DSP_SUBFRAME_SIZE * pitch;
    const int advance = static_cast<int>(to >> 12);
    const int needed = advance + 2;

    FillDecodeBuf(channel, aux, needed);
    const int count = aux.decodeBufCount;
    if (count < needed) {
        channel.mIsFinished = true;
    }

    if (mix) {
        // source[k] is the k-th sample after resamplePrev; past the end of a
        // source that ran dry the last sample repeats (RenderChannel's
        // `next = prev`). Every index up to needed - 1 is read at most.
        s32 source[kFixedSourceSamples];
        source[0] = aux.resamplePrev;
        const int copied = std::min(count, needed);
        for (int k = 0; k < copied; k++) {
            source[k + 1] = aux.decodeBuf[k];
        }
        for (int k = copied + 1; k <= needed; k++) {
            source[k] = source[k - 1];
        }
        MixVoiceFixed(mix, source, from, pitch, start[0], step[0], start[1], step[1]);
    }

    aux.resamplePos = static_cast<f32>(to & 0xFFF) * (1.0f / 4096.0f);
    if (advance > 0) {
        if (advance <= count) {
            aux.resamplePrev = aux.decodeBuf[advance - 1];
        } else if (count > 0) {
            aux.resamplePrev = aux.decodeBuf[count - 1];
        }
    }

    const int remainingDecodeBuf = count - advance;
    if (remainingDecodeBuf > 0) {
        memmove(aux.decodeBuf, aux.decodeBuf + advance, remainingDecodeBuf * sizeof(s16));
    }
    aux.decodeBufCount = std::max(0, remainingDecodeBuf);
}

// RenderSubframe(subframe, true) with the ADPCM/PCM voices mixed in fixed
// point. Oscillators, biquad voices, pitches above 0x7FFF and volumes
// outside 0..1 take the float path into `subframe`; the fixed sum is added
// to it before the master volume, which both paths apply the same way.
static void RenderSubframeFixed(OutputSubframe& subframe) {
    ZoneScoped;
    SyncChannelDumpFiles();

    GenerateEvolvingHarmonic();

    alignas(8) s32 mix[DSP_SUBFRAME_SIZE * 2] = {};
    bool anyFixed = false;

    std::span voices(JASDsp::CH_BUF, DSP_CHANNELS);
    for (int i = 0; i < voices.size(); i++) {
        auto& voice = voices[i];
        auto& aux = ChannelAux[i];

        if (!voice.mIsActive) {
            continue;
        } else if (voice.mPauseFlag) {
            continue;
        } else if (voice.mForcedStop) {
            voice.mIsFinished = true;
            continue;
        }

        const bool oscillator = voice.mWaveAramAddress == 0 && !voice.mAramBaseAddress;
        if (oscillator || (voice.mFilterMode & 0x20) != 0 || voice.mPitch > 0x7FFF) {
            DspSubframe monoBuf;
            if (oscillator) {
                RenderOscChannel(voice, aux, monoBuf);
            } else {
                ValidateChannel(voice);
                RenderChannel(voice, aux, monoBuf);
            }
            ApplyPanning(voice, aux, monoBuf, subframe, true);
            continue;
        }

        ValidateChannel(voice);
        if (voice.mResetFlag) {
            ResetChannel(voice, aux);
        }

        // ApplyPanning's volumes (they depend on the channel table only),
        // with the first render's NaN resolved and stored as it does.
        VolumeArray volumes = {};
        CalcVoiceVolumes(voice, volumes);
        f32 from[2], to[2];
        bool silent = true, inRange = true;
        for (int c = 0; c < 2; c++) {
            f32& previous = aux.prevVolume[c];
            if (std::isnan(previous)) {
                previous = volumes[c].Init;
            }
            from[c] = previous;
            to[c] = volumes[c].Target;
            silent = silent && from[c] == 0 && to[c] == 0;
            inRange = inRange && from[c] >= 0.0f && from[c] <= 1.0f && to[c] >= 0.0f && to[c] <= 1.0f;
        }
        if (!inRange) {
            DspSubframe monoBuf;
            RenderChannel(voice, aux, monoBuf);
            ApplyPanning(voice, aux, monoBuf, subframe, true);
            continue;
        }

        s32 start[2], step[2];
        for (int c = 0; c < 2; c++) {
            start[c] = static_cast<s32>(from[c] * kFixedVolumeOne + 0.5f);
            step[c] = from[c] == to[c] ? 0 : static_cast<s32>(lrintf((to[c] - from[c]) * kFixedRampScale));
        }
        RenderChannelFixed(voice, aux, silent ? nullptr : mix, start, step);
        // A channel at zero before and after keeps its value (ApplyPanning
        // skips it); every other one ends at its target.
        for (int c = 0; c < 2; c++) {
            if (!(from[c] == 0 && to[c] == 0)) {
                aux.prevVolume[c] = to[c];
            }
        }
        anyFixed = anyFixed || !silent;
    }

    if (anyFixed) {
        auto& left = subframe.channels[0];
        auto& right = subframe.channels[1];
        for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
            left[i] += static_cast<f32>(mix[i * 2]) * kFixedToFloat;
            right[i] += static_cast<f32>(mix[i * 2 + 1]) * kFixedToFloat;
        }
    }

    for (int i = 0; i < OutChannelCount; i++) {
        auto& channel = subframe.channels[i];
        ApplyVolume(channel, channel, PrevMasterVolume, MasterVolume);
    }
    PrevMasterVolume = MasterVolume;
}

bool dusk::audio::DspPlainMix = false;
bool dusk::audio::DspVerifyMix = false;
u64 dusk::audio::DspVerifiedSubframes = 0;
u64 dusk::audio::DspVerifyMismatches = 0;
bool dusk::audio::DspFixedMix = false;
f32 dusk::audio::DspVerifyLargestStep = 0.0f;
u64 dusk::audio::DspVerifySamplesOverStep = 0;

// Mixer state a render changes, for the verify modes: the DSP channel table,
// the active voices' aux data (the decode buffer only up to its count; the
// rest is written before it is read), the evolving harmonic and the master
// volume ramp. Inactive voices are not touched by a render.
struct MixerState {
    JASDsp::TChannel voices[DSP_CHANNELS];
    ChannelAuxData aux[DSP_CHANNELS];
    s16 harmonic[64];
    f32 prevMaster;
};

static void CopyAux(ChannelAuxData& to, const ChannelAuxData& from) {
    constexpr size_t head = offsetof(ChannelAuxData, decodeBuf);
    constexpr size_t tail = offsetof(ChannelAuxData, decodeBufCount);
    memcpy(reinterpret_cast<u8*>(&to), reinterpret_cast<const u8*>(&from), head);
    memcpy(reinterpret_cast<u8*>(&to) + tail, reinterpret_cast<const u8*>(&from) + tail, sizeof(ChannelAuxData) - tail);
    const int count = std::clamp(from.decodeBufCount, 0, ChannelAuxData::DECODE_BUF_SIZE);
    memcpy(to.decodeBuf, from.decodeBuf, count * sizeof(s16));
}

static void SaveMixerState(MixerState& state) {
    memcpy(static_cast<void*>(state.voices), JASDsp::CH_BUF, sizeof(state.voices));
    for (int i = 0; i < DSP_CHANNELS; i++) {
        if (state.voices[i].mIsActive) {
            CopyAux(state.aux[i], ChannelAux[i]);
        }
    }
    memcpy(state.harmonic, gEvolvingHarmonic, sizeof(state.harmonic));
    state.prevMaster = PrevMasterVolume;
}

static void RestoreMixerState(const MixerState& state) {
    memcpy(static_cast<void*>(JASDsp::CH_BUF), state.voices, sizeof(state.voices));
    for (int i = 0; i < DSP_CHANNELS; i++) {
        if (state.voices[i].mIsActive) {
            CopyAux(ChannelAux[i], state.aux[i]);
        }
    }
    memcpy(gEvolvingHarmonic, state.harmonic, sizeof(state.harmonic));
    PrevMasterVolume = state.prevMaster;
}

void dusk::audio::DspRender(OutputSubframe& subframe) {
    const bool direct = !DspPlainMix && OutChannelCount == 2 && !EnableReverb && !EnableHrtf && !DumpAudio;
    const bool fixed = direct && DspFixedMix;
    if (!direct || !DspVerifyMix) {
        if (fixed) {
            RenderSubframeFixed(subframe);
        } else {
            RenderSubframe(subframe, direct);
        }
        return;
    }
    static MixerState* before = nullptr;
    if (!before) {
        before = static_cast<MixerState*>(malloc(sizeof(MixerState)));
    }
    if (!before) {
        if (fixed) {
            RenderSubframeFixed(subframe);
        } else {
            RenderSubframe(subframe, direct);
        }
        return;
    }
    // Verify: the same subframe through both paths, from the same state.
    // Fixed point against the float direct path (bounded rounding difference),
    // or the float direct path against the buffered one (equal samples).
    SaveMixerState(*before);
    OutputSubframe reference = subframe;
    RenderSubframe(reference, fixed);
    RestoreMixerState(*before);
    if (fixed) {
        RenderSubframeFixed(subframe);
    } else {
        RenderSubframe(subframe, true);
    }
    ++DspVerifiedSubframes;
    bool differs = false;
    for (int channel = 0; channel < 2; channel++) {
        for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
            // Equal values (a zero of either sign is the same sample).
            if (reference.channels[channel][i] == subframe.channels[channel][i]) {
                continue;
            }
            differs = true;
            if (!fixed) {
                break;
            }
            const f32 steps = fabsf(reference.channels[channel][i] - subframe.channels[channel][i]) * 32768.0f;
            if (!(steps <= DspVerifyLargestStep)) {
                DspVerifyLargestStep = steps;
            }
            if (!(steps <= 1.0f)) {
                ++DspVerifySamplesOverStep;
            }
        }
    }
    if (differs) {
        ++DspVerifyMismatches;
    }
}
