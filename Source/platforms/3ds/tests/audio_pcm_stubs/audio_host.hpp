#pragma once
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
#include <thread>
#include <vector>
using u16=std::uint16_t;using u32=std::uint32_t;using u64=std::uint64_t;using s16=std::int16_t;using f32=float;using Result=std::int32_t;
inline constexpr unsigned DSP_SUBFRAME_SIZE=80,DSP_CHANNELS=64,MIX_MODE_INTERLEAVE=3;
enum JASOutputRate { OUTPUT_RATE_0 };
struct OSThread { std::thread native;std::atomic<bool> done{true};void*(*entry)(void*){};void* arg{}; };
inline bool OSCreateThread(OSThread* t,void*(*entry)(void*),void* arg,void*,u32,int,std::uint16_t){t->entry=entry;t->arg=arg;return true;}
inline thread_local bool audioWorker=false;
inline void OSResumeThread(OSThread* t){t->done=false;t->native=std::thread([t]{audioWorker=true;t->entry(t->arg);t->done.store(true,std::memory_order_release);});}
inline bool OSIsThreadTerminated(OSThread* t){return t->done.load(std::memory_order_acquire);}
inline bool OSJoinThread(OSThread* t,void*){t->native.join();return true;}
inline int OSReport(const char* fmt,...){va_list ap;va_start(ap,fmt);int n=std::vprintf(fmt,ap);va_end(ap);return n;}
[[noreturn]] inline void OSPanic(const char*,int,const char* msg){throw std::runtime_error(msg);}
struct JASCriticalSection { inline static std::recursive_mutex mutex;JASCriticalSection(){mutex.lock();}~JASCriticalSection(){mutex.unlock();} };
namespace JASDriver {
inline unsigned getSubFrames(){return 10;}inline void setOutputRate(JASOutputRate){}
inline void updateDSP(){}inline float getDacRate(){return 48000;}inline float getChannelLevel_dsp(){return 32768;}
inline s16*(*extMixCallback)(u32){};inline unsigned sMixMode=MIX_MODE_INTERLEAVE;
}
struct JASAudioThread { static void setDSPSyncCount(unsigned){} inline static int snIntCount=0; };
struct JASChannel {};
template<class T>struct JASPoolAllocObject_MultiThreaded { static void newMemPool(unsigned){} };
namespace JASDsp {inline void initBuffer(){}}
struct JASDSPChannel {static void initAll(){}};
struct JKRHeap {static JKRHeap* getSystemHeap(){static JKRHeap h;return &h;}static void setCurrentHeap(JKRHeap*){}};
namespace dusk::audio {
inline constexpr unsigned SampleRate=48000;
inline unsigned OutChannelCount{};inline bool EnableHrtf{},EnableReverb{},DumpAudio{};inline float MasterVolume{};
struct OutputSubframe { std::array<std::array<float,DSP_SUBFRAME_SIZE>,8> channels{}; };
inline void DspInit(){}
inline void DspRender(OutputSubframe& out){for(unsigned i=0;i<DSP_SUBFRAME_SIZE;++i){out.channels[0][i]=.25f;out.channels[1][i]=-.5f;}}
struct ChannelAuxData {u32 resetCount{};};inline ChannelAuxData ChannelAux[DSP_CHANNELS];
void Initialize();void Shutdown();void SetPaused(bool);void Reinitialize();
void SetEnableReverb(bool);void SetMasterVolume(f32);u32 GetResetCount(int);f32 VolumeFromU16(u16);
}
inline constexpr int NDSP_WBUF_FREE=0,NDSP_WBUF_QUEUED=1,NDSP_WBUF_DONE=3;
inline constexpr int NDSP_OUTPUT_STEREO=1,NDSP_INTERP_LINEAR=1,NDSP_FORMAT_STEREO_PCM16=1;
inline constexpr u64 SYSCLOCK_ARM11=1000000000;
struct ndspWaveBuf {s16* data_pcm16{};u32 nsamples{};unsigned status{};};
inline std::vector<ndspWaveBuf*> queued;
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
inline void ndspChnSetRate(int,unsigned rate){assert(rate==48000);}
inline void ndspChnSetFormat(int,int){}inline void ndspChnSetMix(int,float mix[12]){assert(mix[0]==1&&mix[1]==1);}
inline void ndspChnSetPaused(int,bool p){devicePaused=p;}
inline void ndspChnWaveBufAdd(int,ndspWaveBuf* w){assert(w->nsamples==800);w->status=NDSP_WBUF_QUEUED;queued.push_back(w);++queueCalls;}
inline void ndspChnWaveBufClear(int){queued.clear();++clearCalls;}
inline void* linearAlloc(std::size_t bytes){++linearAllocations;++liveLinear;return std::malloc(bytes);}
inline void linearFree(void* p){if(p)--liveLinear;std::free(p);}
inline void DSP_FlushDataCache(void*,std::size_t){++flushes;}
inline u64 svcGetSystemTick(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void svcSleepThread(std::int64_t){
    // Fake device advances on the same worker, avoiding host races in mock wave status.
    if(audioWorker&&!devicePaused.load()){for(auto* w:queued)w->status=NDSP_WBUF_DONE;queued.clear();}
    std::this_thread::sleep_for(std::chrono::microseconds(200));
}
struct FS_ArchiveResource {u32 clusterSize;u64 freeClusters;};
inline Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource* p){++spaceChecks;p->clusterSize=4096;p->freeClusters=4096;return 0;}
inline bool R_SUCCEEDED(Result r){return r>=0;}inline bool R_FAILED(Result r){return r<0;}
inline constexpr int RL_PERMANENT=0,RS_NOTFOUND=0,RM_DSP=0,RD_NOT_FOUND=0;
#define MAKERESULT(...) static_cast<Result>(0xd880a7fau)
inline constexpr unsigned AI_SAMPLERATE_48KHZ=1,AI_SAMPLERATE_32KHZ=0;
