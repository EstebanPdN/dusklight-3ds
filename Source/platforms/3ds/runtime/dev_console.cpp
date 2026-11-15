#include "dev_console.hpp"
#include "release.hpp"
#include "frame_telemetry.hpp"
#include "gpu_sync.hpp"
#include "gpu_canary.hpp"
#include "bootstrap.hpp"
#include "helpers/native_effects.h"
#include <3ds.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DUSK_DUMP_BUILD_ID
#define DUSK_DUMP_BUILD_ID "unavailable"
#endif
namespace dusk::native {
namespace {
constexpr char dataRoot[]="sdmc:/3ds/DuskLight";
constexpr char statePath[]="sdmc:/3ds/DuskLight/tour-state.txt";
constexpr unsigned stageTotal=sizeof(zoneStages)/sizeof(*zoneStages);
constexpr unsigned roomTotal=sizeof(zoneRooms)/sizeof(*zoneRooms);
// libctru console: 40 x 30 cells of 8 px. Never print in the last cell of the
// last row or end a line there: the console would scroll.
#define C_RST "\x1b[0m"
#define C_HDR "\x1b[33;1m"
#define C_DIM "\x1b[37m"
#define C_VAL "\x1b[37;1m"
#define C_OK  "\x1b[32;1m"
#define C_BAD "\x1b[31;1m"
#define C_CYN "\x1b[36;1m"
enum Page {pageStats,pageZones,pageTest};
DevGameHooks hooks;
DevGameStatus last;
int page=pageStats;
bool dirty=true,exitRequested,chordLatched,selectHeld;
std::uint64_t exitArmedUntil,noticeUntil;
char notice[44];
float smoothLoop,smoothExec,smoothTicks;
// Game speed: original 30 Hz ticks run per second of real time, in percent.
int gameSpeedPercent(){return smoothLoop>0?int(smoothTicks*(1000.0f/30.0f)/smoothLoop*100.0f+0.5f):0;}
// ZONES page
constexpr int listRow=6,listRows=20;
int selectedStage,selectedRoom=zoneStages[0].primary,selectedForm,listTop;
unsigned repeatHold;

std::uint64_t seconds(std::uint64_t ticks){return ticks/SYSCLOCK_ARM11;}
void say(const char* text){
    std::snprintf(notice,sizeof(notice),"%s",text);noticeUntil=svcGetSystemTick()+4ull*SYSCLOCK_ARM11;dirty=true;
}
bool directory(const char* path){
    if(!mkdir(path,0777))return true;
    struct stat info{};
    return errno==EEXIST&&!stat(path,&info)&&S_ISDIR(info.st_mode);
}

// ---- automatic test ----
enum TourState {tourIdle,tourWarp,tourLoad,tourSettle,tourRun,tourDump};
enum TourMode {modeMain,modeStages,modeRooms};
struct Phase {const char* name;unsigned frames;};
// The still and plain phases both hold still, one after the other: still
// with the native effects on, plain with them off (shadows, the world
// particle system, bloom and haze). Their difference is what the effects cost
// in that zone. (The idle phase cannot serve: it runs right after arrival,
// with caches cold.) The plain phase stays out of the zone's overall figures.
// The last phase holds still with the detailed profiler on (two sparse reports).
constexpr Phase phases[]={{"idle",60},{"camera",90},{"walk",150},{"action",120},{"target",60},{"items",120},{"still",60},{"plain",60},{"profile",62}};
constexpr unsigned phaseCount=sizeof(phases)/sizeof(*phases),profilePhase=phaseCount-1,plainPhase=phaseCount-2;
bool plainActive;int plainSaved[3];
void setPlain(bool on){
    if(on==plainActive)return;
    plainActive=on;
    using namespace dusk::native_effects;
    if(on){plainSaved[0]=shadows;plainSaved[1]=particles;plainSaved[2]=bloom;shadows=particles=bloom=0;}
    else{shadows=plainSaved[0];particles=plainSaved[1];bloom=plainSaved[2];}
}
struct Entry {std::uint16_t room;std::uint8_t stage,form;};
Entry entries[roomTotal+stageTotal];
unsigned entryCount;
struct Tour {
    int state{tourIdle},mode{modeStages},scale{100};
    bool fullDumps{true},autoExit{},bootWarp{},moved{},restarting{};
    unsigned index{},limit{},phase{},phaseFrame{},waitFrames{},stableFrames{},failures{},finished{};
    std::uint64_t startTick{},zoneTick{},loadTicks{},rejected0{},degraded0{};
    std::size_t logMark{};
    char dir[64]{},zoneDir[112]{};
} tour;
unsigned phaseFrames(unsigned phase){
    if(phase==profilePhase)return phases[phase].frames;
    return std::max(12u,phases[phase].frames*unsigned(tour.scale)/100u);
}
void buildEntries(int mode){
    entryCount=0;
    auto add=[](unsigned stage,unsigned room,unsigned form){entries[entryCount++]={std::uint16_t(room),std::uint8_t(stage),std::uint8_t(form)};};
    for(unsigned s=0;s<stageTotal;++s){
        const auto& z=zoneStages[s];
        if(mode==modeMain&&!z.mainTour)continue;
        if(mode==modeRooms)for(unsigned r=0;r<z.roomCount;++r)add(s,z.firstRoom+r,0);
        else add(s,z.firstRoom+z.primary,0);
    }
    // Second pass as the wolf through the main outdoor stages.
    for(unsigned s=0;s<stageTotal;++s)if(zoneStages[s].mainTour&&zoneStages[s].stage[0]=='F')add(s,zoneStages[s].firstRoom+zoneStages[s].primary,1);
}
unsigned modeCount(int mode){
    unsigned count=0,wolf=0;
    for(unsigned s=0;s<stageTotal;++s){
        const auto& z=zoneStages[s];
        if(z.mainTour&&z.stage[0]=='F')++wolf;
        if(mode==modeMain&&!z.mainTour)continue;
        count+=mode==modeRooms?z.roomCount:1;
    }
    return count+wolf;
}
// resume: the zone in "next" has not been tried yet (a deliberate restart);
// otherwise the previous run ended inside it.
void writeState(bool resume=false){
    if(auto* file=std::fopen(statePath,"wb")){
        std::fprintf(file,"dir=%s\nmode=%d\nnext=%u\nscale=%d\nfull=%d\nautoexit=%d\nlimit=%u\nfailures=%u\nresume=%d\nfinished=%u\n",tour.dir,tour.mode,tour.index,
            tour.scale,int(tour.fullDumps),int(tour.autoExit),tour.limit,tour.failures,int(resume),tour.finished);
        std::fclose(file);
    }
}
void summaryHeader(){
    char path[128];std::snprintf(path,sizeof(path),"%s/summary.csv",tour.dir);
    if(auto* file=std::fopen(path,"wb")){
        std::fprintf(file,"zone,stage,room,point,form,status,load_s,frames,loop_ms,loop_p50,loop_p95,loop_p99,loop_max,fps,exec_ms,exec_p95,"
            "paint_ms,update_ms,draw_ms,gpu_wait_ms,gpu_tail_ms,audio_ms,draws,over34,over40,over50,rejected,degraded,linear_free_kb,game_free_kb,texture_kb");
        for(unsigned p=0;p<profilePhase;++p)std::fprintf(file,",%s_ms",phases[p].name);
        std::fputc('\n',file);std::fclose(file);
    }
}
void summaryFailure(unsigned index,const char* status){
    char path[128];std::snprintf(path,sizeof(path),"%s/summary.csv",tour.dir);
    if(auto* file=std::fopen(path,"ab")){
        const auto& e=entries[index];
        std::fprintf(file,"%u,%s,%d,%d,%s,%s\n",index+1,zoneStages[e.stage].stage,int(zoneRooms[e.room].room),int(zoneRooms[e.room].point),
            e.form?"wolf":"human",status);
        std::fclose(file);
    }
}
unsigned nextTourNumber(){
    char path[96];std::snprintf(path,sizeof(path),"%s/tour",dataRoot);
    unsigned next=1;
    if(auto* list=opendir(path)){
        while(auto* entry=readdir(list)){
            char* end{};const auto value=std::strtoul(entry->d_name,&end,10);
            if(end!=entry->d_name&&!*end&&value>=next&&value<100000)next=unsigned(value)+1;
        }
        closedir(list);
    }
    return next;
}
bool createTour(){
    char path[96];std::snprintf(path,sizeof(path),"%s/tour",dataRoot);
    if(!directory("sdmc:/3ds")||!directory(dataRoot)||!directory(path))return false;
    std::snprintf(tour.dir,sizeof(tour.dir),"%s/tour/%03u",dataRoot,nextTourNumber());
    if(!directory(tour.dir))return false;
    buildEntries(tour.mode);
    char file[128];std::snprintf(file,sizeof(file),"%s/tour.txt",tour.dir);
    if(auto* out=std::fopen(file,"wb")){
        bool new3ds=false;APT_CheckNew3DS(&new3ds);
        std::fprintf(out,"build=%s\ncompiled=%s %s\nmode=%s\nzones=%u\nscale_percent=%d\nfull_dumps=%d\nnew3ds=%d\nfirm=%08lx\nkernel=%08lx\n"
            "application_region=%lu\napplication_used=%lu\nlinear_free=%lu\nvram_free=%lu\n"
            "note=times are native ticks of this device; flags=1 rows carry measurement cost and are excluded from the statistics\n",
            DUSK_DUMP_BUILD_ID,__DATE__,__TIME__,tour.mode==modeMain?"main":tour.mode==modeStages?"stages":"rooms",entryCount,tour.scale,
            int(tour.fullDumps),int(new3ds),static_cast<unsigned long>(osGetFirmVersion()),static_cast<unsigned long>(osGetKernelVersion()),
            static_cast<unsigned long>(osGetMemRegionSize(MEMREGION_APPLICATION)),static_cast<unsigned long>(osGetMemRegionUsed(MEMREGION_APPLICATION)),
            static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()));
        std::fprintf(out,"phases=");
        for(unsigned p=0;p<phaseCount;++p)std::fprintf(out,"%u:%s:%u ",p,phases[p].name,phaseFrames(p));
        std::fputc('\n',out);std::fclose(out);
    }
    std::snprintf(file,sizeof(file),"%s/boot-log.txt",tour.dir);
    if(auto* out=std::fopen(file,"wb")){writeNativeLogSince(0,out);std::fclose(out);}
    summaryHeader();
    tour.index=0;tour.failures=tour.finished=0;tour.startTick=svcGetSystemTick();
    return true;
}
const Entry& current(){return entries[tour.index];}
void beginZone(){
    setFatalUnattended(true);
    tour.state=tourWarp;tour.waitFrames=tour.stableFrames=0;tour.zoneTick=svcGetSystemTick();
    // Between zones: the SD log is current if this zone hangs or the power goes.
    setFrameTelemetryTag(0,0);writeState();flushNativeLog();dirty=true;
}
void endTour(const char* why){
    char file[128];std::snprintf(file,sizeof(file),"%s/tour.txt",tour.dir);
    if(auto* out=std::fopen(file,"ab")){
        std::fprintf(out,"result=%s\nzones_finished=%u\nfailures=%u\nelapsed_seconds=%llu\n",why,tour.finished,tour.failures,
            static_cast<unsigned long long>(seconds(svcGetSystemTick()-tour.startTick)));
        std::fclose(out);
    }
    unlink(statePath);setFatalUnattended(false);
    setFrameTelemetryTag(0,0);tour.state=tourIdle;
    char text[44];std::snprintf(text,sizeof(text),"Test %s: %u zones, %u failed",why,tour.finished,tour.failures);say(text);
    if(tour.autoExit)exitRequested=true;
}
void nextZone(){
    ++tour.index;
    if(tour.index>=entryCount||(tour.limit&&tour.index>=tour.limit)){endTour("complete");return;}
    beginZone();
}
void failZone(const char* status){
    summaryFailure(tour.index,status);++tour.failures;nextZone();
}
// A stage that never finishes loading leaves no scene to travel from: record
// it and start the game again in the next zone.
void restartAfterStuckZone(const char* status){
    if(hooks.stuck)hooks.stuck();
    summaryFailure(tour.index,status);++tour.failures;++tour.index;
    if(tour.index>=entryCount||(tour.limit&&tour.index>=tour.limit)){endTour("complete");exitRequested=true;return;}
    writeState(true);flushNativeLog();
    tour.restarting=true;tour.state=tourIdle;setFrameTelemetryTag(0,0);
    aptSetChainloaderToSelf();exitRequested=true;
    say("Zone did not load: restarting the game");
}
unsigned number(const char* text,const char* key,unsigned fallback);
bool readText(const char* path,char* text,std::size_t capacity);
void startTour(){
    if(tour.state!=tourIdle)return;
    // Optional settings for a test started by hand (START+SELECT or the TEST
    // tab): tour-options.txt with mode= scale= limit= full= exit=. The
    // emulator gate uses it to start a short test from the title screen, the
    // way it is started on a console.
    {char text[256];char path[96];std::snprintf(path,sizeof(path),"%s/tour-options.txt",dataRoot);
     if(readText(path,text,sizeof(text))){
        tour.mode=std::clamp(int(number(text,"mode=",tour.mode)),0,2);tour.scale=std::clamp(int(number(text,"scale=",tour.scale)),10,400);
        tour.limit=number(text,"limit=",tour.limit);tour.fullDumps=number(text,"full=",tour.fullDumps)!=0;
        tour.autoExit=number(text,"exit=",tour.autoExit)!=0;
     }}
    if(!createTour()){say("Test: cannot create the tour folder");return;}
    page=pageTest;beginZone();
}
// 24-bit BMP of a rotated LCD buffer, written in rows through a stdio buffer.
bool writeBmp(const char* path,gfxScreen_t screen,int width){
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return false;
    const auto format=gfxGetScreenFormat(screen);
    if(format!=GSP_BGR8_OES&&format!=GSP_RGB565_OES)return false;
    const int bytes=format==GSP_BGR8_OES?3:2;
    auto* pixels=gfxGetFramebuffer(screen,GFX_LEFT,nullptr,nullptr);
    if(!pixels)return false;
    GSPGPU_InvalidateDataCache(pixels,240*width*bytes);
    auto* file=std::fopen(path,"wb");
    if(!file)return false;
    static char buffer[16384];std::setvbuf(file,buffer,_IOFBF,sizeof(buffer));
    const int rowBytes=width*3; // 400 and 320 are multiples of 4: no padding
    unsigned char header[54]{'B','M'};
    auto put=[&](int at,std::uint32_t value){std::memcpy(header+at,&value,4);};
    put(2,54+rowBytes*240);put(10,54);put(14,40);put(18,width);put(22,240);put(26,1|(24u<<16));put(34,rowBytes*240);
    std::fwrite(header,1,sizeof(header),file);
    static unsigned char row[400*3];
    for(int y=0;y<240;++y){ // BMP rows run bottom-up, like the buffer's columns
        for(int x=0;x<width;++x){
            const auto* p=pixels+(x*240+y)*bytes;
            if(bytes==3){row[x*3]=p[0];row[x*3+1]=p[1];row[x*3+2]=p[2];}
            else{const unsigned v=p[0]|p[1]<<8;row[x*3]=(v&31)*255/31;row[x*3+1]=((v>>5)&63)*255/63;row[x*3+2]=(v>>11)*255/31;}
        }
        std::fwrite(row,1,rowBytes,file);
    }
    const bool okay=!std::ferror(file);
    return !std::fclose(file)&&okay;
}
void statsLine(std::FILE* file,const char* name,const TelemetryStats& s){
    std::fprintf(file,"[%s]\nframes=%lu\nflagged_frames=%lu\nsaturated=%lu\nloop_mean_ms=%.2f\nloop_p50_ms=%.2f\nloop_p95_ms=%.2f\nloop_p99_ms=%.2f\nloop_max_ms=%.2f\nfps=%.2f\n"
        "exec_mean_ms=%.2f\nexec_p95_ms=%.2f\nexec_max_ms=%.2f\npaint_ms=%.2f\nupdate_ms=%.2f\ndraw_ms=%.2f\npace_ms=%.2f\ngpu_wait_ms=%.2f\ngpu_tail_ms=%.2f\naudio_mix_ms=%.2f\n"
        "draws=%.1f\nframes_over_34ms=%lu\nframes_over_40ms=%lu\nframes_over_50ms=%lu\n",name,
        static_cast<unsigned long>(s.frames),static_cast<unsigned long>(s.flagged),static_cast<unsigned long>(s.saturated),
        s.loopMean,s.loopP50,s.loopP95,s.loopP99,s.loopMax,s.loopMean>0?1000.0f/s.loopMean:0.0f,s.execMean,s.execP95,s.execMax,
        s.paint,s.update,s.draw,s.pace,s.gpuWait,s.gpuTail,s.audio,s.draws,
        static_cast<unsigned long>(s.over34),static_cast<unsigned long>(s.over40),static_cast<unsigned long>(s.over50));
}
void finishZone(const DevGameStatus& status){
    const auto& e=current();const auto& z=zoneStages[e.stage];const auto& r=zoneRooms[e.room];
    const std::uint16_t zone=std::uint16_t(tour.index+1);
    char path[160];
    TelemetryStats all{};frameTelemetryStats(zone,-2-int(plainPhase),all);
    std::snprintf(path,sizeof(path),"%s/report.txt",tour.zoneDir);
    if(auto* file=std::fopen(path,"wb")){
        std::fprintf(file,"zone=%u\nstage=%s\nlabel=%s\nroom=%d\npoint=%d\nform=%s\nstatus=%s\nfinal_stage=%.8s\nfinal_room=%d\nfinal_layer=%d\n"
            "load_seconds=%.2f\nplayer_position=%.1f %.1f %.1f\nrejected_draws_in_zone=%llu\ndegraded_draws_in_zone=%llu\n",
            unsigned(zone),z.stage,z.label,int(r.room),int(r.point),e.form?"wolf":"human",tour.moved?"moved":"ok",status.stage,status.room,status.layer,
            double(tour.loadTicks)/SYSCLOCK_ARM11,status.position[0],status.position[1],status.position[2],
            static_cast<unsigned long long>(status.rejected-tour.rejected0),static_cast<unsigned long long>(status.degraded-tour.degraded0));
        statsLine(file,"all phases except plain and profile",all);
        for(unsigned p=0;p<profilePhase;++p){TelemetryStats part{};frameTelemetryStats(zone,int(p),part);statsLine(file,phases[p].name,part);}
        const auto heap=mallinfo();
        std::fprintf(file,"[memory]\nlinear_free=%lu\nvram_free=%lu\napplication_used=%lu\nmalloc_used=%lu\nmalloc_free=%lu\n"
            "telemetry_dropped=%lu\ntelemetry_saturated=%lu\n[renderer]\n",
            static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()),
            static_cast<unsigned long>(osGetMemRegionUsed(MEMREGION_APPLICATION)),static_cast<unsigned long>(heap.uordblks),
            static_cast<unsigned long>(heap.fordblks),static_cast<unsigned long>(frameTelemetryDropped()),
            static_cast<unsigned long>(frameTelemetrySaturated()));
        if(hooks.report)hooks.report(file);
        std::fclose(file);
    }
    std::snprintf(path,sizeof(path),"%s/frames.csv",tour.zoneDir);
    if(auto* file=std::fopen(path,"wb")){
        static char buffer[16384];std::setvbuf(file,buffer,_IOFBF,sizeof(buffer));
        writeFrameTelemetryZone(file,zone);std::fclose(file);
    }
    std::snprintf(path,sizeof(path),"%s/log.txt",tour.zoneDir);
    if(auto* file=std::fopen(path,"wb")){writeNativeLogSince(tour.logMark,file);std::fclose(file);}
    std::snprintf(path,sizeof(path),"%s/shot2.bmp",tour.zoneDir);writeBmp(path,GFX_TOP,400);
    std::snprintf(path,sizeof(path),"%s/bottom.bmp",tour.zoneDir);writeBmp(path,GFX_BOTTOM,320);
    std::snprintf(path,sizeof(path),"%s/summary.csv",tour.dir);
    if(auto* file=std::fopen(path,"ab")){
        std::fprintf(file,"%u,%s,%d,%d,%s,%s,%.1f,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%lu,%lu,%lu,%llu,%llu,%lu,%lu,%lu",
            unsigned(zone),z.stage,int(r.room),int(r.point),e.form?"wolf":"human",tour.moved?"moved":"ok",double(tour.loadTicks)/SYSCLOCK_ARM11,
            static_cast<unsigned long>(all.frames),all.loopMean,all.loopP50,all.loopP95,all.loopP99,all.loopMax,
            all.loopMean>0?1000.0f/all.loopMean:0.0f,all.execMean,all.execP95,all.paint,all.update,all.draw,all.gpuWait,all.gpuTail,all.audio,
            all.draws,static_cast<unsigned long>(all.over34),static_cast<unsigned long>(all.over40),static_cast<unsigned long>(all.over50),
            static_cast<unsigned long long>(status.rejected-tour.rejected0),static_cast<unsigned long long>(status.degraded-tour.degraded0),
            static_cast<unsigned long>(status.linearFree/1024),static_cast<unsigned long>(status.gameHeapFree/1024),
            static_cast<unsigned long>(status.textureBytes/1024));
        for(unsigned p=0;p<profilePhase;++p){TelemetryStats part{};frameTelemetryStats(zone,int(p),part);std::fprintf(file,",%.2f",part.loopMean);}
        std::fputc('\n',file);std::fclose(file);
    }
    ++tour.finished;
}
// One step per completed frame. Returns true after SD work.
bool stepTour(const DevGameStatus& status){
    setPlain(tour.state==tourRun&&tour.phase==plainPhase);
    if(tour.state==tourIdle)return false;
    const auto& e=current();const auto& z=zoneStages[e.stage];const auto& r=zoneRooms[e.room];
    switch(tour.state){
    case tourWarp:
        if(tour.bootWarp||(hooks.warp&&hooks.warp(z.stage,r.room,r.point,-1,e.form,true))){
            tour.bootWarp=false;tour.state=tourLoad;tour.waitFrames=tour.stableFrames=0;
        }else if(++tour.waitFrames>900)restartAfterStuckZone("warp-refused");
        return false;
    case tourLoad:{
        ++tour.waitFrames;
        // Some start points hand the player straight to another stage: after
        // a while a running scene elsewhere counts as arrival (reported "moved").
        const bool there=!std::strncmp(status.stage,z.stage,8);
        const bool arrived=status.playScene&&status.playerReady&&!status.stageChanging&&
            (there?status.room==int(r.room)||tour.waitFrames>300:tour.waitFrames>450);
        tour.stableFrames=arrived&&tour.waitFrames>=20?tour.stableFrames+1:0;
        if(tour.stableFrames>=20){
            tour.loadTicks=svcGetSystemTick()-tour.zoneTick;tour.state=tourSettle;tour.waitFrames=0;
        }else if(seconds(svcGetSystemTick()-tour.zoneTick)>=150)restartAfterStuckZone("load-timeout");
        return false;}
    case tourSettle:
        if(++tour.waitFrames<std::max(15u,45u*unsigned(tour.scale)/100u))return false;
        std::snprintf(tour.zoneDir,sizeof(tour.zoneDir),"%s/%03u-%s-r%02d-%c",tour.dir,tour.index+1,z.stage,int(r.room),e.form?'w':'h');
        directory(tour.zoneDir);
        tour.phase=tour.phaseFrame=0;tour.moved=false;tour.rejected0=status.rejected;tour.degraded0=status.degraded;tour.logMark=nativeLogPosition();
        setFrameTelemetryTag(std::uint16_t(tour.index+1),0);flagNextFrameTelemetry(telemetryDiagnosticFrame);
        tour.state=tourRun;dirty=true;
        return true;
    case tourRun:{
        bool wrote=false;
        // The game itself may leave the zone (story events on the empty save).
        // Loading frames are not zone performance: flag them and record the move.
        if(std::strncmp(status.stage,z.stage,8))tour.moved=true;
        if(!status.playScene||status.stageChanging)flagNextFrameTelemetry(telemetryDiagnosticFrame);
        if(++tour.phaseFrame>=phaseFrames(tour.phase)){
            if(tour.phase==1){ // the view after the camera sweep
                char path[160];std::snprintf(path,sizeof(path),"%s/shot1.bmp",tour.zoneDir);
                writeBmp(path,GFX_TOP,400);wrote=true;flagNextFrameTelemetry(telemetryDiagnosticFrame);
            }
            tour.phaseFrame=0;
            if(++tour.phase==phaseCount){
                setFrameTelemetryTag(0,0);finishZone(status);
                if(tour.fullDumps&&hooks.fullDump){hooks.fullDump();tour.state=tourDump;tour.waitFrames=0;return true;}
                nextZone();return true;
            }
            setFrameTelemetryTag(std::uint16_t(tour.index+1),std::uint8_t(tour.phase));dirty=true;
        }
        if(tour.phase==profilePhase)flagNextFrameTelemetry(telemetryDiagnosticFrame);
        return wrote;}
    case tourDump:
        if(++tour.waitFrames>=3&&!(hooks.fullDumpBusy&&hooks.fullDumpBusy())){dirty=true;nextZone();}
        return false;
    }
    return false;
}

