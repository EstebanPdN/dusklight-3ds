#include "developer_script.hpp"
#include "dump_io.hpp"
#include "gx_state.hpp"
#include "gx_profile.hpp"
#include "gx_framebuffer.hpp"
#include "resource_archive.hpp"
#include <3ds.h>
#include <dolphin/os.h>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
namespace dusk::native {
namespace {
constexpr char root[]="sdmc:/3ds/dusklight/";
std::array<ScriptedInput,256> inputs{};unsigned inputCount{};
std::array<std::uint64_t,64> captures{};unsigned captureCount{};
std::array<std::uint64_t,16> dumps{};unsigned dumpCount{};
std::array<std::uint64_t,16> depthAudits{};unsigned depthAuditCount{};
std::array<std::uint64_t,16> retainedFrames{};unsigned retainedFrameCount{};
std::uint64_t exitFrame{},currentFrame{};bool hasExit{};
const void* dumpData{};std::size_t dumpBytes{};
std::string_view token(std::string_view& text){
    const auto begin=text.find_first_not_of(" \t\r");
    if(begin==text.npos){text={};return {};}
    text.remove_prefix(begin);const auto end=text.find_first_of(" \t\r");
    const auto word=text.substr(0,end);text.remove_prefix(end==text.npos?text.size():end);return word;
}
template<class T> bool number(std::string_view word,T& value){
    const auto r=std::from_chars(word.data(),word.data()+word.size(),value);
    return !word.empty()&&r.ec==std::errc{}&&r.ptr==word.data()+word.size();
}
std::uint32_t key(std::string_view name){
    constexpr struct {const char* name;std::uint32_t key;} table[]={
        {"A",KEY_A},{"B",KEY_B},{"X",KEY_X},{"Y",KEY_Y},{"L",KEY_L},{"R",KEY_R},{"ZL",KEY_ZL},{"ZR",KEY_ZR},
        {"START",KEY_START},{"SELECT",KEY_SELECT},{"UP",KEY_DUP},{"DOWN",KEY_DDOWN},{"LEFT",KEY_DLEFT},{"RIGHT",KEY_DRIGHT}};
    for(const auto& entry:table)if(name==entry.name)return entry.key;
    return 0;
}
template<class F> bool eachLine(const char* name,F&& handle){
    char path[64];std::snprintf(path,sizeof(path),"%s%s",root,name);
    auto* file=std::fopen(path,"rb");if(!file)return true;
    static char text[16*1024];
    const auto count=std::fread(text,1,sizeof(text),file);const bool error=std::ferror(file)||count==sizeof(text);std::fclose(file);
    if(error)return false;
    std::string_view rest{text,count};unsigned line=0;
    while(!rest.empty()){
        const auto end=rest.find('\n');auto current=rest.substr(0,end);
        rest.remove_prefix(end==rest.npos?rest.size():end+1);++line;
        const auto comment=current.find('#');if(comment!=current.npos)current=current.substr(0,comment);
        if(current.find_first_not_of(" \t\r")==current.npos)continue;
        if(!handle(current)){OSReport("Native developer script %s line %u rejected\n",name,line);return false;}
    }
    return true;
}
template<std::size_t N> bool frameList(const char* name,std::array<std::uint64_t,N>& out,unsigned& count){
    return eachLine(name,[&](std::string_view line){
        std::uint64_t frame{};if(!number(token(line),frame)||!token(line).empty()||count>=N)return false;
        out[count++]=frame;return true;
    });
}
template<std::size_t N> bool contains(const std::array<std::uint64_t,N>& list,unsigned count,std::uint64_t frame){
    for(unsigned i=0;i<count;++i)if(list[i]==frame)return true;
    return false;
}
}
bool parseInputScriptLine(std::string_view line,ScriptedInput& out){
    ScriptedInput parsed;
    if(!number(token(line),parsed.first)||!number(token(line),parsed.last)||parsed.last<parsed.first)return false;
    auto buttons=token(line);if(buttons.empty())return false;
    if(buttons!="-")while(!buttons.empty()){
        const auto plus=buttons.find('+');const auto value=key(buttons.substr(0,plus));
        if(!value)return false;
        parsed.keys|=value;buttons.remove_prefix(plus==buttons.npos?buttons.size():plus+1);
    }
    const auto x=token(line);
    if(!x.empty()){
        int cx{},cy{};
        if(!number(x,cx)||!number(token(line),cy)||cx<-156||cx>156||cy<-156||cy>156)return false;
        parsed.circle=true;parsed.x=std::int16_t(cx);parsed.y=std::int16_t(cy);
    }
    if(!token(line).empty())return false;
    out=parsed;return true;
}
void loadDeveloperScripts(){
    // Explicit A/B switch for measuring instrumentation overhead with one binary.
    if(auto* profile=std::fopen("sdmc:/3ds/dusklight/profile-all-frames.txt","rb")){
        std::fclose(profile);gxProfileEveryFrame=true;
    }
    if(auto* cls=std::fopen("sdmc:/3ds/dusklight/profile-class.txt","rb")){
        const int c=std::fgetc(cls);std::fclose(cls);
        if(c>='0'&&c<='3'){gxProfileClassFilter=c-'0';OSReport("Native profile class filter: %d (2 = orthographic only)\n",gxProfileClassFilter);}
    }
    OSReport("Native detailed profiling: %s\n",gxProfileEveryFrame?"every frame":"one frame in 30");
    const bool okay=eachLine("input-script.txt",[](std::string_view line){
            ScriptedInput entry;if(inputCount>=inputs.size()||!parseInputScriptLine(line,entry))return false;
            inputs[inputCount++]=entry;return true;})&&
        frameList("capture-frames.txt",captures,captureCount)&&
        frameList("dump-frames.txt",dumps,dumpCount)&&
        frameList("depth-audit-frames.txt",depthAudits,depthAuditCount)&&
        frameList("depth-retained-frames.txt",retainedFrames,retainedFrameCount)&&
        eachLine("exit-frame.txt",[](std::string_view line){
            if(hasExit||!number(token(line),exitFrame)||!token(line).empty())return false;
            hasExit=true;return true;});
    if(!okay)OSPanic(__FILE__,__LINE__,"Invalid developer script on SD card");
    for(unsigned i=0;i<retainedFrameCount;++i){
        if(!retainedFrames[i]||!contains(depthAudits,depthAuditCount,retainedFrames[i])||
           (hasExit&&retainedFrames[i]>=exitFrame))
            OSPanic(__FILE__,__LINE__,"Retained depth frames must be nonzero audit frames before exit-frame");
        for(unsigned j=0;j<i;++j)if(retainedFrames[j]==retainedFrames[i])
            OSPanic(__FILE__,__LINE__,"Duplicate retained depth frame");
    }
    if(retainedFrameCount)OSReport("Native developer retained-depth: %u selected post boundaries; intrusive A/B experiment\n",retainedFrameCount);
    if(inputCount||captureCount||dumpCount||hasExit)
        OSReport("Native developer scripts: inputs=%u captures=%u dumps=%u exit=%llu\n",inputCount,captureCount,dumpCount,
            static_cast<unsigned long long>(hasExit?exitFrame:0));
}
void runDeveloperCalibration(){
    auto* file=std::fopen("sdmc:/3ds/dusklight/calibrate.txt","rb");if(!file)return;
    std::fclose(file);
    // Known instruction counts against the guest tick counter. Interprets
    // emulator "milliseconds" as instruction budgets; not hardware timing.
    constexpr unsigned iterations=4000000;
    auto ticks=[](auto&& body){const auto start=svcGetSystemTick();body();return svcGetSystemTick()-start;};
    const auto branch=ticks([]{unsigned n=iterations;asm volatile("1: subs %0,%0,#1\n bne 1b":"+r"(n)::"cc");});
    static volatile std::uint32_t memory[1024];
    const auto load=ticks([]{unsigned n=iterations;std::uint32_t sum=0;
        asm volatile("1: ldr r12,[%2]\n add %1,%1,r12\n subs %0,%0,#1\n bne 1b":"+r"(n),"+r"(sum):"r"(memory):"r12","cc","memory");});
    const auto vfp=ticks([]{unsigned n=iterations;float a=1.0001f,b=0.9999f;
        asm volatile("1: vmul.f32 %1,%1,%2\n subs %0,%0,#1\n bne 1b":"+r"(n),"+t"(a):"t"(b):"cc");});
    OSReport("Native developer calibration iterations=%u branch-loop=%llu (2 insn) load-loop=%llu (4 insn) vfp-loop=%llu (3 insn) ticks\n",
        iterations,static_cast<unsigned long long>(branch),static_cast<unsigned long long>(load),static_cast<unsigned long long>(vfp));
}
void setDeveloperFrame(std::uint64_t frame){currentFrame=frame;}
void setDeveloperDumpRegion(const void* data,std::size_t bytes){dumpData=data;dumpBytes=bytes;}
void applyScriptedInput(std::uint32_t& keys,std::int16_t& circleX,std::int16_t& circleY){
    for(unsigned i=0;i<inputCount;++i){
        const auto& entry=inputs[i];
        if(currentFrame<entry.first||currentFrame>entry.last)continue;
        keys|=entry.keys;
        if(entry.circle){circleX=entry.x;circleY=entry.y;}
    }
}
bool developerCaptureRequested(std::uint64_t frame){return contains(captures,captureCount,frame);}
bool developerExitRequested(std::uint64_t frame){return hasExit&&frame>=exitFrame;}
bool developerDumpRequested(std::uint64_t frame){return contains(dumps,dumpCount,frame);}
namespace {
// Explicitly intrusive developer capture, outside the measured game frame.
// Export only validated live registered images, never scan arbitrary memory.
bool dumpDeveloperTextures(std::uint64_t frame){
    constexpr std::size_t budget=16*1024*1024;
    std::size_t bytes=24+8;std::uint32_t count=0;
    for(std::size_t slot=0;slot<gx::textureRegistrationCapacity();++slot){
        gx::Texture texture;if(!gx::registeredTextureAt(slot,texture))continue;
        BtiInfo info;
        if(inspectBti({texture.bti.data(),texture.bti.size()},1024,info)!=ResourceStatus::ready)return false;
        const auto size=56+info.image.size+info.palette.size;
        if(size>budget-bytes)return false;
        bytes+=size;++count;
    }
    FS_ArchiveResource space{};
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&space))||
       !hasDumpSpace(std::uint64_t(space.clusterSize)*space.freeClusters,bytes,4096))return false;
    char path[128];std::snprintf(path,sizeof(path),"%sdumps/frame-%06llu-textures.dstx",root,
        static_cast<unsigned long long>(frame));
    const int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;
    auto* file=fdopen(fd,"wb");if(!file){close(fd);return false;}
    std::uint32_t checksum=2166136261u;bool okay=true;
    auto write=[&](const void* data,std::size_t size){
        if(okay){okay=std::fwrite(data,1,size,file)==size;checksum=dumpHash(data,size,checksum);}
    };
    const std::uint32_t reserved=0;
    write("DSKTEX01",8);write(&frame,8);write(&count,4);write(&reserved,4);
    for(std::size_t slot=0;okay&&slot<gx::textureRegistrationCapacity();++slot){
        gx::Texture texture;if(!gx::registeredTextureAt(slot,texture))continue;
        BtiInfo info;
        if(inspectBti({texture.bti.data(),texture.bti.size()},1024,info)!=ResourceStatus::ready){okay=false;break;}
        const std::uint32_t imageAddress=reinterpret_cast<std::uintptr_t>(info.image.data);
        const std::uint32_t paletteAddress=reinterpret_cast<std::uintptr_t>(info.palette.data);
        const std::uint32_t imageBytes=info.image.size,paletteBytes=info.palette.size;
        // Original 32-byte header plus exact image/palette spans; the host
        // rebuilds relative offsets when decoding this private snapshot.
        write(&texture.handle,8);write(&imageAddress,4);write(&paletteAddress,4);
        write(texture.bti.data(),32);write(&imageBytes,4);write(&paletteBytes,4);
        write(info.image.data,info.image.size);write(info.palette.data,info.palette.size);
    }
    const auto expected=checksum;
    if(okay)okay=std::fwrite("END!",1,4,file)==4&&std::fwrite(&expected,1,4,file)==4;
    if(!flushDumpFile(file))okay=false;
    // Read back the full bounded file before reporting a complete export.
    file=okay?std::fopen(path,"rb"):nullptr;if(!file)return false;
    std::array<unsigned char,4096> scratch{};std::size_t remaining=bytes-8;checksum=2166136261u;
    while(okay&&remaining){
        const auto size=remaining<scratch.size()?remaining:scratch.size();
        if(std::fread(scratch.data(),1,size,file)!=size){okay=false;break;}
        checksum=dumpHash(scratch.data(),size,checksum);remaining-=size;
    }
    unsigned char footer[8]{};
    if(std::fread(footer,1,8,file)!=8||std::memcmp(footer,"END!",4)||
       std::memcmp(footer+4,&expected,4)||std::fgetc(file)!=EOF||std::ferror(file))okay=false;
    if(std::fclose(file))okay=false;
    okay=okay&&checksum==expected;
    OSReport("Native developer textures frame=%llu images=%lu bytes=%lu checksum=%08lx %s\n",
        static_cast<unsigned long long>(frame),static_cast<unsigned long>(count),static_cast<unsigned long>(bytes),
        static_cast<unsigned long>(expected),okay?"written":"FAILED");
    return okay;
}
}
bool dumpDeveloperMemory(std::uint64_t frame){
    if(!dumpData||!dumpBytes)return false;
    FS_ArchiveResource space{};
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&space))||
       !hasDumpSpace(std::uint64_t(space.clusterSize)*space.freeClusters,dumpBytes,16*1024*1024+4096)){
        OSReport("Native developer dump frame=%llu FAILED: insufficient SD reserve\n",static_cast<unsigned long long>(frame));return false;
    }
    mkdir("sdmc:/3ds/dusklight/dumps",0777);
    char path[96];std::snprintf(path,sizeof(path),"%sdumps/frame-%06llu-arena-%08lx.bin",root,
        static_cast<unsigned long long>(frame),static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(dumpData)));
    static std::array<unsigned char,64*1024> scratch{};std::uint32_t checksum{};
    const bool okay=writeStreamedDumpFile(path,dumpData,dumpBytes,scratch.data(),scratch.size(),checksum);
    OSReport("Native developer dump frame=%llu bytes=%lu checksum=%08lx %s\n",static_cast<unsigned long long>(frame),
        static_cast<unsigned long>(dumpBytes),static_cast<unsigned long>(checksum),okay?"written":"FAILED");
    OSReport("Native developer dump coherence=chunked-live-memory-not-atomic; render-owner texture snapshot follows\n");
    const bool textures=dumpDeveloperTextures(frame);
    return okay&&textures;
}
bool developerDepthAuditRequested(std::uint64_t frame){
    return contains(depthAudits,depthAuditCount,frame);
}
bool developerRetainedDepthDiagnosticFrame(std::uint64_t frame){
    if(!retainedFrameCount)return false;
    return contains(retainedFrames,retainedFrameCount,frame)||(frame&&contains(retainedFrames,retainedFrameCount,frame-1));
}
namespace {
bool writeDepthAudit(const char* phase,const gx::TargetExtent& extent,
                     std::span<const std::byte> snapshot,
                     std::span<const FramebufferDepthQuery> queries,bool main){
    // Stable borrowed GPU readback; no game callback or next GPU read occurs
    // until both private files have been written and read back for integrity.
    if(!extent.valid()||snapshot.size()!=std::size_t(extent.pixelWidth)*extent.pixelHeight*4||
       snapshot.size()>4*1024*1024||queries.size()>maxFramebufferDepthQueries)return false;
    FS_ArchiveResource space{};
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&space))||
       !hasDumpSpace(std::uint64_t(space.clusterSize)*space.freeClusters,snapshot.size(),8192))return false;
    mkdir("sdmc:/3ds/dusklight/depth-audit",0700);
    static std::uint64_t sequenceFrame{};static unsigned sequence{};
    if(sequenceFrame!=currentFrame){sequenceFrame=currentFrame;sequence=0;}
    const auto serial=sequence++;
    char filename[80],path[160];
    std::snprintf(filename,sizeof(filename),"frame-%06llu-%s-%02u.bin",
                  static_cast<unsigned long long>(currentFrame),phase,serial);
    std::snprintf(path,sizeof(path),"%sdepth-audit/%s",root,filename);
    std::array<unsigned char,4096> scratch{};std::uint32_t checksum{};
    if(!writeVerifiedDumpFile(path,snapshot.data(),snapshot.size(),scratch.data(),scratch.size(),checksum))return false;
    std::array<char,8192> metadata{};std::size_t used=0;bool okay=true;
    const auto append=[&](const char* format,auto... args){
        if(!okay)return;
        const auto count=std::snprintf(metadata.data()+used,metadata.size()-used,format,args...);
        if(count<0||std::size_t(count)>=metadata.size()-used){okay=false;return;}
        used+=std::size_t(count);
    };
    append("{\"schema\":1,\"frame\":%llu,\"phase\":\"%s\",\"main\":%s,\"file\":\"%s\","
           "\"format\":\"PICA_D24S8_LE_MORTON8_ROTATED\",\"logical_width\":%u,\"logical_height\":%u,"
           "\"pixel_width\":%u,\"pixel_height\":%u,\"bytes\":%lu,\"fnv1a32\":\"%08lx\",\"queries\":[",
           static_cast<unsigned long long>(currentFrame),phase,main?"true":"false",filename,
           extent.logicalWidth,extent.logicalHeight,extent.pixelWidth,extent.pixelHeight,
           static_cast<unsigned long>(snapshot.size()),static_cast<unsigned long>(checksum));
    bool first=true;
    for(const auto& query:queries)if(query.output){
        append("%s{\"x\":%u,\"y\":%u,\"gx_depth\":%lu}",first?"":",",unsigned(query.x),unsigned(query.y),
               static_cast<unsigned long>(*query.output));first=false;
    }
    append("]}\n");
    std::snprintf(path,sizeof(path),"%sdepth-audit/frame-%06llu-%s-%02u.json",root,
                  static_cast<unsigned long long>(currentFrame),phase,serial);
    std::uint32_t metadataChecksum{};
    return okay&&writeVerifiedDumpFile(path,metadata.data(),used,scratch.data(),scratch.size(),metadataChecksum);
}
}
namespace {
struct RetainedObservation {
    const char* stage{};std::uint64_t frame{};DepthProbeState state{};
    std::uint32_t hashA{},hashB{},fileHashA{},fileHashB{};
    bool hasB{},equal{},payloadSaved{};
};
struct RetainedExperiment {
    bool active{};std::uint64_t frame{};RetainedDepthProbe images{};DepthProbeCopy copy{};
    std::array<RetainedObservation,16> observations{};unsigned count{};int finalInvalidate{-1};
} retained;
// Exact file-byte readback in addition to the existing writer's length/FNV
// verification. Sources remain live; mutation while writing fails the probe.
bool writeRetainedFile(const char* path,const void* data,std::size_t bytes,std::uint32_t& hash){
    std::array<unsigned char,4096> scratch{};
    if(!writeVerifiedDumpFile(path,data,bytes,scratch.data(),scratch.size(),hash))return false;
    auto* file=std::fopen(path,"rb");if(!file)return false;
    const auto* source=static_cast<const unsigned char*>(data);bool okay=true;
    for(std::size_t offset=0;okay&&offset<bytes;){
        const auto n=bytes-offset<scratch.size()?bytes-offset:scratch.size();
        okay=std::fread(scratch.data(),1,n,file)==n&&!std::memcmp(scratch.data(),source+offset,n);offset+=n;
    }
    if(std::fgetc(file)!=EOF||std::ferror(file))okay=false;
    if(std::fclose(file))okay=false;
    return okay;
}
RetainedObservation& recordRetained(const char* stage){
    if(retained.count>=retained.observations.size())OSPanic(__FILE__,__LINE__,"Retained depth ledger overflow");
    auto& record=retained.observations[retained.count++];record={};
    record.stage=stage;record.frame=currentFrame;record.state=platformDepthProbeState();
    const auto& images=retained.images;
    record.hashA=dumpHash(images.imageA.data(),images.imageA.size());
    record.hasB=!images.imageB.empty();
    if(record.hasB){
        record.hashB=dumpHash(images.imageB.data(),images.imageB.size());
        record.equal=!std::memcmp(images.imageA.data(),images.imageB.data(),images.imageA.size());
    }
    return record;
}
void saveRetainedPair(RetainedObservation& record){
    // Only immediate mismatches and a changed/mismatching final observation
    // write payloads. No observation between these points performs file I/O.
    char path[192];
    std::snprintf(path,sizeof(path),"%sdepth-retained/frame-%06llu-%s-A.bin",root,
        static_cast<unsigned long long>(retained.frame),record.stage);
    if(!writeRetainedFile(path,retained.images.imageA.data(),retained.images.imageA.size(),record.fileHashA)||record.fileHashA!=record.hashA)
        OSPanic(__FILE__,__LINE__,"Retained depth A payload changed or write failed");
    std::snprintf(path,sizeof(path),"%sdepth-retained/frame-%06llu-%s-B.bin",root,
        static_cast<unsigned long long>(retained.frame),record.stage);
    if(!writeRetainedFile(path,retained.images.imageB.data(),retained.images.imageB.size(),record.fileHashB)||record.fileHashB!=record.hashB)
        OSPanic(__FILE__,__LINE__,"Retained depth B payload changed or write failed");
    record.payloadSaved=true;
}
void writeRetainedLedger(bool complete,const char* ending){
    static std::array<char,16384> metadata{};std::size_t used=0;bool okay=true;
    const auto append=[&](const char* format,auto... args){
        if(!okay)return;
        const auto n=std::snprintf(metadata.data()+used,metadata.size()-used,format,args...);
        if(n<0||std::size_t(n)>=metadata.size()-used){okay=false;return;}used+=std::size_t(n);
    };
    const auto state=[&](const DepthProbeState& s){
        append("{\"tick\":%llu,\"commands\":%llu,\"command_words\":%lu,\"command_offset\":%lu,\"caller\":%llu,\"owner\":%llu}",
            static_cast<unsigned long long>(s.tick),static_cast<unsigned long long>(s.commands),
            static_cast<unsigned long>(s.commandWords),static_cast<unsigned long>(s.commandOffset),
            static_cast<unsigned long long>(s.caller),static_cast<unsigned long long>(s.owner));
    };
    append("{\"schema\":1,\"kind\":\"retained-depth-boundary-probe\",\"frame\":%llu,\"attempt\":1,\"complete\":%s,\"ending\":\"%s\","
        "\"format\":\"PICA_D24S8_LE_MORTON8_ROTATED\",\"pixel_width\":400,\"pixel_height\":240,"
        "\"target\":%llu,\"color_capture_requested\":%s,\"console_refresh_expected\":%s,"
        "\"queue\":null,\"queue_unavailable\":\"binding and generation private to unchanged gpu_sync_ctru.cpp\","
        "\"a_original_waits\":\"existing readback returned success; internal states not instrumented\","
        "\"window_starts\":\"after existing verified main post SD export, before B and color\",\"ranges\":{",
        static_cast<unsigned long long>(retained.frame),complete?"true":"false",ending,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(retained.images.target)),
        developerCaptureRequested(retained.frame)?"true":"false",retained.frame==1||retained.frame%30==0?"true":"false");
    const auto range=[&](const char* name,const DepthProbeRange& r,bool comma){
        append("%s\"%s\":{\"va\":%llu,\"physical\":%llu,\"bytes\":%lu,\"capacity\":%lu}",comma?",":"",name,
            static_cast<unsigned long long>(r.address),static_cast<unsigned long long>(r.physical),
            static_cast<unsigned long>(r.bytes),static_cast<unsigned long>(r.capacity));
    };
    range("A",retained.images.a,false);range("B",retained.images.b,true);range("depth",retained.images.depth,true);
    range("color",retained.images.color,true);range("bottom",retained.images.bottom,true);
    append("},\"B_copy\":{\"timeout_ns\":500000000,\"bytes\":%lu,\"flags\":8,\"in_dim\":0,\"out_dim\":0,"
        "\"completion_values\":\"-1=not attempted,0=complete,1=notReady,2=wrongThread,3=timedOut,4=exhausted\","
        "\"prior_completion\":%d,\"copy_completion\":%d,\"invalidate_result\":%d,\"before_prior\":",
        static_cast<unsigned long>(retainedDepthProbeBytes),retained.copy.priorCompletion,retained.copy.copyCompletion,retained.copy.invalidateResult);
    state(retained.copy.beforePrior);append(",\"before_copy\":");state(retained.copy.beforeCopy);
    append(",\"after_copy\":");state(retained.copy.afterCopy);append(",\"after_wait\":");state(retained.copy.afterWait);
    append("},\"final_A_invalidate_result\":%d,\"observations\":[",retained.finalInvalidate);
    for(unsigned i=0;i<retained.count;++i){
        const auto& r=retained.observations[i];
        append("%s{\"stage\":\"%s\",\"observed_frame\":%llu,\"state\":",i?",":"",r.stage,static_cast<unsigned long long>(r.frame));state(r.state);
        append(",\"A_fnv1a32\":\"%08lx\",\"B_fnv1a32\":\"%08lx\",\"has_B\":%s,\"A_equals_B_bytes\":%s,"
            "\"payload_saved\":%s,\"payload_A_fnv1a32\":\"%08lx\",\"payload_B_fnv1a32\":\"%08lx\"}",
            static_cast<unsigned long>(r.hashA),static_cast<unsigned long>(r.hashB),r.hasB?"true":"false",r.equal?"true":"false",
            r.payloadSaved?"true":"false",static_cast<unsigned long>(r.fileHashA),static_cast<unsigned long>(r.fileHashB));
    }
    append("]}\n");char path[128];std::uint32_t hash{};
    std::snprintf(path,sizeof(path),"%sdepth-retained/frame-%06llu-ledger.json",root,static_cast<unsigned long long>(retained.frame));
    if(!okay||!writeRetainedFile(path,metadata.data(),used,hash))OSPanic(__FILE__,__LINE__,"Retained depth ledger write/verification failed");
    OSReport("Native developer retained-depth frame=%llu ending=%s ledger verified\n",static_cast<unsigned long long>(retained.frame),ending);
}
void beginRetainedDepth(std::span<const std::byte> snapshot){
    if(retained.active)OSPanic(__FILE__,__LINE__,"Previous retained depth observation not finished");
    retained={};retained.frame=currentFrame;retained.images.imageA=snapshot;
    recordRetained("post-A-immediate");
    // Reserve enough space for two immediate and two final payloads plus the
    // bounded ledger before acquiring the independent linear destination.
    FS_ArchiveResource space{};
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&space))||
       !hasDumpSpace(std::uint64_t(space.clusterSize)*space.freeClusters,4*retainedDepthProbeBytes,16384))
        OSPanic(__FILE__,__LINE__,"Retained depth probe SD reserve unavailable");
    mkdir("sdmc:/3ds/dusklight/depth-retained",0700);
    if(!platformRetainDepthReadback(mainFramebuffer(),snapshot,retained.images))
        OSPanic(__FILE__,__LINE__,"Retained depth probe allocation/identity/ownership failure");
    retained.active=true;
    recordRetained("post-A-repeat-before-B");
    if(!platformCaptureRetainedDepthB(retained.images,retained.copy)){
        recordRetained("B-capture-failed");writeRetainedLedger(false,"B-capture-failed");
        // Do not unlock, free or expose B after a failed producer fence.
        OSPanic(__FILE__,__LINE__,"Retained depth B capture failed; allocations quarantined");
    }
    auto& record=recordRetained("post-after-B");
    if(!record.equal)saveRetainedPair(record);
}
}
void observeDeveloperRetainedDepth(RetainedDepthStage stage){
    if(!retained.active)return;
    constexpr const char* names[]={"after-color","after-FrameEnd","before-console","after-console-writes-before-flush","after-console-flush","after-reporting-before-nextBegin"};
    recordRetained(names[unsigned(stage)]); // No GPU commands, invalidations or SD output.
}
void finishDeveloperRetainedDepth(bool afterNextBegin){
    if(!retained.active)return;
    recordRetained(afterNextBegin?"after-next-blocking-FrameBegin-before-pre":"shutdown-after-completion-no-nextBegin");
    retained.finalInvalidate=platformInvalidateRetainedDepthA();
    auto& last=recordRetained("final-A-explicit-invalidate");
    bool changed=false;
    for(unsigned i=0;i<retained.count;++i){
        const auto& r=retained.observations[i];
        changed|=r.hashA!=retained.observations[0].hashA||(r.hasB&&(!r.equal||r.hashB!=retained.observations[2].hashB));
    }
    if(changed)saveRetainedPair(last);
    writeRetainedLedger(afterNextBegin&&!R_FAILED(retained.finalInvalidate),afterNextBegin?"nextBegin":"shutdown-incomplete-window");
    if(R_FAILED(retained.finalInvalidate)||!platformReleaseRetainedDepth())
        OSPanic(__FILE__,__LINE__,"Retained depth final invalidation/release failed");
    retained.active=false;
}
void captureDeveloperDepthBoundary(bool afterRender){
    if(!developerDepthAuditRequested(currentFrame))return;
    std::span<const std::byte> snapshot;
    const auto phase=afterRender?"post":"pre";
    const bool captured=platformReadFramebufferDepthImage(mainFramebuffer(),snapshot);
    const bool probe=afterRender&&contains(retainedFrames,retainedFrameCount,currentFrame);
    if(probe&&!captured)OSPanic(__FILE__,__LINE__,"Retained depth original post capture failed");
    const bool okay=captured&&writeDepthAudit(phase,gx::currentTargetExtent,snapshot,{},true);
    // Preserve the ordinary post artifact before the extra B transfer. The
    // retained window starts after that existing synchronous SD export.
    if(probe){
        if(!okay)OSPanic(__FILE__,__LINE__,"Retained depth original post export failed");
        beginRetainedDepth(snapshot);
    }
    OSReport("Native developer depth-audit frame=%llu phase=%s %s\n",
             static_cast<unsigned long long>(currentFrame),phase,okay?"written":"FAILED");
}
void captureDeveloperDepthQueries(const gx::TargetExtent& extent,std::span<const std::byte> snapshot,
                                 std::span<const FramebufferDepthQuery> queries){
    if(!developerDepthAuditRequested(currentFrame))return;
    const bool okay=writeDepthAudit("peek",extent,snapshot,queries,activeFramebuffer()==mainFramebuffer());
    OSReport("Native developer depth-audit frame=%llu phase=peek queries=%lu %s\n",
             static_cast<unsigned long long>(currentFrame),static_cast<unsigned long>(queries.size()),okay?"written":"FAILED");
}
}
