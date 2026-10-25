#include <cstdio>
#include "dusk/audio/DuskAudioSystem.h"
#include "dusk/audio/DuskDsp.hpp"
#include "audio_control.hpp"
#include "bootstrap.hpp"
#include "os_thread.hpp"
#include "JSystem/JAudio2/JASAiCtrl.h"
#include "JSystem/JAudio2/JASAudioThread.h"
#include "JSystem/JAudio2/JASChannel.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JAudio2/JASDriverIF.h"
#include "JSystem/JAudio2/JASDSPChannel.h"
#include "JSystem/JAudio2/JASHeapCtrl.h"
#include "JSystem/JKernel/JKRHeap.h"
#include <3ds.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <span>
#include <cassert>
extern const unsigned char dusk_opendsp_component[];
extern const unsigned int dusk_opendsp_component_size;
namespace {
using namespace dusk::audio;
constexpr unsigned subframes=10,samples=DSP_SUBFRAME_SIZE*subframes,blocks=3;
std::array<ndspWaveBuf,blocks> waves{};
OSThread worker{};
bool workerStarted{};
bool ndspAvailable{};
std::atomic<bool> stop{},paused{};
std::atomic<dusk::native::AudioOutputState> outputState{};
std::atomic<u32> outputResult{},filledBlocks{},submittedBlocks{},observedCompletedBlocks{},underruns{},mixTicks{};
dusk::native::AudioPcmCapture pcmCapture;
std::array<std::uint64_t,blocks> waveBlocks{};
std::array<bool,blocks> inFlight{};
// Only startup owner, sole worker, then joined shutdown owner touch capture/tag state.
void observeCompletion(unsigned index,unsigned status){
    if(inFlight[index]&&status==NDSP_WBUF_DONE){
        pcmCapture.completed(waveBlocks[index]);++observedCompletedBlocks;inFlight[index]=false;
    }
}
void submit(unsigned index,std::uint64_t block){
    ndspChnWaveBufAdd(0,&waves[index]);
    waveBlocks[index]=block;inFlight[index]=true;
    pcmCapture.submitted(block);++submittedBlocks;
}
constexpr auto kExtChannels = 2;
constexpr auto kExtSrcInSampleCount = DSP_SUBFRAME_SIZE * 2;
constexpr auto kExtSrcOutSampleCount = DSP_SUBFRAME_SIZE * 3;

// simple 3:2 resampler
struct LinearResampler {
    f32 prev = 0.0f;
    int phase = 0;

    void resample(std::span<const s16> in, std::span<f32> out, int skip = 1, int offset = 0) {
        const auto inSize = in.size() / skip;
        const auto outSize = out.size() / skip;
        assert(inSize * 3 == outSize * 2);

        int idx = -1;
        int phi = phase;
        int n = 0;

        while (n < outSize) {
            f32 s0 = (idx < 0) ? prev : in[idx * skip + offset] / 32768.0f;
            f32 s1 = in[idx * skip + skip + offset] / 32768.0f;
            f32 frac = phi * (1.0f / 3.0f);

            out[n++ * skip + offset] = s0 + frac * (s1 - s0);

            phi += 2;
            if (phi >= 3) {
                phi -= 3;
                idx++;
            }
        }

        prev = in[(inSize - 1) * skip + offset] / 32768.0f;
        phase = phi;
    }
};

static struct {
    std::array<f32, kExtSrcOutSampleCount * kExtChannels> buf;
    int available = 0;
    LinearResampler leftSrc;
    LinearResampler rightSrc;

    bool hungry() const { return available <= 0; }

    void feed(std::span<const s16> in) {
        const auto out = std::span{buf};
        leftSrc.resample(in, out, kExtChannels, 0);
        rightSrc.resample(in, out, kExtChannels, 1);
        available = buf.size();
    }