// ---- drawing ----
struct Button {int row,col,height,width;};
constexpr Button tabs[4]={{2,1,3,10},{2,11,3,10},{2,21,3,10},{2,31,3,9}};
constexpr Button testButtons[5]={{6,1,3,39},{9,1,3,39},{12,1,3,39},{15,1,3,39},{18,1,3,19}};
constexpr Button dumpButton{18,21,3,19};
// STATS page: A/B switch of the rigid shader route for the next start (SD file
// rigid-single-on.txt, see game_startup). The single-texture shader stopped
// the console PICA until 2026-11-14; if its repair does not hold, the GPU
// guard learns it on the first stop and this button reports it. Both routes
// draw the same picture at the same speed in the emulator.
constexpr Button fastButton{25,1,3,39};
constexpr char fastSwitch[]="sdmc:/3ds/DuskLight/rigid-single-on.txt";
int fastAsked=-1; // unknown until first drawn
bool fastRouteAsked(){
    if(fastAsked<0){auto* file=std::fopen(fastSwitch,"rb");fastAsked=file?1:0;if(file)std::fclose(file);}
    return fastAsked==1;
}
bool hit(const Button& b,int row,int col){return row>=b.row&&row<b.row+b.height&&col>=b.col&&col<b.col+b.width;}
void button(const Button& b,const char* label,int style){
    const char* colors=style==2?"\x1b[42m\x1b[30m":style==1?"\x1b[46m\x1b[30m":"\x1b[44m\x1b[37;1m";
    char line[41];int length=int(std::strlen(label));if(length>b.width)length=b.width;
    for(int r=0;r<b.height;++r){
        std::memset(line,' ',b.width);line[b.width]=0;
        if(r==b.height/2)std::memcpy(line+(b.width-length)/2,label,length);
        std::printf("\x1b[%d;%dH%s%s" C_RST,b.row+r,b.col,colors,line);
    }
}
const char* tourStateName(){
    switch(tour.state){case tourWarp:return "requesting";case tourLoad:return "loading";case tourSettle:return "settling";
    case tourRun:return phases[tour.phase].name;case tourDump:return "RAM+VRAM dump";}
    return "idle";
}
void drawStats(){
    const float fps=smoothLoop>0?1000.0f/smoothLoop:0;
    std::printf("\x1b[6;1H" C_HDR " PERFORMANCE" C_RST "\x1b[K");
    std::printf("\x1b[7;1H" C_DIM "  Frame " C_VAL "%5.1f ms %s%5.1f fps" C_DIM "  CPU " C_VAL "%5.1f" C_RST "\x1b[K",smoothLoop,fps>=29?C_OK:fps>=24?C_HDR:C_BAD,fps,smoothExec);
    std::printf("\x1b[8;1H" C_DIM "  Paint " C_VAL "%5.1f" C_DIM " Update " C_VAL "%5.1f" C_DIM " GPUwait " C_VAL "%4.1f" C_RST "\x1b[K",last.paintMs,last.updateMs,last.gpuWaitMs);
    const int speed=gameSpeedPercent();
    std::printf("\x1b[9;1H" C_DIM "  Game speed %s%3d%%" C_DIM "  logic ticks/picture " C_VAL "%.2f" C_RST "\x1b[K",speed>=97?C_OK:speed>=85?C_HDR:C_BAD,speed,smoothTicks);
    std::printf("\x1b[10;1H" C_HDR " SCENE" C_RST "\x1b[K");
    std::printf("\x1b[11;1H" C_DIM "  Stage " C_VAL "%-8.8s" C_DIM " room " C_VAL "%d" C_DIM " layer " C_VAL "%d" C_RST "\x1b[K",last.stage,last.room,last.layer);
    std::printf("\x1b[12;1H" C_DIM "  Draws " C_VAL "%lu" C_DIM "  rejected " C_VAL "%llu" C_RST "\x1b[K",static_cast<unsigned long>(last.draws),static_cast<unsigned long long>(last.rejected));
    std::printf("\x1b[13;1H" C_DIM "  Simplified draws " C_VAL "%llu" C_RST "\x1b[K",static_cast<unsigned long long>(last.degraded));
    std::printf("\x1b[14;1H" C_DIM "  Pos " C_VAL "%.0f %.0f %.0f" C_RST "\x1b[K",last.position[0],last.position[1],last.position[2]);
    std::printf("\x1b[16;1H" C_HDR " MEMORY" C_RST "\x1b[K");
    std::printf("\x1b[17;1H" C_DIM "  Linear free " C_VAL "%5.1f MB" C_DIM "  Game heap " C_VAL "%5.1f MB" C_RST "\x1b[K",last.linearFree/1048576.0f,last.gameHeapFree/1048576.0f);
    std::printf("\x1b[18;1H" C_DIM "  Textures " C_VAL "%5.1f MB" C_DIM "  Geometry " C_VAL "%lu KB" C_RST "\x1b[K",last.textureBytes/1048576.0f,static_cast<unsigned long>(last.geometryBytes/1024));
    std::printf("\x1b[20;1H" C_HDR " CONTROLS" C_RST "\x1b[K");
    std::printf("\x1b[21;1H" C_CYN "  START+SELECT" C_DIM " automatic test on/off" C_RST "\x1b[K");
    std::printf("\x1b[22;1H" C_CYN "  L+R+A" C_DIM " RAM + VRAM dump (one file)" C_RST "\x1b[K");
    std::printf("\x1b[23;1H" C_CYN "  ZONES" C_DIM " tab: travel to any stage" C_RST "\x1b[K");
    if(gpuCanaryRigidSingleRefused())button(fastButton,"SHADER A/B: single stops this console",1);
    else button(fastButton,fastRouteAsked()?"SHADER A/B: SINGLE at next start":"SHADER A/B: MULTI (touch = single)",fastRouteAsked()?2:0);
}
void drawZones(){
    std::printf("\x1b[5;1H" C_DIM " %u stages, %u rooms" C_RST "\x1b[K",stageTotal,roomTotal);
    for(int n=0;n<listRows;++n){
        const int index=listTop+n;
        if(index>=int(stageTotal)){std::printf("\x1b[%d;1H\x1b[K",listRow+n);continue;}
        const auto& z=zoneStages[index];const bool on=index==selectedStage;
        const int number=on?zoneRooms[z.firstRoom+selectedRoom].room:z.roomCount;
        std::printf("\x1b[%d;1H%s%c%-7.7s %-22.22s %c%02d " C_RST "\x1b[K",listRow+n,on?"\x1b[42m\x1b[30m":(index&1)?"\x1b[40m\x1b[37;1m":"\x1b[40m\x1b[37m",
            on?'>':' ',z.stage,z.label,on?'r':'x',number);
    }
    const auto& z=zoneStages[selectedStage];const auto& r=zoneRooms[z.firstRoom+selectedRoom];
    std::printf("\x1b[26;1H" C_DIM " Room " C_VAL "%d" C_DIM " (%d/%d) point " C_VAL "%d" C_DIM " form " C_VAL "%s" C_RST "\x1b[K",int(r.room),selectedRoom+1,int(z.roomCount),int(r.point),selectedForm?"wolf":"human");
    std::printf("\x1b[27;1H" C_CYN " A" C_DIM " go " C_CYN "<>" C_DIM " room " C_CYN "X" C_DIM " form " C_CYN "L/R" C_DIM " page " C_CYN "B" C_DIM " back" C_RST "\x1b[K");
}
void drawTest(){
    const bool running=tour.state!=tourIdle;
    char label[40];
    static const char* const names[3]={"MAIN ZONES","ALL STAGES","ALL ROOMS"};
    for(int mode=0;mode<3;++mode){
        std::snprintf(label,sizeof(label),"%s %s (%u zones)",tour.mode==mode?(running?"RUNNING:":"SELECTED:"):"SELECT:",names[mode],modeCount(mode));
        button(testButtons[mode],label,tour.mode==mode?2:0);
    }
    button(testButtons[3],running?"STOP TEST":"START TEST  (or START+SELECT)",running?1:0);
    button(testButtons[4],tour.fullDumps?"RAM+VRAM/ZONE: ON":"RAM+VRAM/ZONE: OFF",tour.fullDumps?2:0);
    button(dumpButton,"FULL DUMP NOW",0);
    if(running){
        const auto& e=current();const auto& z=zoneStages[e.stage];
        const auto elapsed=seconds(svcGetSystemTick()-tour.startTick);
        std::printf("\x1b[22;1H" C_HDR " Zone " C_VAL "%u/%u " C_VAL "%s" C_DIM " r%d %s" C_RST "\x1b[K",tour.index+1,tour.limit?std::min(tour.limit,entryCount):entryCount,
            z.stage,int(zoneRooms[e.room].room),e.form?"wolf":"human");
        std::printf("\x1b[23;1H" C_DIM " %-22.22s " C_VAL "%s" C_RST "\x1b[K",z.label,tourStateName());
        std::printf("\x1b[24;1H" C_DIM " Elapsed " C_VAL "%llu:%02llu" C_DIM "  failed " C_VAL "%u" C_RST "\x1b[K",
            static_cast<unsigned long long>(elapsed/60),static_cast<unsigned long long>(elapsed%60),tour.failures);
        std::printf("\x1b[25;1H" C_DIM " %s" C_RST "\x1b[K",tour.dir+std::strlen(dataRoot)+1);
        std::printf("\x1b[26;1H" C_DIM " The pad is ignored while the test runs" C_RST "\x1b[K");
    }else{
        std::printf("\x1b[22;1H" C_DIM " Each zone: camera sweep, walking, rolls," C_RST "\x1b[K");
        std::printf("\x1b[23;1H" C_DIM " sword, targeting, items. Results in" C_RST "\x1b[K");
        std::printf("\x1b[24;1H" C_VAL " /3ds/DuskLight/tour/NNN/" C_RST "\x1b[K");
        std::printf("\x1b[25;1H" C_DIM " A crash resumes at the next zone when" C_RST "\x1b[K");
        std::printf("\x1b[26;1H" C_DIM " the game is opened again." C_RST "\x1b[K");
    }
}
void draw(bool clear){
    if(clear)std::printf("\x1b[2J");
    std::printf("\x1b[1;1H" C_CYN " DUSKLIGHT " DUSKLIGHT_RELEASE_UPPER C_DIM " %-8.8s " C_VAL "%4.1ffps %3d%%" C_RST "\x1b[K",last.stage,smoothLoop>0?1000.0f/smoothLoop:0.0f,gameSpeedPercent());
    button(tabs[0],"STATS",page==pageStats?2:0);button(tabs[1],"ZONES",page==pageZones?2:0);
    button(tabs[2],"TEST",page==pageTest?2:tour.state!=tourIdle?1:0);button(tabs[3],"EXIT",svcGetSystemTick()<exitArmedUntil?1:0);
    if(page==pageZones)drawZones();else if(page==pageTest)drawTest();else drawStats();
    if(svcGetSystemTick()<noticeUntil)std::printf("\x1b[29;1H" C_HDR " %-38.38s" C_RST,notice);
    else std::printf("\x1b[29;1H\x1b[K");
    std::fflush(stdout);
    if(auto* pixels=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr))GSPGPU_FlushDataCache(pixels,320*240*2);
}
void moveSelection(int delta){
    selectedStage=std::clamp(selectedStage+delta,0,int(stageTotal)-1);selectedRoom=zoneStages[selectedStage].primary;
    if(selectedStage<listTop)listTop=selectedStage;
    if(selectedStage>=listTop+listRows)listTop=selectedStage-listRows+1;
    dirty=true;
}
void travel(){
    const auto& z=zoneStages[selectedStage];const auto& r=zoneRooms[z.firstRoom+selectedRoom];
    char text[44];
    if(hooks.warp&&hooks.warp(z.stage,r.room,r.point,-1,selectedForm,false)){
        std::snprintf(text,sizeof(text),"Travelling to %s room %d",z.stage,int(r.room));page=pageStats;
    }else std::snprintf(text,sizeof(text),"Not possible here yet: wait for the title");
    say(text);
}
void touch(int row,int col){
    for(int n=0;n<3;++n)if(hit(tabs[n],row,col)){page=n;dirty=true;return;}
    if(hit(tabs[3],row,col)){
        if(svcGetSystemTick()<exitArmedUntil)exitRequested=true;
        else{exitArmedUntil=svcGetSystemTick()+3ull*SYSCLOCK_ARM11;say("Touch EXIT again to leave the game");}
        return;
    }
    if(page==pageZones&&row>=listRow&&row<listRow+listRows&&tour.state==tourIdle){
        const int index=listTop+row-listRow;
        if(index>=int(stageTotal))return;
        if(index==selectedStage)travel();else{selectedStage=index;selectedRoom=zoneStages[index].primary;dirty=true;}
        return;
    }
    if(page==pageStats&&hit(fastButton,row,col)){
        if(gpuCanaryRigidSingleRefused()){say("It stopped the GPU here: kept off");return;}
        if(fastRouteAsked()){std::remove(fastSwitch);fastAsked=0;say("Multi-texture route at next start");}
        else if(auto* file=std::fopen(fastSwitch,"wb")){
            std::fputs("1\n",file);std::fclose(file);fastAsked=1;say("Single-texture route at next start");
        }else say("Could not write to the SD card");
        return;
    }
    if(page==pageTest){
        for(int mode=0;mode<3;++mode)if(hit(testButtons[mode],row,col)&&tour.state==tourIdle){tour.mode=mode;dirty=true;}
        if(hit(testButtons[3],row,col)){if(tour.state==tourIdle)startTour();else endTour("stopped");dirty=true;}
        if(hit(testButtons[4],row,col)){tour.fullDumps=!tour.fullDumps;dirty=true;}
        if(hit(dumpButton,row,col)&&tour.state==tourIdle&&hooks.fullDump){hooks.fullDump();dirty=true;}
    }
}
unsigned number(const char* text,const char* key,unsigned fallback){
    const auto* found=std::strstr(text,key);
    return found?unsigned(std::strtoul(found+std::strlen(key),nullptr,10)):fallback;
}
bool readText(const char* path,char* text,std::size_t capacity){
    auto* file=std::fopen(path,"rb");if(!file)return false;
    const auto count=std::fread(text,1,capacity-1,file);text[count]=0;std::fclose(file);return true;
}
}
void initializeDevConsole(const DevGameHooks& game){
    hooks=game;
    char text[512];
    if(readText(statePath,text,sizeof(text))){
        // The previous run ended inside a test zone (crash, hang or power off):
        // keep its log beside the results and continue with the next zone.
        if(const auto* dir=std::strstr(text,"dir=")){
            unsigned n=0;for(dir+=4;*dir&&*dir!='\n'&&*dir!='\r'&&n<sizeof(tour.dir)-1;++dir)tour.dir[n++]=*dir;
            tour.dir[n]=0;
        }
        tour.mode=std::clamp(int(number(text,"mode=",modeStages)),0,2);tour.scale=std::clamp(int(number(text,"scale=",100)),10,400);
        tour.fullDumps=number(text,"full=",0)!=0;tour.autoExit=number(text,"autoexit=",0)!=0;tour.limit=number(text,"limit=",0);
        tour.failures=number(text,"failures=",0);
        const unsigned failed=number(text,"next=",0);
        buildEntries(tour.mode);
        if(tour.dir[0]&&failed<entryCount&&number(text,"resume=",0)){
            // Deliberate restart after a zone that never loaded: continue here.
            char saved[128];std::snprintf(saved,sizeof(saved),"%s/%03u-stuck-log.txt",tour.dir,failed);
            std::rename("sdmc:/3ds/DuskLight/log.txt",saved);
            tour.index=failed;tour.finished=number(text,"finished=",0);tour.startTick=svcGetSystemTick();
            setFatalUnattended(true);tour.state=tourWarp;tour.bootWarp=true;tour.zoneTick=svcGetSystemTick();page=pageTest;writeState();
        }else if(tour.dir[0]&&failed<entryCount){
            char saved[128];std::snprintf(saved,sizeof(saved),"%s/%03u-crash-log.txt",tour.dir,failed+1);
            std::rename("sdmc:/3ds/DuskLight/log.txt",saved);
            summaryFailure(failed,"crash-or-reset");++tour.failures;
            tour.index=failed+1;tour.finished=failed;tour.startTick=svcGetSystemTick();
            if(tour.index<entryCount&&!(tour.limit&&tour.index>=tour.limit)){
                setFatalUnattended(true);tour.state=tourWarp;tour.bootWarp=true;tour.zoneTick=svcGetSystemTick();page=pageTest;writeState();
            }else{tour.state=tourWarp;endTour("complete after a crash");}
        }else unlink(statePath);
        return;
    }
    char path[96];std::snprintf(path,sizeof(path),"%s/tour-auto.txt",dataRoot);
    if(readText(path,text,sizeof(text))){
        // Unattended run (emulator automation): mode= scale= limit= first= exit= full=
        tour.mode=std::clamp(int(number(text,"mode=",modeMain)),0,2);tour.scale=std::clamp(int(number(text,"scale=",100)),10,400);
        tour.limit=number(text,"limit=",0);tour.autoExit=number(text,"exit=",1)!=0;tour.fullDumps=number(text,"full=",0)!=0;
        if(createTour()){
            tour.index=tour.finished=std::min(number(text,"first=",0),entryCount-1);
            setFatalUnattended(true);tour.state=tourWarp;tour.bootWarp=true;tour.zoneTick=svcGetSystemTick();page=pageTest;writeState();
        }
    }
}
bool devConsoleBootStage(BootStage& stage){
    if(tour.state!=tourWarp||!tour.bootWarp)return false;
    const auto& e=current();const auto& z=zoneStages[e.stage];const auto& r=zoneRooms[e.room];
    stage={};std::snprintf(stage.name,sizeof(stage.name),"%s",z.stage);
    stage.room=r.room;stage.point=r.point;stage.layer=-1;
    return true;
}
void updateDevConsoleInput(std::uint32_t held,std::uint32_t down){
    selectHeld=(held&KEY_SELECT)!=0;
    const bool chord=(held&(KEY_START|KEY_SELECT))==(KEY_START|KEY_SELECT);
    if(chord&&!chordLatched){
        chordLatched=true;
        if(tour.state==tourIdle)startTour();else endTour("stopped");
        dirty=true;
    }
    if(!(held&(KEY_START|KEY_SELECT)))chordLatched=false;
    if(down&KEY_TOUCH){touchPosition point;hidTouchRead(&point);touch(point.py/8+1,point.px/8+1);}
    if(tour.state!=tourIdle||page==pageStats)return;
    if(down&KEY_B){page=pageStats;dirty=true;return;}
    if(page!=pageZones)return;
    // Held D-pad repeats after a short delay.
    const std::uint32_t moves=held&(KEY_DUP|KEY_DDOWN);
    repeatHold=moves?repeatHold+1:0;
    if(moves&&(repeatHold==1||(repeatHold>8&&repeatHold%2==0)))moveSelection(moves&KEY_DUP?-1:1);
    if(down&KEY_L)moveSelection(-listRows);
    if(down&KEY_R)moveSelection(listRows);
    const int rooms=zoneStages[selectedStage].roomCount;
    if(down&KEY_DLEFT){selectedRoom=(selectedRoom+rooms-1)%rooms;dirty=true;}
    if(down&KEY_DRIGHT){selectedRoom=(selectedRoom+1)%rooms;dirty=true;}
    if(down&KEY_X){selectedForm^=1;dirty=true;}
    if(down&KEY_A)travel();
}
bool devConsoleOwnsPad(){return tour.state!=tourIdle||page!=pageStats||selectHeld||chordLatched;}
void devConsoleVirtualPad(std::uint32_t& keys,std::int16_t& circleX,std::int16_t& circleY,std::int16_t& cstickX,std::int16_t& cstickY){
    keys=0;circleX=circleY=cstickX=cstickY=0;
    if(tour.state!=tourRun)return;
    const unsigned f=tour.phaseFrame,n=phaseFrames(tour.phase),percent=f*100/n;
    auto pulse=[&](unsigned period,unsigned offset){return (f+period-offset)%period<3;};
    switch(tour.phase){
    case 1:cstickX=f<n/2?120:-120;break; // camera sweep right, then back
    case 2: // run forward, left, right and back
        if(percent<25)circleY=150;else if(percent<50)circleX=-150;else if(percent<75)circleX=150;else circleY=-150;
        break;
    case 3: // rolls while running, then sword swings on the spot
        if(percent<50){circleY=150;if(pulse(20,6))keys|=KEY_A;}else if(pulse(12,0))keys|=KEY_B;
        break;
    case 4: // targeting: strafing, swings and jumps
        keys|=KEY_L;if(percent>=50)circleX=150;
        if(pulse(16,0))keys|=KEY_B;
        if(pulse(32,8))keys|=KEY_A;
        break;
    case 5: // Y item, X item (lantern on), shield, X again (lantern away)
        if(percent>=5&&percent<9)keys|=KEY_Y;
        if(percent>=40&&percent<44)keys|=KEY_X;
        if(percent>=60&&percent<75)keys|=KEY_R;
        if(percent>=85&&percent<89)keys|=KEY_X;
        break;
    }
}
bool devConsoleExitRequested(){return exitRequested;}
void devConsoleRates(float& fps,int& speedPercent){fps=smoothLoop>0?1000.0f/smoothLoop:0.0f;speedPercent=gameSpeedPercent();}
bool devConsoleProfiling(){return tour.state==tourRun&&tour.phase==profilePhase;}
bool serviceDevConsole(const DevGameStatus& status){
    last=status;
    smoothLoop=smoothLoop>0?smoothLoop*0.9f+status.loopMs*0.1f:status.loopMs;
    smoothExec=smoothExec>0?smoothExec*0.9f+status.execMs*0.1f:status.execMs;
    smoothTicks=smoothTicks>0?smoothTicks*0.9f+float(status.ticks)*0.1f:float(status.ticks);
    const bool wrote=stepTour(status);
    if(exitRequested&&tour.state!=tourIdle&&!tour.restarting){ // a clean exit does not resume later
        tour.autoExit=false;endTour("stopped by exit");
    }
    if(dirty||status.frame%30==0){
        const bool clear=dirty;dirty=false;draw(clear);
    }
    return wrote;
}
void devConsoleRedraw(){dirty=true;}
void shutdownDevConsole(){
    // A clean exit (EXIT, HOME close) must not resume the test at the next launch.
    if(tour.state!=tourIdle&&!tour.restarting){tour.autoExit=false;endTour("stopped by exit");}
}
}
