#pragma once
// ReSharper disable once CppUnusedIncludeDirective
#include <dolphin/os.h>
#include "global.h"

#include "JSystem/JAudio2/JASDSPInterface.h"

#if !defined(DUSK_TARGET_3DS)
#include <SDL3/SDL_audio.h>
#endif

#include <array>
#include <cassert>

namespace dusk::audio {
    constexpr int SampleRate = 48000;

    enum class OutputChannel : u8 {
        // same as SDL channel layout for 7.1
        FRONT_LEFT,
        FRONT_RIGHT,
        FRONT_CENTER,
        LFE,
        REAR_LEFT,
        REAR_RIGHT,
        SURROUND_LEFT,
        SURROUND_RIGHT,
        OutputChannel_MAX
    };

    struct BiquadCoeffs { float b1, b2, a1, a2; };

    /**
     * Data stored by DSP implementation for each DSP channel.
     */
    struct ChannelAuxData {
        s16 hist1;
        s16 hist0;

        // Used for debugging tools.
        u32 resetCount;

        /**
         * Previous volume values, per output channel.
         * Used to avoid clicking when volumes change.
         * Set to NaN after channel reset, indicating that initial volume value is previous.
         */
        f32 prevVolume[static_cast<int>(OutputChannel::OutputChannel_MAX)];

        f32& PrevVolume(OutputChannel channel) {
            assert(channel < OutputChannel::OutputChannel_MAX);
            return prevVolume[static_cast<int>(channel)];
        }

        // buffer for decoding before resampling, size is chosen based on how many input samples we would need to fetch for the highest possible pitch
        // to fill one subframe of output samples after resampling
        static constexpr int DECODE_BUF_SIZE = 2048;
        s16 decodeBuf[DECODE_BUF_SIZE];
        int decodeBufCount;

        // basically stores our position between resamplePrev and decodeBuf[0] so we don't lose that fractional resampler position next subframe
        f32 resamplePos;
        // last consumed sample from decodeBuf
        s16 resamplePrev;

        // phase of oscillator channels
        u16 oscPhase;

        // low pass previous state
        f32 prev_lp_out;  // out[n-1]
        f32 prev_lp_in;   // in[n-1]

        std::array<u16, 4> curBiquadCoefs;
        BiquadCoeffs remappedBiquadCoefs;

        // biquad state
        f32 biq_in1; // in[n-1]
        f32 biq_in2; // in[n-2]
        f32 biq_out1; // out[n-1]
        f32 biq_out2; // out[n-2]
    };

    extern ChannelAuxData ChannelAux[DSP_CHANNELS];

    /**
     * Data storage for a single subframe and output channel's worth of samples.
     */
    using DspSubframe = std::array<f32, DSP_SUBFRAME_SIZE>;

    /**
     * Data storage for a single subframe's worth of samples, across all output channels.
     */
    struct OutputSubframe {
        static constexpr int NUM_CHANNELS = static_cast<int>(OutputChannel::OutputChannel_MAX);

        std::array<DspSubframe, NUM_CHANNELS> channels;

        DspSubframe& operator[](OutputChannel channel) {
            assert(channel < OutputChannel::OutputChannel_MAX);
            return channels[static_cast<int>(channel)];
        }
    };

    /**
     * Initialize the DSP system, creating data storage needed for channels and such.
     */
    void DspInit();

    /**
     * Render a subframe of audio with the current DSP state.
     */
    void DspRender(OutputSubframe& subframe);

    /**
     * Plain stereo output (no reverb, surround bus or per-voice dump) mixes
     * each voice straight into the output. DspPlainMix keeps the original
     * path (a buffer per voice); DspVerifyMix renders every subframe through
     * both from the same state and counts the subframes whose samples differ
     * (the count must stay zero).
     */
    extern bool DspPlainMix;
    extern bool DspVerifyMix;
    extern u64 DspVerifiedSubframes;
    extern u64 DspVerifyMismatches;

    /**
     * Rate the mixer renders at (the desktop: SampleRate). The 3DS renders at
     * the GameCube's own 32 kHz (JAS output rate 0: 7 subframes of 80 samples
     * per 17.5 ms block) unless an SD switch asks for 48 kHz.
     */
    extern int RenderRate;

    /**
     * Fixed-point plain stereo mix (the 3DS). ADPCM and PCM voices without a
     * biquad are resampled and mixed in integers (ARMv6 SMMLAR) straight into
     * a 32-bit stereo accumulator, and voices whose two volumes stay zero only
     * advance through their source. Oscillators, filtered voices and volumes
     * outside 0..1 keep the float path. Positions advance exactly as in the
     * float path (multiples of 1/4096); the samples differ from it by rounding
     * only. With DspVerifyMix, every subframe is also rendered by the float
     * path from the same state; the largest difference is kept in 1/32768
     * steps (one step = one 16-bit output step at gain 1) with the number of
     * samples that differ by more than one step.
     */
    extern bool DspFixedMix;
    extern f32 DspVerifyLargestStep;
    extern u64 DspVerifySamplesOverStep;

    /**
     * Get the amount of samples a single "block" of this channel's data has.
     */
    constexpr u32 BlockSamples(const JASDsp::TChannel& channel) {
        return channel.mSamplesPerBlock;
    }

    /**
     * Get the amount of bytes a single "block" of this channel's data has.
     */
    constexpr u32 BlockBytes(const JASDsp::TChannel& channel) {
        if (channel.mSamplesPerBlock == 1) {
            if (channel.mBytesPerBlock == 16) {
                return 2;
            }
            if (channel.mBytesPerBlock == 8) {
                return 1;
            }

            CRASH("Unknown format");
        }

        return channel.mBytesPerBlock;
    }

    extern f32 MasterVolume;
    extern f32 PrevMasterVolume;
    extern bool EnableReverb;
    extern bool DumpAudio;
    extern bool EnableHrtf;
    extern f32 HrtfGain;
    extern u8 OutChannelCount;
}
