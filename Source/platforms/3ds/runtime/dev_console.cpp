#include "dev_console.hpp"
#include "beta_screen.hpp"
#include "benchmark_input.hpp"
#include "story_chapter_pad.hpp"
#include "core_probe.hpp"
#include "release.hpp"
#include "frame_telemetry.hpp"
#include "gpu_sync.hpp"
#include "gpu_list_capture.hpp"
#include "gpu_canary.hpp"
#include "bootstrap.hpp"
#include "tour_elapsed.hpp"
#include "tour_cadence.hpp"
#include "tour_visual_capture.hpp"
#include "measured_dump.hpp"
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
extern "C" void OSReport(const char*,...);
namespace dusk::native {
namespace {
constexpr char tourBinaryHeader[]="build=" DUSK_DUMP_BUILD_ID "\ncompiled=" __DATE__ " " __TIME__ "\n";
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
enum TourMode {modeMain,modeStages,modeRooms,modeRandom,modeStory,modeEverything,modeTotal};
// The random test: this many zones drawn from every room of every stage, in
// a random order, each in a random state (dev_console only picks the seed;
// game_startup.cpp turns it into clothes, sword, shield, items, hearts and
// time of day). The seed is kept in tour-state.txt, so a restart after a crash
// continues the same plan, and tour-options.txt seed=... replays it.
constexpr unsigned randomZones=94;
constexpr const char* modeNames[]{"main","stages","rooms","random","story","everything"};
struct Phase {const char* name;unsigned frames;};
// The still and plain phases both hold still, one after the other: still
// with the native effects on, plain with them off (shadows, the world
// particle system, bloom and haze). Their difference is what the effects cost
// in that zone. (The idle phase cannot serve: it runs right after arrival,
// with caches cold.) The plain phase stays out of the zone's overall figures.
// Between them the onecore phase holds still with the second-core worker off
// (gx_worker.hpp): still minus onecore is what the worker gains in that zone
// on the machine the test runs on, which no emulator run can tell. It stays
// out of the overall figures too, and is the still phase again when the
// worker is off anyway.
// The last phase holds still with the detailed profiler on (two sparse reports).
// After it the menu phase opens the game's collection screen (START), holds
// it and closes it: the cost of the game's own 2D menus, which the owner
// reported as the slowest part (Alpha 7). It comes last so that a menu that
// does not close cannot change what the other phases measure.
constexpr Phase phases[]={{"idle",60},{"camera",90},{"walk",150},{"action",120},{"target",60},{"items",120},{"still",60},{"onecore",60},{"plain",60},
    {"profile",62},{"menu",150}};
constexpr unsigned phaseCount=sizeof(phases)/sizeof(*phases),menuPhase=phaseCount-1,profilePhase=phaseCount-2,plainPhase=phaseCount-3,
    oneCorePhase=phaseCount-4,stillPhase=phaseCount-5;
// Phases left out of a zone's overall figures (profile frames are flagged).
constexpr std::uint32_t apartPhases=(1u<<plainPhase)|(1u<<menuPhase)|(1u<<oneCorePhase);
bool menuPhaseOn=true;
// The probe of the other cores at the end of a test that ran every zone
// (core_probe.hpp); tour-options.txt probe=0 leaves it out.
bool coreProbeOn=true;
bool plainActive;int plainSaved[3];
void setPlain(bool on){
    if(on==plainActive)return;
    plainActive=on;
    using namespace dusk::native_effects;
    if(on){plainSaved[0]=shadows;plainSaved[1]=particles;plainSaved[2]=bloom;shadows=particles=bloom=0;}
    else{shadows=plainSaved[0];particles=plainSaved[1];bloom=plainSaved[2];}
}
// The still phase measures the second-core worker on (and not resting), the
// onecore phase right after it the worker off.
SecondCoreTest secondCoreState=SecondCoreTest::play;
void setSecondCore(SecondCoreTest state){
    if(state==secondCoreState)return;
    secondCoreState=state;
    if(hooks.secondCore)hooks.secondCore(state);
}
struct Entry {std::uint16_t room;std::uint8_t stage,form;};
Entry entries[roomTotal+stageTotal];
unsigned entryCount;
struct Tour {
    int state{tourIdle},mode{modeEverything},scale{100};
    std::uint32_t seed{};
    bool fullDumps{},autoExit{},bootWarp{},moved{},died{},restarting{},invalidPosition{},roomRetry{};
    unsigned index{},limit{},phase{},phaseFrame{},waitFrames{},stableFrames{},failures{},finished{},diedPhase{};
    // Pictures of the menu phase the game was paused in (its menu was open).
    unsigned menuPictures{};
    // startTick is this launch's start; elapsedBefore the test time of the
    // launches before a restart (crash or stuck zone), and startedMs the
    // wall clock (osGetTime) of the test's first start. Alpha 6 reset the
    // elapsed time on every restart.
    std::uint64_t startTick{},elapsedBefore{},startedMs{},zoneTick{},loadTicks{},rejected0{},degraded0{};
    std::uint32_t sceneChanges0{};
    unsigned storyFirst{}, forensicBuckets{}, slowRoomDumps{}, lastSlowRoom{};
    TourVisualSchedule visualSchedule;
    struct StoryRun {
        story::Phase phase{story::loading};
        unsigned facts{}, rendered{}, startScene{}, destinationFrames{}, cutsceneFirst{}, cutsceneLast{},
            enemyPeak{}, damageSamples{}, midnaPictures{}, lanternPictures{}, attackPictures{};
        bool cutsceneSeen{}, portalInitiallyOpen{}, portalActivated{}, passedWaypoint{};
        unsigned playableTicks{};
        float startX{}, startZ{}, furthest{};
        story::Change change{};
        story::ChapterEvidence chapterEvidence{};
        story::PortalWarpProof warpProof{};
        char originStage[9]{};
        int originRoom{-1}, originPoint{-1}, originLayer{-1};
        bool originWolf{};
    } storyRun;
    std::size_t logMark{};
    char dir[64]{},zoneDir[112]{};
    // The game's first own scene change request after the zone's travel
    // (during the load too), as logged by game_startup.
    char leftBy[200]{};
} tour;
std::uint64_t elapsedSeconds(){return tour.elapsedBefore+seconds(svcGetSystemTick()-tour.startTick);}
// ---- test finished ----
// The console is left running the test overnight. When the last zone is done
// the game stands still and the bottom screen says TESTS FINISHED; after
// holdSeconds without a button or a touch the log is written and the console
// is powered off (dev_console.hpp). 0 keeps the old behaviour: the test ends
// and the game goes on.
constexpr unsigned defaultHoldSeconds=15*60;
unsigned holdSeconds=defaultHoldSeconds;
struct Finished {
    bool active{};
    // 0 counting down; 1 the shutdown notification was sent; 2 PTM was asked.
    int powerStage{};
    std::uint64_t untilTick{},stageTick{},elapsed{};
    unsigned zones{},failures{},hold{},shown{~0u};
    std::uint32_t held{~0u};
    char result[28]{},dir[64]{};
} finished;
// died: Link reached zero life (Game Over); moved: the game left the zone's stage.
bool validPlayerPosition(const DevGameStatus& status){
    return std::isfinite(status.position[0]) && std::isfinite(status.position[1]) && std::isfinite(status.position[2]);
}
const char* zoneStatus(const DevGameStatus& status){
    if(tour.invalidPosition)return "invalid-player-position";
    if(status.rejected>tour.rejected0)return "render-rejected";
    return tour.died?"died":tour.moved?"moved":"ok";
}
bool hasStory(int mode){return mode==modeStory||mode==modeEverything;}
bool storyZone(unsigned index){return hasStory(tour.mode)&&index<story::stepCount;}
bool randomLoadout(){return tour.mode==modeRandom||(tour.mode==modeEverything&&!storyZone(tour.index));}
unsigned roomIndex(unsigned index){return tour.mode==modeEverything?index-story::stepCount:index;}
unsigned phaseFrames(unsigned phase){
    if(phase==profilePhase)return phases[phase].frames;
    if(phase==menuPhase&&!menuPhaseOn)return 1;
    const unsigned scale=tour.mode==modeEverything?unsigned(tour.scale)*40/100:unsigned(tour.scale);
    return std::max(12u,phases[phase].frames*scale/100u);
}
std::uint32_t mix(std::uint32_t value){
    value+=0x9E3779B9u;value=(value^(value>>16))*0x85EBCA6Bu;value=(value^(value>>13))*0xC2B2AE35u;return value^(value>>16);
}
// Seed of a zone's random state: never 0 (0 is the fixed test equipment).
std::uint32_t zoneSeed(unsigned index){const auto value=mix(tour.seed^mix(index+1));return value?value:1;}
void buildEntries(int mode){
    entryCount=0;
    if(mode==modeStory){entryCount=story::stepCount;return;}
    auto add=[](unsigned stage,unsigned room,unsigned form){entries[entryCount++]={std::uint16_t(room),std::uint8_t(stage),std::uint8_t(form)};};
    if(mode==modeRandom){
        // Every room once, shuffled with the tour's seed; the first zones are
        // the plan. Outdoor stages (F_) are played as the wolf one time in four.
        // The title movie stage (S_MV000) is left out: only the title enters
        // it, and a travel there never happens (the zone waited 70 seconds
        // and ended "moved" in the stage it started from).
        for(unsigned s=0;s<stageTotal;++s)if(std::strcmp(zoneStages[s].stage,"S_MV000"))
            for(unsigned r=0;r<zoneStages[s].roomCount;++r)add(s,zoneStages[s].firstRoom+r,0);
        std::uint32_t state=tour.seed;
        for(unsigned i=entryCount;i>1;--i){state=mix(state);std::swap(entries[i-1],entries[state%i]);}
        entryCount=std::min(entryCount,randomZones);
        for(unsigned i=0;i<entryCount;++i)
            if(zoneStages[entries[i].stage].stage[0]=='F'&&mix(tour.seed^(i*0x2545F491u))%4==0)entries[i].form=1;
        return;
    }
    for(unsigned s=0;s<stageTotal;++s){
        const auto& z=zoneStages[s];
        if(mode==modeMain&&!z.mainTour)continue;
        if(mode==modeEverything&&std::strcmp(z.stage,"S_MV000")==0)continue;
        if(mode==modeRooms||mode==modeEverything)for(unsigned r=0;r<z.roomCount;++r)add(s,z.firstRoom+r,0);
        else add(s,z.firstRoom+z.primary,0);
    }
    // Second pass as the wolf through the main outdoor stages.
    for(unsigned s=0;s<stageTotal;++s)if(zoneStages[s].mainTour&&zoneStages[s].stage[0]=='F')add(s,zoneStages[s].firstRoom+zoneStages[s].primary,1);
    if(mode==modeEverything)entryCount+=story::stepCount;
}
unsigned modeCount(int mode){
    if(mode==modeStory)return story::stepCount;
    if(mode==modeRandom)return std::min(randomZones,roomTotal);
    unsigned count=mode==modeEverything?story::stepCount:0,wolf=0;
    for(unsigned s=0;s<stageTotal;++s){
        const auto& z=zoneStages[s];
        if(z.mainTour&&z.stage[0]=='F')++wolf;
        if(mode==modeMain&&!z.mainTour)continue;
        if(mode==modeEverything&&std::strcmp(z.stage,"S_MV000")==0)continue;
        count+=(mode==modeRooms||mode==modeEverything)?z.roomCount:1;
    }
    return count+wolf;
}
enum ResumePoint {inZone,nextUntested,afterRoomResult};
bool writeState(ResumePoint resume=inZone){
    if(auto* file=std::fopen(statePath,"wb")){
        const int written=std::fprintf(file,"dir=%s\nmode=%d\nnext=%u\nscale=%d\nfull=%d\nautoexit=%d\nlimit=%u\nfailures=%u\nresume=%d\nfinished=%u\nseed=%08lx\n"
            "elapsed=%llu\nstarted_ms=%llu\nhold=%u\nmenu=%d\nprobe=%d\nstory_first=%u\nstory_version=%u\nforensic_buckets=%u\nroom_retry=%d\nslow_room_dumps=%u\nlast_slow_room=%u\n",tour.dir,tour.mode,tour.index,
            tour.scale,int(tour.fullDumps),int(tour.autoExit),tour.limit,tour.failures,int(resume),tour.finished,static_cast<unsigned long>(tour.seed),
            static_cast<unsigned long long>(elapsedSeconds()),static_cast<unsigned long long>(tour.startedMs),holdSeconds,int(menuPhaseOn),
            int(coreProbeOn),tour.storyFirst,story::planVersion,tour.forensicBuckets,int(tour.roomRetry),tour.slowRoomDumps,tour.lastSlowRoom);
        if(!std::fclose(file)&&written>=0)return true;
    }
    OSReport("Native test: could not write checkpoint for zone %u\n",tour.index+1);
    return false;
}
std::uint64_t number64(const char* text,const char* key,std::uint64_t fallback);
// A restarted test continues its clock (rule and limit in tour_elapsed.hpp).
void restoreElapsed(const char* text){
    const auto saved=number64(text,"elapsed=",0);
    const auto resumed=resumedElapsed(saved,number64(text,"started_ms=",0),osGetTime());
    tour.elapsedBefore=resumed.seconds;tour.startTick=svcGetSystemTick();tour.startedMs=resumed.startedMs;
    static const char* const sources[3]{"wall clock","capped: the console was stopped or off","saved figure: wall clock not recorded or set back"};
    OSReport("Native test resumed: elapsed %llu s (%s; saved %llu s, wall clock %llu s, limit saved+%llu s)\n",
        static_cast<unsigned long long>(resumed.seconds),sources[resumed.source],static_cast<unsigned long long>(saved),
        static_cast<unsigned long long>(resumed.wallSeconds),static_cast<unsigned long long>(resumeGapLimit));
}
void summaryHeader(){
    char path[128];std::snprintf(path,sizeof(path),"%s/summary.csv",tour.dir);
    if(auto* file=std::fopen(path,"wb")){
        std::fprintf(file,"zone,stage,room,point,form,status,load_s,frames,loop_ms,loop_p50,loop_p95,loop_p99,loop_max,fps,exec_ms,exec_p95,"
            "paint_ms,update_ms,draw_ms,gpu_wait_ms,gpu_tail_ms,audio_ms,draws,over34,over40,over50,rejected,degraded,linear_free_kb,game_free_kb,texture_kb");
        for(unsigned p=0;p<phaseCount;++p)if(p!=profilePhase)std::fprintf(file,",%s_ms",phases[p].name);
        std::fprintf(file,",gpu_busy_ms,gpu_idle_ms");
        std::fputc('\n',file);std::fclose(file);
    }
}
void summaryFailure(unsigned index,const char* status){
    char path[128];std::snprintf(path,sizeof(path),"%s/summary.csv",tour.dir);
    if(auto* file=std::fopen(path,"ab")){
        if(storyZone(index)){
            const auto& step=story::steps[index];
            std::fprintf(file,"%u,%s,%d,%d,%s,%s\n",index+1,step.start.stage,step.start.room,step.start.point,
                step.wolf?"wolf":"human",status);
        }else{
            const auto& e=entries[roomIndex(index)];
            std::fprintf(file,"%u,%s,%d,%d,%s,%s\n",index+1,zoneStages[e.stage].stage,int(zoneRooms[e.room].room),int(zoneRooms[e.room].point),
                e.form?"wolf":"human",status);
        }
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
// Battery level (0-5) and whether the charger is connected, for tour.txt: an
// overnight run on the battery ends when it is empty, not when the game stops.
void powerLine(std::FILE* out,const char* when){
    u8 level=0,charging=0;bool adapter=false;
    if(R_FAILED(ptmuInit())){std::fprintf(out,"power_%s=unknown\n",when);return;}
    const bool known=R_SUCCEEDED(PTMU_GetBatteryLevel(&level));
    PTMU_GetBatteryChargeState(&charging);PTMU_GetAdapterState(&adapter);
    ptmuExit();
    if(known)std::fprintf(out,"power_%s=battery %u/5, %s, charger %s\n",when,unsigned(level),charging?"charging":"not charging",
        adapter?"connected":"not connected");
    else std::fprintf(out,"power_%s=unknown\n",when);
}
unsigned lastZone(){
    const unsigned first=hasStory(tour.mode)?tour.storyFirst:0;
    return tour.limit?first+std::min(tour.limit,entryCount-first):entryCount;
}
bool createTour(){
    char path[96];std::snprintf(path,sizeof(path),"%s/tour",dataRoot);
    if(!directory("sdmc:/3ds")||!directory(dataRoot)||!directory(path))return false;
    std::snprintf(tour.dir,sizeof(tour.dir),"%s/tour/%03u",dataRoot,nextTourNumber());
    if(!directory(tour.dir))return false;
    if(hasStory(tour.mode)){
        if(!tour.seed||tour.mode==modeStory)tour.seed=story::seed;
        tour.autoExit=true;
        menuPhaseOn=coreProbeOn=false;
    }else if(!tour.seed)tour.seed=mix(std::uint32_t(svcGetSystemTick())^std::uint32_t(osGetTime()));
    buildEntries(tour.mode);
    tour.storyFirst=std::min(tour.storyFirst,entryCount-1);
    char file[128];std::snprintf(file,sizeof(file),"%s/tour.txt",tour.dir);
    if(auto* out=std::fopen(file,"wb")){
        bool new3ds=false;APT_CheckNew3DS(&new3ds);
        std::fprintf(out,"%smode=%s\nzones=%u\nscale_percent=%d\nfull_dumps=%d\nnew3ds=%d\nfirm=%08lx\nkernel=%08lx\n"
            "application_region=%lu\napplication_used=%lu\nlinear_free=%lu\nvram_free=%lu\n"
            "note=cadence includes every picture and all flags; legacy zone summaries exclude diagnostic pictures\n",
            tourBinaryHeader,modeNames[tour.mode],entryCount,tour.scale,
            int(tour.fullDumps),int(new3ds),static_cast<unsigned long>(osGetFirmVersion()),static_cast<unsigned long>(osGetKernelVersion()),
            static_cast<unsigned long>(osGetMemRegionSize(MEMREGION_APPLICATION)),static_cast<unsigned long>(osGetMemRegionUsed(MEMREGION_APPLICATION)),
            static_cast<unsigned long>(linearSpaceFree()),static_cast<unsigned long>(vramSpaceFree()));
        std::fprintf(out,"seed=%08lx\n",static_cast<unsigned long>(tour.seed));
        powerLine(out,"at_start");
        std::fprintf(out,"finished_screen=%u s without input, then power off (tour-options.txt hold=)\nmenu_phase=%d\ncore_probe=%d\n",holdSeconds,
            int(menuPhaseOn),int(coreProbeOn));
        if(tour.mode==modeEverything)std::fputs("room_scale_factor=0.40\nautomatic_forensic_dump_cap=24\ndistributed_dump_cap=16\nslow_room_dump_cap=8\nslow_room_trigger=12+ capture-free pictures in still or ordinary room play, median >=100 ms or mean GPU busy >=80 ms\n",out);
        if(hasStory(tour.mode)){
            std::fprintf(out,"story_version=%u\nfirst_step=%u\nlast_step=%u\nphases=0:loading 1:play 2:arrival 3:evidence\n",
                story::planVersion,tour.storyFirst+1,std::min(lastZone(),story::stepCount));
        }
        if(tour.mode!=modeStory){
            if(tour.mode==modeEverything)
                std::fprintf(out,"first_case=%u\nlast_case=%u\ngpu_samples=camera,still\n",tour.storyFirst+1,lastZone());
            std::fputs(tour.mode==modeEverything?"room_phases=":"phases=",out);
            for(unsigned p=0;p<phaseCount;++p)std::fprintf(out,"%u:%s:%u ",p,phases[p].name,phaseFrames(p));
            std::fputc('\n',out);
        }
        if(!hasStory(tour.mode)){
            std::fprintf(out,"shots=shot1.bmp at the end of the camera phase; shot3.bmp in the menu phase with the collection screen open; "
                "shot2.bmp and bottom.bmp at the end of the zone (an item from the items phase may still be out)\n"
                "overall=the zone figures leave out the plain, profile and menu phases\n"
                "life=restored to the zone's starting value at every phase start (random test: at least 3 hearts); status=died when Link dies anyway, "
                "and the frames after the death are flagged out of the statistics\n"
                "dumps=numbered in order of capture under /3ds/DuskLight/dumps; their info.txt test_zone= names the zone\n");
        }
        std::fclose(out);
    }
    std::snprintf(file,sizeof(file),"%s/boot-log.txt",tour.dir);
    if(auto* out=std::fopen(file,"wb")){writeNativeLogSince(0,out);std::fclose(out);}
    summaryHeader();
    if(hasStory(tour.mode)){
        std::snprintf(file,sizeof(file),"%s/story.csv",tour.dir);
        if(auto* out=std::fopen(file,"wb")){
            std::fputs("zone,step,result,required,facts,ticks,pictures,movement,cutscene_first,cutscene_last,enemies,damage_samples,midna_pictures,lantern_pictures,attack_pictures,portal_activated,life,rejected,"
                 "from_stage,from_room,to_stage,to_room,to_point,to_layer,exit,final_stage,final_room,final_point,final_layer,origin_scene,final_scene,arrival_pictures,ability_ticks,demo_ticks,timeline_ticks,target_hits,finishing_blow,duel_ending,origin_stage,origin_room,origin_point,origin_layer,origin_form,demo_ended,warp_portal,warp_spawn,warp_confirmed,warp_departure_pictures\n",out);
            std::fclose(out);
        }else return false;
        std::snprintf(file,sizeof(file),"%s/story-plan.csv",tour.dir);
        if(auto* out=std::fopen(file,"wb")){
            std::fputs("zone,step,checkpoint,stage,room,point,layer,form,ticks,required,to_stage,to_room,to_point,to_layer,exit,chapter_goal,subject,ability,warp_portal,warp_spawn,target_x,target_z,via_x,via_z,intro_end_room\n",out);
            for(unsigned i=tour.storyFirst;i<std::min(lastZone(),story::stepCount);++i){
                const auto& step=story::steps[i];
                const auto& route=step.destination;
                std::fprintf(out,"%u,%s,%u,%s,%d,%d,%d,%s,%u,%u,%s,%d,%d,%d,%d,%s,%s,%u,%d,%d,%.9g,%.9g,",
                    i+1,step.id,unsigned(step.checkpoint),step.start.stage,step.start.room,step.start.point,step.start.layer,
                    step.wolf?"wolf":"human",step.ticks,story::requirements(step),route.stage?route.stage:"",route.room,
                    route.point,route.layer,step.exit,story::chapterGoalName(step.chapter.goal),
                    step.chapter.subject?step.chapter.subject:"",unsigned(step.chapter.ability),step.warpPortal,step.warpSpawn,step.x,step.z);
                if(step.via)std::fprintf(out,"%.9g,%.9g",step.via->x,step.via->z);
                else std::fputc(',',out);
                std::fprintf(out,",%d\n",step.chapter.introEndRoom);
            }
            if(std::fclose(out))return false;
        }else return false;
    }
    // The plan, one line per zone as it starts (kept even if the zone crashes):
    // the seed alone reproduces the zone's random state (tour-options seed=).
    std::snprintf(file,sizeof(file),"%s/zones.csv",tour.dir);
    if(auto* out=std::fopen(file,"wb")){std::fputs("zone,stage,room,point,form,state_seed\n",out);std::fclose(out);}
    tour.roomRetry=false;
    tour.index=0;tour.slowRoomDumps=tour.lastSlowRoom=0;tour.failures=tour.finished=tour.forensicBuckets=0;tour.startTick=svcGetSystemTick();tour.elapsedBefore=0;tour.startedMs=osGetTime();
    return true;
}
const Entry& current(){return entries[roomIndex(tour.index)];}
void roomDirectory(char* path,std::size_t size){
    const auto& e=current();
    std::snprintf(path,size,"%s/%03u-%s-r%02d-%c",tour.dir,tour.index+1,zoneStages[e.stage].stage,
        int(zoneRooms[e.room].room),e.form?'w':'h');
}
void planLine(){
    beginTourCadence(tour.dir,tour.index+1,svcGetSystemTick());
    if(tour.roomRetry)return; // this ordinal already has its plan row
    char path[128];std::snprintf(path,sizeof(path),"%s/zones.csv",tour.dir);
    if(auto* file=std::fopen(path,"ab")){
        if(storyZone(tour.index)){
            const auto& step=story::steps[tour.index];
            std::fprintf(file,"%u,%s,%d,%d,%s,%08lx\n",tour.index+1,step.start.stage,step.start.room,step.start.point,
                step.wolf?"wolf":"human",static_cast<unsigned long>(story::seed));
            std::fclose(file);
            return;
        }
        const auto& e=current();
        std::fprintf(file,"%u,%s,%d,%d,%s,%08lx\n",tour.index+1,zoneStages[e.stage].stage,int(zoneRooms[e.room].room),int(zoneRooms[e.room].point),
            e.form?"wolf":"human",static_cast<unsigned long>(randomLoadout()?zoneSeed(tour.index):0));
        std::fclose(file);
    }
}
void beginZone(){
    tour.storyRun={};
    if(storyZone(tour.index))tour.phaseFrame=0;
    planLine();
    setFatalUnattended(true);
    tour.state=tourWarp;tour.waitFrames=tour.stableFrames=0;tour.zoneTick=svcGetSystemTick();
    // Between zones: the SD log is current if this zone hangs or the power goes.
    setFrameTelemetryTag(0,0);writeState();flushNativeLog();dirty=true;
}
bool writeUnfinishedMeasurements();
void endTour(const char* why){
    if(!endTourCadence())OSReport("Native test: cadence write failed\n");
    writeUnfinishedMeasurements(); // of a zone the test was stopped in (measured_dump.hpp)
    const auto elapsed=elapsedSeconds();
    // Every zone was run (not stopped by hand, not an emulator run that ends
    // the process): hold the finished screen.
    const bool hold=!std::strncmp(why,"complete",8)&&!tour.autoExit&&holdSeconds;
    char file[128];std::snprintf(file,sizeof(file),"%s/tour.txt",tour.dir);
    if(auto* out=std::fopen(file,"ab")){
        std::fprintf(out,"result=%s\nzones_finished=%u\nfailures=%u\nelapsed_seconds=%llu\n",why,tour.finished,tour.failures,
            static_cast<unsigned long long>(elapsed));
        if(hold)std::fprintf(out,"finished_screen_seconds=%u\n",holdSeconds);
        powerLine(out,"at_end");
        std::fclose(out);
    }
    unlink(statePath);setFatalUnattended(false);
    setPlain(false);setSecondCore(SecondCoreTest::play);
    setFrameTelemetryTag(0,0);tour.state=tourIdle;
    // After the results are written and the test state is gone: a console
    // the probe stops keeps every result and does not start the test again.
    if(!std::strncmp(why,"complete",8)&&coreProbeOn){
        flushNativeLog();
        if(auto* out=std::fopen(file,"ab")){runCoreProbe(out);std::fclose(out);}
    }
    char text[44];std::snprintf(text,sizeof(text),"Test %s: %u zones, %u failed",why,tour.finished,tour.failures);say(text);
    if(tour.autoExit)exitRequested=true;
    if(hold){
        finished={};
        finished.active=true;finished.untilTick=svcGetSystemTick()+std::uint64_t(holdSeconds)*SYSCLOCK_ARM11;
        finished.elapsed=elapsed;finished.zones=tour.finished;finished.failures=tour.failures;finished.hold=holdSeconds;
        std::snprintf(finished.result,sizeof(finished.result),"%s",why);
        std::snprintf(finished.dir,sizeof(finished.dir),"%s",tour.dir);
        page=pageTest;dirty=true;
        OSReport("Native test finished (%s): %u zones, %u failed, %llu s; the console powers off after %u s without input\n",why,
            tour.finished,tour.failures,static_cast<unsigned long long>(elapsed),holdSeconds);
        flushNativeLog();
    }
}
// A complete power-off in two steps. First the notification a long press of
// the power button sends (0x203, what Rosalina's "Power off" uses): the HOME
// menu then closes this application through APT, the game loop ends as on any
// exit and the system powers down. If the application is still running ten
// seconds later, PTM is asked directly (the service this application already
// uses for the CPU speed).
bool notifyPowerOff(){
    const Result result=srvPublishToSubscriber(0x203,0);
    OSReport("Native power off: shutdown notification 0x203 %s (result %08lx)\n",R_SUCCEEDED(result)?"sent":"refused",
        static_cast<unsigned long>(result));
    return R_SUCCEEDED(result);
}
bool requestPtmPowerOff(){
    Result result=ptmSysmInit();
    if(R_SUCCEEDED(result)){result=PTMSYSM_ShutdownAsync(0);ptmSysmExit();}
    OSReport("Native power off: PTMSYSM_ShutdownAsync %s (result %08lx)\n",R_SUCCEEDED(result)?"accepted":"refused",
        static_cast<unsigned long>(result));
    return R_SUCCEEDED(result);
}
void finishedLine(const char* format,unsigned value){
    char file[128];std::snprintf(file,sizeof(file),"%s/tour.txt",finished.dir);
    if(auto* out=std::fopen(file,"ab")){std::fprintf(out,format,value);std::fclose(out);}
}
void nextZone(){
    tour.roomRetry=false;
    ++tour.index;
    if(tour.index>=lastZone()){endTour("complete");return;}
    beginZone();
}
void failZone(const char* status){
    summaryFailure(tour.index,status);++tour.failures;nextZone();
}
void appendStoryResult(unsigned index,const char* result,const DevGameStatus& status);
// A stage that never finishes loading leaves no scene to travel from: record
// it and start the game again in the next zone.
void restartAfterStuckZone(const char* status){
    if(hooks.stuck)hooks.stuck();
    if(storyZone(tour.index))appendStoryResult(tour.index,status,last);
    summaryFailure(tour.index,status);++tour.failures;++tour.index;
    if(tour.index>=lastZone()){
        // The last zone never loaded: the finished screen holds without a
        // scene behind it; without that screen the game is left.
        endTour("complete");if(!finished.active)exitRequested=true;return;
    }
    tour.roomRetry=false;
    writeState(nextUntested);flushNativeLog();
    if(!endTourCadence())OSReport("Native test: cadence write failed before restart\n");
    tour.restarting=true;tour.state=tourIdle;setFrameTelemetryTag(0,0);
    aptSetChainloaderToSelf();exitRequested=true;
    say("Zone did not load: restarting the game");
}
unsigned number(const char* text,const char* key,unsigned fallback);
int configuredMode(const char* text,int fallback);
std::uint32_t hexNumber(const char* text,const char* key,std::uint32_t fallback);
bool readText(const char* path,char* text,std::size_t capacity);
void startTour(){
    if(tour.state!=tourIdle)return;
    tour.seed=0; // a new plan unless tour-options.txt names a seed
    tour.storyFirst=0;
    // Optional settings for a test started by hand (START+SELECT or the TEST
    // tab): tour-options.txt with mode= scale= limit= full= exit=. The
    // emulator gate uses it to start a short test from the title screen, the
    // way it is started on a console.
    {char text[256];char path[96];std::snprintf(path,sizeof(path),"%s/tour-options.txt",dataRoot);
     if(readText(path,text,sizeof(text))){
        tour.mode=configuredMode(text,tour.mode);tour.scale=std::clamp(int(number(text,"scale=",tour.scale)),10,400);
        tour.storyFirst=number(text,"first=",0);
        tour.limit=number(text,"limit=",tour.limit);tour.fullDumps=number(text,"full=",tour.fullDumps)!=0;
        tour.autoExit=number(text,"exit=",tour.autoExit)!=0;tour.seed=hexNumber(text,"seed=",0);
        holdSeconds=std::min(number(text,"hold=",defaultHoldSeconds),24u*60u*60u);
        menuPhaseOn=number(text,"menu=",1)!=0;coreProbeOn=number(text,"probe=",1)!=0;
     }}
    if(!createTour()){say("Test: cannot create the tour folder");return;}
    if(hasStory(tour.mode))tour.index=tour.storyFirst;
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
        "draws=%.1f\nframes_over_34ms=%lu\nframes_over_40ms=%lu\nframes_over_50ms=%lu\ngpu_busy_ms=%.2f\ngpu_idle_ms=%.2f\n",name,
        static_cast<unsigned long>(s.frames),static_cast<unsigned long>(s.flagged),static_cast<unsigned long>(s.saturated),
        s.loopMean,s.loopP50,s.loopP95,s.loopP99,s.loopMax,s.loopMean>0?1000.0f/s.loopMean:0.0f,s.execMean,s.execP95,s.execMax,
        s.paint,s.update,s.draw,s.pace,s.gpuWait,s.gpuTail,s.audio,s.draws,
        static_cast<unsigned long>(s.over34),static_cast<unsigned long>(s.over40),static_cast<unsigned long>(s.over50),s.gpuBusy,s.gpuIdle);
}
bool writeZoneMeasurements(const char* dir);
void finishZone(const DevGameStatus& status){
    const auto& e=current();const auto& z=zoneStages[e.stage];const auto& r=zoneRooms[e.room];
    const std::uint16_t zone=std::uint16_t(tour.index+1);
    writeZoneMeasurements(tour.zoneDir);
    char path[160];
    TelemetryStats all{};frameTelemetryStats(zone,-1,all,apartPhases);
    std::snprintf(path,sizeof(path),"%s/report.txt",tour.zoneDir);
    if(auto* file=std::fopen(path,"wb")){
        std::fprintf(file,"zone=%u\nstage=%s\nlabel=%s\nroom=%d\npoint=%d\nform=%s\nstatus=%s\nfinal_stage=%.8s\nfinal_room=%d\nfinal_layer=%d\n"
            "load_seconds=%.2f\nplayer_position=%.1f %.1f %.1f\nrejected_draws_in_zone=%llu\ndegraded_draws_in_zone=%llu\n",
            unsigned(zone),z.stage,z.label,int(r.room),int(r.point),e.form?"wolf":"human",zoneStatus(status),status.stage,status.room,status.layer,
            double(tour.loadTicks)/SYSCLOCK_ARM11,status.position[0],status.position[1],status.position[2],
            static_cast<unsigned long long>(status.rejected-tour.rejected0),static_cast<unsigned long long>(status.degraded-tour.degraded0));
        std::fprintf(file,"state_seed=%08lx\nloadout=%s\n",static_cast<unsigned long>(randomLoadout()?zoneSeed(tour.index):0),
            hooks.loadout?hooks.loadout():"unknown");
        // Why a zone ended "died" or "moved", without the disc or a dump.
        std::fprintf(file,"life_at_end=%d\ndied_in_phase=%s\nscene_change_in_zone=%s\n",status.life,tour.died?phases[tour.diedPhase].name:"none",
            tour.leftBy[0]?tour.leftBy:"none");
        std::fprintf(file,"menu_pictures_open=%u\nmenu_phase_pictures=%u\n",tour.menuPictures,menuPhaseOn?phaseFrames(menuPhase):0u);
        // The section keeps its Alpha 6 name: the menu phase is left out of it too.
        statsLine(file,"all phases except plain and profile",all);
        for(unsigned p=0;p<phaseCount;++p)if(p!=profilePhase){TelemetryStats part{};frameTelemetryStats(zone,int(p),part);statsLine(file,phases[p].name,part);}
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
    waitForGpuListCaptureWrite(); // Include this room's sidecar receipt even on a slow card.
    if(auto* file=std::fopen(path,"wb")){writeNativeLogSince(tour.logMark,file);std::fclose(file);}
    // The zone's last picture: after the profile phase (see dev_console.hpp).
    if(tour.mode!=modeEverything){
        std::snprintf(path,sizeof(path),"%s/shot2.bmp",tour.zoneDir);writeBmp(path,GFX_TOP,400);
        std::snprintf(path,sizeof(path),"%s/bottom.bmp",tour.zoneDir);writeBmp(path,GFX_BOTTOM,320);
    }
    std::snprintf(path,sizeof(path),"%s/summary.csv",tour.dir);
    if(auto* file=std::fopen(path,"ab")){
        std::fprintf(file,"%u,%s,%d,%d,%s,%s,%.1f,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%lu,%lu,%lu,%llu,%llu,%lu,%lu,%lu",
            unsigned(zone),z.stage,int(r.room),int(r.point),e.form?"wolf":"human",zoneStatus(status),double(tour.loadTicks)/SYSCLOCK_ARM11,
            static_cast<unsigned long>(all.frames),all.loopMean,all.loopP50,all.loopP95,all.loopP99,all.loopMax,
            all.loopMean>0?1000.0f/all.loopMean:0.0f,all.execMean,all.execP95,all.paint,all.update,all.draw,all.gpuWait,all.gpuTail,all.audio,
            all.draws,static_cast<unsigned long>(all.over34),static_cast<unsigned long>(all.over40),static_cast<unsigned long>(all.over50),
            static_cast<unsigned long long>(status.rejected-tour.rejected0),static_cast<unsigned long long>(status.degraded-tour.degraded0),
            static_cast<unsigned long>(status.linearFree/1024),static_cast<unsigned long>(status.gameHeapFree/1024),
            static_cast<unsigned long>(status.textureBytes/1024));
        for(unsigned p=0;p<phaseCount;++p)if(p!=profilePhase){TelemetryStats part{};frameTelemetryStats(zone,int(p),part);std::fprintf(file,",%.2f",part.loopMean);}
        std::fprintf(file,",%.2f,%.2f",all.gpuBusy,all.gpuIdle);
        std::fputc('\n',file);std::fclose(file);
    }
    if(tour.invalidPosition || status.rejected>tour.rejected0 || tour.died)++tour.failures;
    ++tour.finished;
}
const char* tourStateName();
// The game's first own scene change request of the zone goes to report.txt
// (scene_change_in_zone): a "moved" zone then names its exit and caller.
void noteSceneChange(const DevGameStatus& status){
    if(tour.leftBy[0]||status.sceneChanges==tour.sceneChanges0)return;
    std::snprintf(tour.leftBy,sizeof(tour.leftBy),"%s",status.sceneChange);
    OSReport("Native test zone %u: the game requested a scene change (%s)\n",tour.index+1,tourStateName());
}
void setStoryPhase(story::Phase phase){
    tour.storyRun.phase=phase;
    setFrameTelemetryTag(std::uint16_t(tour.index+1),phase);
    OSReport("Native story step %u %s phase=%s tick=%u\n",tour.index+1,story::steps[tour.index].id,
        story::phaseNames[phase],tour.phaseFrame);
    dirty=true;
}

void appendStoryResult(unsigned index,const char* result,const DevGameStatus& status){
    const auto& step=story::steps[index];
    const auto& run=tour.storyRun;
    const auto& change=run.change;
    const auto& chapter=run.chapterEvidence;
    char path[128];std::snprintf(path,sizeof(path),"%s/story.csv",tour.dir);
    if(auto* file=std::fopen(path,"ab")){
        std::fprintf(file,"%u,%s,%s,%u,%u,%u,%u,%.1f,%u,%u,%u,%u,%u,%u,%u,%u,%d,%llu,%s,%d,%s,%d,%d,%d,%d,%s,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%d,%d,%d,%s,%u,%d,%d,%u,%u\n",
            index+1,step.id,result,story::requirements(step),run.facts,tour.phaseFrame,run.rendered,run.furthest,
            run.cutsceneFirst,run.cutsceneLast,run.enemyPeak,run.damageSamples,run.midnaPictures,run.lanternPictures,
            run.attackPictures,unsigned(run.portalActivated),status.life,
            static_cast<unsigned long long>((run.facts&story::origin)?status.rejected-tour.rejected0:0),
            change.fromStage,change.fromRoom,change.toStage,change.toRoom,change.point,change.layer,change.exit,
            status.stage,status.room,status.point,status.layer,unsigned(run.startScene),unsigned(status.playSceneId),run.destinationFrames,
            chapter.abilityTicks,chapter.demoTicks,chapter.timelineTicks,chapter.hits,unsigned(chapter.finishingBlow),unsigned(chapter.duelEnding),
            run.originStage,run.originRoom,run.originPoint,run.originLayer,run.originWolf?"wolf":"human",unsigned(chapter.demoEnded),
            change.warpPortal,change.warpSpawn,unsigned(run.warpProof.confirmed),run.warpProof.departurePictures);
        std::fclose(file);
    }
    OSReport("Native story result %u %s %s: facts=%03x required=%03x ticks=%u pictures=%u enemies=%u damage=%u\n",
        index+1,step.id,result,run.facts,story::requirements(step),tour.phaseFrame,run.rendered,run.enemyPeak,run.damageSamples);
}

// A slow view can share its regular dump bucket with a fast room visited
// earlier. Capture the active view while its PICA job buffers still describe it.
void captureSlowRoom(const DevGameStatus& status){
    const unsigned zone=tour.index+1;
    if(tour.mode!=modeEverything || tour.fullDumps || !hooks.fullDump ||
       tour.slowRoomDumps>=8 || tour.lastSlowRoom==zone || tour.moved || tour.died ||
       !status.playScene || status.stageChanging || status.room!=zoneRooms[current().room].room)return;
    TelemetryStats timing{};
    flagNextFrameTelemetry(telemetryDiagnosticFrame);
    constexpr std::uint8_t captureFlags=telemetryDiagnosticFrame|telemetryCaptureOwner|telemetryCaptureWriter|telemetryFullDump;
    const char* window="still";
    if(!frameTelemetryStats(std::uint16_t(zone),stillPhase,timing,0,captureFlags)||timing.frames<12){
        // An asynchronous screenshot can overlap the entire still phase on a
        // slow card. Use uncontaminated ordinary play from this room instead.
        constexpr std::uint32_t afterStill=~((1u<<(stillPhase+1))-1u);
        if(!frameTelemetryStats(std::uint16_t(zone),-1,timing,afterStill,captureFlags)||timing.frames<12)return;
        window="play";
    }
    if(timing.loopP50<100.0f && timing.gpuBusy<80.0f)return;
    const unsigned previousZone=tour.lastSlowRoom;
    ++tour.slowRoomDumps;tour.lastSlowRoom=zone;
    if(!writeState()){
        --tour.slowRoomDumps;tour.lastSlowRoom=previousZone;
        return;
    }
    OSReport("Native slow-room capture zone=%u %s median=%.2f ms GPU busy=%.2f ms pictures=%u\n",
        zone,window,timing.loopP50,timing.gpuBusy,timing.frames);
    hooks.fullDump(false);
}

bool requestZoneFullDump(bool failed){
    if(!hooks.fullDump)return false;
    if(!tour.fullDumps){
        if(tour.mode!=modeEverything||tour.lastSlowRoom==tour.index+1)return false;
        TelemetryStats timing{};
        frameTelemetryStats(std::uint16_t(tour.index+1),-1,timing);
        if(!failed&&timing.loopP95<=50.0f)return false;
        // One automatic RAM/VRAM snapshot per sixteenth of the itinerary.
        // Save the budget before requesting the dump so a reset cannot refill it.
        const unsigned bucket=1u<<(16u*tour.index/entryCount);
        if(tour.forensicBuckets&bucket)return false;
        tour.forensicBuckets|=bucket;
        if(storyZone(tour.index))writeState();
    }
    // With the room result already written, a stop in the dump resumes at
    // its successor and leaves the result and dump evidence intact.
    if(tour.mode==modeEverything&&!storyZone(tour.index))writeState(afterRoomResult);
    hooks.fullDump(false);
    tour.state=tourDump;tour.waitFrames=0;
    return true;
}

void captureTourEvidence(TourVisualReason reason){
    if(tour.mode==modeEverything){
        if(hooks.tourVisualCapture)hooks.tourVisualCapture(reason);
    }else if(hooks.benchmarkCapture){
        hooks.benchmarkCapture();
    }
}

void captureScheduledTourPicture(const DevGameStatus& status){
    if(tour.mode!=modeEverything||!hooks.tourVisualCapture)return;
    const auto reason=tour.visualSchedule.sample(status.submitTick,status.draws&&!status.submittedTransition);
    if(reason!=TourVisualReason::none)hooks.tourVisualCapture(reason);
}

bool finishStoryStep(const DevGameStatus& status,const char* failure=nullptr){
    const auto& step=story::steps[tour.index];
    captureTourEvidence(TourVisualReason::exit);
    const auto rejected=(tour.storyRun.facts&story::origin)?status.rejected-tour.rejected0:0;
    const char* result=failure?failure:story::outcome(step,tour.storyRun.facts,status.life,rejected);
    setStoryPhase(story::evidence);
    appendStoryResult(tour.index,result,status);
    summaryFailure(tour.index,result);
    ++tour.finished;
    if(std::strcmp(result,"pass"))++tour.failures;

    std::snprintf(tour.zoneDir,sizeof(tour.zoneDir),"%s/%03u-%s",tour.dir,tour.index+1,step.id);
    if(directory(tour.zoneDir)){
        writeZoneMeasurements(tour.zoneDir);
        const auto start = svcGetSystemTick();
        char path[160];std::snprintf(path,sizeof(path),"%s/frames.csv",tour.zoneDir);
        const auto zone = std::uint16_t(tour.index + 1);
        const char* csvStatus = "failed";
        if (queueZoneFrameCsv(path,zone)) {
            if (kickNativeSdWriterAsync()) csvStatus = "queued";
            else if (writePendingZoneFrameCsv()) csvStatus = "written";
        } else if (auto* file=std::fopen(path,"wb")) {
            const bool written = writeFrameTelemetryZone(file,zone);
            if (std::fclose(file)==0 && written) csvStatus = "written";
        }
        OSReport("Native story frame CSV zone=%u status=%s render-thread=%.3f ms\n",tour.index+1,
            csvStatus,(svcGetSystemTick()-start)*1000.0/SYSCLOCK_ARM11);
    }
    if(!requestZoneFullDump(std::strcmp(result,"pass")!=0))nextZone();
    return true;
}

bool stepStory(const DevGameStatus& status){
    const auto& step=story::steps[tour.index];
    auto& run=tour.storyRun;
    const story::Place place{status.stage,status.room,status.point,status.layer};
    const bool ready=status.playScene&&status.playerReady&&!status.stageChanging&&!status.transitionHeld;
    const bool playerWarp=step.script==story::Script::warp;

    if(tour.state==tourWarp){
        if(tour.bootWarp||(hooks.warp&&hooks.warp(step.start.stage,step.start.room,step.start.point,step.start.layer,step.wolf,true))){
            tour.bootWarp=false;tour.state=tourLoad;tour.waitFrames=tour.phaseFrame=tour.stableFrames=0;
            tour.sceneChanges0=status.sceneChanges;
            setStoryPhase(story::loading);
        }else if(++tour.waitFrames>900)restartAfterStuckZone("warp-refused");
        return false;
    }
    if(tour.state==tourDump){
        if(++tour.waitFrames>=3&&!(hooks.fullDumpBusy&&hooks.fullDumpBusy()))nextZone();
        return false;
    }
    if(tour.state==tourLoad){
        ++tour.waitFrames;
        if(ready&&story::matches(step.start,place)&&status.wolf==step.wolf){
            run.facts=story::origin;
            run.startScene=status.playSceneId;
            std::snprintf(run.originStage,sizeof(run.originStage),"%.8s",status.stage);
            run.originRoom=status.room;run.originPoint=status.point;run.originLayer=status.layer;
            run.originWolf=status.wolf;
            run.startX=status.position[0];run.startZ=status.position[2];
            tour.rejected0=status.rejected;
            run.portalInitiallyOpen=status.portalOpen;
            tour.state=tourRun;
            setStoryPhase(story::play);
            tour.visualSchedule.enter();
            captureScheduledTourPicture(status);
        }else if(ready&&++tour.stableFrames>=45){
            return finishStoryStep(status,"checkpoint-mismatch");
        }else if(seconds(svcGetSystemTick()-tour.zoneTick)>=150){
            restartAfterStuckZone("load-timeout");
        }else if(!ready)tour.stableFrames=0;
        return false;
    }

    captureScheduledTourPicture(status);
    if(ready && !validPlayerPosition(status))return finishStoryStep(status,"invalid-player-position");
    tour.phaseFrame+=status.ticks;
    if (status.chapterScene && status.chapterScene == run.startScene) {
        run.chapterEvidence = status.chapterEvidence;
        if (run.chapterEvidence.witnessed(step.chapter.goal)) run.facts |= story::chapterAction;
    }
    if(status.sceneChanges!=tour.sceneChanges0){
        tour.sceneChanges0=status.sceneChanges;
        run.change=status.storyChange;
        if(run.phase==story::arrival || !story::matchesExit(step,run.change) ||
            (playerWarp && !run.warpProof.witnessed()))
            return finishStoryStep(status,"unexpected-transition");
        run.facts|=story::requested;
        setStoryPhase(story::arrival);
        tour.waitFrames=0;
    }
    if(status.life==0)return finishStoryStep(status);
    if(ready&&status.draws>0&&((step.chapter.goal==story::ChapterGoal::legacy&&!playerWarp)||
        (run.phase==story::play&&run.startScene==status.playSceneId&&(!playerWarp||!status.menuOpen)))){
        if(++run.rendered>=3)run.facts|=story::rendered;
    }
    if(run.phase==story::arrival){
        if(ready&&story::matchesArrival(step,place,run.startScene,status.playSceneId)){
            if(status.draws)++run.destinationFrames;
            if(run.destinationFrames>=3&&(!playerWarp||(!status.eventRunning&&status.portalOpen))){
                run.facts|=story::arrived;
                return finishStoryStep(status);
            }
        }else run.destinationFrames=0;
        if(++tour.waitFrames>=480)return finishStoryStep(status,"arrival-timeout");
        return false;
    }
    if(!ready)return tour.phaseFrame>=step.ticks?finishStoryStep(status,"play-timeout"):false;
    const bool inIntroEndRoom = story::inIntroEndRoom(step,place) &&
        run.startScene == status.playSceneId && run.chapterEvidence.demoTicks >= 7;
    if(!story::sameArea(step.start,place) && !inIntroEndRoom)return finishStoryStep(status,"unexpected-room");
    const float distance=std::hypot(status.position[0]-run.startX,status.position[2]-run.startZ);
    if(!status.eventRunning){
        run.playableTicks+=status.ticks;
        if(std::isfinite(distance))run.furthest=std::max(run.furthest,distance);
        if(step.via && std::hypot(status.position[0]-step.via->x,status.position[2]-step.via->z)<42.0f)
            run.passedWaypoint=true;
    }
    if(run.furthest>=80.0f)run.facts|=story::moved;
    if(status.cutsceneCamera&&(step.script!=story::Script::portal||status.draws)){
        if(!run.cutsceneSeen){run.cutsceneSeen=true;run.cutsceneFirst=status.cutsceneFrame;}
        run.cutsceneLast=status.cutsceneFrame;
        if(run.cutsceneLast>run.cutsceneFirst+6 && !(run.facts&story::cutscene)){
            run.facts|=story::cutscene;
            if(status.draws&&(step.script==story::Script::cutscene||step.script==story::Script::portal))
                captureTourEvidence(TourVisualReason::evidence);
        }
    }
    if(status.lanternLit){++run.lanternPictures;run.facts|=story::equipment;}
    if(status.attacked){++run.attackPictures;run.facts|=story::attacked;}
    if(status.midnaVisible){++run.midnaPictures;run.facts|=story::midna;}
    run.enemyPeak=std::max(run.enemyPeak,status.enemyCount);
    if(run.enemyPeak>=2)run.facts|=story::enemies;
    run.damageSamples+=status.damagedEnemies;
    if(run.damageSamples)run.facts|=story::damage;
    if(status.portalOpen&&!run.portalInitiallyOpen){run.portalActivated=true;run.facts|=story::portal;}
    if(playerWarp){
        if(status.playSceneId==run.startScene){
            auto sample=status.portalWarp;
            sample.departing &= status.draws!=0;
            run.warpProof.record(sample);
            if(run.warpProof.witnessed()&&!(run.facts&story::playerWarp)){
                run.facts|=story::playerWarp;
                captureTourEvidence(TourVisualReason::evidence);
            }
        }
        return tour.phaseFrame>=step.ticks?finishStoryStep(status):false;
    }
    if(step.script==story::Script::portal&&!status.eventRunning&&
        (run.facts&story::requirements(step))==story::requirements(step))
        return finishStoryStep(status);
    const bool timedDemo = step.chapter.goal==story::ChapterGoal::event ||
                           step.chapter.goal==story::ChapterGoal::bossIntro;
    if(timedDemo && !run.chapterEvidence.demoEnded){
        // The normal tick budget bounds the search for the authored event.
        // Once it has started, render until it ends (or its authored portal).
        if(run.chapterEvidence.demoTicks && tour.phaseFrame < std::max(step.ticks,7200u))return false;
        if(tour.phaseFrame >= step.ticks)return finishStoryStep(status,"cutscene-incomplete");
    }
    if(step.chapter.introEndRoom >= 0){
        if(inIntroEndRoom && run.chapterEvidence.demoEnded && !status.eventRunning && status.draws)
            ++run.destinationFrames;
        else run.destinationFrames=0;
        if(run.destinationFrames < 3){
            if(tour.phaseFrame >= std::max(step.ticks,7200u))return finishStoryStep(status,"arrival-timeout");
            return false;
        }
    }
    if (step.chapter.goal >= story::ChapterGoal::ability && !step.destination.stage &&
        (run.facts & story::requirements(step)) == story::requirements(step))
        return finishStoryStep(status);
    // Count original interactive ticks for controls. An arrival conversation
    // can own Link for longer than the entire planned walk or item sequence.
    const bool interactive=step.script!=story::Script::cutscene && !timedDemo && step.chapter.goal!=story::ChapterGoal::duel;
    if(interactive && run.playableTicks<step.ticks && tour.phaseFrame<step.ticks+1800)return false;
    return tour.phaseFrame>=step.ticks?finishStoryStep(status):false;
}

// ---- measurements of a zone ----
// Each zone of the test is measured several times while it plays
// (measured_dump.hpp): where each process and each owner's draws spend the
// picture, at different moments of the same zone. The first measurement of a
// zone carries the named sections; then two groups of three: an ordinary
// measurement, one with improvements of perf_features.hpp off, an ordinary
// one, starting right after each other: the view moves during a test, and the
// mean of the two neighbours cancels a steady change. The first group turns
// every improvement off (the reference: what they are worth together in this
// zone), the second a single one, the next in every zone the test enters
// (what each is worth by itself, over the zones it was left out in). Only the phases that count
// in a zone's figures are measured (not onecore, plain, profile or menu), and
// only when the whole measurement fits before them. The measurements wait in
// RAM (an SD write inside the zone would land in its statistics) and are
// written to the zone's measure.txt where the zone writes its own results:
// a room zone has its folder from its arrival, a story step only once it
// ends, so the folder is the one the zone names there, never a remembered one.
constexpr unsigned measureFirstPictures=20,measureEveryPictures=40,measurePairPictures=2,measureMostPerZone=7;
struct ZoneMeasure {unsigned zone{},taken{},waited{},zones{};} zoneMeasure; // zone: tour.index+1 of the measurements taken
// The zone writes its results into `dir`: its measurements with them. One
// still running is dropped: kept later, it would be read as the next zone's.
bool writeZoneMeasurements(const char* dir){
    measured::abandon();
    char path[176];std::snprintf(path,sizeof(path),"%s/measure.txt",dir);
    return measured::flush(path);
}
// What a zone left that never wrote its results (the test stopped by hand in
// it, a zone that did not load): kept apart, its labels say where it was.
bool writeUnfinishedMeasurements(){
    measured::abandon();
    zoneMeasure.zone=0; // a test started again counts its first zone from the start
    char path[112];std::snprintf(path,sizeof(path),"%s/measure-unfinished.txt",tour.dir);
    return measured::flush(path);
}
// Pictures left of the phases that count, for a room zone (a story step is
// one stretch of play: no limit).
unsigned measurablePicturesLeft(){
    if(storyZone(tour.index))return ~0u;
    if(tour.phase>stillPhase)return 0;
    unsigned left=phaseFrames(tour.phase)-std::min(phaseFrames(tour.phase),tour.phaseFrame);
    for(unsigned phase=tour.phase+1;phase<=stillPhase;++phase)left+=phaseFrames(phase);
    return left;
}
// Once per completed frame while a test runs. Returns true after SD work.
bool measureZone(const DevGameStatus& status){
    if(zoneMeasure.zone!=tour.index+1){
        // The test is in another zone. The last one wrote its measurements with
        // its results; anything still waiting is of a zone that never did.
        const bool wrote=writeUnfinishedMeasurements();
        zoneMeasure.zone=tour.index+1;zoneMeasure.taken=zoneMeasure.waited=0;++zoneMeasure.zones;
        return wrote;
    }
    const bool playing=tour.state==tourRun&&status.playScene&&status.playerReady&&!status.stageChanging&&!status.transitionHeld&&!tour.died;
    if(!playing||measured::active()||zoneMeasure.taken>=measureMostPerZone)return false;
    // Measurements 1 (with the sections), then 2 3 4: ordinary, reference, ordinary,
    // and 5 6 7: ordinary, without one improvement, ordinary.
    const unsigned place=zoneMeasure.taken%3;
    const bool changed=place==2,afterChanged=place==0&&zoneMeasure.taken;
    const unsigned wait=!zoneMeasure.taken?measureFirstPictures:changed||afterChanged?measurePairPictures:measureEveryPictures;
    if(++zoneMeasure.waited<wait)return false;
    // Nothing chosen to leave out: the second group repeats the reference.
    const unsigned one=changed&&zoneMeasure.taken>3?features::chosenOne(zoneMeasure.zones):0;
    const measured::Plan plan{8,zoneMeasure.taken?0u:6u,2,changed&&!one,one};
    // The first of a group of three needs room for the whole group.
    const unsigned own=measured::picturesOf(plan);
    const unsigned needed=place==1?2*own+2*measurePairPictures+measured::picturesOf({8,0,2,true}):own;
    if(measurablePicturesLeft()<needed)return false;
    const auto context=devConsoleZoneCaptureContext();
    char label[160];
    std::snprintf(label,sizeof(label),"zone=%u measurement=%u stage=%.8s room=%d phase=%s picture=%u story=%s",context.loadout.zone,
        zoneMeasure.taken+1,status.stage,status.room,context.phase?context.phase:"-",context.phasePicture,
        context.story.step?context.story.step:"-");
    if(measured::request(plan,label)){++zoneMeasure.taken;zoneMeasure.waited=0;}
    return false;
}

// One step per completed frame. Returns true after SD work.
bool stepTour(const DevGameStatus& status){
    if(tour.state==tourIdle)return false;
    const bool measuredWrote=measureZone(status);
    if(storyZone(tour.index))return stepStory(status)||measuredWrote;
    setPlain(tour.state==tourRun&&tour.phase==plainPhase);
    setSecondCore(tour.state!=tourRun?SecondCoreTest::play:tour.phase==oneCorePhase?SecondCoreTest::off:
                  tour.phase==stillPhase?SecondCoreTest::measured:SecondCoreTest::play);
    const auto& e=current();const auto& z=zoneStages[e.stage];const auto& r=zoneRooms[e.room];
    switch(tour.state){
    case tourWarp:
        if(tour.bootWarp||(hooks.warp&&hooks.warp(z.stage,r.room,r.point,-1,e.form,true))){
            tour.bootWarp=false;tour.state=tourLoad;tour.waitFrames=tour.stableFrames=0;
            // From here a request by the game itself is the zone's: a start
            // point can hand Link to another stage during the load (zone 92).
            tour.sceneChanges0=status.sceneChanges;tour.leftBy[0]=0;
        }else if(++tour.waitFrames>900)restartAfterStuckZone("warp-refused");
        return false;
    case tourLoad:{
        ++tour.waitFrames;
        noteSceneChange(status);
        // Some start points hand the player straight to another stage: after
        // a while a running scene elsewhere counts as arrival (reported "moved").
        const bool there=!std::strncmp(status.stage,z.stage,8);
        const bool arrived=status.playScene&&status.playerReady&&!status.stageChanging&&!status.transitionHeld&&
            (there?status.room==int(r.room)||tour.waitFrames>300:tour.waitFrames>450);
        tour.stableFrames=arrived&&tour.waitFrames>=20?tour.stableFrames+1:0;
        if(tour.stableFrames>=20){
            tour.loadTicks=svcGetSystemTick()-tour.zoneTick;tour.state=tourSettle;tour.waitFrames=0;
        }else if(seconds(svcGetSystemTick()-tour.zoneTick)>=150)restartAfterStuckZone("load-timeout");
        return false;}
    case tourSettle:
        if(++tour.waitFrames<std::max(15u,45u*unsigned(tour.scale)/100u))return false;
        roomDirectory(tour.zoneDir,sizeof(tour.zoneDir));
        directory(tour.zoneDir);
        tour.phase=tour.phaseFrame=0;tour.moved=tour.died=tour.invalidPosition=false;tour.diedPhase=0;tour.menuPictures=0;
        tour.rejected0=status.rejected;tour.degraded0=status.degraded;tour.logMark=nativeLogPosition();
        if(hooks.refillLife)hooks.refillLife();
        setFrameTelemetryTag(std::uint16_t(tour.index+1),0);flagNextFrameTelemetry(telemetryDiagnosticFrame);
        tour.state=tourRun;dirty=true;
        tour.visualSchedule.enter();
        captureScheduledTourPicture(status);
        return true;
    case tourRun:{
        captureScheduledTourPicture(status);
        bool wrote=false;
        if(status.playerReady && !validPlayerPosition(status))tour.invalidPosition=true;
        // The game itself may leave the zone (story events on the empty save).
        // Loading frames are not zone performance: flag them and record the move.
        if(std::strncmp(status.stage,z.stage,8))tour.moved=true;
        noteSceneChange(status);
        if(!status.playScene||status.stageChanging)flagNextFrameTelemetry(telemetryDiagnosticFrame);
        // Death: the rest of the zone measures the Game Over sequence, not the
        // zone (Alpha 6: 12 of 93 zones). Its frames leave the statistics.
        if(!tour.died&&status.playerReady&&status.life==0){
            tour.died=true;tour.diedPhase=tour.phase;
            OSReport("Native test zone %u: Link died in the %s phase; status died, later frames left out of the statistics\n",
                tour.index+1,phases[tour.phase].name);
        }
        if(tour.died)flagNextFrameTelemetry(telemetryDiagnosticFrame);
        if(tour.phase==menuPhase&&menuPhaseOn){
            if(status.menuOpen)++tour.menuPictures;
            // shot3: the menu, open (45 % of the phase; it closes from 70 %).
            if(tour.phaseFrame==phaseFrames(menuPhase)*45/100){
                if(tour.mode==modeEverything)captureTourEvidence(TourVisualReason::evidence);
                else{
                    char path[160];std::snprintf(path,sizeof(path),"%s/shot3.bmp",tour.zoneDir);
                    writeBmp(path,GFX_TOP,400);wrote=true;flagNextFrameTelemetry(telemetryDiagnosticFrame);
                }
            }
        }
        if(tour.mode==modeEverything && (tour.phase==1||tour.phase==stillPhase) &&
           tour.phaseFrame==phaseFrames(tour.phase)/2 && !tour.moved && !tour.died &&
           status.playScene && !status.stageChanging && status.room==int(r.room)){
            // The completed GSP ring retains timings after the game recycles
            // command buffers. Save the words as well for these two views.
            requestGpuListCapture(tour.zoneDir,phases[tour.phase].name,z.stage,r.room);
        }
        if(tour.phase==stillPhase && tour.phaseFrame==phaseFrames(stillPhase)*3/4)
            captureSlowRoom(status);
        // Development (SD switch gpu-trace.txt): the draws of one picture of
        // the still phase, line by line (gpu_canary.hpp). The game frame is the
        // same in every run of a zone, so two runs can be compared draw by draw.
        // The switch holds `e` for one of the zone's last pictures instead
        // (three before the one shot2.bmp shows).
        {
            static int wanted=-1;
            if(wanted<0){
                char text[4]{};auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-trace.txt","rb");
                if(file){(void)std::fread(text,1,sizeof(text)-1,file);std::fclose(file);}
                wanted=!file?0:text[0]=='e'?2:1;
            }
            if((wanted==1&&tour.phase==6&&tour.phaseFrame==phaseFrames(6)/2)||
               (wanted==2&&tour.phase==phaseCount-1&&tour.phaseFrame+5==phaseFrames(phaseCount-1)))gpuCanaryTraceNextPicture();
        }
        if(tour.phaseFrame+1>=phaseFrames(tour.phase)&&tour.phase+1==phaseCount)
            captureTourEvidence(TourVisualReason::exit);
        if(++tour.phaseFrame>=phaseFrames(tour.phase)){
            if(tour.phase==1){ // shot1: the view after the camera sweep
                if(tour.mode==modeEverything)captureTourEvidence(TourVisualReason::evidence);
                else{
                    char path[160];std::snprintf(path,sizeof(path),"%s/shot1.bmp",tour.zoneDir);
                    writeBmp(path,GFX_TOP,400);wrote=true;flagNextFrameTelemetry(telemetryDiagnosticFrame);
                }
            }
            tour.phaseFrame=0;
            if(++tour.phase==phaseCount){
                setFrameTelemetryTag(0,0);finishZone(status);
                if(requestZoneFullDump(std::strcmp(zoneStatus(status),"ok")!=0))return true;
                nextZone();return true;
            }
            // Every phase starts with the zone's starting life (never after a death).
            if(!tour.died&&hooks.refillLife)hooks.refillLife();
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
// TEST page modes, start/stop, then the dump buttons.
constexpr Button testButtons[]={{5,1,2,39},{7,1,2,39},{9,1,2,39},{11,1,2,39},{13,1,2,39},{15,1,2,39},{17,1,2,39},{19,1,2,19}};
constexpr Button dumpButton{19,21,2,19};
constexpr int modeOrder[]{modeEverything,modeStory,modeRandom,modeMain,modeStages,modeRooms};
// STATS page: A/B switch of the rigid shader route for the next start (SD file
// rigid-single-on.txt, see game_startup). The single-texture shader stopped
// the console PICA until 2026-11-14; if its repair does not hold, the GPU
// guard learns it on the first stop and this button reports it. Both routes
// draw the same picture at the same speed in the emulator.
constexpr Button fastButton{25,1,3,39};
// The improvements of perf_features.hpp as chosen, or all off (the reference),
// from the next picture on: for comparing the same view with and without them.
constexpr Button improvementsButton{24,1,1,39}; // one row: the message line owns the last rows
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
    if(storyZone(tour.index)&&tour.state!=tourIdle)return story::phaseNames[tour.storyRun.phase];
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
    button(improvementsButton,features::manualReference?"IMPROVEMENTS OFF: reference (touch=on)":"IMPROVEMENTS: as chosen (touch = off)",
        features::manualReference?2:0);
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
    static const char* const names[modeTotal]={"MAIN ZONES","ALL STAGES","ALL ROOMS","RANDOM","STORY","TEST EVERYTHING"};
    for(int at=0;at<modeTotal;++at){
        const int mode=modeOrder[at];
        std::snprintf(label,sizeof(label),"%s %s (%u zones)",tour.mode==mode?(running?"RUNNING:":"SELECTED:"):"SELECT:",names[mode],modeCount(mode));
        button(testButtons[at],label,tour.mode==mode?2:0);
    }
    button(testButtons[modeTotal],running?"STOP TEST":"START TEST  (or START+SELECT)",running?1:0);
    button(testButtons[modeTotal+1],tour.fullDumps?"RAM+VRAM/ZONE: ON":"RAM+VRAM/ZONE: OFF",tour.fullDumps?2:0);
    button(dumpButton,"FULL DUMP NOW",0);
    if(running){
        const auto elapsed=elapsedSeconds();
        if(storyZone(tour.index)){
            const auto& step=story::steps[tour.index];
            std::printf("\x1b[22;1H" C_HDR " Story %u/%u " C_VAL "%s r%d" C_RST "\x1b[K",
                tour.index+1,lastZone(),step.start.stage,step.start.room);
            std::printf("\x1b[23;1H" C_DIM " %-26.26s " C_VAL "%s" C_RST "\x1b[K",step.id,tourStateName());
        }else{
            const auto& e=current();const auto& z=zoneStages[e.stage];
            std::printf("\x1b[22;1H" C_HDR " Zone " C_VAL "%u/%u " C_VAL "%s" C_DIM " r%d %s" C_RST "\x1b[K",
                tour.index+1,lastZone(),z.stage,int(zoneRooms[e.room].room),e.form?"wolf":"human");
            std::printf("\x1b[23;1H" C_DIM " %-22.22s " C_VAL "%s" C_RST "\x1b[K",z.label,tourStateName());
        }
        std::printf("\x1b[24;1H" C_DIM " Elapsed " C_VAL "%llu:%02llu" C_DIM "  failed " C_VAL "%u" C_RST "\x1b[K",
            static_cast<unsigned long long>(elapsed/60),static_cast<unsigned long long>(elapsed%60),tour.failures);
        std::printf("\x1b[25;1H" C_DIM " %s  seed " C_VAL "%08lx" C_RST "\x1b[K",tour.dir+std::strlen(dataRoot)+1,static_cast<unsigned long>(tour.seed));
        std::printf("\x1b[26;1H" C_DIM " A" C_VAL " Save diagnostic" C_DIM " (test continues)" C_RST "\x1b[K");
    }else{
        std::printf("\x1b[22;1H" C_DIM " EVERYTHING: story, then all rooms." C_RST "\x1b[K");
        std::printf("\x1b[23;1H" C_DIM " Reports and frame times are saved in" C_RST "\x1b[K");
        std::printf("\x1b[24;1H" C_VAL " /3ds/DuskLight/tour/NNN/" C_RST "\x1b[K");
        if(tour.mode==modeEverything){
            std::printf("\x1b[25;1H" C_DIM " Reopen to retry an interrupted room." C_RST "\x1b[K");
            std::printf("\x1b[26;1H" C_DIM " A second interruption advances it." C_RST "\x1b[K");
        }else{
            std::printf("\x1b[25;1H" C_DIM " Reopen after an interruption to" C_RST "\x1b[K");
            std::printf("\x1b[26;1H" C_DIM " continue at the next zone." C_RST "\x1b[K");
        }
    }
}
// The finished screen: nothing else on the bottom screen.
void drawFinished(bool clear){
    if(clear)std::printf("\x1b[2J");
    const auto now=svcGetSystemTick();
    const unsigned left=now<finished.untilTick?unsigned((finished.untilTick-now)/SYSCLOCK_ARM11):0;
    std::printf("\x1b[1;1H" C_CYN " DUSKLIGHT " DUSKLIGHT_RELEASE_UPPER C_RST "\x1b[K");
    std::printf("\x1b[8;1H" C_OK  "            TESTS FINISHED" C_RST "\x1b[K");
    std::printf("\x1b[11;1H" C_DIM "  Zones run   " C_VAL "%u" C_DIM "   failed " C_VAL "%u" C_RST "\x1b[K",finished.zones,finished.failures);
    std::printf("\x1b[12;1H" C_DIM "  Test time   " C_VAL "%llu:%02llu:%02llu" C_RST "\x1b[K",static_cast<unsigned long long>(finished.elapsed/3600),
        static_cast<unsigned long long>(finished.elapsed/60%60),static_cast<unsigned long long>(finished.elapsed%60));
    std::printf("\x1b[13;1H" C_DIM "  Result      " C_VAL "%-24.24s" C_RST "\x1b[K",finished.result);
    std::printf("\x1b[15;1H" C_DIM "  Files: " C_VAL "%-29.29s" C_RST "\x1b[K",finished.dir+5);
    std::printf("\x1b[19;1H" C_HDR "  The console powers off in " C_VAL "%2u:%02u" C_RST "\x1b[K",left/60,left%60);
    std::printf("\x1b[21;1H" C_CYN "  Press any button or touch the screen" C_RST "\x1b[K");
    std::printf("\x1b[22;1H" C_CYN "  to keep it on." C_RST "\x1b[K");
    std::fflush(stdout);
    if(auto* pixels=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr))GSPGPU_FlushDataCache(pixels,320*240*2);
}
void draw(bool clear){
    if(finished.active){drawFinished(clear);return;}
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
    if(page==pageStats&&hit(improvementsButton,row,col)){
        features::setManualReference(!features::manualReference);
        say(features::manualReference?"Reference pictures: improvements off":"Improvements as chosen");
        dirty=true;return;
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
        for(int at=0;at<modeTotal;++at)if(hit(testButtons[at],row,col)&&tour.state==tourIdle){tour.mode=modeOrder[at];dirty=true;}
        if(hit(testButtons[modeTotal],row,col)){if(tour.state==tourIdle)startTour();else endTour("stopped");dirty=true;}
        if(hit(testButtons[modeTotal+1],row,col)){tour.fullDumps=!tour.fullDumps;dirty=true;}
        if(hit(dumpButton,row,col)&&tour.state==tourIdle&&hooks.fullDump){hooks.fullDump(true);dirty=true;}
    }
}
unsigned number(const char* text,const char* key,unsigned fallback){
    const auto* found=std::strstr(text,key);
    return found?unsigned(std::strtoul(found+std::strlen(key),nullptr,10)):fallback;
}
int configuredMode(const char* text,int fallback){
    const auto* value=std::strstr(text,"mode=");
    if(value&&(value[5]<'0'||value[5]>'9')){
        value+=5;
        const std::string_view word(value,std::strcspn(value," \t\r\n"));
        for(int n=0;n<modeTotal;++n)if(word==modeNames[n])return n;
        return fallback;
    }
    return std::clamp(int(number(text,"mode=",fallback)),0,modeTotal-1);
}
std::uint32_t hexNumber(const char* text,const char* key,std::uint32_t fallback){
    const auto* found=std::strstr(text,key);
    return found?std::uint32_t(std::strtoul(found+std::strlen(key),nullptr,16)):fallback;
}
std::uint64_t number64(const char* text,const char* key,std::uint64_t fallback){
    const auto* found=std::strstr(text,key);
    return found?std::uint64_t(std::strtoull(found+std::strlen(key),nullptr,10)):fallback;
}
bool readText(const char* path,char* text,std::size_t capacity){
    auto* file=std::fopen(path,"rb");if(!file)return false;
    const auto count=std::fread(text,1,capacity-1,file);text[count]=0;std::fclose(file);return true;
}
// The second attempt uses the ordinary zone paths. Preserve the first
// attempt's files before reopening them, including an unsealed cadence trace.
bool archiveInterruptedRoom(){
    char archive[96];std::snprintf(archive,sizeof(archive),"%s/%03u-interrupted",tour.dir,tour.index+1);
    if(!directory(archive))return false;
    const auto move=[&](const char* source,const char* name){
        struct stat info{};
        if(stat(source,&info))return errno==ENOENT;
        char target[128];std::snprintf(target,sizeof(target),"%s/%s",archive,name);
        return stat(target,&info)!=0&&errno==ENOENT&&std::rename(source,target)==0;
    };
    char path[128];roomDirectory(path,sizeof(path));
    if(!move(path,"room"))return false;
    std::snprintf(path,sizeof(path),"%s/cadence-%03u.bin",tour.dir,tour.index+1);
    return move(path,"cadence.bin")&&move("sdmc:/3ds/DuskLight/log.txt","log.txt");
}
void resumeBootZone(){
    planLine();
    setFatalUnattended(true);tour.state=tourWarp;tour.bootWarp=true;tour.zoneTick=svcGetSystemTick();page=pageTest;
}
}
void initializeDevConsole(const DevGameHooks& game){
    hooks=game;
    if(betaScreenEnabled())return;
    char text[512];
    if(readText(statePath,text,sizeof(text))){
        // Resume the checkpoint with the same itinerary and seed.
        if(const auto* dir=std::strstr(text,"dir=")){
            unsigned n=0;for(dir+=4;*dir&&*dir!='\n'&&*dir!='\r'&&n<sizeof(tour.dir)-1;++dir)tour.dir[n++]=*dir;
            tour.dir[n]=0;
        }
        tour.mode=configuredMode(text,modeStages);tour.scale=std::clamp(int(number(text,"scale=",100)),10,400);
        tour.fullDumps=number(text,"full=",0)!=0;tour.autoExit=number(text,"autoexit=",0)!=0;tour.limit=number(text,"limit=",0);
        tour.seed=hexNumber(text,"seed=",0);
        tour.failures=number(text,"failures=",0);
        tour.forensicBuckets=number(text,"forensic_buckets=",0);
        tour.slowRoomDumps=number(text,"slow_room_dumps=",0);tour.lastSlowRoom=number(text,"last_slow_room=",0);
        holdSeconds=std::min(number(text,"hold=",defaultHoldSeconds),24u*60u*60u);menuPhaseOn=number(text,"menu=",1)!=0;coreProbeOn=number(text,"probe=",1)!=0;
        if(hasStory(tour.mode)){
            char path[128];std::snprintf(path,sizeof(path),"%s/tour.txt",tour.dir);
            char header[512];
            if(number(text,"story_version=",1)!=story::planVersion ||
               !readText(path,header,sizeof(header)) || !std::string_view(header).starts_with(tourBinaryHeader)){
                if(auto* file=std::fopen(path,"ab")){
                    std::fputs("result=interrupted by build or story route change\n",file);
                    std::fclose(file);
                }
                unlink(statePath);
                OSReport("Native test: build or route changed, previous results kept\n");
                return;
            }
        }
        const unsigned failed=number(text,"next=",0);
        buildEntries(tour.mode);
        tour.storyFirst=std::min(number(text,"story_first=",0),entryCount-1);
        const unsigned point=number(text,"resume=",inZone);
        if(tour.dir[0]&&failed<lastZone()&&point==afterRoomResult&&tour.mode==modeEverything&&!storyZone(failed)){
            tour.index=failed+1;tour.finished=number(text,"finished=",0);restoreElapsed(text);
            if(tour.index<lastZone()){resumeBootZone();writeState();}
            else endTour("complete after an interrupted dump");
        }else if(tour.dir[0]&&failed<lastZone()&&point==nextUntested){
            // Deliberate restart after a zone that never loaded: continue here.
            char saved[128];std::snprintf(saved,sizeof(saved),"%s/%03u-stuck-log.txt",tour.dir,failed);
            std::rename("sdmc:/3ds/DuskLight/log.txt",saved);
            tour.index=failed;tour.finished=number(text,"finished=",0);restoreElapsed(text);
            resumeBootZone();writeState();
        }else if(tour.dir[0]&&failed<lastZone()){
            if(tour.mode==modeEverything&&!storyZone(failed)&&!number(text,"room_retry=",0)){
                tour.index=failed;tour.finished=number(text,"finished=",0);restoreElapsed(text);
                if(archiveInterruptedRoom()){
                    tour.roomRetry=true;
                    if(writeState()){
                        OSReport("Native test: retrying interrupted room zone %u once\n",failed+1);
                        resumeBootZone();
                        return;
                    }
                    tour.roomRetry=false;
                }else OSReport("Native test: could not archive interrupted room zone %u\n",failed+1);
            }
            char saved[128];std::snprintf(saved,sizeof(saved),"%s/%03u-crash-log.txt",tour.dir,failed+1);
            std::rename("sdmc:/3ds/DuskLight/log.txt",saved);
            if(storyZone(failed))appendStoryResult(failed,"crash-or-reset",{});
            summaryFailure(failed,"crash-or-reset");++tour.failures;
            // finished= was written when the crashed zone began; earlier failed
            // zones are not finished ones (the old count assumed none).
            tour.index=failed+1;tour.finished=number(text,"finished=",failed);restoreElapsed(text);
            if(tour.index<lastZone()){
                resumeBootZone();writeState();
            }else{tour.state=tourWarp;endTour("complete after a crash");}
        }else unlink(statePath);
        return;
    }
    char path[96];std::snprintf(path,sizeof(path),"%s/tour-auto.txt",dataRoot);
    if(readText(path,text,sizeof(text))){
        // Unattended run (emulator automation): mode= scale= limit= first= exit= full=
        tour.mode=configuredMode(text,modeStory);tour.scale=std::clamp(int(number(text,"scale=",100)),10,400);
        tour.limit=number(text,"limit=",0);tour.autoExit=number(text,"exit=",1)!=0;tour.fullDumps=number(text,"full=",0)!=0;
        tour.seed=hexNumber(text,"seed=",0);holdSeconds=std::min(number(text,"hold=",defaultHoldSeconds),24u*60u*60u);
        menuPhaseOn=number(text,"menu=",1)!=0;coreProbeOn=number(text,"probe=",1)!=0;
        tour.storyFirst=number(text,"first=",0);
        if(createTour()){
            tour.index=tour.storyFirst;
            if(!hasStory(tour.mode))tour.finished=tour.index;
            planLine();
            setFatalUnattended(true);tour.state=tourWarp;tour.bootWarp=true;tour.zoneTick=svcGetSystemTick();page=pageTest;writeState();
        }
    }
}
DevZoneLoadout devConsoleZoneLoadout(){
    if(tour.state==tourIdle||tour.index>=entryCount)return {};
    if(storyZone(tour.index)){
        const auto& step=story::steps[tour.index];
        return {step.wolf,story::seed,tour.index+1,tour.dir,step.checkpoint};
    }
    return {int(current().form),randomLoadout()?zoneSeed(tour.index):0u,tour.index+1,tour.dir};
}
bool devConsoleTourVisualActive(){return tour.mode==modeEverything&&tour.state!=tourIdle;}
DevZoneCaptureContext devConsoleZoneCaptureContext(){
    const auto loadout=devConsoleZoneLoadout();
    if(!loadout.zone)return {};
    const auto route=devConsoleStoryContext();
    if(route.definition){
        const auto& start=route.definition->start;
        return {loadout,tourStateName(),start.stage,start.room,start.point,start.layer,
            route,tour.state==tourRun?tour.phaseFrame:tour.waitFrames};
    }
    const auto& entry=current();
    const auto& location=zoneRooms[entry.room];
    return {loadout,tourStateName(),zoneStages[entry.stage].stage,location.room,location.point,-1,
        route,tour.state==tourRun?tour.phaseFrame:tour.waitFrames};
}
DevStoryContext devConsoleStoryContext(){
    if(!storyZone(tour.index)||tour.state==tourIdle||tour.index>=entryCount)return {};
    const auto& step=story::steps[tour.index];
    return {step.id,story::phaseNames[tour.storyRun.phase],
        tour.storyRun.phase==story::loading?tour.waitFrames:tour.phaseFrame,&step};
}
bool devConsoleBootStage(BootStage& stage){
    if(tour.state!=tourWarp||!tour.bootWarp)return false;
    if(storyZone(tour.index)){
        const auto& start=story::steps[tour.index].start;
        stage={};std::snprintf(stage.name,sizeof(stage.name),"%s",start.stage);
        stage.room=start.room;stage.point=start.point;stage.layer=start.layer;
        return true;
    }
    const auto& e=current();const auto& z=zoneStages[e.stage];const auto& r=zoneRooms[e.room];
    stage={};std::snprintf(stage.name,sizeof(stage.name),"%s",z.stage);
    stage.room=r.room;stage.point=r.point;stage.layer=-1;
    return true;
}
void updateDevConsoleInput(std::uint32_t held,std::uint32_t down){
    if(betaScreenEnabled()){
        if(down&KEY_TOUCH){const auto& p=nativeGameInput();touchBetaScreen(p.touchX,p.touchY,true);}
        return;
    }
    if(finished.active){
        // Any button or a touch keeps the console on; the press itself does
        // nothing else (it would otherwise toggle the test or reach the game).
        // Keys held when the screen came up count once they were released
        // (scripted emulator input arrives as held keys only).
        const std::uint32_t pressed=down|(held&~finished.held);
        finished.held=held;
        if(pressed&&!finished.powerStage){
            finished.active=false;chordLatched=true;
            finishedLine("power_off=cancelled by input %u s before it\n",
                unsigned((finished.untilTick>svcGetSystemTick()?finished.untilTick-svcGetSystemTick():0)/SYSCLOCK_ARM11));
            OSReport("Native test finished: input received, the console stays on\n");
            say("Power off cancelled");
        }
        return;
    }
    selectHeld=(held&KEY_SELECT)!=0;
    const bool chord=(held&(KEY_START|KEY_SELECT))==(KEY_START|KEY_SELECT);
    if(chord&&!chordLatched){
        chordLatched=true;
        if(tour.state==tourIdle)startTour();else endTour("stopped");
        dirty=true;
    }
    if(!(held&(KEY_START|KEY_SELECT)))chordLatched=false;
    if(down&KEY_TOUCH){const auto& point=nativeGameInput();touch(point.touchY/8+1,point.touchX/8+1);}
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
bool devConsoleOwnsPad(){return finished.active||tour.state!=tourIdle||page!=pageStats||selectHeld||chordLatched;}
static unsigned padTickOffset{};
static const DevGameStatus* tickChapterStatus{};
void devConsolePadTick(unsigned offset,const DevGameStatus* chapterStatus){
    padTickOffset=offset;
    tickChapterStatus=chapterStatus;
}
void devConsoleVirtualPad(std::uint32_t& keys,std::int16_t& circleX,std::int16_t& circleY,std::int16_t& cstickX,std::int16_t& cstickY){
    keys=0;circleX=circleY=cstickX=cstickY=0;
    // Started on the warning screen or the logos: those scenes wait for A and
    // refuse a warp, and the pad is ignored while the test runs. Press A once
    // a second until the warp is accepted (Alpha 5: zone 1 never started).
    if(tour.state==tourWarp){if(tour.waitFrames%60>=30&&tour.waitFrames%60<33)keys|=KEY_A;return;}
    if(tour.state!=tourRun)return;
    if(storyZone(tour.index)){
        if(tour.storyRun.phase!=story::play)return;
        const auto& step=story::steps[tour.index];
        const auto& padStatus=tickChapterStatus?*tickChapterStatus:last;
        const unsigned elapsed=tour.phaseFrame+padTickOffset;
        if(padStatus.eventRunning && step.chapter.goal!=story::ChapterGoal::duel){
            if(padStatus.dialogueActive && elapsed%18==0)keys|=KEY_A;
            return;
        }
        const unsigned tick=(step.chapter.goal==story::ChapterGoal::duel?tour.phaseFrame:tour.storyRun.playableTicks)+padTickOffset;
        if(tick<6)return;
        if(step.script==story::Script::warp){
            if(tour.storyRun.warpProof.confirmed)return;
            using Input=story::WarpInput;
            const auto& warp=last.portalWarp;
            if(warp.input==Input::moveCursor)story::steerMap(warp.cursorDx,warp.cursorDy,circleX,circleY);
            if(elapsed%14==0){
                switch(warp.input){
                case Input::openMap:keys|=KEY_DLEFT;break;
                case Input::showPortals:keys|=KEY_ZL;break;
                case Input::select:
                case Input::confirm:keys|=KEY_A;break;
                case Input::chooseYes:circleY=150;break;
                default:break;
                }
            }
            return;
        }
        if(story::chapterPad(step,padStatus,tick,keys,circleX,circleY,cstickY))return;
        if(step.script==story::Script::cutscene)return;
        if(step.script==story::Script::walk){
            if(tick<48)circleY=125;
            else circleX=105;
        }else if(step.script==story::Script::equipment){
            if(tick==8||tick==75)keys|=KEY_X;
            if(tick>=22&&tick<51)circleY=85;
        }else if(step.script==story::Script::route){
            const auto target=step.via&&!tour.storyRun.passedWaypoint?*step.via:story::RoutePoint{step.x,step.z};
            story::steer(target.x-last.position[0],target.z-last.position[2],
                last.cameraForwardX,last.cameraForwardZ,circleX,circleY);
        }else if(step.script==story::Script::portal){
            story::castleWestPad(step,last,tour.storyRun.cutsceneSeen,tick,keys,circleX,circleY);
        }else{
            const bool near=last.enemyCount&&last.enemyDistance<430;
            if(near)keys|=KEY_L;
            if(tick%16==0)keys|=KEY_B;
            if(!near)story::steer((last.enemyCount?last.enemyX:step.x)-last.position[0],
                (last.enemyCount?last.enemyZ:step.z)-last.position[2],last.cameraForwardX,last.cameraForwardZ,circleX,circleY);
        }
        return;
    }
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
    case menuPhase: // START opens the collection screen and closes it; B makes sure
        if(!menuPhaseOn)break;
        if(percent>=4&&percent<8)keys|=KEY_START;
        if(percent>=70&&percent<74)keys|=KEY_START;
        if((percent>=84&&percent<87)||(percent>=92&&percent<95))keys|=KEY_B;
        break;
    }
}
bool devConsoleExitRequested(){return exitRequested;}
bool devConsoleHolding(){return finished.active;}
void serviceDevConsoleHold(){
    if(!finished.active)return;
    const auto now=svcGetSystemTick();
    if(finished.powerStage){
        // The system closes the application when the power-off proceeds; the
        // game loop then ends by itself (aptMainLoop).
        const auto waited=(now-finished.stageTick)/SYSCLOCK_ARM11;
        if(finished.powerStage==1&&waited>=10){
            finished.powerStage=2;finished.stageTick=now;
            requestPtmPowerOff();flushNativeLog();
        }else if(finished.powerStage==2&&waited>=10){
            // Neither request powered the console off (an emulator): leave
            // the game, the least that keeps running is the HOME menu.
            OSReport("Native power off: not available here; leaving the game instead\n");
            finished.active=false;exitRequested=true;
        }
        return;
    }
    if(now>=finished.untilTick){
        finishedLine("power_off=requested after %u s without input\n",finished.hold);
        OSReport("Native test finished: no input for %u s, powering the console off\n",finished.hold);
        std::printf("\x1b[19;1H" C_HDR "  Powering off...                     " C_RST "\x1b[K");std::fflush(stdout);
        if(auto* pixels=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr))GSPGPU_FlushDataCache(pixels,320*240*2);
        flushNativeLog();
        finished.powerStage=1;finished.stageTick=now;
        notifyPowerOff();flushNativeLog();
        return;
    }
    const unsigned left=unsigned((finished.untilTick-now)/SYSCLOCK_ARM11);
    const bool first=finished.shown==~0u;
    if(dirty||left!=finished.shown){finished.shown=left;const bool clear=dirty;dirty=false;draw(clear);}
    // The screen as it was shown, beside the test's results.
    if(first){char path[128];std::snprintf(path,sizeof(path),"%s/finished.bmp",finished.dir);writeBmp(path,GFX_BOTTOM,320);}
}
void devConsoleRates(float& fps,int& speedPercent){fps=smoothLoop>0?1000.0f/smoothLoop:0.0f;speedPercent=gameSpeedPercent();}
bool devConsoleProfiling(){return !storyZone(tour.index)&&tour.state==tourRun&&tour.phase==profilePhase;}
bool serviceDevConsole(const DevGameStatus& status){
    last=status;
    smoothLoop=smoothLoop>0?smoothLoop*0.9f+status.loopMs*0.1f:status.loopMs;
    smoothExec=smoothExec>0?smoothExec*0.9f+status.execMs*0.1f:status.execMs;
    smoothTicks=smoothTicks>0?smoothTicks*0.9f+float(status.ticks)*0.1f:float(status.ticks);
    if(betaScreenEnabled()){updateBetaScreen(smoothLoop,gameSpeedPercent());return false;}
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