    std::span<const f32> drain_subframe() {
        constexpr auto kDrainSize = DSP_SUBFRAME_SIZE * kExtChannels;
        assert(available >= kDrainSize);
        const auto rv = std::span{&buf[buf.size() - available], kDrainSize};
        available -= kDrainSize;
        return rv;
    }
} extResampler;


std::uint64_t fill(ndspWaveBuf& wave){
    const auto started=svcGetSystemTick();
    auto* pcm=static_cast<s16*>(wave.data_pcm16);
    JASCriticalSection section;
    if(JASDriver::getSubFrames()!=subframes)dusk::native::fatal("Native audio subframe count changed");
    JASAudioThread::setDSPSyncCount(subframes);
    for(unsigned at=0;at<subframes;++at){
        OutputSubframe output{};JASDriver::updateDSP();DspRender(output);
        std::span<const f32> extra;
        if(JASDriver::extMixCallback&&JASDriver::sMixMode==MIX_MODE_INTERLEAVE){
            if(extResampler.hungry())if(auto* data=JASDriver::extMixCallback(kExtSrcInSampleCount))extResampler.feed(std::span{data,kExtSrcInSampleCount*kExtChannels});
            if(!extResampler.hungry())extra=extResampler.drain_subframe();
        }
        const float gain=dusk::native::nativeDspMixerLevel();
        for(unsigned i=0;i<DSP_SUBFRAME_SIZE;++i)for(unsigned c=0;c<2;++c){
            float value=(output.channels[c][i]+(extra.empty()?0:extra[i*2+c]))*gain;
            if(!std::isfinite(value))dusk::native::fatal("Nonfinite original software DSP output");
            pcm[(at*DSP_SUBFRAME_SIZE+i)*2+c]=dusk::native::nativePcm16(value);
        }
        --JASAudioThread::snIntCount;
    }
    // Observe final mixed PCM before device submission, including the explicitly muted path.
    const auto block=pcmCapture.filled(pcm);++filledBlocks;
    if(ndspAvailable)DSP_FlushDataCache(pcm,samples*2*sizeof(s16));
    mixTicks.fetch_add(static_cast<u32>(svcGetSystemTick()-started),std::memory_order_relaxed);
    return block;
}
void* run(void*){
    JKRHeap::setCurrentHeap(JKRHeap::getSystemHeap());
    const u64 blockTicks=(u64(samples)*SYSCLOCK_ARM11+SampleRate-1)/SampleRate;
    u64 nextBlock=svcGetSystemTick()+blockTicks;
    while(!stop.load(std::memory_order_acquire)){
        if(paused.load(std::memory_order_acquire)){
            nextBlock=svcGetSystemTick()+blockTicks;svcSleepThread(1000000);continue;
        }
        if(!ndspAvailable){
            // Keep the actual original JAS/software DSP update clock alive
            // when libctru reports no DSP component. PCM is explicitly muted;
            // no fictitious NDSP completion or output buffer is reported.
            const auto now=svcGetSystemTick();
            if(now>=nextBlock){fill(waves[0]);nextBlock=now+blockTicks;}
            svcSleepThread(1000000);continue;
        }
        unsigned ready=0;
        for(auto& wave:waves)if(wave.status==NDSP_WBUF_DONE||wave.status==NDSP_WBUF_FREE)++ready;
        // Queue-empty observations are a starvation proxy, not a measured device underrun duration.
        if(ready==blocks&&submittedBlocks.load(std::memory_order_relaxed)){++underruns;pcmCapture.underrun();}
        for(unsigned i=0;i<blocks;++i){
            const auto status=waves[i].status;
            observeCompletion(i,status);
            if(status==NDSP_WBUF_DONE||status==NDSP_WBUF_FREE)submit(i,fill(waves[i]));
        }
        svcSleepThread(1000000);
    }
    JKRHeap::setCurrentHeap(nullptr);
    return nullptr;
}
}
void dusk::audio::Initialize(){
    if(workerStarted)dusk::native::fatal("Native audio initialized twice");
    // Persistent linked OpenDSP bytes avoid an SD firmware dependency. The build
    // verifies their pinned SHA-256; ordinary NDSP error handling remains intact.
    // Hardware order: audio-off switch, then the console's own dumped DSP
    // firmware (sdmc:/3ds/dspfirm.cdc, loaded by libctru), then embedded OpenDSP.
    bool audioOff=false,consoleFirmware=false;
    if(auto* f=std::fopen("sdmc:/3ds/DuskLight/audio-off.txt","rb")){std::fclose(f);audioOff=true;}
    if(auto* f=std::fopen("sdmc:/3ds/dspfirm.cdc","rb")){std::fclose(f);consoleFirmware=true;}
    OSReport("Native audio: init begin (audio-off=%d console dspfirm=%d)\n",int(audioOff),int(consoleFirmware));
    Result result=MAKERESULT(RL_PERMANENT,RS_NOTFOUND,RM_DSP,RD_NOT_FOUND);
    if(!audioOff){
        if(!consoleFirmware)ndspUseComponent(dusk_opendsp_component,dusk_opendsp_component_size,0xff,0xff);
        // Watchdog: a DSP that never answers must not stall the game's audio
        // startup forever. ndspInit runs on a helper; after 3 s the game goes on
        // muted and never touches NDSP again (the helper stays blocked).
        static std::atomic<int> initState{0};static Result initResult{};
        initState.store(0,std::memory_order_release);
        auto* helper=threadCreate([](void*){initResult=ndspInit();initState.store(1,std::memory_order_release);},nullptr,16*1024,0x2f,-2,true);
        if(!helper){result=ndspInit();}
        else{
            const u64 deadline=svcGetSystemTick()+3ull*SYSCLOCK_ARM11;
            while(!initState.load(std::memory_order_acquire)&&svcGetSystemTick()<deadline)svcSleepThread(5000000);
            if(initState.load(std::memory_order_acquire))result=initResult;
            else{
                OSReport("Native audio: ndspInit did not return within 3 s; continuing muted\n");
                result=MAKERESULT(RL_PERMANENT,RS_NOTFOUND,RM_DSP,RD_NOT_FOUND);
            }
        }
    }
    outputResult.store(static_cast<u32>(result),std::memory_order_release);
    OSReport("Native audio: %s; NDSP result %08lx\n",audioOff?"disabled by audio-off.txt":consoleFirmware?"console dspfirm.cdc":"embedded OpenDSP component",
        static_cast<unsigned long>(result));
    ndspAvailable=R_SUCCEEDED(result);
    if(!ndspAvailable){
        // libctru v2.7.0's exact missing-component result. Other service,
        // firmware-load and queue failures remain fatal with their real code.
        if(result!=MAKERESULT(RL_PERMANENT,RS_NOTFOUND,RM_DSP,RD_NOT_FOUND)){
            OSReport("Native NDSP initialization result %08lx\n",static_cast<unsigned long>(result));
            dusk::native::fatal("Native NDSP service initialization failed");
        }
        OSReport("Native audio muted: DSP component unavailable (%08lx); original software mixer retains a real clock\n",static_cast<unsigned long>(result));
    }
    JASDriver::setOutputRate(static_cast<JASOutputRate>(-1));
    OutChannelCount=2;EnableHrtf=false;EnableReverb=false;DumpAudio=false;
    DspInit();JASDsp::initBuffer();JASDSPChannel::initAll();
    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);
    if(ndspAvailable){
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);ndspChnReset(0);ndspChnSetInterp(0,NDSP_INTERP_LINEAR);ndspChnSetRate(0,SampleRate);ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16);
        float mix[12]={1,1};ndspChnSetMix(0,mix);
    }
    filledBlocks=0;submittedBlocks=0;observedCompletedBlocks=0;underruns=0;mixTicks=0;inFlight={};
    const auto captureResult=pcmCapture.initialize(dusk::native::audioPcmConfigPath,samples,SampleRate);
    if(captureResult!=dusk::native::AudioPcmResult::disabled)
        OSReport("Native PCM observation: %s; pre-device mixer only; config %s; CPU bytes %lu\n",
                 dusk::native::audioPcmResultName(captureResult),dusk::native::audioPcmConfigPath,
                 static_cast<unsigned long>(pcmCapture.allocatedBytes()));
    for(unsigned i=0;i<(ndspAvailable?blocks:1u);++i){
        auto& wave=waves[i];wave={};wave.data_pcm16=static_cast<s16*>(linearAlloc(samples*2*sizeof(s16)));
        if(!wave.data_pcm16)dusk::native::fatal("Native PCM buffer allocation failed");
        wave.nsamples=samples;const auto block=fill(wave);
        if(ndspAvailable)submit(i,block);
    }
    stop.store(false);paused.store(false);
    if(!dusk::native::createThreadOnCore(&worker,run,nullptr,nullptr,64*1024,6,0,2)){
        OSReport("Native audio core2 creation unavailable; falling back to title core\n");
        if(!OSCreateThread(&worker,run,nullptr,nullptr,64*1024,6,0))dusk::native::fatal("Native audio worker creation failed");
    }else OSReport("Native audio worker created with requested core2 affinity\n");
    workerStarted=true;
    outputState.store(ndspAvailable?dusk::native::AudioOutputState::ndsp:dusk::native::AudioOutputState::componentUnavailable,std::memory_order_release);
    OSResumeThread(&worker);
}
void dusk::audio::Shutdown(){
    if(!workerStarted)return;
    stop.store(true,std::memory_order_release);
    const auto deadline=svcGetSystemTick()+2*SYSCLOCK_ARM11;
    while(!OSIsThreadTerminated(&worker)){
        if(svcGetSystemTick()>=deadline)dusk::native::fatal("Native audio worker shutdown timeout");
        svcSleepThread(1000000);
    }
    if(!OSJoinThread(&worker,nullptr))dusk::native::fatal("Native audio worker join failed");
    workerStarted=false;
    if(ndspAvailable){
        // Final nonblocking observation before clearing any still-queued waves.
        for(unsigned i=0;i<blocks;++i)observeCompletion(i,waves[i].status);
        ndspChnWaveBufClear(0);ndspExit();
    }
    if(pcmCapture.enabled()){
        FS_ArchiveResource space{};
        const std::uint64_t freeBytes=R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&space))?
            std::uint64_t(space.clusterSize)*space.freeClusters:0;
        const auto result=pcmCapture.finish(dusk::native::audioPcmWavePath,dusk::native::audioPcmJsonPath,freeBytes,
            outputState.load(std::memory_order_acquire),outputResult.load(std::memory_order_acquire));
        const auto& health=pcmCapture.healthAfterJoin();
        OSReport("Native PCM observation shutdown: %s; captured %llu blocks; filled %llu queue calls %llu observed DONE %llu; WAV FNV %08lx JSON FNV %08lx\n",
            dusk::native::audioPcmResultName(result),static_cast<unsigned long long>(health.captured),
            static_cast<unsigned long long>(health.filled),static_cast<unsigned long long>(health.submitted),
            static_cast<unsigned long long>(health.completed),static_cast<unsigned long>(pcmCapture.wavHash()),
            static_cast<unsigned long>(pcmCapture.jsonHash()));
    }
    for(auto& wave:waves){linearFree(wave.data_pcm16);wave={};}
    ndspAvailable=false;outputState.store(dusk::native::AudioOutputState::uninitialized,std::memory_order_release);
}
void dusk::audio::SetPaused(bool value){paused.store(value,std::memory_order_release);if(outputState.load(std::memory_order_acquire)==dusk::native::AudioOutputState::ndsp)ndspChnSetPaused(0,value);}
void dusk::audio::SetEnableReverb(bool){JASCriticalSection section;EnableReverb=false;}
void dusk::audio::SetMasterVolume(f32 value){JASCriticalSection section;MasterVolume=std::clamp(value,0.0f,1.0f);}
void dusk::audio::Reinitialize(){if(!workerStarted)Initialize();}
u32 dusk::audio::GetResetCount(int index){return index>=0&&index<DSP_CHANNELS?ChannelAux[index].resetCount:0;}
f32 dusk::audio::VolumeFromU16(u16 value){return static_cast<f32>(value)/JASDriver::getChannelLevel_dsp();}
dusk::native::AudioRuntimeStatus dusk::native::nativeAudioStatus(){
    return {outputState.load(std::memory_order_acquire),outputResult.load(std::memory_order_acquire),
            filledBlocks.load(std::memory_order_acquire),underruns.load(std::memory_order_acquire),mixTicks.load(std::memory_order_relaxed),
            submittedBlocks.load(std::memory_order_acquire),observedCompletedBlocks.load(std::memory_order_acquire)};
}
