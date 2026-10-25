// Actual bounded SD reader, exact parser and extracted original-startup call.
#include "../runtime/fused_color_config.hpp"
#include "../gx_texture_pica.hpp"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
using namespace dusk::native;
static unsigned reports,calls,fatals;
static bool expected,configured,refuse;
static char report[256];
extern "C" void OSReport(const char* fmt,...){
    ++reports;va_list args;va_start(args,fmt);std::vsnprintf(report,sizeof(report),fmt,args);va_end(args);
}
namespace dusk::native {
bool configureGxTextureCache(std::size_t bytes,unsigned slots,bool reference,
    std::span<const NativeTextureFingerprint> policy,bool fused){
    ++calls;assert(bytes==10*1024*1024&&slots==128&&reference&&policy.size()==2);
    assert(policy[0][0]==17&&policy[1][0]==23);assert(fused==expected);configured=fused;return !refuse;
}
const char* gxTextureFailure(){return "original configuration error";}
}
[[noreturn]] static void fatal(const char* reason){
    ++fatals;assert(std::strcmp(reason,"original configuration error")==0);throw std::runtime_error(reason);
}
static void startup(){
    bool rgb565Reference=true;
    std::array<NativeTextureFingerprint,64> halfPolicy{};const unsigned halfCount=2;
    halfPolicy[0][0]=17;halfPolicy[1][0]=23;
#include "startup_cache.inc"
}
static void parser(){
    bool enabled=true;
    for(std::size_t size=0;size<32;++size){assert(!parseFusedColorCopyConfig(nullptr,size,enabled)&&!enabled);enabled=true;}
    const char on[]="enabled=1\n";
    for(std::size_t size=0;size<32;++size)if(size!=10){enabled=true;assert(!parseFusedColorCopyConfig(on,size,enabled)&&!enabled);}
    assert(parseFusedColorCopyConfig(on,10,enabled)&&enabled);
    assert(parseFusedColorCopyConfig("enabled=0\n",10,enabled)&&!enabled);
    for(unsigned i=0;i<10;++i)for(unsigned byte=0;byte<256;++byte){
        std::string text(on,10);text[i]=char(byte);enabled=true;
        const bool valid=text=="enabled=1\n"||text=="enabled=0\n";
        assert(parseFusedColorCopyConfig(text.data(),text.size(),enabled)==valid);
        assert(enabled==(text=="enabled=1\n"));
    }
    for(const char* value:{"enabled=1\r\n","enabled=1\n\n","enabled=1","enabled=1\n#"," enabled=1\n","ENABLED=1\n"}){
        enabled=true;assert(!parseFusedColorCopyConfig(value,std::strlen(value),enabled)&&!enabled);
    }
}
int main(int argc,char** argv){
    assert(argc==2);parser();expected=std::string(argv[1])=="on";
    startup();assert(calls==1&&configured==expected&&reports==1&&fatals==0);
    assert(std::strstr(report,expected?"enabled (":"disabled ("));
    // Replace staged token: the same process must keep its first result and log once.
    if(auto* file=std::fopen("sdmc:/3ds/dusklight/fused-color-copy.txt","wb")){
        const char* other=expected?"enabled=0\n":"enabled=1\n";
        assert(std::fwrite(other,1,10,file)==10);assert(std::fclose(file)==0);
    }
    assert(readFusedColorCopyConfigFromSd()==expected&&reports==1);
    refuse=true;try{startup();assert(false);}catch(const std::runtime_error&){}
    assert(fatals==1&&calls==2&&reports==1);
    std::printf("PASS parser/null/wrong-size/all-invalid-bytes, staged-%s fifth argument, unchanged configure fatal, one-shot SD/log\n",argv[1]);
}
