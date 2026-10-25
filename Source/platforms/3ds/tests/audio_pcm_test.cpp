#include "audio_control.hpp"
#include "dump_io.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
using namespace dusk::native;
namespace fs=std::filesystem;
static void text(const fs::path& path,const std::string& value){std::ofstream(path,std::ios::binary)<<value;}
static std::vector<unsigned char> read(const fs::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static unsigned le32(const std::vector<unsigned char>& v,unsigned at){return v[at]|v[at+1]<<8|v[at+2]<<16|unsigned(v[at+3])<<24;}
int main(int argc,char** argv){
    assert(argc==2);const fs::path root=argv[1];fs::create_directories(root);
    AudioPcmConfig config{11,22};
    for(const std::string& bad:std::vector<std::string>{"","1","0 0","-1 1","+1 1","0 1 x","4294967296 1","0 328","0 999999999999999999999","0 1.0",std::string("0 1\0",4)}){
        assert(!parseAudioPcmConfig(bad.data(),bad.size(),800,config));assert(config.startBlock==11&&config.blockCount==22);
    }
    assert(parseAudioPcmConfig(" \r\n4294967295\t327\n",18,800,config));
    assert(config.startBlock==UINT32_MAX&&config.blockCount==327);
    assert(!parseAudioPcmConfig("0 1",3,UINT32_MAX,config));
    assert(!parseAudioPcmConfig("0 1",3,0,config));
    assert(nativePcm16(-2)==-32768&&nativePcm16(2)==32767);
    assert(nativePcm16(-.5f)==-16384&&nativePcm16(.5f)==16384&&nativePcm16(0)==0);
    assert(nativePcm16(1.f/65536)==0&&nativePcm16(-1.f/65536)==0); // truncation, not rounding
    // Disabled path must tolerate unreadable PCM and invalid output paths: no allocation, scan or output.
    AudioPcmCapture disabled;
    assert(disabled.initialize((root/"absent").c_str(),800,48000)==AudioPcmResult::disabled);
    for(int i=0;i<100;++i)disabled.filled(nullptr);
    assert(!disabled.allocatedBytes());
    assert(disabled.finish(nullptr,nullptr,0,AudioOutputState::ndsp,0)==AudioPcmResult::disabled);
    assert(disabled.healthAfterJoin().filled==100);
    text(root/"config","0 328");
    assert(disabled.initialize((root/"config").c_str(),800,48000)==AudioPcmResult::invalidConfig);
    assert(!disabled.allocatedBytes());
    text(root/"config",std::string(96,' '));
    assert(disabled.initialize((root/"config").c_str(),800,48000)==AudioPcmResult::invalidConfig);
    text(root/"config","1 2\n");
    AudioPcmCapture capture;
    assert(capture.initialize((root/"config").c_str(),2,48000)==AudioPcmResult::armed);
    assert(capture.allocatedBytes()==60);
    // Startup owner performs prefill, worker owns all mutations, join transfers ownership back.
    assert(capture.filled(nullptr)==0); // outside interval: may not dereference
    capture.submitted(0);
    std::thread worker([&]{
        const std::int16_t one[]={1,-2,0,32767},two[]={-32768,0,100,-100};
        assert(capture.filled(one)==1);capture.submitted(1);capture.completed(0);
        assert(capture.filled(two)==2);capture.submitted(2);capture.completed(1);
        capture.underrun();
        for(unsigned i=3;i<10000;++i)assert(capture.filled(nullptr)==i);
    });worker.join();
    const auto& h=capture.healthAfterJoin();
    assert(h.filled==10000&&h.submitted==3&&h.completed==2&&h.underruns==1);
    assert(h.captured==2&&h.captureSubmitted==2&&h.captureCompleted==1);
    assert(h.nonzero[0]==3&&h.nonzero[1]==3&&h.fullScale[0]==1&&h.fullScale[1]==1);
    assert(h.peak[0]==32768&&h.peak[1]==32767);
    assert(capture.finish((root/"capture.wav").c_str(),(root/"capture.json").c_str(),dumpReserve+8192+60,AudioOutputState::ndsp,0)==AudioPcmResult::written);
    const auto wav=read(root/"capture.wav");
    // Independent known little-endian stereo fixture; verify channel order, signs and interval exclusion.
    const std::vector<unsigned char> expected={1,0,254,255,0,0,255,127,0,128,0,0,100,0,156,255};
    assert(wav.size()==60&&std::equal(expected.begin(),expected.end(),wav.begin()+44));
    assert(std::string(wav.begin(),wav.begin()+4)=="RIFF"&&le32(wav,4)==52&&le32(wav,16)==16);
    assert(wav[20]==1&&wav[22]==2&&le32(wav,24)==48000&&le32(wav,28)==192000&&wav[32]==4&&wav[34]==16);
    assert(le32(wav,40)==16&&capture.wavHash()==dumpHash(wav.data(),wav.size()));
    auto json=read(root/"capture.json");assert(capture.jsonHash()==dumpHash(json.data(),json.size()));
    assert(!capture.allocatedBytes());
    // Repeated shutdown is inert even if paths are unusable; existing verified files survive.
    assert(capture.finish(nullptr,nullptr,0,AudioOutputState::ndsp,0)==AudioPcmResult::written);
    assert(read(root/"capture.wav")==wav);
    // Early shutdown, muted mode and unsubmitted captures are honest, including zero captured frames.
    text(root/"config","2 3");
    assert(capture.initialize((root/"config").c_str(),2,48000)==AudioPcmResult::armed);
    capture.filled(nullptr);
    assert(capture.finish((root/"empty.wav").c_str(),(root/"empty.json").c_str(),UINT64_MAX,AudioOutputState::componentUnavailable,0xd880a7fa)==AudioPcmResult::written);
    assert(read(root/"empty.wav").size()==44);
    // Requested range may be cut short by normal exit; export only actual captured blocks.
    assert(capture.initialize((root/"config").c_str(),2,48000)==AudioPcmResult::armed);
    capture.filled(nullptr);capture.filled(nullptr);
    const std::int16_t partial[4]={12,-12,0,0};capture.filled(partial);
    assert(capture.finish((root/"partial.wav").c_str(),(root/"partial.json").c_str(),UINT64_MAX,AudioOutputState::componentUnavailable,0xd880a7fa)==AudioPcmResult::written);
    assert(read(root/"partial.wav").size()==52);
    text(root/"config","0 1");
    auto arm=[&]{assert(capture.initialize((root/"config").c_str(),2,48000)==AudioPcmResult::armed);const std::int16_t p[4]={1,2,3,4};capture.filled(p);};
    arm();assert(capture.finish((root/"space.wav").c_str(),(root/"space.json").c_str(),dumpReserve+8192+51,AudioOutputState::ndsp,0)==AudioPcmResult::insufficientSpace);
    assert(!fs::exists(root/"space.wav")&&!capture.allocatedBytes());
    arm();assert(capture.finish((root/"capture.wav").c_str(),(root/"collision.json").c_str(),UINT64_MAX,AudioOutputState::ndsp,0)==AudioPcmResult::writeFailed);
    assert(read(root/"capture.wav")==wav&&!fs::exists(root/"collision.json"));
    arm();assert(capture.finish((root/"orphan.wav").c_str(),(root/"missing/meta.json").c_str(),UINT64_MAX,AudioOutputState::ndsp,0)==AudioPcmResult::writeFailed);
    assert(fs::exists(root/"orphan.wav")&&!capture.allocatedBytes());
    arm();assert(capture.finish((root/"missing/child.wav").c_str(),(root/"missing/meta.json").c_str(),UINT64_MAX,AudioOutputState::ndsp,0)==AudioPcmResult::writeFailed);
    // Exact maximum native count is accepted and freed without requiring any captured data.
    text(root/"config","4294967295 327");
    assert(capture.initialize((root/"config").c_str(),800,48000)==AudioPcmResult::armed);
    assert(capture.allocatedBytes()==1046444&&capture.allocatedBytes()<=audioPcmMaxBytes);
    capture.filled(nullptr);
    assert(capture.finish((root/"max-empty.wav").c_str(),(root/"max-empty.json").c_str(),UINT64_MAX,AudioOutputState::ndsp,0)==AudioPcmResult::written);
    std::puts("PASS: config bounds, PCM/channel conversion, disabled path, interval, ownership after join, WAV bytes/hash, shutdown and export failures");
}
