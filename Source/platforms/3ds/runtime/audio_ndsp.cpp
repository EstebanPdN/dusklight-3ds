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
#include <cstdint>
#include <span>
#include <cassert>
extern const unsigned char dusk_opendsp_component[];
extern const unsigned int dusk_opendsp_component_size;
namespace dusk::native {
// main.cpp: invalidations of a range only this process's CPU threads use are
// skipped (the kernel cache call bought nothing there).
void ignoreCacheInvalidation(const void* memory,std::uint32_t bytes);
std::uint32_t ignoredCacheInvalidations();
}
namespace {
using namespace dusk::audio;
// One block is the JAS frame: 7 subframes of 80 samples at the GameCube's
// 32 kHz (17.5 ms), or 10 at 48 kHz (16.7 ms, SD switch mixer-48k.txt).
// Set by Initialize before the worker starts; buffers hold the larger one.
constexpr unsigned maxSubframes=10,maxSamples=DSP_SUBFRAME_SIZE*maxSubframes,blocks=3;
unsigned subframes=7,samples=DSP_SUBFRAME_SIZE*7;
u64 blockTicks=1;
std::array<ndspWaveBuf,blocks> waves{};
OSThread worker{};
bool workerStarted{};
bool ndspAvailable{};
std::atomic<bool> stop{},paused{};
std::atomic<dusk::native::AudioOutputState> outputState{};
std::atomic<u32> outputResult{},filledBlocks{},submittedBlocks{},observedCompletedBlocks{},underruns{},mixTicks{};
// SetMasterVolume runs on game threads. The mixer copies the value before each
// subframe render, so MasterVolume is written only by the thread that renders.
std::atomic<float> pendingMasterVolume{1.0f};
dusk::native::AudioPcmCapture pcmCapture;
std::array<std::uint64_t,blocks> waveBlocks{};
std::array<bool,blocks> inFlight{};
// Mixer cost split, summed over a report window: waiting for the JAS lock,
// inside it (updateDSP and the external stream), rendering the voices outside
// it, and converting/flushing the PCM. Startup owner, then the sole worker.
struct WorkerSplit {
    u64 lockWait{},update{},render{},output{},longestFill{},longestSilence{};
    u32 blocks{},underruns{};
} split;
// Estimated time the queued audio runs out (last submission plus the blocks
// then in the device): an upper bound, so a reported silence is a lower bound.
u64 expectedDry{};
// Where the mixer runs (SD switch audio-core.txt, read by Initialize):
//   auto (no file, the default) or 3: core 3, else core 2;
//   1 [percent]: core 1 under APT_SetAppCpuTimeLimit (40 if not given), else core 2;
//   2: core 2 without any check, as up to Alpha 14.
// Core 2 holds the GX worker (priority 12, 0x27) below the mixer (6, 0x20):
// the mixer took 10-15 % of it per 17.5 ms block in Azahar's village and
// ranch, and preempted the worker for it. Nothing of the game runs on core 3.
// Core 1 runs the system modules; the application gets at most its APT share
// there, and one thread only (libctru thread.h). Console probe (Alpha 11):
// 0.30 / 0.50 / 0.79 of core 0 at 30 / 50 / 80 %, while GSP requests from
// core 0 took 53 / 994 / 1003 us (19 idle) against a busy thread on core 1.
// So auto never picks core 1: at 30 % the console's ranch minigame mix (about
// 30 % of a core, estimated from Alpha 13's 48 % at 48 kHz in float) would
// not fit, and with more every GSP request of the game thread and of the GX
// worker may wait about 1 ms while the mixer runs.
int workerCore{-3}; // the core the worker was made on; -2 the title's core, -3 none
u32 limitBefore{};bool limitSet{};
void restoreCpuLimit(){
    if(!limitSet)return;
    // As the core probe does: a console that had none keeps the smallest one.
    const Result restored=APT_SetAppCpuTimeLimit(limitBefore>=5?limitBefore:5);
    OSReport("Native audio: APT CPU time limit back to %lu (result %08lx)\n",static_cast<unsigned long>(limitBefore>=5?limitBefore:5),
        static_cast<unsigned long>(static_cast<u32>(restored)));
    limitSet=false;
}
// A thread made on `core`, below the caller's priority, sets a flag while the
// caller spins without yielding. Had the kernel put it on the caller's core it
// could not run before the caller stops, so the flag proves that another core
// runs it at the same time. svcGetProcessorID is no proof: Azahar answers 0
// on every core (Alpha 14), and libctru documents core 3 as closed to
// applications although the owner's console ran a probe thread there.
std::atomic<bool> checkRan{};std::atomic<s32> checkSays{-1};
struct CoreCheck {bool created{},ran{};s32 says{-1};double ms{};};
CoreCheck checkCore(int core){
    CoreCheck result;
    s32 own=0x30;
    if(R_FAILED(svcGetThreadPriority(&own,CUR_THREAD_HANDLE)))own=0x30;
    checkRan.store(false,std::memory_order_relaxed);checkSays.store(-1,std::memory_order_relaxed);
    const u64 started=svcGetSystemTick(),deadline=started+SYSCLOCK_ARM11/50; // 20 ms
    const Thread thread=threadCreate([](void*){
        checkSays.store(svcGetProcessorID(),std::memory_order_relaxed);checkRan.store(true,std::memory_order_release);
    },nullptr,4096,own+8<0x3f?own+8:0x3f,core,false);
    if(!thread)return result;
    result.created=true;
    while(!checkRan.load(std::memory_order_acquire)&&svcGetSystemTick()<deadline){}
    result.ran=checkRan.load(std::memory_order_acquire);
    result.ms=double(svcGetSystemTick()-started)*1000.0/SYSCLOCK_ARM11;
    // On the caller's core it runs now that the caller waits. The kernel
    // handle, not threadJoin: libctru marks a thread finished before it exits.
    if(svcWaitSynchronization(threadGetHandle(thread),100000000)==0)threadFree(thread);
    else threadDetach(thread);
    result.says=checkSays.load(std::memory_order_relaxed);
    return result;
}
struct Placement {int core{2};char why[200]{};};
Placement choosePlacement(){
    Placement placement;
    char text[24]{};
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/audio-core.txt","rb")){(void)std::fread(text,1,sizeof(text)-1,file);std::fclose(file);}
    const char* at=text;
    while(*at==' '||*at=='\t'||*at=='\r'||*at=='\n')++at;
    // Anything but 1, 2 or 3 (no file, "auto", an unknown word) is auto.
    const int want=*at>='1'&&*at<='3'?*at-'0':0;
    const char* asked=want==1?"1":want==2?"2":want==3?"3":"auto";
    auto& why=placement.why;
    if(want==2){std::snprintf(why,sizeof(why),"audio-core.txt 2, as up to Alpha 14");return placement;}
    if(want==1){
        unsigned limit=40,value=0;
        if(std::sscanf(at+1,"%u",&value)==1)limit=std::clamp(value,5u,80u);
        u32 before=0;
        const Result got=APT_GetAppCpuTimeLimit(&before);
        const Result set=APT_SetAppCpuTimeLimit(limit);
        if(R_FAILED(set)){
            std::snprintf(why,sizeof(why),"audio-core.txt 1: APT_SetAppCpuTimeLimit(%u) refused (%08lx)",limit,static_cast<unsigned long>(static_cast<u32>(set)));
            return placement;
        }
        limitBefore=R_SUCCEEDED(got)?before:0;limitSet=true;
        const auto check=checkCore(1);
        if(check.ran){
            placement.core=1;
            std::snprintf(why,sizeof(why),"audio-core.txt 1: APT CPU time limit %u %% (was %lu); a thread there ran beside this one after %.2f ms "
                "(svcGetProcessorID %ld)",limit,static_cast<unsigned long>(limitBefore),check.ms,static_cast<long>(check.says));
            return placement;
        }
        restoreCpuLimit();
        std::snprintf(why,sizeof(why),"audio-core.txt 1: core 1 %s",check.created?"made a thread that did not run beside this one within 20 ms":"refused a thread");
        return placement;
    }
    const auto check=checkCore(3);
    if(check.ran){
        placement.core=3;
        std::snprintf(why,sizeof(why),"audio-core.txt %s: a thread there ran beside this one after %.2f ms (svcGetProcessorID %ld); core 2 is the GX worker's",
            asked,check.ms,static_cast<long>(check.says));
    }else if(check.created)
        std::snprintf(why,sizeof(why),"audio-core.txt %s: core 3 made a thread that did not run beside this one within 20 ms (svcGetProcessorID %ld)",
            asked,static_cast<long>(check.says));
    else std::snprintf(why,sizeof(why),"audio-core.txt %s: core 3 refused a thread",asked);
    return placement;
}
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
// Starvation is checked when each block is queued: nothing given to NDSP
// before it is still queued or playing, so the output ran dry. The old check
// looked only at the top of the worker loop, so one fill lasting hundreds of
// milliseconds (Alpha 6: 363 of them, after the SD log flushes) went unseen.
void submitChecked(unsigned index,std::uint64_t block,u64 fillTicks){
    const auto now=svcGetSystemTick();
    unsigned inDevice=0;
    for(const auto& wave:waves)inDevice+=wave.status!=NDSP_WBUF_DONE&&wave.status!=NDSP_WBUF_FREE;
    if(!inDevice&&submittedBlocks.load(std::memory_order_relaxed)){
        ++underruns;++split.underruns;pcmCapture.underrun();
        const u64 silent=now>expectedDry?now-expectedDry:0;
        split.longestSilence=std::max(split.longestSilence,silent);
        const auto count=underruns.load(std::memory_order_relaxed);
        if(count<=8||count%32==0)OSReport("Native audio underrun %lu: NDSP ran dry before block %llu; this fill %.1f ms, silent for at least %.0f ms\n",
            static_cast<unsigned long>(count),static_cast<unsigned long long>(block),fillTicks*1000.0/SYSCLOCK_ARM11,silent*1000.0/SYSCLOCK_ARM11);
    }
    submit(index,block);
    expectedDry=now+u64(inDevice+1)*blockTicks;
}
// Two lines per report window from the worker itself, rate-limited (A0).
// Each stays under OSReport's 255 characters (bootstrap.cpp).
constexpr u32 reportBlocks=600; // 10 s of audio
dusk::native::LockWaitTotals reportedWaits{};u32 reportedSkips{};
void reportWorker(){
    const auto waits=dusk::native::watchedLockWaits();const auto skips=dusk::native::ignoredCacheInvalidations();
    const double perBlock=1000.0/(double(SYSCLOCK_ARM11)*split.blocks);
    // The share of the core: the four parts against the block's playing time
    // (blocks are 17.5 ms at 32 kHz, 16.7 ms at 48 kHz).
    const double share=100.0*double(split.lockWait+split.update+split.render+split.output)/(double(blockTicks)*split.blocks);
    OSReport("Native audio worker %lu blocks: JAS lock wait %.3f update %.3f render %.3f output %.3f ms per block; longest fill %.1f ms; "
        "underruns %lu, longest silence >= %.0f ms; %.1f %% of a %.1f ms block\n",
        static_cast<unsigned long>(split.blocks),split.lockWait*perBlock,split.update*perBlock,split.render*perBlock,split.output*perBlock,
        split.longestFill*1000.0/SYSCLOCK_ARM11,static_cast<unsigned long>(split.underruns),split.longestSilence*1000.0/SYSCLOCK_ARM11,
        share,blockTicks*1000.0/SYSCLOCK_ARM11);
    // SD switch mixer-verify.txt: every subframe mixed through both paths.
    if(DspVerifyMix&&DspFixedMix)OSReport("Native audio mixer verify: %llu subframes fixed point against float, %llu differ; largest %.3f/32768, %llu samples over 1/32768\n",
        static_cast<unsigned long long>(DspVerifiedSubframes),static_cast<unsigned long long>(DspVerifyMismatches),
        double(DspVerifyLargestStep),static_cast<unsigned long long>(DspVerifySamplesOverStep));
    else if(DspVerifyMix)OSReport("Native audio mixer verify: %llu subframes through both mix paths, %llu with a sample that differs (must stay 0)\n",
        static_cast<unsigned long long>(DspVerifiedSubframes),static_cast<unsigned long long>(DspVerifyMismatches));
    // Waits of every thread for the JAS lock in the same window: main is the
    // game thread, workers include this mixer.
    OSReport("Native audio JAS lock waits: main %lu (%.2f ms) workers %lu (%.2f ms); channel-table invalidations skipped %lu\n",
        static_cast<unsigned long>(waits.mainWaits-reportedWaits.mainWaits),(waits.mainNanoseconds-reportedWaits.mainNanoseconds)/1e6,
        static_cast<unsigned long>(waits.workerWaits-reportedWaits.workerWaits),(waits.workerNanoseconds-reportedWaits.workerNanoseconds)/1e6,
        static_cast<unsigned long>(skips-reportedSkips));
    reportedWaits=waits;reportedSkips=skips;split={};
}
void watchJasLock(){
#if defined(__3DS__)
    // The JAS exclusion is the only OSMutex this worker owns here, so it heads
    // the thread's owned-mutex list (os_sync.cpp links a mutex on first entry).
    JASCriticalSection section;
    auto* self=OSGetCurrentThread();
    const bool found=self&&self->queueMutex.head&&self->queueMutex.head==self->queueMutex.tail;
    if(found)dusk::native::watchLockWaits(&self->queueMutex.head->nativeLock);
    OSReport("Native audio worker: voices render outside the JAS lock; lock wait accounting %s\n",found?"on":"unavailable");
#endif
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
// At 32 kHz the movie's PCM (32 kHz, as on the GameCube) is mixed 1:1, one
// subframe per call, as JASDriver::mixInterleaveTrack took it per frame.
std::array<f32, DSP_SUBFRAME_SIZE * kExtChannels> extDirect;


std::uint64_t fill(ndspWaveBuf& wave,u64* fillTicks=nullptr){
    const auto started=svcGetSystemTick();
    auto* pcm=static_cast<s16*>(wave.data_pcm16);
    u64 lockWait=0,update=0,render=0,output=0;
    {
        JASCriticalSection section;
        if(JASDriver::getSubFrames()!=subframes)dusk::native::fatal("Native audio subframe count changed");
        JASAudioThread::setDSPSyncCount(subframes);
    }
    for(unsigned at=0;at<subframes;++at){
        // The original audio thread took its exclusion once per DSP subframe
        // for the CPU side (updateDSP) while the GameCube DSP mixed the voices
        // by itself. Only that CPU side is inside the lock here: the main
        // thread used to wait for whole voice renders (Alpha 6: up to 25 ms of
        // mDoAud_Execute in Hyrule Field). DspRender reads and writes the DSP
        // channel table and ChannelAux, which only this thread touches
        // (updateDSP runs here too; no game thread writes JASDsp::CH_BUF), and
        // wave memory, whose loads never took this lock. Its mixer settings
        // are written before the worker starts (reverb, HRTF, channels) or
        // copied here (MasterVolume).
        std::span<const f32> extra;
        const auto waitStart=svcGetSystemTick();
        {
            JASCriticalSection section;
            const auto updateStart=svcGetSystemTick();lockWait+=updateStart-waitStart;
            JASDriver::updateDSP();
            // The movie's mix callback reads game-thread buffers: inside the lock.
            if(JASDriver::extMixCallback&&JASDriver::sMixMode==MIX_MODE_INTERLEAVE){
                if(RenderRate==32000){
                    if(auto* data=JASDriver::extMixCallback(DSP_SUBFRAME_SIZE)){
                        for(unsigned i=0;i<extDirect.size();++i)extDirect[i]=data[i]/32768.0f;
                        extra=extDirect;
                    }
                }else{
                    if(extResampler.hungry())if(auto* data=JASDriver::extMixCallback(kExtSrcInSampleCount))extResampler.feed(std::span{data,kExtSrcInSampleCount*kExtChannels});
                    if(!extResampler.hungry())extra=extResampler.drain_subframe();
                }
            }
            --JASAudioThread::snIntCount;
            update+=svcGetSystemTick()-updateStart;
        }
        const auto renderStart=svcGetSystemTick();
        MasterVolume=pendingMasterVolume.load(std::memory_order_relaxed);
        OutputSubframe mixed{};DspRender(mixed);
        const auto renderEnd=svcGetSystemTick();render+=renderEnd-renderStart;
        // nativePcm16((mixed + extra) * gain) for every sample: the product
        // by gain * 32768 (a power-of-two scale of the same rounding),
        // truncated, then saturated in integers. The VFP compares of the
        // clamp and of a finiteness test per sample cost more than the mix
        // of a voice; a nonfinite sample still makes the subframe's sum
        // nonfinite and stops the game as before.
        const float scale=dusk::native::nativeDspMixerLevel()*32768.0f;
        float sum=0;
        auto* out=pcm+at*DSP_SUBFRAME_SIZE*2;
        for(unsigned i=0;i<DSP_SUBFRAME_SIZE;++i)for(unsigned c=0;c<2;++c){
            const float value=mixed.channels[c][i]+(extra.empty()?0:extra[i*2+c]);
            sum+=value;
            out[i*2+c]=dusk::native::nativePcm16Scaled(value*scale);
        }
        if(!std::isfinite(sum))dusk::native::fatal("Nonfinite original software DSP output");
        output+=svcGetSystemTick()-renderEnd;
    }
    // Observe final mixed PCM before device submission, including the explicitly muted path.
    const auto flushStart=svcGetSystemTick();
    const auto block=pcmCapture.filled(pcm);++filledBlocks;
    if(ndspAvailable){
        // The DSP reads the block from RAM. A kernel cache call, not the DSP
        // service: DSP_FlushDataCache is an IPC to a system module on the
        // system core, next to the FS module writing the SD log, and fills
        // that included it lasted hundreds of milliseconds right after the
        // periodic log flush (Alpha 6, probable cause).
        const u32 bytes=samples*2*sizeof(s16);
        if(R_FAILED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE,static_cast<u32>(reinterpret_cast<std::uintptr_t>(pcm)),bytes))){
            static bool reported;
            if(!reported){reported=true;OSReport("Native audio: kernel cache flush refused; using the DSP service\n");}
            DSP_FlushDataCache(pcm,bytes);
        }
    }
    const auto finished=svcGetSystemTick();
    output+=finished-flushStart;
    mixTicks.fetch_add(static_cast<u32>(finished-started),std::memory_order_relaxed);
    split.lockWait+=lockWait;split.update+=update;split.render+=render;split.output+=output;
    split.longestFill=std::max(split.longestFill,u64(finished-started));++split.blocks;
    if(fillTicks)*fillTicks=finished-started;
    return block;
}
void* run(void*){
    JKRHeap::setCurrentHeap(JKRHeap::getSystemHeap());
    // Set by Initialize before OSResumeThread (under the thread table's lock).
    OSReport("Native audio worker: running (made on core %d; svcGetProcessorID %ld)\n",workerCore,static_cast<long>(svcGetProcessorID()));
    watchJasLock();
    u64 nextBlock=svcGetSystemTick()+blockTicks;
    while(!stop.load(std::memory_order_acquire)){
        if(paused.load(std::memory_order_acquire)){
            // The paused device keeps at most `blocks` queued: a silence after
            // resuming is measured from here, not from before the pause.
            nextBlock=svcGetSystemTick()+blockTicks;expectedDry=svcGetSystemTick()+u64(blocks)*blockTicks;
            svcSleepThread(1000000);continue;
        }
        if(!ndspAvailable){
            // Keep the actual original JAS/software DSP update clock alive
            // when libctru reports no DSP component. PCM is explicitly muted;
            // no fictitious NDSP completion or output buffer is reported.
            const auto now=svcGetSystemTick();
            if(now>=nextBlock){fill(waves[0]);nextBlock=now+blockTicks;}
            if(split.blocks>=reportBlocks)reportWorker();
            svcSleepThread(1000000);continue;
        }
        for(unsigned i=0;i<blocks;++i){
            const auto status=waves[i].status;
            observeCompletion(i,status);
            if(status==NDSP_WBUF_DONE||status==NDSP_WBUF_FREE){
                u64 fillTicks=0;const auto block=fill(waves[i],&fillTicks);
                submitChecked(i,block,fillTicks);
            }
        }
        if(split.blocks>=reportBlocks)reportWorker();
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
    OutChannelCount=2;EnableHrtf=false;EnableReverb=false;DumpAudio=false;
    // The mixer renders at the GameCube's 32 kHz (JAS output rate 0: 7
    // subframes per 17.5 ms block, pitches and tempos from the same rate) in
    // fixed point, each voice straight into the two output channels
    // (DuskDsp.hpp). SD switches: mixer-48k.txt renders at 48 kHz (10
    // subframes, Alpha 13); mixer-float.txt mixes in float (Alpha 13);
    // mixer-old.txt is both, the Alpha 13 mixer. mixer-plain.txt keeps the
    // float path with a buffer per voice; mixer-verify.txt renders every
    // subframe through two paths and compares them (fixed against float, or
    // float straight against float buffered).
    {
        const auto present=[](const char* path){auto* file=std::fopen(path,"rb");if(file)std::fclose(file);return file!=nullptr;};
        const bool oldMixer=present("sdmc:/3ds/DuskLight/mixer-old.txt");
        const bool rate48=oldMixer||present("sdmc:/3ds/DuskLight/mixer-48k.txt");
        DspPlainMix=present("sdmc:/3ds/DuskLight/mixer-plain.txt");
        DspFixedMix=!DspPlainMix&&!oldMixer&&!present("sdmc:/3ds/DuskLight/mixer-float.txt");
        DspVerifyMix=!DspPlainMix&&present("sdmc:/3ds/DuskLight/mixer-verify.txt");
        JASDriver::setOutputRate(rate48?static_cast<JASOutputRate>(-1):OUTPUT_RATE_0);
        RenderRate=rate48?48000:32000;
        subframes=JASDriver::getSubFrames();samples=DSP_SUBFRAME_SIZE*subframes;
        if(subframes>maxSubframes||JASDriver::getDacRate()!=RenderRate)dusk::native::fatal("Native audio rate and JAS output rate differ");
        blockTicks=(u64(samples)*SYSCLOCK_ARM11+RenderRate-1)/RenderRate;
        OSReport("Native audio mixer: %d Hz, %u subframes per %.1f ms block; %s%s\n",RenderRate,subframes,samples*1000.0/RenderRate,
            DspPlainMix?"float, a buffer per voice (mixer-plain.txt)":DspFixedMix?"fixed point, voices straight into the stereo mix":
            "float, voices straight into the stereo mix",
            DspVerifyMix?(DspFixedMix?"; every subframe compared with the float path (mixer-verify.txt)":
                "; every subframe compared with the buffered path (mixer-verify.txt)"):"");
    }
    DspInit();JASDsp::initBuffer();JASDSPChannel::initAll();
    // updateDSP invalidates the whole DSP channel table every subframe (the
    // GameCube DSP wrote it by DMA): 600 kernel cache calls a second inside
    // the JAS lock. Here only this process's CPU threads use the table and the
    // cores' data caches are coherent, so main.cpp skips that range.
    dusk::native::ignoreCacheInvalidation(JASDsp::CH_BUF,sizeof(JASDsp::TChannel)*DSP_CHANNELS);
    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);
    if(ndspAvailable){
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);ndspChnReset(0);ndspChnSetInterp(0,NDSP_INTERP_LINEAR);ndspChnSetRate(0,static_cast<float>(RenderRate));ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16);
        float mix[12]={1,1};ndspChnSetMix(0,mix);
    }
    filledBlocks=0;submittedBlocks=0;observedCompletedBlocks=0;underruns=0;mixTicks=0;inFlight={};split={};
    const auto captureResult=pcmCapture.initialize(dusk::native::audioPcmConfigPath,samples,static_cast<u32>(RenderRate));
    if(captureResult!=dusk::native::AudioPcmResult::disabled)
        OSReport("Native PCM observation: %s; pre-device mixer only; config %s; CPU bytes %lu\n",
                 dusk::native::audioPcmResultName(captureResult),dusk::native::audioPcmConfigPath,
                 static_cast<unsigned long>(pcmCapture.allocatedBytes()));
    // Before the first blocks are queued: the check may spin up to 20 ms.
    const auto placement=choosePlacement();
    for(unsigned i=0;i<(ndspAvailable?blocks:1u);++i){
        auto& wave=waves[i];wave={};wave.data_pcm16=static_cast<s16*>(linearAlloc(maxSamples*2*sizeof(s16)));
        if(!wave.data_pcm16)dusk::native::fatal("Native PCM buffer allocation failed");
        wave.nsamples=samples;const auto block=fill(wave);
        if(ndspAvailable)submit(i,block);
    }
    expectedDry=svcGetSystemTick()+u64(ndspAvailable?blocks:0u)*blockTicks;
    stop.store(false);paused.store(false);
    // The same priority on every core (6, 0x20). On core 3 nothing of the
    // game shares it; on core 2 the GX worker (0x27) and the DVD thread
    // (0x2d) stay below it, as before.
    int core=placement.core;
    bool made=core!=2&&dusk::native::createThreadOnCore(&worker,run,nullptr,nullptr,64*1024,6,0,core);
    if(core!=2&&!made){
        OSReport("Native audio worker: core %d refused the mixer after its check; core 2 instead\n",core);
        if(core==1)restoreCpuLimit();
        core=2;
    }
    if(!made&&!(made=dusk::native::createThreadOnCore(&worker,run,nullptr,nullptr,64*1024,6,0,2))){
        OSReport("Native audio core2 creation unavailable; falling back to title core\n");
        if(!OSCreateThread(&worker,run,nullptr,nullptr,64*1024,6,0))dusk::native::fatal("Native audio worker creation failed");
        core=-2;
    }
    workerCore=core;
    if(core==2&&placement.core==2)OSReport("Native audio worker created with requested core2 affinity (%s)\n",placement.why);
    else if(core==2)OSReport("Native audio worker created with requested core2 affinity\n");
    else if(core>=0)OSReport("Native audio worker created on core %d (%s)\n",core,placement.why);
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
    // OSJoinThread waits on the kernel thread handle: the thread has exited,
    // not only left its loop, before its stack is freed.
    if(!OSJoinThread(&worker,nullptr))dusk::native::fatal("Native audio worker join failed");
    OSReport("Native audio worker: joined (made on core %d)\n",workerCore);
    workerStarted=false;workerCore=-3;
    restoreCpuLimit(); // core 1 only
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
// Reverb stays off on 3DS (cleared before the worker starts). Nothing else
// writes the flags the voice render reads outside the JAS lock.
void dusk::audio::SetEnableReverb(bool){}
// Applied by the mixer before its next subframe render (see fill).
void dusk::audio::SetMasterVolume(f32 value){pendingMasterVolume.store(std::clamp(value,0.0f,1.0f),std::memory_order_relaxed);}
void dusk::audio::Reinitialize(){if(!workerStarted)Initialize();}
u32 dusk::audio::GetResetCount(int index){return index>=0&&index<DSP_CHANNELS?ChannelAux[index].resetCount:0;}
f32 dusk::audio::VolumeFromU16(u16 value){return static_cast<f32>(value)/JASDriver::getChannelLevel_dsp();}
int dusk::native::nativeAudioWorkerCore(){return workerCore;}
dusk::native::AudioRuntimeStatus dusk::native::nativeAudioStatus(){
    return {outputState.load(std::memory_order_acquire),outputResult.load(std::memory_order_acquire),
            filledBlocks.load(std::memory_order_acquire),underruns.load(std::memory_order_acquire),mixTicks.load(std::memory_order_relaxed),
            submittedBlocks.load(std::memory_order_acquire),observedCompletedBlocks.load(std::memory_order_acquire)};
}
