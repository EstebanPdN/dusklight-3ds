#include "audio_control.hpp"
#if defined(DUSK_TARGET_3DS)
#include <JSystem/JAudio2/JASAiCtrl.h>
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <atomic>
#include <cmath>
namespace {std::atomic<float> mixerLevel{4.f};}
float dusk::native::nativeDspMixerLevel(){return mixerLevel.load(std::memory_order_acquire)/4.f;}
void DsetMixerLevel(float value){
    if(!std::isfinite(value)||value<0||value>8)OSPanic(__FILE__,__LINE__,"Invalid native DSP mixer level");
    mixerLevel.store(value,std::memory_order_release);
}
extern "C" u32 AIGetDSPSampleRate(){
    // Original JAS retains the rate used by movie/external PCM resampling.
    return JASDriver::getDacRate()>40000?AI_SAMPLERATE_48KHZ:AI_SAMPLERATE_32KHZ;
}

#endif

#include "dump_io.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace dusk::native {
namespace {
bool space(char c){return c==' '||c=='\n'||c=='\r'||c=='\t'||c=='\v'||c=='\f';}
void put16(unsigned char* p,std::uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(unsigned char* p,std::uint32_t v){put16(p,v);put16(p+2,v>>16);}
}
bool parseAudioPcmConfig(const char* text,std::size_t size,std::uint32_t frames,AudioPcmConfig& out){
    if(!text||!frames||frames>(audioPcmMaxBytes-44)/4)return false;
    std::size_t at=0;std::uint32_t fields[2]{};
    for(auto& value:fields){
        while(at<size&&space(text[at]))++at;
        const auto begin=at;
        while(at<size&&text[at]>='0'&&text[at]<='9'){
            const auto digit=static_cast<unsigned>(text[at++]-'0');
            if(value>(UINT32_MAX-digit)/10)return false;
            value=value*10+digit;
        }
        if(at==begin||(at<size&&!space(text[at])))return false;
    }
    while(at<size&&space(text[at]))++at;
    if(at!=size||!fields[1]||fields[1]>(audioPcmMaxBytes-44)/(std::size_t(frames)*4))return false;
    out={fields[0],fields[1]};return true;
}
const char* audioPcmResultName(AudioPcmResult r){
    switch(r){
    case AudioPcmResult::disabled:return "disabled";
    case AudioPcmResult::armed:return "armed";
    case AudioPcmResult::configReadFailed:return "config_read_failed";
    case AudioPcmResult::invalidConfig:return "invalid_config";
    case AudioPcmResult::allocationFailed:return "allocation_failed";
    case AudioPcmResult::insufficientSpace:return "insufficient_space";
    case AudioPcmResult::writeFailed:return "write_failed";
    case AudioPcmResult::written:return "written_verified";
    }return "unknown";
}
void AudioPcmCapture::release(){std::free(buffer_);buffer_=nullptr;capacity_=0;}
AudioPcmCapture::~AudioPcmCapture(){release();}
AudioPcmResult AudioPcmCapture::initialize(const char* path,std::uint32_t frames,std::uint32_t rate){
    // Only before worker start (or after prior join); permits normal audio reinitialization.
    release();health_={};config_={};wavHash_=jsonHash_=0;frames_=frames;rate_=rate;
    result_=AudioPcmResult::disabled;
    if(!path){result_=AudioPcmResult::configReadFailed;return result_;}
    auto* file=std::fopen(path,"rb");
    if(!file){if(errno!=ENOENT)result_=AudioPcmResult::configReadFailed;return result_;}
    char text[96];const auto size=std::fread(text,1,sizeof(text),file);
    bool okay=!std::ferror(file);if(std::fclose(file))okay=false;
    if(!okay){result_=AudioPcmResult::configReadFailed;return result_;}
    if(size==sizeof(text)||!rate||rate>UINT32_MAX/4||!parseAudioPcmConfig(text,size,frames,config_)){
        result_=AudioPcmResult::invalidConfig;return result_;
    }
    capacity_=44+std::size_t(config_.blockCount)*frames*4;
    buffer_=static_cast<unsigned char*>(std::malloc(capacity_)); // ordinary CPU heap, never linearAlloc
    if(!buffer_){capacity_=0;result_=AudioPcmResult::allocationFailed;return result_;}
    result_=AudioPcmResult::armed;return result_;
}
bool AudioPcmCapture::selected(std::uint64_t block)const{
    return enabled()&&block>=config_.startBlock&&block- config_.startBlock<config_.blockCount;
}
std::uint64_t AudioPcmCapture::filled(const std::int16_t* pcm){
    const auto block=health_.filled++;
    if(!selected(block))return block;
    auto* dst=buffer_+44+std::size_t(health_.captured)*frames_*4;
    for(std::size_t i=0;i<std::size_t(frames_)*2;++i){
        const std::int32_t sample=pcm[i];const auto channel=i%2;
        const auto magnitude=static_cast<std::uint32_t>(sample<0?-sample:sample);
        health_.nonzero[channel]+=sample!=0;
        // Endpoint count includes legitimate full-scale PCM, not proof of float overdrive.
        health_.fullScale[channel]+=sample==-32768||sample==32767;
        health_.peak[channel]=std::max(health_.peak[channel],magnitude);
        put16(dst+i*2,static_cast<std::uint16_t>(sample));
    }
    ++health_.captured;return block;
}
void AudioPcmCapture::submitted(std::uint64_t block){++health_.submitted;if(selected(block))++health_.captureSubmitted;}
void AudioPcmCapture::completed(std::uint64_t block){++health_.completed;if(selected(block))++health_.captureCompleted;}
AudioPcmResult AudioPcmCapture::finish(const char* wavPath,const char* jsonPath,std::uint64_t freeBytes,
                                      AudioOutputState output,std::uint32_t ndspResult){
    if(!enabled())return result_; // disabled shutdown does no SD I/O
    const auto dataBytes=static_cast<std::uint32_t>(health_.captured*frames_*4);
    const auto bytes=44+std::size_t(dataBytes);
    if(!hasDumpSpace(freeBytes,bytes,8192)){
        result_=AudioPcmResult::insufficientSpace;release();return result_;
    }
    std::memcpy(buffer_,"RIFF",4);put32(buffer_+4,36+dataBytes);std::memcpy(buffer_+8,"WAVEfmt ",8);
    put32(buffer_+16,16);put16(buffer_+20,1);put16(buffer_+22,2);put32(buffer_+24,rate_);
    put32(buffer_+28,rate_*4);put16(buffer_+32,4);put16(buffer_+34,16);
    std::memcpy(buffer_+36,"data",4);put32(buffer_+40,dataBytes);
    unsigned char scratch[1024];
    if(!writeVerifiedDumpFile(wavPath,buffer_,bytes,scratch,sizeof(scratch),wavHash_)){
        result_=AudioPcmResult::writeFailed;release();return result_;
    }
    char json[4096];
    const auto count=std::snprintf(json,sizeof(json),
        "{\n\"schema\":1,\"observation\":\"pre_device_mixer\",\"host_playback_verified\":false,"
        "\"format\":\"signed16_le_interleaved_stereo\",\"sample_rate\":%u,\"frames_per_block\":%u,\n"
        "\"output\":\"%s\",\"ndsp_result\":%u,\"requested_start_block\":%u,\"requested_block_count\":%u,\n"
        "\"captured_blocks\":%llu,\"first_captured_block\":%llu,\"end_block_exclusive\":%llu,"
        "\"range_valid\":%s,\"block_clock\":\"zero_based_fill_order_including_initial_prefill_not_game_frames\",\n"
        "\"filled_blocks\":%llu,\"submitted_queue_calls\":%llu,\"observed_completed_blocks\":%llu,"
        "\"queue_empty_observations\":%llu,\"capture_submitted_queue_calls\":%llu,\"capture_observed_completed_blocks\":%llu,\n"
        "\"sample_values\":%llu,\"nonzero_samples\":[%llu,%llu],\"clipped_or_full_scale_samples\":[%llu,%llu],"
        "\"peak_abs_s16\":[%u,%u],\"wav_bytes\":%zu,\"wav_fnv1a32\":%u,\"wav_readback_verified\":true,\n"
        "\"limits\":\"NDSP queue calls and observed DONE states do not prove every sample reached hardware or host playback; endpoint counts include valid full scale; partial final queues may be cleared on shutdown\"\n}\n",
        rate_,frames_,output==AudioOutputState::ndsp?"ndsp":output==AudioOutputState::componentUnavailable?"muted_missing_component":"uninitialized",
        ndspResult,config_.startBlock,config_.blockCount,
        static_cast<unsigned long long>(health_.captured),static_cast<unsigned long long>(config_.startBlock),
        static_cast<unsigned long long>(std::uint64_t(config_.startBlock)+health_.captured),health_.captured?"true":"false",
        static_cast<unsigned long long>(health_.filled),static_cast<unsigned long long>(health_.submitted),
        static_cast<unsigned long long>(health_.completed),static_cast<unsigned long long>(health_.underruns),
        static_cast<unsigned long long>(health_.captureSubmitted),static_cast<unsigned long long>(health_.captureCompleted),
        static_cast<unsigned long long>(health_.captured*frames_*2),
        static_cast<unsigned long long>(health_.nonzero[0]),static_cast<unsigned long long>(health_.nonzero[1]),
        static_cast<unsigned long long>(health_.fullScale[0]),static_cast<unsigned long long>(health_.fullScale[1]),
        health_.peak[0],health_.peak[1],bytes,wavHash_);
    const bool okay=count>0&&std::size_t(count)<sizeof(json)&&
        writeVerifiedDumpFile(jsonPath,json,count,scratch,sizeof(scratch),jsonHash_);
    result_=okay?AudioPcmResult::written:AudioPcmResult::writeFailed;release();return result_;
}
}
