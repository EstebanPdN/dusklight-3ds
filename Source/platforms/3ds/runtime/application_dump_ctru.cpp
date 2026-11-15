#include "application_dump_ctru.hpp"
#include "application_dump.hpp"
#include "gx_framebuffer.hpp"
#include "gpu_sync.hpp"
#include "dump_io.hpp"
#include "bootstrap.hpp"
#include "gpu_canary.hpp"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <cstdarg>
extern "C" void OSReport(const char*,...);
#include <3ds.h>
#include <citro3d.h>
#include <atomic>
#include <span>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <unistd.h>
#include <dirent.h>
#include "release.hpp"
#include "frame_telemetry.hpp"

#ifndef DUSK_DUMP_BUILD_ID
#define DUSK_DUMP_BUILD_ID "unavailable-rebuild-with-DUSK_DUMP_BUILD_ID"
#endif
extern "C" unsigned duskGxJobRingCopy(u32* out,unsigned max,u32* submittedTotal);
namespace dusk::native {
namespace {
using namespace appdump;
// Fixed workspace includes presented LCD conversion; excluded from the RAM walk.
alignas(4096) Workspace workspace;
Trigger trigger;
std::atomic<bool> suppressed{},active{};
bool requested{},resumeBlocked{};
std::uint64_t lastProgressTick{};
char completedPath[256]{};
char dumpNote[512]{};
void (*dumpStateReport)(std::FILE*){};
bool mkdirOrExists(const char* name);
// One folder per dump (Alpha 1): NNN-dump-YYYYMMDD-HHMMSS/ holds the packed
// dump, both screens as presented (BMP) and info.txt. NNN follows the highest
// number already in the dumps folder.
unsigned nextDumpNumber(){
    unsigned highest=0;
    if(auto* dir=opendir("sdmc:/3ds/DuskLight/dumps")){
        while(auto* entry=readdir(dir)){
            unsigned number=0;char tail[2]{};
            if(std::sscanf(entry->d_name,"%u-dump-%1s",&number,tail)==2&&number>highest)highest=number;
        }
        closedir(dir);
    }
    return highest+1;
}
// The GSP front buffer (240 pixels per column, columns left to right, bottom
// row first) is a bottom-up BMP turned by 90 degrees: BMP row r is element r
// of every column. One unbuffered write per file.
bool writeScreenBmp(const char* name,const u8* source,unsigned width,unsigned bytesPerPixel,u8* scratch,std::size_t capacity){
    const std::size_t row=std::size_t(width)*3,bytes=54+row*240;
    if(!source||!scratch||bytes>capacity)return false;
    std::memset(scratch,0,54);
    auto put=[scratch](unsigned at,std::uint32_t v){scratch[at]=u8(v);scratch[at+1]=u8(v>>8);scratch[at+2]=u8(v>>16);scratch[at+3]=u8(v>>24);};
    scratch[0]='B';scratch[1]='M';put(2,std::uint32_t(bytes));put(10,54);put(14,40);put(18,width);put(22,240);
    scratch[26]=1;scratch[28]=24;put(34,std::uint32_t(row*240));put(38,2835);put(42,2835);
    u8* out=scratch+54;
    for(unsigned r=0;r<240;++r)for(unsigned x=0;x<width;++x){
        const u8* p=source+(std::size_t(x)*240+r)*bytesPerPixel;u8* o=out+r*row+std::size_t(x)*3;
        if(bytesPerPixel==3){o[0]=p[0];o[1]=p[1];o[2]=p[2];}
        else{const unsigned v=unsigned(p[0])|unsigned(p[1])<<8;o[0]=u8((v&31)*255/31);o[1]=u8((v>>5&63)*255/63);o[2]=u8((v>>11)*255/31);}
    }
    auto* file=std::fopen(name,"wb");if(!file)return false;
    std::setvbuf(file,nullptr,_IONBF,0);
    const bool okay=std::fwrite(scratch,1,bytes,file)==bytes;
    return std::fclose(file)==0&&okay;
}
bool query(void*,std::uint32_t address,Region& r) {
    MemInfo m{};PageInfo p{};
    if(R_FAILED(svcQueryMemory(&m,&p,address)))return false;
    r={m.base_addr,m.size,m.perm,m.state,p.flags};return true;
}
bool read(void*,const Region& original,std::uint32_t address,void* output,std::size_t n) {
    Region now{};
    if(!n||n>chunkBytes||std::uint64_t(address)+n>addressLimit||
       !query(nullptr,address,now)||!validRegion(now,address)||exclusion(now)||
       now.perm!=original.perm||now.state!=original.state||now.flags!=original.flags||
       std::uint64_t(address)+n>std::uint64_t(now.base)+now.size)return false;
    // No debug handle / svcReadProcessMemory assumption. Workers continue.
    // There is an unavoidable query-to-copy mapping race in a normal process;
    // no recoverable data-abort handler or pinning API is claimed here.
    std::memcpy(output,reinterpret_cast<const void*>(address),n);
    return true;
}
bool space(void*,std::uint64_t& free) {
    FS_ArchiveResource s{};if(R_FAILED(FSUSER_GetSdmcArchiveResource(&s)))return false;
    free=std::uint64_t(s.clusterSize)*s.freeClusters;return true;
}
bool poll(void*) {
    if(!aptMainLoop())return false;
    hidScanInput();
    return !(hidKeysDown()&KEY_B);
}
void progress(void*,const char* text,std::uint64_t done,std::uint64_t total) {
    const auto now=svcGetSystemTick();
    // Terminal messages always repaint; active progress at most four times/sec.
    if(done&&done<total&&now-lastProgressTick<SYSCLOCK_ARM11/4)return;
    lastProgressTick=now;
    std::printf("\x1b[2J\x1b[1;1HDUSKLIGHT - Private diagnostic dump\n\n%.40s\n\n%llu / %llu KiB\n\nL+R+A: RAM + LCD + VRAM\nB: cancel the dump\n\nCaptured in blocks, not atomic\nDo not switch off or remove the SD\n\nFolder: /3ds/DuskLight/dumps/\n",
        text,(unsigned long long)(done/1024),(unsigned long long)(total/1024));
    auto* fb=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr);
    // consoleInit uses a single bottom framebuffer; do not swap/reset top UI.
    // No retrace wait: the capture must not idle 16 ms per progress line.
    if(fb)GSPGPU_FlushDataCache(fb,320*240*2);
}
// A failed GPU completion cannot return to APT, FrameBegin or GPU teardown:
// those SDK paths can wait forever for the same pending work. Preserve every
// referenced allocation and use the existing native worker-fault containment
// policy: direct kernel process exit, no C++/service teardown or normal resume.
bool completeDumpGpu(){
    if(platformCompleteGpu(500000000)==GpuCompletion::complete)return true;
    constexpr char message[]="Native application dump GPU completion failed; capture incomplete; direct kernel process exit\n";
    svcOutputDebugString(message,sizeof(message)-1);
    svcExitProcess();
}
GSPGPU_CaptureInfo lcd{};
bool readable(const void* pointer,std::size_t bytes,bool vram=false) {
    const auto begin=reinterpret_cast<std::uintptr_t>(pointer);
    const auto end=std::uint64_t(begin)+bytes;
    if(!pointer||!bytes||end>addressLimit)return false;
    if(vram&&(begin<OS_VRAM_VADDR||end>OS_VRAM_VADDR+OS_VRAM_SIZE))return false;
    for(auto at=std::uint64_t(begin);at<end;){
        Region r{};if(!query(nullptr,std::uint32_t(at),r)||!validRegion(r,at)||!(r.perm&1)||(r.perm&~7u))return false;
        // Exact SDK VRAM mapping is separately admitted; never arbitrary MMIO.
        if(vram){if(r.state!=3&&r.state!=7&&r.state!=11)return false;}
        else if(exclusion(r))return false;
        at=std::min(end,std::uint64_t(r.base)+r.size);
    }
    return true;
}
bool screens(void*,Blob& top,Blob& bottom) {
    // Drain the already submitted frame, then observe the display service's
    // frontbuffers after VBlank. No EFB/offscreen readback supplies these pixels.
    if(!completeDumpGpu())return false;
    gspWaitForVBlank();
    if(R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&lcd)))return false;
    const auto& t=lcd.screencapture[0];const auto& b=lcd.screencapture[1];
    if((t.format&7)!=GSP_BGR8_OES||t.framebuf_widthbytesize!=240*3||
       (b.format&7)!=GSP_RGB565_OES||b.framebuf_widthbytesize!=240*2||
       !readable(t.framebuf0_vaddr,400*240*3)||!readable(b.framebuf0_vaddr,320*240*2))return false;
    if(R_FAILED(GSPGPU_InvalidateDataCache(t.framebuf0_vaddr,400*240*3)))return false;
    const auto* source=reinterpret_cast<const unsigned char*>(t.framebuf0_vaddr);
    for(unsigned i=0;i<400*240;++i){
        workspace.lcdTop[4*i]=255;workspace.lcdTop[4*i+1]=source[3*i];
        workspace.lcdTop[4*i+2]=source[3*i+1];workspace.lcdTop[4*i+3]=source[3*i+2];
    }
    top={workspace.lcdTop,sizeof(workspace.lcdTop)};
    bottom={b.framebuf0_vaddr,320*240*2};return true;
}
void* vramStaging{}; // one bounded64KiB retained diagnostic allocation, reused after GPU drain
bool writeVram(const char* name,Workspace& w,std::uint32_t& hash,bool& cancelled){
    if(!vramStaging)vramStaging=linearAlloc(chunkBytes);
    auto* staging=vramStaging;if(!staging)return false;
    auto* file=std::fopen(name,"wx");if(!file)return false;
    bool okay=true;bool completed=true;hash=2166136261u;
    for(std::size_t offset=0;offset<OS_VRAM_SIZE&&okay;offset+=chunkBytes){
        // Direct GX enqueue has no Citro3D outside-frame infinite wait.
        // Complete/reclaim the owner queue BEFORE admission, then retain the
        // staging address until deadline-based completion of the submitted copy.
        completed=completeDumpGpu();
        if(!completed){okay=false;break;}
        if(!poll(nullptr)){cancelled=true;okay=false;break;}
        if(R_FAILED(GX_TextureCopy(reinterpret_cast<u32*>(OS_VRAM_VADDR+offset),0,static_cast<u32*>(staging),0,chunkBytes,8))){okay=false;break;}
        completed=completeDumpGpu();
        okay=completed&&R_SUCCEEDED(GSPGPU_InvalidateDataCache(staging,chunkBytes));
        if(okay){okay=std::fwrite(staging,1,chunkBytes,file)==chunkBytes;hash=dumpHash(staging,chunkBytes,hash);}
        if(okay)progress(nullptr,"Capturing and verifying VRAM",offset+chunkBytes,OS_VRAM_SIZE);
    }
    if(!flushDumpFile(file))okay=false;
    // Storage remains valid even on a failed GPU wait; no dangling DMA destination.
    if(!okay)return false;
    file=std::fopen(name,"rb");if(!file)return false;
    std::uint32_t checked=2166136261u;std::size_t remaining=OS_VRAM_SIZE;
    while(remaining&&okay){
        if(remaining%chunkBytes==0&&!poll(nullptr)){cancelled=true;okay=false;break;}
        auto n=std::min(remaining,sizeof(w.verify));
        okay=std::fread(w.verify,1,n,file)==n;checked=dumpHash(w.verify,n,checked);remaining-=n;}
    if(std::fgetc(file)!=EOF||std::ferror(file))okay=false;
    if(std::fclose(file))okay=false;return okay&&hash==checked;
}
constexpr const char* extraNames[]={"vram.bin","lcd-top-left.bin","lcd-top-right.bin","lcd-bottom.bin","extras.txt"};
void removeSupplement(void*,const char* dir){
    char name[320];for(const auto* file:extraNames){std::snprintf(name,sizeof(name),"%s/%s",dir,file);::unlink(name);}
}
bool supplement(void*,const char* dir,Workspace& w,Supplement& out){
    struct File {const char* name;const void* data;std::size_t bytes;const char* layout;};
    const auto& t=lcd.screencapture[0];const auto& b=lcd.screencapture[1];
    File files[]={{"lcd-top-left.bin",t.framebuf0_vaddr,400*240*3,"GSP-BGR8-rotated"},
        {"lcd-top-right.bin",t.framebuf1_vaddr,400*240*3,"GSP-BGR8-rotated"},
        {"lcd-bottom.bin",b.framebuf0_vaddr,320*240*2,"GSP-RGB565-rotated"},
        {"vram.bin",reinterpret_cast<const void*>(OS_VRAM_VADDR),OS_VRAM_SIZE,"raw-VRAM-GPU-copy"}};
    char inventory[2048]{};std::size_t used=0;
    const int header=std::snprintf(inventory,sizeof(inventory),
        "schema=1\nsource=GSP-ImportDisplayCaptureInfo-after-GPU-drain-and-VBlank\n"
        "coherence=sequential-live-not-atomic\nphysical-LCD-photons=not-measured\nVRAM_access=GPU-copy-64KiB-staging\n"
        "vram_base=%08x\nvram_size=%u\nvram_free=%lu\nlinear_free=%lu\n"
        "application_used=%lu\napplication_free=%lu\n"
        "top_format=%08lx\ntop_stride=%lu\nbottom_format=%08lx\nbottom_stride=%lu\n"
        "top_right_aliases_left=%u\nregisters=not-captured\nMMIO=not-read\n",
        unsigned(OS_VRAM_VADDR),unsigned(OS_VRAM_SIZE),(unsigned long)vramSpaceFree(),(unsigned long)linearSpaceFree(),
        (unsigned long)osGetMemRegionUsed(MEMREGION_APPLICATION),(unsigned long)osGetMemRegionFree(MEMREGION_APPLICATION),
        (unsigned long)t.format,(unsigned long)t.framebuf_widthbytesize,(unsigned long)b.format,(unsigned long)b.framebuf_widthbytesize,
        t.framebuf0_vaddr==t.framebuf1_vaddr);
    if(header<0||std::size_t(header)>=sizeof(inventory))return false;used=header;
    for(unsigned i=0;i<4;++i){
        const auto& f=files[i];
        if(!poll(nullptr)){out.cancelled=true;return false;}
        if(i==1&&!f.data)continue; // explicitly absent right buffer in 2D
        if(i!=3&&!readable(f.data,f.bytes))return false;
        char name[320];std::snprintf(name,sizeof(name),"%s/%s",dir,f.name);std::uint32_t hash{};
        if(i==3){if(!writeVram(name,w,hash,out.cancelled))return false;}
        else if(!writeStreamedDumpFile(name,f.data,f.bytes,w.verify,sizeof(w.verify),hash))return false;
        const int n=std::snprintf(inventory+used,sizeof(inventory)-used,
            "file=%s bytes=%lu fnv1a32=%08lx address=%08lx layout=%s\n",f.name,(unsigned long)f.bytes,
            (unsigned long)hash,(unsigned long)reinterpret_cast<std::uintptr_t>(f.data),f.layout);
        if(n<0||std::size_t(n)>=sizeof(inventory)-used)return false;
        used+=n;out.payloadBytes+=f.bytes;
    }
    char name[320];std::snprintf(name,sizeof(name),"%s/extras.txt",dir);
    if(!writeVerifiedDumpFile(name,inventory,used,w.verify,sizeof(w.verify),out.inventoryHash))return false;
    out.inventoryBytes=used;return true;
}

