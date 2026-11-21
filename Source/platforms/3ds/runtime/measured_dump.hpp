#pragma once
// Measurements: what the pictures cost, process by process and draw by draw,
// measured on the running game.
//
// A full dump of ordinary play only held the coarse times of the last
// pictures (update, draw, paint, GPU busy): enough to see that a place is
// slow, not which process or which draw makes it slow. The detailed timers
// exist but only ran in the diagnostic mode. A measurement is a short burst
// of ordinary pictures, the game going on as it was, with the timers on in
// stages:
//
//   warm       four pictures not measured, in every measurement: what turning
//              improvements of perf_features.hpp off costs once (new records,
//              re-made samplers), and the logic ticks the pictures before
//              left owing (a profiled picture, the last measurement's
//              one-draw lists): measured from the same state, a measurement
//              with improvements off and an ordinary one differ by the
//              improvements alone.
//   processes  the process timers alone (two clock reads per process method,
//              per painted packet and per replayed shape): for every process
//              profile, the time of its execute method, of its draw method
//              (entering its models), of the game thread painting its packets
//              and of the GPU-owning thread drawing them. Light enough that
//              the pictures keep their ordinary times.
//   sections   (when asked) every named section timer too (collision,
//              animation, J3D, queue, 2D...). They nest and cost time
//              themselves: their pictures are slower, the stage's own totals
//              say by how much.
//   gpu        one draw per GPU command list: each draw is one GSP job. The
//              time of each is summed by the owner of its model and by
//              painter phase (ctru_gxqueue.c), with the draws and indices
//              each owner sent.
//   settle     one ordinary picture: its start waits for the GPU, so every
//              list of the gpu stage has completed and been summed.
//
// Who asks for one:
// - the full dump (L+R+A): 4 + 12 + 6 + 3 + 1 pictures before the RAM is copied;
//   the result goes into the dump's info.txt and the job ring into dump.dpk
//   (gpu-jobs, gpu-job-owners) for the per-draw view;
// - the automatic test (dev_console.cpp): several per zone, 4 + 8 + 2 + 1
//   pictures each, the first of a zone with the sections, one a reference
//   (the improvements off) and one without a single improvement (another one
//   in every zone: what each is worth, zone by zone), each between two
//   ordinary ones; kept in RAM and written to the zone's measure.txt when the
//   zone ends;
// - sdmc:/3ds/DuskLight/measure-at.txt (game_startup.cpp): at given picture
//   numbers, for scripted emulator runs; appended to measure.txt on the SD.
// Tools/3ds/measure_report.py reads all three.
//
// The second-core worker's own trials (gx_worker_runtime.hpp: some pictures
// painted without it, to see whether it pays) would land in a measurement as
// pictures of another kind: none starts while one runs, a measurement that is
// asked for during one waits, and the pictures a dump's measurement shares
// with one are counted (worker_rested).
//
// Only the game thread touches this state, at picture boundaries.
#include "frame_telemetry.hpp"
#include "gx_worker_runtime.hpp"
#include "gpu_sync.hpp"
#include "gx_profile.hpp"
#include "perf_features.hpp"
#include "clock.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" volatile std::uint32_t duskGxOwnerTiming;
extern "C" void duskGxOwnerTimesTake(std::uint32_t* ownerTicks,std::uint16_t* ownerLists,std::uint32_t* phaseTicks,std::uint16_t* phaseLists);

