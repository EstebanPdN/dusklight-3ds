#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace dusk::native {
float nativeDspMixerLevel();
enum class AudioOutputState : std::uint8_t { uninitialized, ndsp, componentUnavailable };
struct AudioRuntimeStatus {
    AudioOutputState output{};
    std::uint32_t result{},blocks{},underruns{}; // blocks is legacy filled count, NOT completion
    std::uint32_t mixTicks{}; // modulo2^32 elapsed native ticks inside worker fill
    std::uint32_t submitted{},completed{}; // queue calls / observed NDSP DONE transitions
};
AudioRuntimeStatus nativeAudioStatus();

// Exactly the mixer's saturating/truncating signed16 conversion; caller rejects nonfinite input.
inline std::int16_t nativePcm16(float value) {
    return static_cast<std::int16_t>(std::clamp(value,-1.0f,32767.0f/32768)*32768);
}
inline constexpr char audioPcmConfigPath[]="sdmc:/3ds/dusklight/audio-pcm.txt";
inline constexpr char audioPcmWavePath[]="sdmc:/3ds/dusklight/audio-pcm.wav";
inline constexpr char audioPcmJsonPath[]="sdmc:/3ds/dusklight/audio-pcm.json";
inline constexpr std::size_t audioPcmMaxBytes=1024*1024; // includes WAV header
struct AudioPcmConfig { std::uint32_t startBlock{},blockCount{}; };
// Two decimal integers: zero-based start block and positive count, ASCII whitespace only.
// Reject overflow, extra fields, signs, embedded NULs and captures exceeding the byte cap.
bool parseAudioPcmConfig(const char*,std::size_t,std::uint32_t framesPerBlock,AudioPcmConfig&);
enum class AudioPcmResult : std::uint8_t {
    disabled,armed,configReadFailed,invalidConfig,allocationFailed,insufficientSpace,writeFailed,written
};
const char* audioPcmResultName(AudioPcmResult);
struct AudioPcmHealth {
    std::uint64_t filled{},submitted{},completed{},underruns{};
    std::uint64_t captured{},captureSubmitted{},captureCompleted{};
    std::uint64_t nonzero[2]{},fullScale[2]{};
    std::uint32_t peak[2]{};
};
// Ownership: initialize on startup owner, transfer to sole mixer writer when worker starts,
// then transfer back ONLY after worker join. Never read health/buffer concurrently with fill.
// No allocation, file operations or PCM scan outside the selected capture interval in filled().
class AudioPcmCapture {
public:
    AudioPcmCapture()=default;
    ~AudioPcmCapture();
    AudioPcmCapture(const AudioPcmCapture&)=delete;
    AudioPcmCapture& operator=(const AudioPcmCapture&)=delete;
    AudioPcmResult initialize(const char* configPath,std::uint32_t framesPerBlock,std::uint32_t rate);
    std::uint64_t filled(const std::int16_t* interleaved);
    void submitted(std::uint64_t block);
    void completed(std::uint64_t block);
    void underrun(){++health_.underruns;}
    // Normal joined shutdown only. Exclusive files; partial failures retained for diagnosis.
    // Caller supplies actual free SD bytes. Both files are flushed and readback verified.
    AudioPcmResult finish(const char* wavPath,const char* jsonPath,std::uint64_t freeBytes,
                          AudioOutputState,std::uint32_t ndspResult);
    bool enabled()const{return result_==AudioPcmResult::armed;}
    AudioPcmResult result()const{return result_;}
    const AudioPcmHealth& healthAfterJoin()const{return health_;}
    std::size_t allocatedBytes()const{return capacity_;}
    std::uint32_t wavHash()const{return wavHash_;}
    std::uint32_t jsonHash()const{return jsonHash_;}
private:
    bool selected(std::uint64_t block)const;
    void release();
    unsigned char* buffer_{};
    std::size_t capacity_{};
    std::uint32_t frames_{},rate_{},wavHash_{},jsonHash_{};
    AudioPcmConfig config_{};
    AudioPcmHealth health_{};
    AudioPcmResult result_{AudioPcmResult::disabled};
};
}