// ---------------------------------------------------------------------------
// Packed capture (format 2). The first console dump (format 1) wrote 1 894
// files in 30 244 writes of 4 KiB and took 2 386 s for 124 MB: every small
// write and every file creation costs the console SD tens of milliseconds.
// More than half of that RAM was zero pages and the rest compresses to about
// a quarter. Format 2 is one file written in megabyte units: zero blocks are
// only recorded, the others are compressed (zstd) from a staging copy, and
// the text index (what format 1 kept in manifest.txt, map.txt and extras.txt)
// follows the data. Tools/3ds/verify_application_dump.py reads both formats.
namespace pack {
constexpr std::size_t tempBytes=3*1024*1024,stageBytes=512*1024,indexBytes=96*1024;
struct Writer {
    Handle file{};bool open{},failed{};
    u8* out{};std::size_t capacity{},used{},unit{};
    std::uint64_t offset{},ticks{};unsigned writes{};
    std::uint64_t position()const{return offset+used;}
    bool drain(bool everything){
        while(open&&!failed&&(used>=unit||(everything&&used))){
            const u32 n=u32(used>=unit?unit:used);
            const auto start=svcGetSystemTick();u32 written=0;
            if(R_FAILED(FSFILE_Write(file,&written,offset,out,n,everything&&n==used?FS_WRITE_FLUSH:0))||written!=n){failed=true;break;}
            ticks+=svcGetSystemTick()-start;++writes;
            offset+=n;used-=n;if(used)std::memmove(out,out+n,used);
        }
        return open&&!failed;
    }
    u8* reserve(std::size_t bytes){
        if(capacity-used<bytes&&!drain(false))return nullptr;
        if(capacity-used<bytes&&!drain(true))return nullptr;
        return capacity-used>=bytes?out+used:nullptr;
    }
};
struct Index {
    char* text{};std::size_t capacity{},used{};bool overflow{};
    void line(const char* format,...) __attribute__((format(printf,2,3))){
        va_list arguments;va_start(arguments,format);
        const int n=std::vsnprintf(text+used,capacity-used,format,arguments);
        va_end(arguments);
        if(n<0||std::size_t(n)>=capacity-used)overflow=true;else used+=n;
    }
};
bool allZero(const u8* data,std::size_t bytes){
    std::size_t i=0;
    if(!(reinterpret_cast<std::uintptr_t>(data)&3))for(;i+16<=bytes;i+=16){
        const auto* w=reinterpret_cast<const u32*>(data+i);
        if(w[0]|w[1]|w[2]|w[3])return false;
    }
    for(;i<bytes;++i)if(data[i])return false;
    return true;
}
struct Packer {
    Writer writer;Index index;
    u8* stage{};std::size_t stageCapacity{};
    ZSTD_CCtx* zstd{};
    std::uint64_t rawBytes{},storedBytes{},zeroBytes{},compressTicks{},hashTicks{};
    unsigned zeroBlocks{},rawBlocks{},zstdBlocks{};
    // stage[0,bytes) holds one block; `prefix` is its index record up to the kind.
    bool store(const char* prefix,std::size_t bytes){
        rawBytes+=bytes;
        if(allZero(stage,bytes)){++zeroBlocks;zeroBytes+=bytes;index.line("%s zero\n",prefix);return !index.overflow;}
        auto tick=svcGetSystemTick();
        const std::uint32_t hash=dumpHash(stage,bytes);
        hashTicks+=svcGetSystemTick()-tick;
        const std::size_t bound=zstd?ZSTD_compressBound(bytes):bytes;
        u8* out=writer.reserve(bound);if(!out)return false;
        std::size_t stored=0;
        if(zstd){
            tick=svcGetSystemTick();
            const auto n=ZSTD_compress2(zstd,out,bound,stage,bytes);
            compressTicks+=svcGetSystemTick()-tick;
            if(!ZSTD_isError(n)&&n<bytes)stored=n;
        }
        const bool compressed=stored!=0;
        if(!compressed){std::memcpy(out,stage,bytes);stored=bytes;++rawBlocks;}else ++zstdBlocks;
        index.line("%s %s %llu %lu fnv1a32=%08lx\n",prefix,compressed?"zstd":"raw",
            static_cast<unsigned long long>(writer.position()),static_cast<unsigned long>(stored),static_cast<unsigned long>(hash));
        writer.used+=stored;storedBytes+=stored;
        return !index.overflow;
    }
    // Any readable bytes as blocks named `name` (offset within the blob).
    bool blob(const char* name,const void* data,std::size_t bytes){
        for(std::size_t at=0;at<bytes;){
            const auto n=std::min(stageCapacity,bytes-at);
            std::memcpy(stage,static_cast<const u8*>(data)+at,n);
            char prefix[96];std::snprintf(prefix,sizeof(prefix),"blob %s %08lx %08lx",name,static_cast<unsigned long>(at),static_cast<unsigned long>(n));
            if(!store(prefix,n))return false;
            at+=n;
        }
        return true;
    }
};
struct Interval {std::uint64_t begin{},end{};};
double seconds(std::uint64_t ticks){return double(ticks)/SYSCLOCK_ARM11;}
}
bool captureLcd(){
    if(!completeDumpGpu())return false;
    gspWaitForVBlank();
    if(R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&lcd)))return false;
    const auto& t=lcd.screencapture[0];const auto& b=lcd.screencapture[1];
    if((t.format&7)!=GSP_BGR8_OES||t.framebuf_widthbytesize!=240*3||
       (b.format&7)!=GSP_RGB565_OES||b.framebuf_widthbytesize!=240*2||
       !readable(t.framebuf0_vaddr,400*240*3)||!readable(b.framebuf0_vaddr,320*240*2))return false;
    return R_SUCCEEDED(GSPGPU_InvalidateDataCache(t.framebuf0_vaddr,400*240*3));
}
Result runPacked(const Metadata& m,char (&output)[256]){
    using namespace pack;
    output[0]=0;
    const auto startTick=svcGetSystemTick();
    Packer p;
    // Working memory outside every heap: a temporary linear block from the
    // free part of the application region (about 5.6 MiB at the title).
    u32 temp=0;
    int level=1;
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/dump-level.txt","rb")){if(std::fscanf(file,"%d",&level)!=1)level=1;std::fclose(file);}
    if(level<-7)level=-7;
    if(level>3)level=3;
    std::size_t zstdBytes=0;
    if(R_SUCCEEDED(svcControlMemory(&temp,0,0,tempBytes,MEMOP_ALLOC_LINEAR,MemPerm(MEMPERM_READ|MEMPERM_WRITE)))&&temp){
        auto* base=reinterpret_cast<u8*>(temp);
        p.stage=base;p.stageCapacity=stageBytes;
        p.index.text=reinterpret_cast<char*>(base+stageBytes);p.index.capacity=indexBytes;
        std::size_t at=stageBytes+indexBytes;
        if(level){
            zstdBytes=(ZSTD_estimateCCtxSize_usingCParams(ZSTD_getCParams(level,stageBytes,0))+63)&~std::size_t(63);
            const std::size_t needed=1024*1024+ZSTD_compressBound(stageBytes);
            if(zstdBytes&&at+zstdBytes+needed<=tempBytes){
                p.zstd=ZSTD_initStaticCCtx(base+at,zstdBytes);
                if(p.zstd&&(ZSTD_isError(ZSTD_CCtx_setParameter(p.zstd,ZSTD_c_compressionLevel,level))||
                            ZSTD_isError(ZSTD_CCtx_setParameter(p.zstd,ZSTD_c_checksumFlag,0))))p.zstd=nullptr;
                if(p.zstd)at+=zstdBytes;
            }
        }
        p.writer.out=base+at;p.writer.capacity=tempBytes-at;p.writer.unit=1024*1024;
    }else{
        // No free region: the fixed workspace. 64 KiB blocks, no compression.
        temp=0;
        p.stage=workspace.data;p.stageCapacity=sizeof(workspace.data);
        p.index.text=reinterpret_cast<char*>(workspace.lcdTop);p.index.capacity=64*1024;
        p.writer.out=workspace.lcdTop+64*1024;p.writer.capacity=sizeof(workspace.lcdTop)-64*1024;p.writer.unit=256*1024;
    }
    char path[160]{},finalPath[160]{};
    auto finish=[&](Result result){
        if(p.writer.open){FSFILE_Close(p.writer.file);p.writer.open=false;}
        if(result!=Result::success&&path[0]){char name[176];std::snprintf(name,sizeof(name),"sdmc:%s",path);std::remove(name);}
        if(temp){u32 ignored=0;svcControlMemory(&ignored,temp,0,tempBytes,MEMOP_FREE,MemPerm(0));}
        if(result!=Result::success)progress(nullptr,resultText(result),0,0);
        return result;
    };
    const auto workspaceBegin=reinterpret_cast<std::uintptr_t>(&workspace)&~std::uintptr_t(4095);
    const auto workspaceEnd=(reinterpret_cast<std::uintptr_t>(&workspace)+sizeof(workspace)+4095)&~std::uintptr_t(4095);
    const Interval excluded[2]{{workspaceBegin,workspaceEnd},{temp,temp?std::uint64_t(temp)+tempBytes:0}};
    auto excludedAt=[&](std::uint64_t address,std::uint64_t& end){
        for(const auto& e:excluded)if(e.end>e.begin&&address>=e.begin&&address<e.end){end=e.end;return true;}
        return false;
    };
    auto clampToExcluded=[&](std::uint64_t address,std::uint64_t bytes){
        for(const auto& e:excluded)if(e.end>e.begin&&address<e.begin&&address+bytes>e.begin)bytes=e.begin-address;
        return bytes;
    };
    if(!captureLcd())return finish(Result::screensError);
    // Memory map first, as in format 1: one snapshot of the regions.
    std::size_t count=0;std::uint32_t cursor=0;std::uint64_t total=0;
    while(cursor<addressLimit){
        if(count==maxRegions)return finish(Result::invalidMap);
        Region r{};
        if(!query(nullptr,cursor,r)||!validRegion(r,cursor))return finish(Result::invalidMap);
        r.size=std::uint32_t(std::uint64_t(r.base)+r.size-cursor);r.base=cursor;
        workspace.regions[count++]=r;
        if(!exclusion(r)){
            std::uint64_t bytes=r.size;
            for(const auto& e:excluded){
                const auto a=std::max<std::uint64_t>(r.base,e.begin),b=std::min<std::uint64_t>(std::uint64_t(r.base)+r.size,e.end);
                if(b>a)bytes-=b-a;
            }
            total+=bytes;if(total>maxPayload)return finish(Result::invalidMap);
        }
        cursor=r.base+r.size;
    }
    if(!total)return finish(Result::invalidMap);
    FS_ArchiveResource sd{};
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&sd))||std::uint64_t(sd.clusterSize)*sd.freeClusters<96ull*1024*1024)
        return finish(Result::insufficientSpace);
    // The folder of this dump, then both screens while they still show the
    // game (the progress text below replaces the bottom screen).
    char folder[96]{},stamp[24]{};const unsigned number=nextDumpNumber();
    {const std::time_t now=std::time(nullptr);std::tm parts{};gmtime_r(&now,&parts);
     std::snprintf(stamp,sizeof(stamp),"%04d-%02d-%02d %02d:%02d:%02d",parts.tm_year+1900,parts.tm_mon+1,parts.tm_mday,parts.tm_hour,parts.tm_min,parts.tm_sec);
     std::snprintf(folder,sizeof(folder),"/3ds/DuskLight/dumps/%03u-dump-%04d%02d%02d-%02d%02d%02d",number,
        parts.tm_year+1900,parts.tm_mon+1,parts.tm_mday,parts.tm_hour,parts.tm_min,parts.tm_sec);}
    {char name[112];std::snprintf(name,sizeof(name),"sdmc:%s",folder);if(!mkdirOrExists(name))return finish(Result::pathError);}
    {u8* scratch=temp?p.stage:workspace.lcdTop;const std::size_t capacity=temp?p.stageCapacity:sizeof(workspace.lcdTop);
     char name[144];
     std::snprintf(name,sizeof(name),"sdmc:%s/top-screen.bmp",folder);
     const bool topSaved=writeScreenBmp(name,reinterpret_cast<const u8*>(lcd.screencapture[0].framebuf0_vaddr),400,3,scratch,capacity);
     std::snprintf(name,sizeof(name),"sdmc:%s/bottom-screen.bmp",folder);
     const bool bottomSaved=writeScreenBmp(name,reinterpret_cast<const u8*>(lcd.screencapture[1].framebuf0_vaddr),320,2,scratch,capacity);
     if(!topSaved||!bottomSaved)OSReport("Native application dump: screenshot failed top=%d bottom=%d\n",int(topSaved),int(bottomSaved));}
    std::snprintf(path,sizeof(path),"%s/dump.dpk.tmp",folder);
    std::snprintf(finalPath,sizeof(finalPath),"sdmc:%s/dump.dpk",folder);
    if(R_FAILED(FSUSER_OpenFileDirectly(&p.writer.file,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,""),fsMakePath(PATH_ASCII,path),FS_OPEN_WRITE|FS_OPEN_CREATE,0)))
        {path[0]=0;return finish(Result::pathError);}
    p.writer.open=true;
    if(R_FAILED(FSFILE_SetSize(p.writer.file,0)))return finish(Result::ioError);
    {u8* header=p.writer.reserve(16);if(!header)return finish(Result::ioError);std::memcpy(header,"DUSKDPK2\0\0\0\0\0\0\0\0",16);p.writer.used+=16;}
    auto& x=p.index;
    x.line("format=DUSKLIGHT_APPLICATION_DUMP_2\ncontainer=single-file-blocks-then-index\nmode=full-readable-allowlisted-application\n"
        "chord=L+R+A instant\ncoherence=chunked-live-non-atomic\nworkers=running-not-suspended\nregisters=unsupported\n"
        "kernel=not-captured\nforeign-processes=not-captured\nmap-stability=best-effort-query-before-copy-not-pinned\n"
        "build=%.127s\nframe=%llu\ntimestamp_ms_2000=%llu\nscene=%.15s\nroom=%d\nnew3ds=%u\n"
        "launcher_application_region_bytes=%u\nkernel_version=%08x\ncompiler=" __VERSION__ "\n"
        "scope=[00000000,40000000)\npolicy=READ-and-CODE-PRIVATE-CONTINUOUS-LOCKED\n"
        "block_bytes=%lu\ncompression=%s\ncompression_level=%d\nworking_memory=%s\n"
        "privacy=private-live-game-bytes-never-upload\n",
        m.build,static_cast<unsigned long long>(m.frame),static_cast<unsigned long long>(m.timestamp),m.scene,m.room,unsigned(m.new3ds),
        unsigned(m.applicationRegionBytes),unsigned(m.kernelVersion),static_cast<unsigned long>(p.stageCapacity),
        p.zstd?"zstd":"none",p.zstd?level:0,temp?"temporary-linear-block":"fixed-workspace");
    // LCD front buffers, exactly as presented.
    const auto captureStart=svcGetSystemTick();
    const auto& top=lcd.screencapture[0];const auto& bottom=lcd.screencapture[1];
    x.line("lcd source=GSP-ImportDisplayCaptureInfo-after-GPU-drain-and-VBlank top_format=%08lx top_stride=%lu bottom_format=%08lx bottom_stride=%lu top_right_aliases_left=%u\n",
        static_cast<unsigned long>(top.format),static_cast<unsigned long>(top.framebuf_widthbytesize),static_cast<unsigned long>(bottom.format),
        static_cast<unsigned long>(bottom.framebuf_widthbytesize),top.framebuf0_vaddr==top.framebuf1_vaddr);
    x.line("layout lcd-top-left GSP-BGR8-rotated address=%08lx\nlayout lcd-bottom GSP-RGB565-rotated address=%08lx\nlayout vram raw-VRAM-GPU-copy address=%08lx\n",
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(top.framebuf0_vaddr)),
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(bottom.framebuf0_vaddr)),static_cast<unsigned long>(OS_VRAM_VADDR));
    if(!p.blob("lcd-top-left",top.framebuf0_vaddr,400*240*3))return finish(Result::ioError);
    if(top.framebuf1_vaddr&&top.framebuf1_vaddr!=top.framebuf0_vaddr&&readable(top.framebuf1_vaddr,400*240*3)){
        x.line("layout lcd-top-right GSP-BGR8-rotated address=%08lx\n",static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(top.framebuf1_vaddr)));
        if(!p.blob("lcd-top-right",top.framebuf1_vaddr,400*240*3))return finish(Result::ioError);
    }
    if(!p.blob("lcd-bottom",bottom.framebuf0_vaddr,320*240*2))return finish(Result::ioError);
    // The PICA state the renderer last sent (shadow registers and uniforms).
    {const std::uint32_t* registers{};std::size_t registerWords{};const std::uint32_t* uniforms{};std::size_t uniformWords{};
     gpuCanaryShadowState(registers,registerWords,uniforms,uniformWords);
     if(registers&&!p.blob("pica-registers",registers,registerWords*4))return finish(Result::ioError);
     if(uniforms&&!p.blob("pica-uniforms",uniforms,uniformWords*4))return finish(Result::ioError);}
    // The GPU's own registers are not read: GSPGPU_ReadHWRegs over
    // 1ef00000-1ef01c00 froze a New 3DS in Alpha 2 (the dump stopped right
    // after the screenshots, before any byte of dump.dpk). The shadow above
    // is what the renderer last sent.
    x.line("gpu_hw_registers=not-read (reading them froze the console)\n");
    // The last 32 jobs sent to GSP: words, submitted and completed ticks,
    // event, counted-complete flag (ctru_gxqueue.c, 14 words each).
    {u32 total=0;const unsigned jobs=duskGxJobRingCopy(reinterpret_cast<u32*>(workspace.lcdTop),32,&total);
     if(jobs){x.line("gpu_jobs_submitted_total=%lu\nlayout gpu-jobs 14-words-per-job address=00000000\n",static_cast<unsigned long>(total));
        if(!p.blob("gpu-jobs",workspace.lcdTop,jobs*14*4))return finish(Result::ioError);}}
    // The frame times of the last pictures (frames.csv is otherwise written
    // only at exit: a crash after the dump would lose them).
    if(temp){
        const std::size_t n=formatRecentFrameTelemetry(reinterpret_cast<char*>(workspace.lcdTop),sizeof(workspace.lcdTop),1500);
        if(n&&!p.blob("frames-csv",workspace.lcdTop,n))return finish(Result::ioError);
    }
    x.line("memory vram_free=%lu linear_free=%lu application_used=%lu application_free=%lu\n",static_cast<unsigned long>(vramSpaceFree()),
        static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(osGetMemRegionUsed(MEMREGION_APPLICATION)),
        static_cast<unsigned long>(osGetMemRegionFree(MEMREGION_APPLICATION)));
    // VRAM through GPU copies into a retained 64 KiB linear staging block.
    progress(nullptr,"Capturing VRAM",0,total);
    if(!vramStaging)vramStaging=linearAlloc(chunkBytes);
    if(!vramStaging)return finish(Result::ioError);
    for(std::size_t offset=0,filled=0,first=0;offset<OS_VRAM_SIZE;offset+=chunkBytes){
        if(!poll(nullptr))return finish(Result::cancelled);
        if(!completeDumpGpu())return finish(Result::ioError);
        if(R_FAILED(GX_TextureCopy(reinterpret_cast<u32*>(OS_VRAM_VADDR+offset),0,static_cast<u32*>(vramStaging),0,chunkBytes,8)))return finish(Result::ioError);
        if(!completeDumpGpu()||R_FAILED(GSPGPU_InvalidateDataCache(vramStaging,chunkBytes)))return finish(Result::ioError);
        if(!filled)first=offset;
        std::memcpy(p.stage+filled,vramStaging,chunkBytes);filled+=chunkBytes;
        if(filled+chunkBytes>p.stageCapacity||offset+chunkBytes>=OS_VRAM_SIZE){
            char prefix[96];std::snprintf(prefix,sizeof(prefix),"blob vram %08lx %08lx",static_cast<unsigned long>(first),static_cast<unsigned long>(filled));
            if(!p.store(prefix,filled))return finish(Result::ioError);
            filled=0;
        }
    }
    const auto captureTicks=svcGetSystemTick()-captureStart;
    // RAM.
    progress(nullptr,"Dumping RAM",0,total);
    x.line("range base size perm state pageflags decision\n");
    std::uint64_t done=0,copyTicks=0;
    for(std::size_t i=0;i<count;++i){
        const auto& r=workspace.regions[i];const char* why=exclusion(r);
        x.line("range %08x %08x %x %u %08x %s\n",unsigned(r.base),unsigned(r.size),unsigned(r.perm),unsigned(r.state),unsigned(r.flags),why?why:"eligible");
        // Shared pages (GSP queue and interrupt state, HID) and the kernel
        // configuration pages are outside the RAM policy but small and
        // readable: kept as named pages for diagnosis.
        if(why&&(r.perm&1)&&r.state==6&&r.size<=0x10000){
            char name[24];std::snprintf(name,sizeof(name),"page-%08x",unsigned(r.base));
            if(!p.blob(name,reinterpret_cast<const void*>(std::uintptr_t(r.base)),r.size))return finish(Result::ioError);
        }
        if(why)continue;
        const auto end=std::uint64_t(r.base)+r.size;
        for(std::uint64_t a=r.base;a<end;){
            if(!poll(nullptr))return finish(Result::cancelled);
            std::uint64_t next=0;
            if(excludedAt(a,next)){
                next=std::min(end,next);
                x.line("exclude %08x %08x diagnostic-workspace\n",unsigned(a),unsigned(next-a));a=next;continue;
            }
            const auto n=std::size_t(clampToExcluded(a,std::min<std::uint64_t>(p.stageCapacity,end-a)));
            // Requery: the region must still be the readable mapping it was.
            Region now{};
            if(!n||!query(nullptr,std::uint32_t(a),now)||!validRegion(now,std::uint32_t(a))||exclusion(now)||
               now.perm!=r.perm||now.state!=r.state||a+n>std::uint64_t(now.base)+now.size)return finish(Result::readError);
            const auto tick=svcGetSystemTick();
            std::memcpy(p.stage,reinterpret_cast<const void*>(std::uintptr_t(a)),n);
            copyTicks+=svcGetSystemTick()-tick;
            char prefix[64];std::snprintf(prefix,sizeof(prefix),"block %08x %08x",unsigned(a),unsigned(n));
            if(!p.store(prefix,n))return finish(p.index.overflow?Result::invalidMap:Result::ioError);
            a+=n;done+=n;progress(nullptr,"Dumping RAM",done,total);
        }
    }
    if(done!=total)return finish(Result::invalidMap);
    const auto elapsed=svcGetSystemTick()-startTick;
    x.line("ram_bytes=%llu\nzero_bytes=%llu\nstored_bytes=%llu\nblocks_zero=%u\nblocks_raw=%u\nblocks_zstd=%u\n"
        "seconds_capture_lcd_vram=%.3f\nseconds_copy=%.3f\nseconds_checksum=%.3f\nseconds_compress=%.3f\nseconds_sd_write=%.3f\nsd_writes=%u\nseconds_before_index=%.3f\n",
        static_cast<unsigned long long>(done),static_cast<unsigned long long>(p.zeroBytes),static_cast<unsigned long long>(p.storedBytes),
        p.zeroBlocks,p.rawBlocks,p.zstdBlocks,seconds(captureTicks),seconds(copyTicks),seconds(p.hashTicks),seconds(p.compressTicks),
        seconds(p.writer.ticks),p.writer.writes,seconds(elapsed));
    if(x.overflow)return finish(Result::invalidMap);
    // Index, then a fixed trailer that locates and checks it.
    const auto indexOffset=p.writer.position();const auto indexSize=x.used;
    const std::uint32_t indexHash=dumpHash(x.text,indexSize);
    for(std::size_t at=0;at<indexSize;){
        const auto n=std::min<std::size_t>(indexSize-at,64*1024);
        u8* out=p.writer.reserve(n);if(!out)return finish(Result::ioError);
        std::memcpy(out,x.text+at,n);p.writer.used+=n;at+=n;
    }
    {u8* trailer=p.writer.reserve(32);if(!trailer)return finish(Result::ioError);
     const std::uint32_t words[6]{std::uint32_t(indexOffset),std::uint32_t(indexOffset>>32),std::uint32_t(indexSize),indexHash,
        p.zeroBlocks+p.rawBlocks+p.zstdBlocks,0};
     std::memcpy(trailer,"DPK2END\0",8);std::memcpy(trailer+8,words,24);p.writer.used+=32;}
    if(!p.writer.drain(true))return finish(Result::ioError);
    FSFILE_Close(p.writer.file);p.writer.open=false;
    {char name[176];std::snprintf(name,sizeof(name),"sdmc:%s",path);
     std::remove(finalPath);
     if(std::rename(name,finalPath))return finish(Result::ioError);}
    path[0]=0;
    std::snprintf(output,sizeof(output),"%s",finalPath);
    const double all=seconds(svcGetSystemTick()-startTick),cardSeconds=seconds(p.writer.ticks);
    {char name[144];std::snprintf(name,sizeof(name),"sdmc:%s/info.txt",folder);
     if(auto* file=std::fopen(name,"wb")){
        std::fprintf(file,"DuskLight " DUSKLIGHT_RELEASE " - full dump (L+R+A)\nnumber=%03u\ndate=%s\nbuild=%.127s\nframe=%llu\nstage=%.15s\nroom=%d\n%s"
            "ram_kib=%llu\nstored_kib=%llu\nseconds=%.1f\n"
            "files=dump.dpk top-screen.bmp bottom-screen.bmp info.txt\n"
            "read_on_the_mac=python3 -I Tools/3ds/verify_application_dump.py dump.dpk --extract <new folder>\n"
            "privacy=private live game bytes; never upload\n",
            number,stamp,m.build,static_cast<unsigned long long>(m.frame),m.scene,m.room,dumpNote,
            static_cast<unsigned long long>(done/1024),static_cast<unsigned long long>(p.writer.offset/1024),all);
        std::fprintf(file,"inside_dump_dpk=RAM (code, data, heaps, stacks), VRAM, both LCD buffers, PICA state sent, "
            "shared and kernel configuration pages, frame times of the last pictures, the last GPU jobs\n");
        if(dumpStateReport){std::fprintf(file,"\n[state]\n");dumpStateReport(file);}
        std::fclose(file);
     }}
    OSReport("Native application dump: packed ram=%llu KiB zero=%llu KiB stored=%llu KiB blocks zero/raw/zstd=%u/%u/%u level=%d memory=%s\n",
        static_cast<unsigned long long>(done/1024),static_cast<unsigned long long>(p.zeroBytes/1024),static_cast<unsigned long long>(p.storedBytes/1024),
        p.zeroBlocks,p.rawBlocks,p.zstdBlocks,p.zstd?level:0,temp?"temporary":"workspace");
    OSReport("Native application dump: lcd+vram=%.2f s copy=%.2f s checksum=%.2f s compress=%.2f s sd=%.2f s (%u writes, %.2f MiB/s) total=%.2f s\n",
        seconds(captureTicks),seconds(copyTicks),seconds(p.hashTicks),seconds(p.compressTicks),cardSeconds,p.writer.writes,
        cardSeconds>0?double(p.writer.offset)/(1024*1024)/cardSeconds:0.0,all);
    std::printf("\x1b[2J\x1b[1;1HDUSKLIGHT - Full dump\n\n%llu MiB of RAM -> %llu KiB\nin %.1f seconds\n\n%s\n",
        static_cast<unsigned long long>(done/(1024*1024)),static_cast<unsigned long long>(p.writer.offset/1024),all,finalPath);
    return finish(Result::success);
}

