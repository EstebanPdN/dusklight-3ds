#include "frame_telemetry.hpp"
#include "clock.hpp"
#include "tour_cadence.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace dusk::native {
namespace {
// 10 us units keep a sample at 60 bytes; 655 ms saturates (counted).
using Sample=FrameTelemetryRow;
constexpr unsigned sampleCapacity=4096;
Sample* samples{};
struct ZoneFrameCsv {
    char path[160];
    std::size_t count{};
    std::unique_ptr<Sample[]> rows;
};
std::atomic<ZoneFrameCsv*> pendingZoneCsv{};
unsigned sampleHead{},sampleCount{};
std::uint16_t tagZone{};std::uint8_t tagPhase{},pendingFlags{};
std::uint32_t droppedSummaries{},saturatedSamples{};
// Upper bounds in milliseconds of the loop-time histogram; the last bucket is open.
constexpr std::array<std::uint16_t,11> bucketLimits{34,36,40,45,50,60,70,85,100,150,250};
struct RoomSummary {
    char stage[8];
    std::int8_t room;
    std::uint32_t frames,flagged;
    std::uint64_t loop,execution,paint,update,draw,pace,gpuWait,gpuTail,audio,draws,ticks,gpuBusy,gpuIdle;
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
    recordTourCadence(in,tagZone ? tagPhase : 255,pendingFlags);
    if(!samples){pendingFlags=0;return;}
    Sample s{};saturating=false;
    s.frame=std::uint32_t(in.frame);
    s.loop=units(in.loopTicks);s.execution=units(in.executionTicks);s.paint=units(in.paintTicks);
    s.update=units(in.updateTicks);s.draw=units(in.drawTicks);s.pace=units(in.paceTicks);
    s.gpuBegin=units(in.gpu.beginWaitTicks);s.gpuEnd=units(in.gpu.endWaitTicks);
    s.gpuComplete=units(in.gpu.completeWaitTicks);s.present=units(in.gpu.presentWaitTicks);
    const float tail=in.gpuTailMilliseconds<0?0:in.gpuTailMilliseconds*100;
    s.gpuTail=tail>65535?std::uint16_t(65535):std::uint16_t(tail);
    s.audio=units(in.audioMixTicks);
    s.gpuLists=units(in.gpu.gpuListTicks);s.gpuCopies=units(in.gpu.gpuCopyTicks+in.gpu.gpuFillTicks);
    s.gpuIdle=units(in.gpu.gpuIdleTicks);s.gpuLongest=units(in.gpu.gpuLongestListTicks);
    s.gpuJobs=in.gpu.gpuJobs>255?std::uint8_t(255):std::uint8_t(in.gpu.gpuJobs);
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
    if(s.flags&telemetryDiagnosticFrame){++total->flagged;return;}
    ++total->frames;
    total->loop+=s.loop;total->execution+=s.execution;total->paint+=s.paint;total->update+=s.update;
    total->draw+=s.draw;total->pace+=s.pace;
    total->gpuWait+=std::uint32_t(s.gpuBegin)+s.gpuEnd+s.gpuComplete+s.present;
    total->gpuTail+=s.gpuTail;total->audio+=s.audio;total->draws+=s.draws;total->ticks+=s.ticks;
    total->gpuBusy+=std::uint32_t(s.gpuLists)+s.gpuCopies;total->gpuIdle+=s.gpuIdle;
    if(s.loop>total->slowestLoop)total->slowestLoop=s.loop;
    unsigned bucket=0;
    while(bucket<bucketLimits.size()&&s.loop>bucketLimits[bucket]*100u)++bucket;
    ++total->histogram[bucket];
}
bool frameTelemetryStats(std::uint16_t zone,int phase,TelemetryStats& out,std::uint32_t excluded,std::uint8_t excludedFlags){
    out={};
    if(!samples)return false;
    static std::uint16_t loops[sampleCapacity],executions[sampleCapacity];
    std::uint64_t loop=0,execution=0,paint=0,update=0,draw=0,pace=0,wait=0,tail=0,audio=0,draws=0,gpuBusy=0,gpuIdle=0;
    unsigned count=0;
    eachSample([&](const Sample& s){
        if(s.zone!=zone||(phase>=0&&s.phase!=phase)||(phase<=-2&&s.phase==-2-phase)||
           (phase==-1&&s.phase<32&&(excluded>>s.phase&1)))return;
        if(s.flags&excludedFlags){++out.flagged;return;}
        if(s.loop==65535)++out.saturated;
        loops[count]=s.loop;executions[count]=s.execution;++count;
        loop+=s.loop;execution+=s.execution;paint+=s.paint;update+=s.update;draw+=s.draw;pace+=s.pace;
        wait+=std::uint32_t(s.gpuBegin)+s.gpuEnd+s.gpuComplete+s.present;tail+=s.gpuTail;audio+=s.audio;draws+=s.draws;
        gpuBusy+=std::uint32_t(s.gpuLists)+s.gpuCopies;gpuIdle+=s.gpuIdle;
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
    out.gpuBusy=gpuBusy*scale;out.gpuIdle=gpuIdle*scale;
    return true;
}
namespace {
constexpr char recentHeader[]="frame,stage,room,zone,phase,flags,loop_ms,exec_ms,paint_ms,update_ms,draw_ms,pace_ms,gpu_begin_ms,gpu_end_ms,gpu_complete_ms,present_ms,gpu_tail_ms,audio_mix_ms,draws,chunks,completes,reclaims,ticks,gpu_busy_ms,gpu_lists_ms,gpu_copies_ms,gpu_idle_ms,gpu_longest_ms,gpu_jobs\n";
// All times already have exactly two fractional digits. This path is shared
// by normal exports, zone reports and the background diagnostic writer.
using CsvLine = std::array<char, 320>;
std::size_t formatRow(CsvLine& line, const Sample& s) {
    // 29 fields, each at most 10 decimal characters, plus delimiters.
    char* cursor = line.data();
    const auto end = line.data() + line.size();
    auto integer = [&](int value) {
        *cursor++ = ',';
        cursor = std::to_chars(cursor, end, value).ptr;
    };
    auto milliseconds = [&](unsigned hundredths) {
        integer(hundredths / 100);
        *cursor++ = '.';
        *cursor++ = char('0' + (hundredths / 10) % 10);
        *cursor++ = char('0' + hundredths % 10);
    };
    cursor = std::to_chars(cursor, end, s.frame).ptr;
    *cursor++ = ',';
    for (const char c : s.stage) {
        if (!c) break;
        *cursor++ = c;
    }
    integer(s.room); integer(s.zone); integer(s.phase); integer(s.flags);
    for (auto value : {s.loop, s.execution, s.paint, s.update, s.draw, s.pace,
                       s.gpuBegin, s.gpuEnd, s.gpuComplete, s.present, s.gpuTail, s.audio})
        milliseconds(value);
    integer(s.draws); integer(s.chunks); integer(s.completes); integer(s.reclaims); integer(s.ticks);
    milliseconds(unsigned(s.gpuLists) + s.gpuCopies);
    for (auto value : {s.gpuLists, s.gpuCopies, s.gpuIdle, s.gpuLongest}) milliseconds(value);
    integer(s.gpuJobs);
    *cursor++ = '\n';
    return cursor - line.data();
}
bool sampleRow(std::FILE* file, const Sample& s) {
    CsvLine line;
    const auto size = formatRow(line, s);
    return std::fwrite(line.data(), 1, size, file) == size;
}
}
bool writeFrameTelemetryZone(std::FILE* file,std::uint16_t zone){
    if(!samples||!file||std::fputs(recentHeader,file)<0)return false;
    eachSample([&](const Sample& s){if(s.zone==zone)sampleRow(file,s);});
    return !std::ferror(file);
}
bool queueZoneFrameCsv(const char* path, std::uint16_t zone) {
    if (!path || !samples || pendingZoneCsv.load(std::memory_order_acquire)) return false;
    auto snapshot = std::unique_ptr<ZoneFrameCsv>(new(std::nothrow) ZoneFrameCsv);
    if (!snapshot || std::strlen(path) >= sizeof(snapshot->path)) return false;
    std::strcpy(snapshot->path, path);
    std::size_t count = 0;
    eachSample([&](const Sample& row) { if (row.zone == zone) ++count; });
    if (count) {
        snapshot->rows.reset(new(std::nothrow) Sample[count]);
        if (!snapshot->rows) return false;
        eachSample([&](const Sample& row) {
            if (row.zone == zone) snapshot->rows[snapshot->count++] = row;
        });
    }
    pendingZoneCsv.store(snapshot.release(), std::memory_order_release);
    return true;
}
bool writePendingZoneFrameCsv() {
    std::unique_ptr<ZoneFrameCsv> snapshot(pendingZoneCsv.exchange(nullptr, std::memory_order_acquire));
    if (!snapshot) return true;
    char temporary[sizeof(snapshot->path)+5];
    std::snprintf(temporary, sizeof(temporary), "%s.tmp", snapshot->path);
    auto* file = std::fopen(temporary, "wb");
    if (!file) return false;
    const bool written = writeFrameTelemetryRows(file, {snapshot->rows.get(), snapshot->count});
    return std::fclose(file) == 0 && written && std::rename(temporary, snapshot->path) == 0;
}
std::size_t copyRecentFrameTelemetry(std::span<FrameTelemetryRow> out){
    if(!samples)return 0;
    const auto count=std::min(out.size(),std::size_t(sampleCount));
    const auto first=(sampleHead+sampleCapacity-count)%sampleCapacity;
    if (count) {
        const auto left = std::min(count, sampleCapacity - first);
        std::memcpy(out.data(), samples + first, left * sizeof(Sample));
        if (left < count) std::memcpy(out.data() + left, samples, (count - left) * sizeof(Sample));
    }
    return count;
}
bool writeFrameTelemetryRows(std::FILE* file,std::span<const FrameTelemetryRow> rows){
    if(std::fputs(recentHeader,file)<0)return false;
    for(const auto& row:rows)if(!sampleRow(file,row))return false;
    return true;
}
std::size_t formatRecentFrameTelemetry(char* out,std::size_t capacity,unsigned frames){
    if(!samples||!out||capacity<sizeof(recentHeader))return 0;
    std::size_t used=sizeof(recentHeader)-1;
    std::memcpy(out,recentHeader,used);
    CsvLine line;
    const unsigned count=frames<sampleCount?frames:sampleCount;
    const unsigned first=(sampleHead+sampleCapacity-count)%sampleCapacity;
    for(unsigned n=0;n<count;++n){
        const Sample& s=samples[(first+n)%sampleCapacity];
        const auto size=formatRow(line,s);
        if(size>=capacity-used)break;
        std::memcpy(out+used,line.data(),size);
        used+=size;
    }
    out[used]=0;
    return used;
}
bool writeFrameTelemetry(bool includeFrames){
    if(!samples||!summaryCount)return false;
    bool written=false;
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/perf-summary.csv","wb")){
        std::fprintf(file,"stage,room,frames,flagged,loop_ms,fps,exec_ms,paint_ms,update_ms,draw_ms,pace_ms,gpu_wait_ms,gpu_tail_ms,audio_mix_ms,draws,slowest_ms");
        for(const auto limit:bucketLimits)std::fprintf(file,",le_%u",unsigned(limit));
        // game_speed_pct: original 30 Hz logic ticks run per second of real time.
        std::fprintf(file,",gt_%u,ticks_per_frame,game_speed_pct,gpu_busy_ms,gpu_idle_ms\n",unsigned(bucketLimits.back()));
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
            std::fprintf(file,",%.2f,%.0f,%.2f,%.2f\n",ticks,loop>0?ticks*(1000.0/30.0)/loop*100.0:0.0,t.gpuBusy*scale,t.gpuIdle*scale);
        }
        std::fprintf(file,"# samples without a summary row=%lu, saturated samples (>655 ms)=%lu\n",
            static_cast<unsigned long>(droppedSummaries),static_cast<unsigned long>(saturatedSamples));
        written=!std::ferror(file);std::fclose(file);
    }
    if(includeFrames)if(auto* file=std::fopen("sdmc:/3ds/DuskLight/frames.csv","wb")){
        std::fputs(recentHeader,file);
        eachSample([&](const Sample& s){sampleRow(file,s);});
        written=written&&!std::ferror(file);std::fclose(file);
    }
    return written;
}
}