namespace dusk::native::measured {
enum class Stage : unsigned {idle,warm,processes,sections,gpu,settle,ready};
inline constexpr unsigned stageCount=5; // warm..settle
inline constexpr const char* stageNames[stageCount]{"warm","processes","sections","gpu","settle"};
inline constexpr unsigned gpuPhases=16;
struct Plan {
    unsigned processes{12},sections{6},gpu{3};
    // The improvements off for this measurement (perf_features.hpp): all of
    // them, or the ones of a mask.
    bool reference{};
    unsigned without{};
    bool changed()const{return reference||without;}
};
inline constexpr Plan dumpPlan{12,6,3,false};
struct Sums {
    unsigned pictures{},ticks{},draws{};
    std::uint64_t loop{},execution{},paint{},update{},draw{},audio{},gpuLists{},gpuCopies{},gpuIdle{},gpuWait{};
};
struct State {
    Stage stage=Stage::idle;
    Plan plan{};
    unsigned left{},workerRested{};
    bool have{},forDump{};
    char label[160]{};
    std::uint64_t firstFrame{},lastFrame{};
    std::array<Sums,stageCount> sums{};
    // Process timers of the `processes` stage (kinds: gx_profile.hpp).
    std::array<std::array<std::uint32_t,gxProcessProfileCapacity>,gxProcessKinds> processTicks{};
    std::array<std::array<std::uint16_t,gxProcessProfileCapacity>,gxProcessKinds> processCalls{};
    // Named sections and renderer parts of the `sections` stage.
    std::array<GxNamedProfileEntry,192> named{};
    // The same tick sections by the process method they ran in (gx_profile.hpp).
    std::array<GxOwnedSection,gxOwnedSectionCapacity> ownedSections{};
    unsigned ownedSectionsRefused{};
    GxProfileTotals partsBefore{},parts{};
    // The gpu stage: command-list time, lists, draws and indices by owner
    // (index 0: nobody's model; n: profile n-1) and by painter phase.
    std::array<std::uint32_t,gxProcessProfileCapacity> ownerTicks{},ownerIndices{};
    std::uint32_t casterIndices{}; // of the shadow caster pass, inside ownerIndices
    std::array<std::uint16_t,gxProcessProfileCapacity> ownerLists{},ownerDraws{};
    std::array<std::uint32_t,gpuPhases> phaseTicks{};
    std::array<std::uint16_t,gpuPhases> phaseLists{};
};
inline State state;
// The GPU time diagnostics chosen at boot (gpu-phases.txt, gpu-draw-times.txt):
// the gpu stage turns one draw per list on and the settle stage restores these.
inline bool basePhases{},baseDrawTimes{};
inline void setBaseDiagnostics(bool phases,bool drawTimes){basePhases=phases;baseDrawTimes=drawTimes;}
// Measurements that are not part of a dump wait here, as text, for a moment
// where an SD write does not disturb what is measured (flush()).
inline char pending[48*1024];
inline std::size_t pendingBytes{};
inline unsigned pendingRefused{};

// The timers of the picture about to start.
inline bool processTimers(){return state.stage==Stage::processes||state.stage==Stage::sections;}
inline bool sectionTimers(){return state.stage==Stage::sections;}
inline bool active(){return state.stage!=Stage::idle&&state.stage!=Stage::ready;}

inline void clearProcessTimers(){
    for(auto& kind:gxProcessTicks)kind={};
    for(auto& kind:gxProcessCalls)kind={};
    gxProcessChildTicks=0;
}
inline unsigned pictures(const Plan& plan,Stage stage){
    switch(stage){
    case Stage::warm:return 4;
    case Stage::processes:return plan.processes;
    case Stage::sections:return plan.sections;
    case Stage::gpu:return plan.gpu;
    case Stage::settle:return plan.gpu?1:0;
    default:return 0;
    }
}
inline unsigned pictures(Stage stage){return pictures(state.plan,stage);}
// The pictures a measurement of this plan lasts.
inline unsigned picturesOf(const Plan& plan){
    unsigned total=0;
    for(unsigned stage=unsigned(Stage::warm);stage<=unsigned(Stage::settle);++stage)total+=pictures(plan,Stage(stage));
    return total;
}
// The warm and process pictures of an ordinary measurement keep their
// ordinary times and stay in the zone statistics; every other picture of a
// measurement leaves them (heavier timers, one draw per list, or the
// improvements off).
inline void flagPicture(){
    const bool ordinary=!state.plan.changed()&&(state.stage==Stage::warm||state.stage==Stage::processes);
    if(!ordinary)flagNextFrameTelemetry(telemetryDiagnosticFrame);
}
// Leaves `stage` set to the next stage that has pictures (or ready).
inline void enter(Stage stage){
    while(stage!=Stage::ready&&!pictures(stage))stage=Stage(unsigned(stage)+1);
    state.stage=stage;state.left=pictures(stage);
    if(stage!=Stage::ready)flagPicture();
    if(stage==Stage::processes||stage==Stage::sections)clearProcessTimers();
    if(stage==Stage::sections){state.partsBefore=gxProfileTotals;gxOwnedSections={};gxOwnedSectionsRefused=0;}
    if(stage==Stage::gpu){
        // Lists added from here on are summed when they complete; the lists of
        // the pictures before are not.
        std::array<std::uint32_t,gxProcessProfileCapacity> ticks;std::array<std::uint16_t,gxProcessProfileCapacity> lists;
        std::array<std::uint32_t,gpuPhases> phaseTicks;std::array<std::uint16_t,gpuPhases> phaseLists;
        duskGxOwnerTimesTake(ticks.data(),lists.data(),phaseTicks.data(),phaseLists.data());
        gxOwnerIndices={};gxOwnerDraws={};gxCasterIndices=0;
        duskGxOwnerTiming=1;gxOwnerCounting=true;
        platformSetGpuDiagnostics(basePhases,true);
    }
    if(stage==Stage::settle){
        duskGxOwnerTiming=0;gxOwnerCounting=false;
        state.ownerIndices=gxOwnerIndices;state.ownerDraws=gxOwnerDraws;state.casterIndices=gxCasterIndices;
        platformSetGpuDiagnostics(basePhases,baseDrawTimes);
    }
}
// Starts a measurement that is not part of a dump. False while another one
// runs. `label`: one line of key=value words that says where and when.
inline void begin(const Plan& plan,bool forDump,const char* label){
    state.plan=plan;state.forDump=forDump;state.have=false;state.firstFrame=state.lastFrame=0;state.workerRested=0;
    holdGxWorkerTrials(true);
    state.sums={};state.named={};state.parts={};
    state.ownerTicks={};state.ownerIndices={};state.ownerLists={};state.ownerDraws={};state.phaseTicks={};state.phaseLists={};
    state.casterIndices=0;
    std::snprintf(state.label,sizeof(state.label),"%s",label?label:"");
    if(plan.reference)features::setReference(true);
    features::setWithout(plan.without);
    enter(Stage::warm);
}
inline bool request(const Plan& plan,const char* label){
    if(state.stage!=Stage::idle||gxWorkerResting())return false;
    begin(plan,false,label);
    return true;
}
inline void write(std::FILE* file);
// The finished measurement as text behind the ones already waiting.
inline void keep(){
    if(sizeof(pending)-pendingBytes<12*1024){++pendingRefused;return;}
    if(auto* file=fmemopen(pending+pendingBytes,sizeof(pending)-pendingBytes,"w")){
        write(file);
        const long written=std::ftell(file);
        std::fclose(file);
        if(written>0)pendingBytes+=std::size_t(written);
    }
}
// End of a picture. `dumpRequested`: a full dump waits for its measurement
// (started here when none runs). True: that dump can be taken now (its
// measurement is complete). `audioTicks`: the game thread's audio call of
// this picture.
inline bool advance(const FrameTelemetryInput& picture,std::uint64_t audioTicks,bool dumpRequested){
    if(state.stage==Stage::ready){
        if(dumpRequested)return true;
        state.stage=Stage::idle;state.have=false; // the request was withdrawn
        holdGxWorkerTrials(false);
        return false;
    }
    if(state.stage==Stage::idle){
        if(!dumpRequested)return false;
        begin(dumpPlan,true,nullptr);
        return false;
    }
    if(!state.firstFrame)state.firstFrame=picture.frame;
    state.workerRested+=gxWorkerResting();
    auto& sum=state.sums[unsigned(state.stage)-1];
    ++sum.pictures;sum.ticks+=picture.ticks;sum.draws+=picture.draws;
    sum.loop+=picture.loopTicks;sum.execution+=picture.executionTicks;sum.paint+=picture.paintTicks;
    sum.update+=picture.updateTicks;sum.draw+=picture.drawTicks;sum.audio+=audioTicks;
    sum.gpuLists+=picture.gpu.gpuListTicks;sum.gpuCopies+=picture.gpu.gpuCopyTicks+picture.gpu.gpuFillTicks;
    sum.gpuIdle+=picture.gpu.gpuIdleTicks;
    sum.gpuWait+=picture.gpu.beginWaitTicks+picture.gpu.endWaitTicks+picture.gpu.completeWaitTicks+picture.gpu.presentWaitTicks;
    if(state.stage==Stage::sections){
        // The named timers are cleared at the start of every picture.
        for(unsigned n=0;n<state.named.size();++n){
            state.named[n].name=gxNamedProfiles[n].name;
            state.named[n].ticks+=gxNamedProfiles[n].ticks;state.named[n].calls+=gxNamedProfiles[n].calls;
        }
    }
    if(--state.left){flagPicture();return false;}
    state.lastFrame=picture.frame;
    switch(state.stage){
    case Stage::warm:enter(Stage::processes);return false;
    case Stage::processes:
        state.processTicks=gxProcessTicks;state.processCalls=gxProcessCalls;
        enter(Stage::sections);break;
    case Stage::sections:
        for(unsigned n=0;n<state.parts.ticks.size();++n){
            state.parts.ticks[n]=gxProfileTotals.ticks[n]-state.partsBefore.ticks[n];
            state.parts.calls[n]=gxProfileTotals.calls[n]-state.partsBefore.calls[n];
        }
        state.ownedSections=gxOwnedSections;state.ownedSectionsRefused=gxOwnedSectionsRefused;
        clearProcessTimers();
        enter(Stage::gpu);break;
    case Stage::gpu:enter(Stage::settle);break;
    default:
        // The settle picture began with the GPU idle: every measured list is in.
        duskGxOwnerTimesTake(state.ownerTicks.data(),state.ownerLists.data(),state.phaseTicks.data(),state.phaseLists.data());
        state.stage=Stage::ready;break;
    }
    if(state.stage!=Stage::ready)return false;
    state.have=true;
    if(state.forDump)return dumpRequested;
    // Not part of a dump: keep the text and go back to ordinary pictures.
    keep();
    if(state.plan.reference)features::setReference(false);
    features::setWithout(0);
    holdGxWorkerTrials(false);
    state.stage=Stage::idle;state.have=false;
    return false;
}
// After the dump was written (or refused): the ordinary pictures again.
inline void finish(){
    if(state.stage==Stage::idle)return;
    duskGxOwnerTiming=0;gxOwnerCounting=false;
    platformSetGpuDiagnostics(basePhases,baseDrawTimes);
    holdGxWorkerTrials(false);
    state.stage=Stage::idle;state.have=false;
}
// A measurement its zone outlived (the automatic test): nothing of it is kept,
// the improvements and the diagnostics go back to what they were. A dump's
// measurement is not touched (finish() ends it).
inline void abandon(){
    if(state.stage==Stage::idle||state.forDump)return;
    duskGxOwnerTiming=0;gxOwnerCounting=false;
    platformSetGpuDiagnostics(basePhases,baseDrawTimes);
    if(state.plan.reference)features::setReference(false);
    features::setWithout(0);
    holdGxWorkerTrials(false);
    state.stage=Stage::idle;state.have=false;
}
// Appends the waiting measurements to `path` and forgets them. SD work: call
// where a pause does not land inside something measured.
inline bool flush(const char* path){
    if(!pendingBytes)return false;
    bool written=false;
    if(auto* file=std::fopen(path,"ab")){
        written=std::fwrite(pending,1,pendingBytes,file)==pendingBytes;
        if(pendingRefused)std::fprintf(file,"\n[measurements refused] count=%u reason=buffer-full\n",pendingRefused);
        std::fclose(file);
    }
    pendingBytes=0;pendingRefused=0;
    return written;
}
// One measurement as text ([measure ...] sections). Times in milliseconds
// per picture of the stage they come from.
inline void write(std::FILE* file){
    if(!state.have)return;
    const double scale=1000.0/double(nativeTickFrequency);
    std::fprintf(file,"\n[measure]\nversion=2\nframes=%llu-%llu\nkind=%s\nreference=%d\nwithout=%s\nworker_rested=%u\nfeatures=%s\n",
        static_cast<unsigned long long>(state.firstFrame),static_cast<unsigned long long>(state.lastFrame),
        state.forDump?"dump":"burst",int(state.plan.reference),features::maskText(state.plan.without),state.workerRested,features::text());
    if(state.label[0])std::fprintf(file,"label=%s\n",state.label);
    std::fprintf(file,"note=ms per picture; processes: exclusive times with the process timers alone; sections: inclusive, "
        "nested, their pictures are slower by the timers themselves; gpu: one draw per GPU list, time summed by owner\n");
    for(unsigned n=0;n<stageCount;++n){
        const auto& s=state.sums[n];
        if(!s.pictures)continue;
        const double per=scale/s.pictures;
        std::fprintf(file,"stage=%s pictures=%u ticks_per_picture=%.2f draws=%.1f loop_ms=%.2f exec_ms=%.2f update_ms=%.2f draw_ms=%.2f paint_ms=%.2f "
            "audio_call_ms=%.2f gpu_lists_ms=%.2f gpu_copies_ms=%.2f gpu_idle_ms=%.2f gpu_wait_ms=%.2f\n",stageNames[n],s.pictures,
            double(s.ticks)/s.pictures,double(s.draws)/s.pictures,s.loop*per,s.execution*per,s.update*per,s.draw*per,s.paint*per,
            s.audio*per,s.gpuLists*per,s.gpuCopies*per,s.gpuIdle*per,s.gpuWait*per);
    }
    if(const unsigned count=state.sums[unsigned(Stage::processes)-1].pictures){
        const double per=scale/count;
        std::fprintf(file,"\n[measure processes] profile execute_ms calls draw_ms calls paint_ms calls replay_ms calls (profile -1: nobody's model)\n");
        std::uint64_t total[gxProcessKinds]{};
        auto row=[&](int profile,unsigned owner,bool named){
            const std::uint32_t t[4]{named?state.processTicks[0][profile]:0u,named?state.processTicks[1][profile]:0u,
                state.processTicks[2][owner],state.processTicks[3][owner]};
            const unsigned c[4]{named?state.processCalls[0][profile]:0u,named?state.processCalls[1][profile]:0u,
                state.processCalls[2][owner],state.processCalls[3][owner]};
            if(!(c[0]|c[1]|c[2]|c[3]))return;
            for(unsigned k=0;k<4;++k)total[k]+=t[k];
            std::fprintf(file,"%d %.3f %.1f %.3f %.1f %.3f %.1f %.3f %.1f\n",profile,t[0]*per,double(c[0])/count,t[1]*per,double(c[1])/count,
                t[2]*per,double(c[2])/count,t[3]*per,double(c[3])/count);
        };
        row(-1,0,false);
        for(unsigned profile=0;profile+1<gxProcessProfileCapacity;++profile)row(int(profile),profile+1,true);
        std::fprintf(file,"sum execute_ms=%.3f draw_ms=%.3f paint_ms=%.3f replay_ms=%.3f\n",total[0]*per,total[1]*per,total[2]*per,total[3]*per);
    }
    if(const unsigned count=state.sums[unsigned(Stage::sections)-1].pictures){
        const double per=scale/count;
        std::fprintf(file,"\n[measure sections] ms calls name\n");
        for(const auto& e:state.named)if(e.calls&&e.name)std::fprintf(file,"%.3f %.1f %s\n",e.ticks*per,double(e.calls)/count,e.name);
        static constexpr const char* partNames[]{"command","texture","decode","vertices","submit","wait","stream","copy","encode","vertexDecode",
            "vertexTransform","vertexLighting","vertexFog","gamePaint","gameUpdate","gameDraw","arrayValidate","vertexRigid","vertexCpu",
            "vertexPosition","fusedConvert","fusedCheckedFlush"};
        static_assert(sizeof(partNames)/sizeof(partNames[0])==unsigned(GxProfilePart::count));
        std::fprintf(file,"\n[measure process sections] kind profile ms calls name (kind 0: execute, 1: draw; inclusive, ms per picture of the sections stage)\n");
        for(const auto& e:state.ownedSections){
            if(!e.key)continue;
            const unsigned key=e.key-1,owner=key>>8,index=key&0xff;
            if(!owner||index>=state.named.size()||!state.named[index].name)continue;
            std::fprintf(file,"%u %u %.3f %.1f %s\n",(owner-1)/1024,(owner-1)%1024,e.ticks*per,double(e.calls)/count,state.named[index].name);
        }
        if(state.ownedSectionsRefused)std::fprintf(file,"refused %u\n",state.ownedSectionsRefused);
        std::fprintf(file,"\n[measure parts] ms calls name\n");
        for(unsigned n=0;n<unsigned(GxProfilePart::count);++n)if(state.parts.calls[n])
            std::fprintf(file,"%.3f %.1f %s\n",state.parts.ticks[n]*per,double(state.parts.calls[n])/count,partNames[n]);
    }
    if(const unsigned count=state.sums[unsigned(Stage::gpu)-1].pictures){
        const double per=scale/count;
        std::fprintf(file,"\n[measure gpu owners] profile ms lists draws indices (per picture; profile -1: nobody's model)\n");
        std::uint64_t ticks=0,indices=0;unsigned draws=0;
        for(unsigned owner=0;owner<gxProcessProfileCapacity;++owner){
            if(!state.ownerLists[owner]&&!state.ownerDraws[owner])continue;
            ticks+=state.ownerTicks[owner];indices+=state.ownerIndices[owner];draws+=state.ownerDraws[owner];
            std::fprintf(file,"%d %.3f %.1f %.1f %.0f\n",int(owner)-1,state.ownerTicks[owner]*per,double(state.ownerLists[owner])/count,
                double(state.ownerDraws[owner])/count,double(state.ownerIndices[owner])/count);
        }
        std::fprintf(file,"sum ms=%.3f draws=%.1f indices=%.0f casters=%.0f\n",ticks*per,double(draws)/count,double(indices)/count,
                     double(state.casterIndices)/count);
        std::fprintf(file,"\n[measure gpu phases] phase ms lists (per picture)\n");
        for(unsigned phase=0;phase<gpuPhases;++phase)if(state.phaseLists[phase])
            std::fprintf(file,"%u %.3f %.1f\n",phase,state.phaseTicks[phase]*per,double(state.phaseLists[phase])/count);
    }
}
}