bool mkdirOrExists(const char* name) {
    if(!mkdir(name,0700))return true;
    if(errno!=EEXIST)return false;
    struct stat st{};return !stat(name,&st)&&S_ISDIR(st.st_mode);
}
}
void updateApplicationDumpInput(std::uint32_t held) {
    if(!held)resumeBlocked=false;
    requested = trigger.update(held,svcGetSystemTick()/(SYSCLOCK_ARM11/1000),active.load()) || requested;
    suppressed.store(trigger.suppress() || requested || active.load() || resumeBlocked);
}
bool applicationDumpInputSuppressed(){return suppressed.load();}
bool applicationDumpActive(){return active.load();}
bool applicationDumpRequested(){return requested;}
void requestApplicationDump(){requested=true;suppressed.store(true);}
void setApplicationDumpNote(const char* text){std::snprintf(dumpNote,sizeof(dumpNote),"%s",text?text:"");}
void setApplicationDumpReport(void (*report)(std::FILE*)){dumpStateReport=report;}
bool serviceApplicationDump(std::uint64_t frame,const char* scene,int room) {
    if(!requested||active.exchange(true))return false;
    requested=false;trigger.update(chord,0,true);suppressed.store(true);
    lastProgressTick=0;completedPath[0]=0;
    Metadata metadata{};metadata.frame=frame;metadata.timestamp=osGetTime();metadata.room=room;
    std::snprintf(metadata.build,sizeof(metadata.build),"%s",DUSK_DUMP_BUILD_ID);
    std::snprintf(metadata.scene,sizeof(metadata.scene),"%.15s",scene?scene:"unavailable");
    metadata.applicationRegionBytes=osGetMemRegionSize(MEMREGION_APPLICATION);
    metadata.kernelVersion=osGetKernelVersion();
    APT_CheckNew3DS(&metadata.new3ds);
    Services s{};s.query=query;s.read=read;s.space=space;s.poll=poll;s.progress=progress;s.screens=screens;s.supplement=supplement;s.removeSupplement=removeSupplement;
    FS_ArchiveResource sd{};
    if(R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&sd)))s.allocationUnit=sd.clusterSize;
    else s.allocationUnit=0; // fail closed; do not guess SD allocation geometry
    const auto start=reinterpret_cast<std::uintptr_t>(&workspace)&~std::uintptr_t(4095);
    const auto end=(reinterpret_cast<std::uintptr_t>(&workspace)+sizeof(workspace)+4095)&~std::uintptr_t(4095);
    s.excludedBase=start;s.excludedSize=end-start;
    Result result=Result::pathError;
    const auto startTick=svcGetSystemTick();
    OSReport("Native application dump: started frame=%llu scene=%.8s room=%d\n",static_cast<unsigned long long>(frame),scene?scene:"-",room);
    flushNativeLog();
    // dump-v1.txt on the SD keeps the first format (one file per 64 KiB).
    bool legacy=false;
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/dump-v1.txt","rb")){std::fclose(file);legacy=true;}
    if(mkdirOrExists("sdmc:/3ds")&&mkdirOrExists("sdmc:/3ds/DuskLight")&&mkdirOrExists("sdmc:/3ds/DuskLight/dumps"))
        result=legacy?run("sdmc:/3ds/DuskLight/dumps",metadata,s,workspace,completedPath):runPacked(metadata,completedPath);
    OSReport("Native application dump: %s; %.1f s; path=%s\n",resultText(result),
        double(svcGetSystemTick()-startTick)/SYSCLOCK_ARM11,completedPath[0]?completedPath:"(none)");
    flushNativeLog();
    // Also cover early failures (map/preflight/screens), which occur before UI.
    if(result!=Result::success&&legacy)progress(nullptr,resultText(result),0,0);
    std::printf("\nA/B: continue\n");
    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr),320*240*2);
    // Leave the result readable until an acknowledgement: 1.5 s after a
    // capture, 4 s after an error.
    const auto until=svcGetSystemTick()+(result==Result::success?3ull*SYSCLOCK_ARM11/2:4ull*SYSCLOCK_ARM11);
    bool released=false;
    while(svcGetSystemTick()<until&&aptMainLoop()) {
        hidScanInput();if(!(hidKeysHeld()&(KEY_A|KEY_B)))released=true;
        if(released&&(hidKeysDown()&(KEY_A|KEY_B)))break;
        gspWaitForVBlank();
    }
    resumeBlocked=true;active.store(false);return true;
}
}
