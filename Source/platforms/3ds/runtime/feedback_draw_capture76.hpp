#pragma once
// Private synchronous observer. No draw/resource pointers survive invoke().
// CAP94 encoder and wire are deliberately unchanged; this is not failure selection.
#include "gx_draw_capture94.hpp"
#include "gx_material.hpp"
#include <limits>

namespace dusk::native::gx::feedback76 {
inline constexpr const char* configPath="sdmc:/feedback-draw-capture76.txt";
inline constexpr const char* dataPaths[]={
    "sdmc:/feedback-draw-capture76-01.txtdata","sdmc:/feedback-draw-capture76-02.txtdata",
    "sdmc:/feedback-draw-capture76-03.txtdata","sdmc:/feedback-draw-capture76-04.txtdata",
    "sdmc:/feedback-draw-capture76-05.txtdata","sdmc:/feedback-draw-capture76-06.txtdata",
    "sdmc:/feedback-draw-capture76-07.txtdata","sdmc:/feedback-draw-capture76-08.txtdata"};
inline constexpr const char* statusPaths[]={
    "sdmc:/feedback-draw-capture76-01.status","sdmc:/feedback-draw-capture76-02.status",
    "sdmc:/feedback-draw-capture76-03.status","sdmc:/feedback-draw-capture76-04.status",
    "sdmc:/feedback-draw-capture76-05.status","sdmc:/feedback-draw-capture76-06.status",
    "sdmc:/feedback-draw-capture76-07.status","sdmc:/feedback-draw-capture76-08.status"};
struct Config {unsigned frame{},limit{};bool enabled()const{return frame&&limit;}};
// Canonical decimal, exact ASCII grammar, LF only, no leading zero/trailing data.
inline bool parse(std::string_view text,Config& out){
    out={};if(!text.starts_with("FRAME76 frame="))return false;text.remove_prefix(14);
    auto number=[&](unsigned max,unsigned& value){
        value=0;if(text.empty()||text.front()<'1'||text.front()>'9')return false;
        do {const unsigned digit=unsigned(text.front()-'0');if(value>max/10||(value==max/10&&digit>max%10))return false;
            value=value*10+digit;text.remove_prefix(1);
        }while(!text.empty()&&text.front()>='0'&&text.front()<='9');
        return value<=max;
    };
    Config c;if(!number(100000,c.frame)||!text.starts_with(" limit="))return false;text.remove_prefix(7);
    if(!number(8,c.limit)||text!="\n")return false;out=c;return true;
}
inline Config readConfig(){
    Config c;auto* f=std::fopen(configPath,"rb");if(!f)return c;
    char text[48];const auto n=std::fread(text,1,sizeof(text),f);
    bool ok=!std::ferror(f);if(std::fclose(f))ok=false;
    if(!ok||n==sizeof(text)||!parse({text,n},c))return {};return c;
}
using Log=void(*)(const char*);
using TextureResolver=capture94::TextureResolver;
// Narrow injectable libc lifecycle, only consulted for an attempted capture.
struct Files {
    FILE* (*open)(const char*,const char*)=std::fopen;
    std::size_t (*write)(void*,const char*,std::size_t)=capture94::fileWrite;
    int (*flush)(FILE*)=std::fflush;
    int (*close)(FILE*)=std::fclose;
};
struct Publisher {
    unsigned attempted{};
    capture94::Receipt publish(const CommandDraw& draw,TextureResolver resolve,Files io={}){
        using namespace capture94;Receipt result;
        if(attempted>=8){result.failure=Failure::bounds;return result;}
        const unsigned slot=attempted++; // Always consume before opening; no retry.
        auto* status=io.open(statusPaths[slot],"wx");
        if(!status){result.failure=Failure::statusOpen;return result;}
        std::setvbuf(status,nullptr,_IONBF,0);
        auto* file=io.open(dataPaths[slot],"wx");
        if(!file)result.failure=Failure::open;
        else {
            std::setvbuf(file,nullptr,_IONBF,0);
            result=encode(draw,currentTargetExtent,{file,io.write},resolve);
            if(io.flush(file)){result.failure=Failure::flush;result.complete=false;}
            if(io.close(file)){result.failure=Failure::close;result.complete=false;}
        }
        char receipt[256];const int n=formatReceipt(result,receipt,sizeof(receipt));
        constexpr char prefix[]="CAP94_STATUS 1 ";
        bool written=n>0&&unsigned(n)<sizeof(receipt);
        if(written)written=io.write(status,prefix,sizeof(prefix)-1)==sizeof(prefix)-1&&
                           io.write(status,receipt,unsigned(n))==unsigned(n);
        // Flush even after a short status write, then always close.
        const int flush=io.flush(status);
        if(!written||flush){result.failure=Failure::statusWrite;result.complete=false;}
        if(io.close(status)){result.failure=Failure::statusClose;result.complete=false;}
        return result; // Caller logs this exact receipt AFTER both close checks.
    }
};
struct Selection {unsigned used{},matching{};bool incomplete{};};
// Use decoded source operands, not textureEnabled or a stale bound unit alone.
// Indirect dependency rules mirror pinned Aurora shader_info.cpp: matrix or
// bump-raster use consumes the referenced ind stage; matrix/addPrev also uses
// the base texture map. No material planning/admission and no texel reads.
inline Selection select(const CommandRegisters& r,const void* identity,
                        TextureResolver resolve=resolveCommandTexture){
    Selection result;MaterialState m;
    if(!decodeMaterial(r,m)){result.incomplete=true;return result;}
    auto direct=[&](const MaterialStage& s){
        if(!s.textureEnabled)return;
        if(s.coordinate>=m.texgenCount){result.incomplete=true;return;}
        result.used|=1u<<s.texture;
    };
    for(unsigned i=0;i<m.stageCount;++i){const auto& s=m.stages[i];
        bool texture=false;for(auto x:s.color)texture|=x==8||x==9;
        for(auto x:s.alpha)texture|=x==4;
        if(texture)direct(s);
        const unsigned ind=s.indirect&3,matrix=(s.indirect>>9)&15;
        if(ind>=m.indirectCount)continue;
        if(matrix||(s.indirect&(1u<<20)))direct(s);
        if(!matrix&&s.raster!=5&&s.raster!=6)continue;
        if(!r.bpValid[0x27]){result.incomplete=true;continue;}
        const unsigned order=(r.bp[0x27]>>(6*ind))&63;
        if((order>>3)>=m.texgenCount){result.incomplete=true;continue;}
        result.used|=1u<<(order&7);
    }
    std::uint32_t token{};Texture target{};
    if(!identity||!textureCommandToken(identity,token)||!textureFromCommandToken(token,target)||!target.handle){
        result.incomplete=true;return result;
    }
    for(unsigned slot=0;slot<8;++slot)if(result.used&(1u<<slot)){
        Texture texture{};const char* why=nullptr;
        if(!resolve||!resolve(r,slot,texture,why)||!texture.handle){result.incomplete=true;continue;}
        if(texture.handle==target.handle)result.matching|=1u<<slot;
    }
    return result;
}
struct Counts {
    std::uint64_t observed{},matched{},captured{},overflow{},incomplete{},originalTrue{},originalFalse{};
    bool saturated{};
    void add(std::uint64_t& n){if(n==std::numeric_limits<std::uint64_t>::max())saturated=true;else ++n;}
};
struct Observer {
    Config config{};std::uint64_t frame{};bool finished{};Counts counts{};Publisher publisher{};
    bool enabled()const{return config.enabled();}
    void finish(Log log){
        if(!enabled()||finished)return;finished=true;
        char text[512];std::snprintf(text,sizeof(text),
            "FRAME76 totals frame=%u reached=%u observed=%llu matched=%llu attempted=%u captured=%llu overflow=%llu incomplete=%llu originalTrue=%llu originalFalse=%llu saturated=%u\n",
            config.frame,unsigned(frame==config.frame),static_cast<unsigned long long>(counts.observed),
            static_cast<unsigned long long>(counts.matched),publisher.attempted,
            static_cast<unsigned long long>(counts.captured),static_cast<unsigned long long>(counts.overflow),
            static_cast<unsigned long long>(counts.incomplete),static_cast<unsigned long long>(counts.originalTrue),
            static_cast<unsigned long long>(counts.originalFalse),unsigned(counts.saturated));
        if(log)log(text);
    }
    void setFrame(std::uint64_t value,Log log){
        if(enabled()&&!finished&&frame==config.frame&&value!=frame)finish(log);frame=value;
    }
    bool invoke(const CommandDraw& draw,void* user,CommandDrawSink original,const void* identity,Log log,
                TextureResolver resolve=resolveCommandTexture,Files io={}){
        const bool active=enabled()&&!finished&&frame==config.frame;
        Selection selection{};unsigned slot=0;bool attempted=false;
        if(active){
            counts.add(counts.observed);selection=select(draw.registers,identity,resolve);
            if(selection.incomplete)counts.add(counts.incomplete);
            if(selection.matching){counts.add(counts.matched);
                if(publisher.attempted<config.limit){
                    slot=publisher.attempted+1;attempted=true;
                    char text[384];std::snprintf(text,sizeof(text),
                        "FRAME76 attempt slot=%u frame=%u ordinal=%llu used=%02x matching=%02x selectionIncomplete=%u phase=pre-submit\n",
                        slot,config.frame,static_cast<unsigned long long>(counts.observed),selection.used,selection.matching,unsigned(selection.incomplete));
                    if(log)log(text);
                    const auto receipt=publisher.publish(draw,resolve,io);
                    if(receipt.complete&&receipt.failure==capture94::Failure::none)counts.add(counts.captured);
                    char returned[256];capture94::formatReceipt(receipt,returned,sizeof(returned));
                    std::snprintf(text,sizeof(text),"FRAME76 returned slot=%u: %s",slot,returned);
                    if(log)log(text);
                }else counts.add(counts.overflow);
            }
        }
        // Exactly once, same immutable draw and user, exact original bool result.
        const bool outcome=original(draw,user);
        if(active){counts.add(outcome?counts.originalTrue:counts.originalFalse);
            if(attempted){char text[96];std::snprintf(text,sizeof(text),
                "FRAME76 outcome slot=%u originalResult=%u\n",slot,unsigned(outcome));if(log)log(text);}}
        return outcome;
    }
};
inline CommandDrawSink chooseSink(bool enabled,CommandDrawSink original,CommandDrawSink wrapper){
    return enabled?wrapper:original;
}
}
