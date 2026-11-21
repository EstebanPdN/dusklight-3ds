#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using u16=std::uint16_t;using u32=std::uint32_t;using u64=std::uint64_t;using s16=std::int16_t;using s32=std::int32_t;using f32=float;using Result=std::int32_t;
using Handle=u32;
inline constexpr unsigned DSP_SUBFRAME_SIZE=80,DSP_CHANNELS=64,MIX_MODE_INTERLEAVE=3;
enum JASOutputRate { OUTPUT_RATE_0 };
// The core a host thread stands for: what svcGetProcessorID answers on it.
inline thread_local int hostCore=0;
// Cores whose thread creation the host refuses (bit per core): threadCreate
// and the test's createThreadOnCore both return failure for them.
inline unsigned hostRefusedCores{};
struct OSThread { std::thread native;std::atomic<bool> done{true};void*(*entry)(void*){};void* arg{};int core{-2}; };
inline bool OSCreateThread(OSThread* t,void*(*entry)(void*),void* arg,void*,u32,int,std::uint16_t){t->entry=entry;t->arg=arg;t->core=-2;return true;}
inline thread_local bool audioWorker=false;
inline void OSResumeThread(OSThread* t){t->done=false;t->native=std::thread([t]{audioWorker=true;hostCore=t->core<0?0:t->core;t->entry(t->arg);t->done.store(true,std::memory_order_release);});}
inline bool OSIsThreadTerminated(OSThread* t){return t->done.load(std::memory_order_acquire);}
inline bool OSJoinThread(OSThread* t,void*){t->native.join();return true;}
// Printed, and kept for the tests that look for a report line.
inline std::mutex reportMutex;inline std::string reports;
inline int OSReport(const char* fmt,...){
    char text[1024];va_list ap;va_start(ap,fmt);int n=std::vsnprintf(text,sizeof(text),fmt,ap);va_end(ap);
    std::fputs(text,stdout);std::lock_guard guard(reportMutex);reports+=text;return n;
}
inline bool reported(const char* text){std::lock_guard guard(reportMutex);return reports.find(text)!=std::string::npos;}
[[noreturn]] inline void OSPanic(const char*,int,const char* msg){throw std::runtime_error(msg);}
struct JASCriticalSection { inline static std::recursive_mutex mutex;JASCriticalSection(){mutex.lock();}~JASCriticalSection(){mutex.unlock();} };
// JAS output rate as JASAiCtrl.cpp sets it: rate 0 is 7 subframes at 32 kHz,
// any other 10 at 48 kHz. The device mocks check the worker against it.
inline unsigned hostSubFrames=7;inline float hostDacRate=32000;
namespace JASDriver {
inline unsigned getSubFrames(){return hostSubFrames;}
inline void setOutputRate(JASOutputRate rate){const bool rate0=static_cast<int>(rate)==0;hostSubFrames=rate0?7:10;hostDacRate=rate0?32000.f:48000.f;}
inline void updateDSP(){}inline float getDacRate(){return hostDacRate;}inline float getChannelLevel_dsp(){return 32768;}
inline s16*(*extMixCallback)(u32){};inline unsigned sMixMode=MIX_MODE_INTERLEAVE;
}
struct JASAudioThread { static void setDSPSyncCount(unsigned){} inline static int snIntCount=0; };
struct JASChannel {};
template<class T>struct JASPoolAllocObject_MultiThreaded { static void newMemPool(unsigned){} };
namespace JASDsp {struct TChannel {unsigned char bytes[0x180];};inline TChannel channels[DSP_CHANNELS];inline TChannel* CH_BUF=channels;inline void initBuffer(){}}
struct JASDSPChannel {static void initAll(){}};
struct JKRHeap {static JKRHeap* getSystemHeap(){static JKRHeap h;return &h;}static void setCurrentHeap(JKRHeap*){}};
namespace dusk::audio {
inline constexpr unsigned SampleRate=48000;
inline unsigned OutChannelCount{};inline bool EnableHrtf{},EnableReverb{},DumpAudio{};inline float MasterVolume{};
// The mixer's paths (DuskDsp.hpp): read from the SD switches by audio_ndsp.cpp.
inline bool DspPlainMix{},DspVerifyMix{};inline u64 DspVerifiedSubframes{},DspVerifyMismatches{};
inline int RenderRate=SampleRate;inline bool DspFixedMix{};inline float DspVerifyLargestStep{};inline u64 DspVerifySamplesOverStep{};
struct OutputSubframe { std::array<std::array<float,DSP_SUBFRAME_SIZE>,8> channels{}; };
inline void DspInit(){}
// Test hooks: one render can stall (a long mix), and the volume the render saw.
inline std::atomic<int> stallMilliseconds{};inline std::atomic<float> renderedVolume{-1};
inline void DspRender(OutputSubframe& out){
    if(const int ms=stallMilliseconds.exchange(0))std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    renderedVolume.store(MasterVolume);
    for(unsigned i=0;i<DSP_SUBFRAME_SIZE;++i){out.channels[0][i]=.25f;out.channels[1][i]=-.5f;}
}
struct ChannelAuxData {u32 resetCount{};};inline ChannelAuxData ChannelAux[DSP_CHANNELS];
void Initialize();void Shutdown();void SetPaused(bool);void Reinitialize();
void SetEnableReverb(bool);void SetMasterVolume(f32);u32 GetResetCount(int);f32 VolumeFromU16(u16);
}
inline constexpr int NDSP_WBUF_FREE=0,NDSP_WBUF_QUEUED=1,NDSP_WBUF_DONE=3;
inline constexpr int NDSP_OUTPUT_STEREO=1,NDSP_INTERP_LINEAR=1,NDSP_FORMAT_STEREO_PCM16=1;
inline constexpr u64 SYSCLOCK_ARM11=1000000000;
struct ndspWaveBuf {s16* data_pcm16{};u32 nsamples{};unsigned status{};};
inline std::vector<ndspWaveBuf*> queued;
// Real-time device (stall test): each queued block plays for its samples at the
// JAS rate after the one before it, instead of all finishing at the worker's next sleep.
inline std::atomic<bool> realtimeDevice{};inline std::vector<u64> playedAt;inline u64 deviceEnd{};
inline Result initResult{};
inline unsigned componentUses{},componentInitializations{};
inline void ndspUseComponent(const void* data,u32 size,u16 prog,u16 mem){
    assert(data&&size==202196&&prog==0xff&&mem==0xff);
    const auto* b=static_cast<const unsigned char*>(data);
    assert(b[0x100]=='D'&&b[0x101]=='S'&&b[0x102]=='P'&&b[0x103]=='1');
    ++componentUses;
}
inline std::atomic<bool> devicePaused{};
inline std::atomic<unsigned> linearAllocations{},liveLinear{},queueCalls{},clearCalls{},exits{},flushes{},spaceChecks{};
inline Result ndspInit(){assert(componentUses==componentInitializations+1);++componentInitializations;return initResult;}
inline void ndspExit(){++exits;}
inline void ndspSetOutputMode(int){}inline void ndspChnReset(int){}inline void ndspChnSetInterp(int,int){}
inline unsigned hostBlockSamples(){return hostSubFrames*DSP_SUBFRAME_SIZE;}
inline void ndspChnSetRate(int,float rate){assert(rate==hostDacRate&&rate==dusk::audio::RenderRate);}
inline void ndspChnSetFormat(int,int){}inline void ndspChnSetMix(int,float mix[12]){assert(mix[0]==1&&mix[1]==1);}
inline void ndspChnSetPaused(int,bool p){devicePaused=p;}
inline void ndspChnWaveBufAdd(int,ndspWaveBuf* w){
    assert(w->nsamples==hostBlockSamples());w->status=NDSP_WBUF_QUEUED;queued.push_back(w);++queueCalls;
    const u64 now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    deviceEnd=std::max(deviceEnd,now)+u64(hostBlockSamples())*1000000000/u64(hostDacRate);playedAt.push_back(deviceEnd);
}
inline void ndspChnWaveBufClear(int){queued.clear();playedAt.clear();deviceEnd=0;++clearCalls;}
inline void playUntil(u64 now){
    while(!queued.empty()&&playedAt.front()<=now){queued.front()->status=NDSP_WBUF_DONE;queued.erase(queued.begin());playedAt.erase(playedAt.begin());}
}
inline void* linearAlloc(std::size_t bytes){++linearAllocations;++liveLinear;return std::malloc(bytes);}
inline void linearFree(void* p){if(p)--liveLinear;std::free(p);}
// The mixer flushes its PCM with the kernel call; the DSP service is only the fallback.
inline std::atomic<unsigned> serviceFlushes{};
inline void DSP_FlushDataCache(void*,std::size_t){++serviceFlushes;}
inline constexpr u32 CUR_PROCESS_HANDLE=0xFFFF8001;
inline Result svcFlushProcessDataCache(u32 process,u32,u32 size){assert(process==CUR_PROCESS_HANDLE&&size==hostBlockSamples()*4);++flushes;return 0;}
inline u64 svcGetSystemTick(){
    const u64 now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(audioWorker&&realtimeDevice.load()&&!devicePaused.load())playUntil(now);
    return now;
}
inline void svcSleepThread(std::int64_t){
    // Fake device advances on the same worker, avoiding host races in mock wave status.
    if(audioWorker&&!devicePaused.load()&&!realtimeDevice.load()){for(auto* w:queued)w->status=NDSP_WBUF_DONE;queued.clear();playedAt.clear();}
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    if(audioWorker&&realtimeDevice.load())(void)svcGetSystemTick(); // the real-time device plays on
}
// The game runs ndspInit on a detached helper under a 3 s watchdog. The host
// runs the helper to completion inside threadCreate: a DSP that answers at once.
// The mixer's core check is such a thread too: it runs at once, as on a
// free core, and answers svcGetProcessorID with the core it was made on.
using Thread=struct HostThread*;
inline unsigned threadCreates{};inline std::vector<int> threadCores;
inline Thread threadCreate(void (*entry)(void*),void* arg,std::size_t,int,int core,bool){
    static int helper;
    if(core>=0&&(hostRefusedCores>>core&1))return nullptr;
    ++threadCreates;threadCores.push_back(core);
    const int was=hostCore;hostCore=core<0?0:core;entry(arg);hostCore=was;
    return reinterpret_cast<Thread>(&helper);
}
inline Handle threadGetHandle(Thread){return 0x1234;}
inline Result svcWaitSynchronization(Handle,std::int64_t){return 0;}
inline unsigned threadFrees{};inline void threadFree(Thread){++threadFrees;}inline void threadDetach(Thread){}
inline constexpr Handle CUR_THREAD_HANDLE=0xFFFF8000;
inline Result svcGetThreadPriority(s32* out,Handle){*out=0x30;return 0;}
inline s32 svcGetProcessorID(){return hostCore;}
// APT's CPU time limit for core 1: the last value set; hostRefuseLimit refuses.
inline u32 hostCpuLimit{};inline bool hostRefuseLimit{};inline std::vector<u32> hostLimitSets;
inline Result APT_GetAppCpuTimeLimit(u32* percent){*percent=hostCpuLimit;return 0;}
inline Result APT_SetAppCpuTimeLimit(u32 percent){if(hostRefuseLimit)return static_cast<Result>(0xc8a0cffc);hostCpuLimit=percent;hostLimitSets.push_back(percent);return 0;}
struct FS_ArchiveResource {u32 clusterSize;u64 freeClusters;};
inline Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource* p){++spaceChecks;p->clusterSize=4096;p->freeClusters=4096;return 0;}
inline bool R_SUCCEEDED(Result r){return r>=0;}inline bool R_FAILED(Result r){return r<0;}
inline constexpr int RL_PERMANENT=0,RS_NOTFOUND=0,RM_DSP=0,RD_NOT_FOUND=0;
#define MAKERESULT(...) static_cast<Result>(0xd880a7fau)
inline constexpr unsigned AI_SAMPLERATE_48KHZ=1,AI_SAMPLERATE_32KHZ=0;
