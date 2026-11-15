#include "frame_telemetry.hpp"
#include "clock.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dusk::native {
namespace {
// 10 us units keep a sample at 48 bytes; 655 ms saturates (counted).
struct Sample {
    std::uint32_t frame;
    std::uint16_t loop,execution,paint,update,draw,pace,gpuBegin,gpuEnd,gpuComplete,present,gpuTail,audio;
    std::uint16_t draws,zone;
    std::uint8_t chunks,completes,reclaims;std::int8_t room;
    std::uint8_t phase,flags,ticks;
    char stage[8];
};
constexpr unsigned sampleCapacity=4096;
Sample* samples{};
unsigned sampleHead{},sampleCount{};
std::uint16_t tagZone{};std::uint8_t tagPhase{},pendingFlags{};
std::uint32_t droppedSummaries{},saturatedSamples{};
// Upper bounds in milliseconds of the loop-time histogram; the last bucket is open.
constexpr std::array<std::uint16_t,11> bucketLimits{34,36,40,45,50,60,70,85,100,150,250};
struct RoomSummary {
    char stage[8];
    std::int8_t room;
    std::uint32_t frames,flagged;
    std::uint64_t loop,execution,paint,update,draw,pace,gpuWait,gpuTail,audio,draws,ticks;
    std::uint32_t slowestLoop; // 10 us units
    std::array<std::uint32_t,bucketLimits.size()+1> histogram;
};
// One entry per visited stage and room: the disc has 304 rooms with start points.
constexpr unsigned summaryCapacity=352;
RoomSummary* summaries{};
unsigned summaryCount{};
bool saturating{};
std::uint16_t units(std::uint64_t ticks){
    const auto value=ticks*100000u/nativeTickFrequency;
    if(value>65535){saturating=true;return 65535;}
    return std::uint16_t(value);
}
RoomSummary* summaryFor(const char* stage,int room){
    char key[8]{};
    if(stage)std::strncpy(key,stage,sizeof(key));
    for(unsigned n=summaryCount;n--;) // the current room is almost always the newest entry
        if(summaries[n].room==std::int8_t(room)&&!std::memcmp(summaries[n].stage,key,sizeof(key)))return &summaries[n];
    if(summaryCount==summaryCapacity)return nullptr;
    auto& fresh=summaries[summaryCount++];fresh={};std::memcpy(fresh.stage,key,sizeof(key));fresh.room=std::int8_t(room);return &fresh;
}
template<class Visit> void eachSample(Visit visit){
    const unsigned first=(sampleHead+sampleCapacity-sampleCount)%sampleCapacity;
    for(unsigned n=0;n<sampleCount;++n)visit(samples[(first+n)%sampleCapacity]);
}
}
void initializeFrameTelemetry(){
    if(!samples)samples=static_cast<Sample*>(std::calloc(sampleCapacity,sizeof(Sample)));
    if(!summaries)summaries=static_cast<RoomSummary*>(std::calloc(summaryCapacity,sizeof(RoomSummary)));
    if(!summaries){std::free(samples);samples=nullptr;}
    sampleHead=sampleCount=0;summaryCount=0;droppedSummaries=saturatedSamples=0;
}
void setFrameTelemetryTag(std::uint16_t zone,std::uint8_t phase){tagZone=zone;tagPhase=phase;}
void flagNextFrameTelemetry(std::uint8_t flags){pendingFlags|=flags;}
std::uint32_t frameTelemetryDropped(){return droppedSummaries;}
std::uint32_t frameTelemetrySaturated(){return saturatedSamples;}
void recordFrameTelemetry(const FrameTelemetryInput& in){
    if(!samples)return;
    Sample s{};saturating=false;
    s.frame=std::uint32_t(in.frame);
    s.loop=units(in.loopTicks);s.execution=units(in.executionTicks);s.paint=units(in.paintTicks);
    s.update=units(in.updateTicks);s.draw=units(in.drawTicks);s.pace=units(in.paceTicks);
    s.gpuBegin=units(in.gpu.beginWaitTicks);s.gpuEnd=units(in.gpu.endWaitTicks);
    s.gpuComplete=units(in.gpu.completeWaitTicks);s.present=units(in.gpu.presentWaitTicks);
    const float tail=in.gpuTailMilliseconds<0?0:in.gpuTailMilliseconds*100;
    s.gpuTail=tail>65535?std::uint16_t(65535):std::uint16_t(tail);
    s.audio=units(in.audioMixTicks);
    s.draws=in.draws>65535?std::uint16_t(65535):std::uint16_t(in.draws);
    s.chunks=in.gpu.chunks>255?255:std::uint8_t(in.gpu.chunks);
    s.completes=in.gpu.completes>255?255:std::uint8_t(in.gpu.completes);
    s.reclaims=in.gpu.reclaims>255?255:std::uint8_t(in.gpu.reclaims);
    s.room=std::int8_t(in.room);s.zone=tagZone;s.phase=tagPhase;s.flags=pendingFlags;pendingFlags=0;
    s.ticks=in.ticks>255?std::uint8_t(255):std::uint8_t(in.ticks);
    if(saturating)++saturatedSamples;
    if(in.stage)std::strncpy(s.stage,in.stage,sizeof(s.stage));
    samples[sampleHead]=s;sampleHead=(sampleHead+1)%sampleCapacity;
    if(sampleCount<sampleCapacity)++sampleCount;
    auto* total=summaryFor(in.stage,in.room);
    if(!total){++droppedSummaries;return;}
    if(s.flags){++total->flagged;return;}
    ++total->frames;
    total->loop+=s.loop;total->execution+=s.execution;total->paint+=s.paint;total->update+=s.update;
    total->draw+=s.draw;total->pace+=s.pace;
    total->gpuWait+=std::uint32_t(s.gpuBegin)+s.gpuEnd+s.gpuComplete+s.present;
    total->gpuTail+=s.gpuTail;total->audio+=s.audio;total->draws+=s.draws;total->ticks+=s.ticks;
    if(s.loop>total->slowestLoop)total->slowestLoop=s.loop;
    unsigned bucket=0;
    while(bucket<bucketLimits.size()&&s.loop>bucketLimits[bucket]*100u)++bucket;
    ++total->histogram[bucket];
}
bool frameTelemetryStats(std::uint16_t zone,int phase,TelemetryStats& out){
    out={};
    if(!samples)return false;
    static std::uint16_t loops[sampleCapacity],executions[sampleCapacity];
    std::uint64_t loop=0,execution=0,paint=0,update=0,draw=0,pace=0,wait=0,tail=0,audio=0,draws=0;
    unsigned count=0;
    eachSample([&](const Sample& s){
        if(s.zone!=zone||(phase>=0&&s.phase!=phase)||(phase<=-2&&s.phase==-2-phase))return;
        if(s.flags){++out.flagged;return;}
        if(s.loop==65535)++out.saturated;
        loops[count]=s.loop;executions[count]=s.execution;++count;
        loop+=s.loop;execution+=s.execution;paint+=s.paint;update+=s.update;draw+=s.draw;pace+=s.pace;
        wait+=std::uint32_t(s.gpuBegin)+s.gpuEnd+s.gpuComplete+s.present;tail+=s.gpuTail;audio+=s.audio;draws+=s.draws;
        if(s.loop>3400)++out.over34;
        if(s.loop>4000)++out.over40;
        if(s.loop>5000)++out.over50;
    });
    out.frames=count;
    if(!count)return false;
    std::sort(loops,loops+count);std::sort(executions,executions+count);
    auto at=[&](const std::uint16_t* values,unsigned percent){return values[std::min(count-1,count*percent/100)]*0.01f;};
    const float scale=0.01f/count;
    out.loopMean=loop*scale;out.loopP50=at(loops,50);out.loopP95=at(loops,95);out.loopP99=at(loops,99);out.loopMax=loops[count-1]*0.01f;
    out.execMean=execution*scale;out.execP95=at(executions,95);out.execMax=executions[count-1]*0.01f;
    out.paint=paint*scale;out.update=update*scale;out.draw=draw*scale;out.pace=pace*scale;
    out.gpuWait=wait*scale;out.gpuTail=tail*scale;out.audio=audio*scale;out.draws=float(draws)/count;
    return true;
}
namespace {
void sampleHeader(std::FILE* file){
    std::fprintf(file,"frame,stage,room,zone,phase,flags,loop_ms,exec_ms,paint_ms,update_ms,draw_ms,pace_ms,gpu_begin_ms,gpu_end_ms,gpu_complete_ms,present_ms,gpu_tail_ms,audio_mix_ms,draws,chunks,completes,reclaims,ticks\n");
}
void sampleRow(std::FILE* file,const Sample& s){
    std::fprintf(file,"%lu,%.8s,%d,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u\n",
        static_cast<unsigned long>(s.frame),s.stage,int(s.room),unsigned(s.zone),unsigned(s.phase),unsigned(s.flags),
        s.loop*0.01,s.execution*0.01,s.paint*0.01,s.update*0.01,
        s.draw*0.01,s.pace*0.01,s.gpuBegin*0.01,s.gpuEnd*0.01,s.gpuComplete*0.01,s.present*0.01,s.gpuTail*0.01,s.audio*0.01,
        unsigned(s.draws),unsigned(s.chunks),unsigned(s.completes),unsigned(s.reclaims),unsigned(s.ticks));
}
}
bool writeFrameTelemetryZone(std::FILE* file,std::uint16_t zone){
    if(!samples||!file)return false;
    sampleHeader(file);
    eachSample([&](const Sample& s){if(s.zone==zone)sampleRow(file,s);});
    return !std::ferror(file);
}
std::size_t formatRecentFrameTelemetry(char* out,std::size_t capacity,unsigned frames){
    if(!samples||!out||capacity<256)return 0;
    std::size_t used=0;
    auto add=[&](const char* format,auto... values){
        const int n=std::snprintf(out+used,capacity-used,format,values...);
        if(n<0||std::size_t(n)>=capacity-used)return false;
        used+=n;return true;
    };
    if(!add("frame,stage,room,zone,phase,flags,loop_ms,exec_ms,paint_ms,update_ms,draw_ms,pace_ms,gpu_begin_ms,gpu_end_ms,gpu_complete_ms,present_ms,gpu_tail_ms,audio_mix_ms,draws,chunks,completes,reclaims,ticks\n"))return 0;
    const unsigned count=frames<sampleCount?frames:sampleCount;
    const unsigned first=(sampleHead+sampleCapacity-count)%sampleCapacity;
    for(unsigned n=0;n<count;++n){
        const Sample& s=samples[(first+n)%sampleCapacity];
        if(!add("%lu,%.8s,%d,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u\n",
            static_cast<unsigned long>(s.frame),s.stage,int(s.room),unsigned(s.zone),unsigned(s.phase),unsigned(s.flags),
            s.loop*0.01,s.execution*0.01,s.paint*0.01,s.update*0.01,s.draw*0.01,s.pace*0.01,s.gpuBegin*0.01,s.gpuEnd*0.01,
            s.gpuComplete*0.01,s.present*0.01,s.gpuTail*0.01,s.audio*0.01,
            unsigned(s.draws),unsigned(s.chunks),unsigned(s.completes),unsigned(s.reclaims),unsigned(s.ticks)))break;
    }
    return used;
}
bool writeFrameTelemetry(bool includeFrames){
    if(!samples||!summaryCount)return false;
    bool written=false;
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/perf-summary.csv","wb")){
        std::fprintf(file,"stage,room,frames,flagged,loop_ms,fps,exec_ms,paint_ms,update_ms,draw_ms,pace_ms,gpu_wait_ms,gpu_tail_ms,audio_mix_ms,draws,slowest_ms");
        for(const auto limit:bucketLimits)std::fprintf(file,",le_%u",unsigned(limit));
        // game_speed_pct: original 30 Hz logic ticks run per second of real time.
        std::fprintf(file,",gt_%u,ticks_per_frame,game_speed_pct\n",unsigned(bucketLimits.back()));
        for(unsigned n=0;n<summaryCount;++n){
            const auto& t=summaries[n];if(!t.frames)continue;
            const double scale=0.01/t.frames; // 10 us units -> mean milliseconds
            const double loop=t.loop*scale;
            std::fprintf(file,"%.8s,%d,%lu,%lu,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f",t.stage,int(t.room),
                static_cast<unsigned long>(t.frames),static_cast<unsigned long>(t.flagged),loop,loop>0?1000.0/loop:0.0,t.execution*scale,t.paint*scale,
                t.update*scale,t.draw*scale,t.pace*scale,t.gpuWait*scale,t.gpuTail*scale,t.audio*scale,
                double(t.draws)/t.frames,t.slowestLoop*0.01);
            for(const auto count:t.histogram)std::fprintf(file,",%lu",static_cast<unsigned long>(count));
            const double ticks=double(t.ticks)/t.frames;
            std::fprintf(file,",%.2f,%.0f\n",ticks,loop>0?ticks*(1000.0/30.0)/loop*100.0:0.0);
        }
        std::fprintf(file,"# samples without a summary row=%lu, saturated samples (>655 ms)=%lu\n",
            static_cast<unsigned long>(droppedSummaries),static_cast<unsigned long>(saturatedSamples));
        written=!std::ferror(file);std::fclose(file);
    }
    if(includeFrames)if(auto* file=std::fopen("sdmc:/3ds/DuskLight/frames.csv","wb")){
        sampleHeader(file);
        eachSample([&](const Sample& s){sampleRow(file,s);});
        written=written&&!std::ferror(file);std::fclose(file);
    }
    return written;
}
}
