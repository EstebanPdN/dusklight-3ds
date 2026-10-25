#include "application_dump_ctru.hpp"
#include "application_dump.hpp"
#include "gx_framebuffer.hpp"
#include "gpu_sync.hpp"
#include "dump_io.hpp"
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

#ifndef DUSK_DUMP_BUILD_ID
#define DUSK_DUMP_BUILD_ID "unavailable-rebuild-with-DUSK_DUMP_BUILD_ID"
#endif
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
    // Terminal messages always repaint; active progress at most ten times/sec.
    if(done&&done<total&&now-lastProgressTick<SYSCLOCK_ARM11/10)return;
    lastProgressTick=now;
    std::printf("\x1b[2J\x1b[1;1HDUSKLIGHT - Diagnostico privado\n\n%.40s\n\n%llu / %llu KiB\n\nL+R+A: RAM + LCD + VRAM\nB: cancelar durante el volcado\n\nCaptura en bloques, no atomica\nNo apagar ni retirar la SD\n\nRuta: /3ds/DuskLight/dumps/\n",
        text,(unsigned long long)(done/1024),(unsigned long long)(total/1024));
    auto* fb=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr);
    if(fb)GSPGPU_FlushDataCache(fb,320*240*2);
    // consoleInit uses a single bottom framebuffer; do not swap/reset top UI.
    gspWaitForVBlank();
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
        if(okay)progress(nullptr,"Capturando y verificando VRAM",offset+chunkBytes,OS_VRAM_SIZE);
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
    if(mkdirOrExists("sdmc:/3ds")&&mkdirOrExists("sdmc:/3ds/DuskLight")&&mkdirOrExists("sdmc:/3ds/DuskLight/dumps"))
        result=run("sdmc:/3ds/DuskLight/dumps",metadata,s,workspace,completedPath);
    // Also cover early failures (map/preflight/screens), which occur before UI.
    if(result!=Result::success)progress(nullptr,resultText(result),0,0);
    std::printf("\nSuelta L, R y A para rearmar.\nA/B: continuar\n");
    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr),320*240*2);
    // Leave completion/error readable until a fresh acknowledgement or 3 s.
    const auto until=svcGetSystemTick()+3ull*SYSCLOCK_ARM11;
    bool released=false;
    while(svcGetSystemTick()<until&&aptMainLoop()) {
        hidScanInput();if(!(hidKeysHeld()&(KEY_A|KEY_B)))released=true;
        if(released&&(hidKeysDown()&(KEY_A|KEY_B)))break;
        gspWaitForVBlank();
    }
    resumeBlocked=true;active.store(false);return true;
}
}
