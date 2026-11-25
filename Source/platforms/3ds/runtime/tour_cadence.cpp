#include "tour_cadence.hpp"
#include "frame_telemetry.hpp"
#include "clock.hpp"
#include "tour_load_cost.hpp"
#include <cstdio>
#include <cstring>

namespace dusk::native {
namespace {
// Little-endian ARM ABI. Python reader checks version, size and close count.
// Inter-picture time stays in native ticks, even across multi-second SD stalls.
struct Row {
    std::uint64_t elapsed;
    std::uint32_t frame, demoFrame, cpuUs, gpuUs, draws;
    char stage[8];
    std::int8_t room;
    std::uint8_t phase, flags, scene;
    std::uint32_t gameReadUs, longestCreateUs;
    std::int16_t process;
    std::uint16_t lookup100us;
    std::uint32_t linearFree, gameHeapFree; // most recent 30-picture sample
    std::uint32_t dvdLockUs;
};
static_assert(sizeof(Row)==64);
struct Header {
    char magic[8];
    std::uint32_t frequency, step, rowSize, count;
};
static_assert(sizeof(Header)==24);
Header header;
Row block[1024];
unsigned used;
std::FILE* output;
bool active, failed;
std::uint64_t previousTick;

void flushBlock(){
    if (!failed && used && std::fwrite(block,sizeof(Row),used,output)!=used) failed=true;
    used=0;
}
std::uint32_t microseconds(std::uint64_t ticks){
    const auto us=ticks/nativeTickFrequency*1000000+(ticks%nativeTickFrequency)*1000000/nativeTickFrequency;
    return us>UINT32_MAX?UINT32_MAX:std::uint32_t(us);
}
}

bool endTourCadence(){
    if(!active)return true;
    if(output){
        flushBlock();
        // The count remains zero on an interrupted write. Never accept a prefix.
        if(!failed && (std::fflush(output) || std::fseek(output,0,SEEK_SET) ||
                      std::fwrite(&header,sizeof(header),1,output)!=1))failed=true;
        if(std::fclose(output))failed=true;
    }
    output=nullptr;active=false;measureTourLoads=false;
    return !failed;
}

void beginTourCadence(const char* directory,unsigned step,std::uint64_t tick){
    const bool continuation=active;
    endTourCadence();
    if(!continuation)previousTick=tick;
    header={{'D','L','C','A','D','0','3',0},std::uint32_t(nativeTickFrequency),step,sizeof(Row),0};
    char path[160];
    std::snprintf(path,sizeof(path),"%s/cadence-%03u.bin",directory,step);
    output=std::fopen(path,"wb");
    active=true;used=0;measureTourLoads=true;tourLoadCost={};
    failed=!output || std::fwrite(&header,sizeof(header),1,output)!=1;
}
bool tourCadenceActive(){return active;}
void recordTourCadence(const FrameTelemetryInput& in,std::uint8_t phase,std::uint8_t flags){
    if(!active || failed)return;
    Row& row=block[used++];
    row={};
    row.elapsed=in.submitTick-previousTick;
    previousTick=in.submitTick;
    row.frame=std::uint32_t(in.frame);row.demoFrame=in.demoFrame;
    row.cpuUs=microseconds(in.executionTicks);
    row.gpuUs=microseconds(in.gpu.gpuListTicks+in.gpu.gpuCopyTicks+in.gpu.gpuFillTicks);
    row.draws=in.draws;
    if(in.stage)std::strncpy(row.stage,in.stage,sizeof(row.stage));
    row.room=std::int8_t(in.room);row.phase=phase;row.flags=flags;row.scene=in.sceneFlags;
    row.gameReadUs=microseconds(tourLoadCost.gameReadTicks);
    row.longestCreateUs=microseconds(tourLoadCost.longestCreateTicks);
    row.process=tourLoadCost.process;
    const auto lookup=microseconds(tourLoadCost.pathLookupTicks)/100;
    row.lookup100us=lookup>UINT16_MAX?UINT16_MAX:std::uint16_t(lookup);
    row.dvdLockUs=microseconds(tourLoadCost.dvdLockTicks);
    row.linearFree=in.linearFree;row.gameHeapFree=in.gameHeapFree;
    tourLoadCost={};
    ++header.count;
    if(used==sizeof(block)/sizeof(block[0]))flushBlock();
}
}
