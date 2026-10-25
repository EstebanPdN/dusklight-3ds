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
bool createThreadOnCore(OSThread* t,void*(*entry)(void*),void* arg,void* stack,u32 bytes,int priority,std::uint16_t attrs,int core){assert(core==2);return OSCreateThread(t,entry,arg,stack,bytes,priority,attrs);}
}
static void waitFilled(unsigned count){
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(dusk::native::nativeAudioStatus().blocks<count){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
int main(int argc,char** argv){
    using namespace dusk::native;using namespace dusk::audio;
    assert(argc==3);fs::create_directories(argv[1]);fs::current_path(argv[1]);
    fs::create_directories("sdmc:/3ds/dusklight");
    const std::string mode=argv[2];
    if(mode=="fatal"){
        initResult=static_cast<Result>(0xd9000001);
        bool fatalSeen=false;try{Initialize();}catch(const std::runtime_error&){fatalSeen=true;}
        assert(fatalSeen&&queueCalls==0&&linearAllocations==0&&componentUses==1);
        assert(nativeAudioStatus().result==0xd9000001);Shutdown();
        std::puts("PASS: non-missing-component NDSP error remains fatal");return 0;
    }
    const bool muted=mode=="muted",enabled=mode!="disabled";
    if(enabled)std::ofstream(audioPcmConfigPath)<<"1 4\n";
    initResult=muted?static_cast<Result>(0xd880a7fa):0;
    Initialize();waitFilled(8);
    assert(nativeAudioStatus().output==(muted?AudioOutputState::componentUnavailable:AudioOutputState::ndsp));
    SetPaused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto pausedBlocks=nativeAudioStatus().blocks;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(nativeAudioStatus().blocks==pausedBlocks);
    // No capture output or preflight before normal joined shutdown, even after the interval ends.
    assert(!fs::exists(audioPcmWavePath)&&!fs::exists(audioPcmJsonPath)&&spaceChecks==0);
    SetPaused(false);waitFilled(pausedBlocks+3);
    Shutdown();assert(liveLinear==0);
    const auto status=nativeAudioStatus();assert(status.output==AudioOutputState::uninitialized);
    assert(linearAllocations==(muted?1u:3u));
    if(muted){assert(queueCalls==0&&status.submitted==0&&status.completed==0&&status.underruns==0&&flushes==0&&exits==0);}
    else {assert(status.submitted==queueCalls&&status.blocks==status.submitted&&status.completed>0&&status.completed<=status.submitted&&clearCalls==1&&exits==1);}
    assert(fs::exists(audioPcmWavePath)==enabled&&fs::exists(audioPcmJsonPath)==enabled);
    assert(spaceChecks==(enabled?1u:0u));
    if(enabled){assert(fs::file_size(audioPcmWavePath)==12844);}
    Shutdown();assert(spaceChecks==(enabled?1u:0u));
    // Normal reinitialize/shutdown path remains available, with capture absent and clean wave ownership.
    if(enabled)fs::remove(audioPcmConfigPath);
    Reinitialize();waitFilled(4);Shutdown();assert(liveLinear==0&&componentUses==2&&componentInitializations==2);
    std::printf("PASS: actual audio_ndsp.cpp host-mocked %s lifecycle, pause/resume, queue accounting, joined-only capture, reinitialize\n",mode.c_str());
}
