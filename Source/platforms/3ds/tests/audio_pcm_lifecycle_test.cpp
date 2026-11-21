#include "audio_host.hpp"
#include "audio_control.hpp"
#include "bootstrap.hpp"
#include "os_thread.hpp"
#include <filesystem>
#include <fstream>
#include <string>
namespace fs=std::filesystem;
namespace dusk::native {
[[noreturn]] void fatal(const char* msg){throw std::runtime_error(msg);}
// The mixer's thread: refused on the cores of hostRefusedCores and of
// refusedMixerCores (a core whose check thread ran but which refuses the mixer).
unsigned refusedMixerCores{};std::vector<int> mixerCores;
bool createThreadOnCore(OSThread* t,void*(*entry)(void*),void* arg,void* stack,u32 bytes,int priority,std::uint16_t attrs,int core){
    assert(priority==6&&(core==1||core==2||core==3));mixerCores.push_back(core);
    if((hostRefusedCores|refusedMixerCores)>>core&1)return false;
    const bool made=OSCreateThread(t,entry,arg,stack,bytes,priority,attrs);t->core=core;return made;
}
// main.cpp and os_thread.cpp counterparts.
const void* ignoredMemory{};std::uint32_t ignoredBytes{};
void ignoreCacheInvalidation(const void* memory,std::uint32_t bytes){ignoredMemory=memory;ignoredBytes=bytes;}
std::uint32_t ignoredCacheInvalidations(){return 0;}
LockWaitTotals watchedLockWaits(){return {};}
}
static void waitFilled(unsigned count){
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(dusk::native::nativeAudioStatus().blocks<count){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
// Where the mixer runs (audio-core.txt): the core it was made on, the cores
// tried, the check threads made, and the APT limit left after Shutdown.
static void placement(const std::string& mode){
    using namespace dusk::native;using namespace dusk::audio;
    const auto write=[](const char* text){std::ofstream("sdmc:/3ds/DuskLight/audio-core.txt")<<text;};
    int expected=3;std::vector<int> tried{3};unsigned checks=1;const char* line="Native audio worker created on core 3 (audio-core.txt auto: a thread there ran beside";
    if(mode=="place-auto"){}
    else if(mode=="place-word"){write("fast\n");}
    else if(mode=="place-3"){write("3\n");line="Native audio worker created on core 3 (audio-core.txt 3: a thread there ran beside";}
    else if(mode=="place-refuse3"){hostRefusedCores=1u<<3;expected=2;tried={2};checks=0;
        line="Native audio worker created with requested core2 affinity (audio-core.txt auto: core 3 refused a thread)";}
    else if(mode=="place-mixer-refuse3"){refusedMixerCores=1u<<3;expected=2;tried={3,2};
        line="Native audio worker: core 3 refused the mixer after its check; core 2 instead";}
    else if(mode=="place-2"){write(" 2\r\n");expected=2;tried={2};checks=0;
        line="Native audio worker created with requested core2 affinity (audio-core.txt 2, as up to Alpha 14)";}
    else if(mode=="place-1"){write("1 45\n");expected=1;tried={1};
        line="Native audio worker created on core 1 (audio-core.txt 1: APT CPU time limit 45 % (was 0); a thread there ran beside";}
    else if(mode=="place-1-default"){write("1\n");expected=1;tried={1};line="APT CPU time limit 40 % (was 0)";}
    else if(mode=="place-1-refused"){write("1\n");hostRefuseLimit=true;expected=2;tried={2};checks=0;
        line="Native audio worker created with requested core2 affinity (audio-core.txt 1: APT_SetAppCpuTimeLimit(40) refused (c8a0cffc))";}
    else if(mode=="place-1-mixer-refused"){write("1 30\n");refusedMixerCores=1u<<1;expected=2;tried={1,2};
        line="Native audio worker: core 1 refused the mixer after its check; core 2 instead";}
    else {std::fprintf(stderr,"unknown mode %s\n",mode.c_str());std::exit(2);}
    const unsigned createsBefore=threadCreates; // the ndspInit helper is one
    Initialize();waitFilled(6);
    assert(nativeAudioWorkerCore()==expected&&mixerCores==tried&&threadCreates-createsBefore==checks+1);
    for(unsigned i=0;i<checks;++i)assert(threadCores[threadCores.size()-checks+i]==(mode.rfind("place-1",0)==0?1:3));
    assert(reported(line));
    char running[96];std::snprintf(running,sizeof(running),"Native audio worker: running (made on core %d; svcGetProcessorID %d)",expected,expected);
    assert(reported(running));
    // Core 1 only: the limit is set before the check and back to the smallest one (it was 0) after.
    const bool coreOne=mode=="place-1"||mode=="place-1-default"||mode=="place-1-mixer-refused";
    if(coreOne)assert(hostLimitSets.size()==(mode=="place-1-mixer-refused"?2u:1u)&&hostLimitSets[0]==(mode=="place-1"?45u:mode=="place-1-default"?40u:30u));
    else assert(hostLimitSets.empty());
    Shutdown();
    // The mixer's own thread (core 3 by default) has ended and been joined
    // when Shutdown returns: game_startup.cpp stops audio before anything else.
    char joined[64];std::snprintf(joined,sizeof(joined),"Native audio worker: joined (made on core %d)",expected);
    assert(reported(joined));
    assert(nativeAudioWorkerCore()==-3&&liveLinear==0);
    if(coreOne)assert(hostLimitSets.size()==2&&hostLimitSets[1]==5&&hostCpuLimit==5);
    std::printf("PASS: audio-core %s: mixer on core %d, cores tried %zu, check threads %u\n",mode.c_str(),expected,tried.size(),checks);
}
int main(int argc,char** argv){
    using namespace dusk::native;using namespace dusk::audio;
    assert(argc==3);fs::create_directories(argv[1]);fs::current_path(argv[1]);
    fs::create_directories("sdmc:/3ds/dusklight");
    const std::string mode=argv[2];
    if(mode.rfind("place-",0)==0){fs::create_directories("sdmc:/3ds/DuskLight");placement(mode);return 0;}
    if(mode=="stall"){
        // One mix far longer than the queued audio (Alpha 6: hundreds of ms
        // after the SD log flush) is an underrun with its silence measured,
        // although the queue is full again at the top of the worker loop.
        realtimeDevice=true;Initialize();waitFilled(12);
        const auto before=nativeAudioStatus().underruns;
        {std::lock_guard guard(reportMutex);reports.clear();}
        stallMilliseconds=150;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!reported("silent for at least")){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        Shutdown();
        const auto at=reports.find("silent for at least ");unsigned silent=0;
        assert(std::sscanf(reports.c_str()+at,"silent for at least %u ms",&silent)==1&&silent>=50&&silent<=200);
        assert(nativeAudioStatus().underruns==before+1&&serviceFlushes==0);
        std::printf("PASS: a 150 ms mix is counted as one underrun with %u ms of silence\n",silent);return 0;
    }
    if(mode=="fatal"){
        initResult=static_cast<Result>(0xd9000001);
        bool fatalSeen=false;try{Initialize();}catch(const std::runtime_error&){fatalSeen=true;}
        assert(fatalSeen&&queueCalls==0&&linearAllocations==0&&componentUses==1);
        assert(nativeAudioStatus().result==0xd9000001);Shutdown();
        std::puts("PASS: non-missing-component NDSP error remains fatal");return 0;
    }
    const bool muted=mode=="muted",enabled=mode!="disabled";
    // ndsp48: SD switch mixer-48k.txt (Alpha 13 rate, fixed point); old:
    // mixer-old.txt (the Alpha 13 mixer: 48 kHz, float). Others: 32 kHz fixed point.
    const bool rate48=mode=="ndsp48"||mode=="old";
    fs::create_directories("sdmc:/3ds/DuskLight");
    if(mode=="ndsp48")std::ofstream("sdmc:/3ds/DuskLight/mixer-48k.txt")<<"\n";
    if(mode=="old")std::ofstream("sdmc:/3ds/DuskLight/mixer-old.txt")<<"\n";
    if(enabled)std::ofstream(audioPcmConfigPath)<<"1 4\n";
    initResult=muted?static_cast<Result>(0xd880a7fa):0;
    Initialize();waitFilled(8);
    assert(RenderRate==(rate48?48000:32000)&&JASDriver::getSubFrames()==(rate48?10u:7u)&&DspFixedMix==(mode!="old"));
    assert(reported(rate48?"Native audio mixer: 48000 Hz, 10 subframes per 16.7 ms block; ":"Native audio mixer: 32000 Hz, 7 subframes per 17.5 ms block; "));
    assert(nativeAudioStatus().output==(muted?AudioOutputState::componentUnavailable:AudioOutputState::ndsp));
    assert(nativeAudioWorkerCore()==3&&reported("Native audio worker: running (made on core 3; svcGetProcessorID 3)"));
    // The DSP channel table is registered as CPU-only (no kernel invalidation).
    assert(ignoredMemory==JASDsp::CH_BUF&&ignoredBytes==sizeof(JASDsp::TChannel)*DSP_CHANNELS);
    // The volume reaches the render through the mixer, not under the JAS lock.
    SetMasterVolume(.5f);
    {auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
     while(renderedVolume.load()!=.5f){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    SetPaused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto pausedBlocks=nativeAudioStatus().blocks;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(nativeAudioStatus().blocks==pausedBlocks);
    // No capture output or preflight before normal joined shutdown, even after the interval ends.
    assert(!fs::exists(audioPcmWavePath)&&!fs::exists(audioPcmJsonPath)&&spaceChecks==0);
    SetPaused(false);waitFilled(pausedBlocks+3);
    if(!muted){
        // The worker's own cost line (fast fake device: 600 blocks in well under a second).
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!reported("Native audio JAS lock waits: main ")){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        assert(reported("Native audio worker 600 blocks: JAS lock wait"));
        assert(reported(rate48?" of a 16.7 ms block\n":" of a 17.5 ms block\n"));
        // The console's OSReport keeps 255 characters (bootstrap.cpp): every line must fit.
        std::lock_guard guard(reportMutex);
        for(std::size_t at=0,end;at<reports.size();at=end+1){end=reports.find('\n',at);if(end==std::string::npos)end=reports.size();assert(end-at<255);}
    }
    Shutdown();assert(liveLinear==0);
    const auto status=nativeAudioStatus();assert(status.output==AudioOutputState::uninitialized);
    assert(linearAllocations==(muted?1u:3u));
    if(muted){assert(queueCalls==0&&status.submitted==0&&status.completed==0&&status.underruns==0&&flushes==0&&exits==0);}
    else {assert(status.submitted==queueCalls&&status.blocks==status.submitted&&status.completed>0&&status.completed<=status.submitted&&clearCalls==1&&exits==1);
          assert(flushes==status.blocks&&serviceFlushes==0);}
    assert(fs::exists(audioPcmWavePath)==enabled&&fs::exists(audioPcmJsonPath)==enabled);
    assert(spaceChecks==(enabled?1u:0u));
    if(enabled){assert(fs::file_size(audioPcmWavePath)==44+4*hostBlockSamples()*4);}
    Shutdown();assert(spaceChecks==(enabled?1u:0u));
    // Normal reinitialize/shutdown path remains available, with capture absent and clean wave ownership.
    if(enabled)fs::remove(audioPcmConfigPath);
    Reinitialize();waitFilled(4);Shutdown();assert(liveLinear==0&&componentUses==2&&componentInitializations==2);
    std::printf("PASS: actual audio_ndsp.cpp host-mocked %s lifecycle, pause/resume, queue accounting, joined-only capture, reinitialize\n",mode.c_str());
}
