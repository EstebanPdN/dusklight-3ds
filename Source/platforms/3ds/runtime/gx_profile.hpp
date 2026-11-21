#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
namespace dusk::native {
enum class GxProfilePart:unsigned { command,texture,decode,vertices,submit,wait,stream,copy,encode,vertexDecode,vertexTransform,vertexLighting,vertexFog,gamePaint,gameUpdate,gameDraw,arrayValidate,vertexRigid,vertexCpu,vertexPosition,fusedConvert,fusedCheckedFlush,count };
struct GxProfileTotals {
    std::array<std::uint64_t,unsigned(GxProfilePart::count)> ticks{},calls{};
    std::uint64_t decodedBytes{},evictions{};
    std::uint64_t vertices{},litVertices{},fogVertices{},rigidVertices{},rigidDecodedVertices{},compactVertices{},positionOnlyVertices{};
};
inline GxProfileTotals gxProfileTotals;
inline bool gxSparseVertexProfiling{};
inline bool gxProfileEveryFrame{};
inline bool gxVertexLayoutTracing{};
// Diagnostic: restrict named/part timers to one draw class (-1 all, 2 ortho, 3 persp).
inline int gxProfileClassFilter=-1;
std::uint64_t gxProfileTick();
struct GxProfileScope {
#if defined(__3DS__)
    unsigned part;std::uint64_t start;bool active;
    explicit GxProfileScope(GxProfilePart p,bool enabled=gxSparseVertexProfiling||gxProfileEveryFrame):part(unsigned(p)),start(enabled?gxProfileTick():0),active(enabled){}
    ~GxProfileScope(){if(active){gxProfileTotals.ticks[part]+=gxProfileTick()-start;++gxProfileTotals.calls[part];}}
#else
    explicit GxProfileScope(GxProfilePart,bool=true){}
#endif
};
// Sparse native painter attribution. Entries are static call sites, not assets.
struct GxNamedProfileEntry {const char* name{};std::uint64_t ticks{},calls{};};
// 192: 125 names were in use (Alpha 14 m3 logs) and logic.txt split adds 12.
inline std::array<GxNamedProfileEntry,192> gxNamedProfiles{};
// Draw classification on sparse frames: 0 static rigid GPU, 1 dynamic rigid
// GPU, 2 orthographic CPU (2D), 3 perspective CPU, 4 culled/rejected/other.
inline std::array<std::uint64_t,5> gxDrawClassTicks{},gxDrawClassCount{};
// Sparse frames: the vertex decodes of the multi-texture route by vertex
// format (the plan's attribute list), to see which draws the decode time of a
// picture belongs to. `reused` counts the draws answered by the static cache.
struct GxDecodeCensusEntry {std::uint32_t kind{},draws{},vertices{},reused{};std::uint64_t ticks{};char text[76]{};};
inline std::array<GxDecodeCensusEntry,12> gxDecodeCensus{};
// Serialized FIFO commands executed through the incremental stream, by opcode.
inline std::array<std::uint32_t,256> gxFifoOpcodeCounts{},gxFifoOpcodeBytes{};
// Atomic: the second-core worker (gx_worker.hpp) can meet a scope for the
// first time while the game thread meets another.
inline std::atomic<unsigned> gxNamedProfileCount{};
// Diagnostic: first gate that kept a perspective draw off the GPU paths.
inline std::array<std::uint64_t,16> gxPerspCpuGate{},gxPerspCpuGateVertices{};
// The same since the start, never cleared (the automatic test's zone reports).
inline std::array<std::uint64_t,16> gxPerspCpuGateTotal{},gxPerspCpuGateVerticesTotal{};
// Per-process attribution: ticks and calls on the profiled pictures (the
// sparse diagnostic frames, and the measured dump's burst: gxProcessProfiling).
// Kind 0: a process profile's execute method; 1: its draw method (the models
// it enters); both indexed by profile name and exclusive (a scene that runs
// its children inside its own method is not charged for them). Kind 2: the
// game thread painting the packets of the models a process entered; 3: the
// thread that owns the GPU drawing them (the second-core worker or a
// takeover); both indexed by owner (profile name + 1; 0: nobody's model, as
// the sky, grass, particles and the 2D lists).
inline constexpr unsigned gxProcessProfileCapacity=800,gxProcessKinds=4;
inline std::array<std::array<std::uint32_t,gxProcessProfileCapacity>,gxProcessKinds> gxProcessTicks{};
inline std::array<std::array<std::uint16_t,gxProcessProfileCapacity>,gxProcessKinds> gxProcessCalls{};
// Measured dump (game_startup.cpp): the process timers alone, every picture.
inline bool gxProcessProfiling{};
// Ticks of the process scopes that ended inside the open one (game thread).
inline std::uint64_t gxProcessChildTicks{};
// Owner (profile name + 1) of what is being drawn. Entry: the process whose
// draw method runs, kept on each model it enters (J3DModel::mNativeOwner).
// Paint: the model whose shape the game thread issues. Replay: the queued
// shape the GPU-owning thread draws. The GPU job of a one-draw command list
// carries the owner of its draw (gpu_sync_ctru.cpp, ctru_gxqueue.c).
// With gxOwnerCounting (the measured pictures that send one draw per list):
// the draws and indices each owner sent (every C3D_DrawElements passes the
// wrapper of gpu_canary_ctru.cpp, on the thread that owns the GPU).
inline bool gxOwnerCounting{};
inline std::array<std::uint32_t,gxProcessProfileCapacity> gxOwnerIndices{};
inline std::array<std::uint16_t,gxProcessProfileCapacity> gxOwnerDraws{};
inline std::uint16_t gxEntryOwner{},gxPaintOwner{},gxReplayOwner{};
inline bool gxReplaying{};
inline unsigned gxDrawOwnerNow(){return gxReplaying?gxReplayOwner:gxPaintOwner;}
// The shadow caster pass (gx_pica.cpp runCasterPass) is running: its indices
// are also summed apart (the shadow image's share of the vertex work).
inline bool gxCasterPassNow{};
inline std::uint32_t gxCasterIndices{};
inline void gxCountOwnerDraw(int indices){
    if(!gxOwnerCounting)[[likely]]return;
    const unsigned owner=gxDrawOwnerNow()<gxProcessProfileCapacity?gxDrawOwnerNow():0;
    gxOwnerIndices[owner]+=std::uint32_t(indices>0?indices:0);++gxOwnerDraws[owner];
    if(gxCasterPassNow)gxCasterIndices+=std::uint32_t(indices>0?indices:0);
}
// Tick sections by process (sections stage): the game-thread sections
// (collision, model calc and entry, camera...) are also summed by the process
// whose execute or draw method they run in, to see where a heavy method's
// time goes. Owner: kind (0 execute, 1 draw) * 1024 + profile + 1; 0 outside
// any process method. Game thread only: written by the process scopes, read
// by GxTickProfileScope.
inline std::uint16_t gxSectionOwner{};
struct GxOwnedSection {std::uint32_t key{},ticks{},calls{};};
inline constexpr unsigned gxOwnedSectionCapacity=1024;
inline std::array<GxOwnedSection,gxOwnedSectionCapacity> gxOwnedSections{};
inline unsigned gxOwnedSectionsRefused{};
inline void gxOwnSection(unsigned index,std::uint64_t ticks){
    const std::uint32_t key=(std::uint32_t(gxSectionOwner)<<8|index)+1;
    unsigned slot=(key*2654435761u)>>22;
    for(unsigned probe=0;probe<32;++probe,slot=(slot+1)&(gxOwnedSectionCapacity-1)){
        auto& e=gxOwnedSections[slot];
        if(e.key==key){e.ticks+=std::uint32_t(ticks);++e.calls;return;}
        if(!e.key){e.key=key;e.ticks=std::uint32_t(ticks);e.calls=1;return;}
    }
    ++gxOwnedSectionsRefused;
}
struct GxProcessProfileScope {
#if defined(__3DS__)
    unsigned kind,profile;std::uint64_t start{},outerChildren{};std::uint16_t outerOwner{},outerSectionOwner{};bool active{};
    GxProcessProfileScope(unsigned k,int name):kind(k),profile(unsigned(name)),
        active((gxSparseVertexProfiling||gxProcessProfiling)&&name>=0&&unsigned(name)<gxProcessProfileCapacity){
        if(kind==1){outerOwner=gxEntryOwner;gxEntryOwner=name>=0?std::uint16_t(name+1):std::uint16_t(0);}
        if(active){
            outerChildren=gxProcessChildTicks;gxProcessChildTicks=0;
            // Execute and draw methods run on the game thread; the replay scope
            // (kind 3) runs on the GPU-owning thread and must not touch it.
            if(kind<2){outerSectionOwner=gxSectionOwner;gxSectionOwner=std::uint16_t(kind*1024+profile+1);}
            start=gxProfileTick();
        }
    }
    ~GxProcessProfileScope(){
        if(active){
            const auto elapsed=gxProfileTick()-start;
            gxProcessTicks[kind][profile]+=std::uint32_t(elapsed-std::min(elapsed,gxProcessChildTicks));++gxProcessCalls[kind][profile];
            gxProcessChildTicks=outerChildren+elapsed;
            if(kind<2)gxSectionOwner=outerSectionOwner;
        }
        if(kind==1)gxEntryOwner=outerOwner;
    }
#else
    GxProcessProfileScope(unsigned,int){}
#endif
};
inline unsigned registerGxNamedProfile(const char* name){
    const unsigned index=gxNamedProfileCount.fetch_add(1);
    if(index<gxNamedProfiles.size())gxNamedProfiles[index].name=name;
    return index;
}
inline void resetGxNamedProfiles(){for(auto& e:gxNamedProfiles){e.ticks=0;e.calls=0;}}
struct GxNamedProfileScope {
#if defined(__3DS__)
    unsigned index;std::uint64_t start{};bool active{};
    explicit GxNamedProfileScope(unsigned value):index(value),active(gxSparseVertexProfiling&&value<gxNamedProfiles.size()){
        if(active)start=gxProfileTick();
    }
    ~GxNamedProfileScope(){if(active){auto& e=gxNamedProfiles[index];e.ticks+=gxProfileTick()-start;++e.calls;}}
#else
    explicit GxNamedProfileScope(unsigned){}
#endif
};

// A named section of the game thread's logic and draw phase (d_tick_profile.h):
// also summed by the process method it runs in.
struct GxTickProfileScope {
#if defined(__3DS__)
    unsigned index;std::uint64_t start{};bool active{};
    explicit GxTickProfileScope(unsigned value):index(value),active(gxSparseVertexProfiling&&value<gxNamedProfiles.size()){
        if(active)start=gxProfileTick();
    }
    ~GxTickProfileScope(){
        if(active){
            const auto elapsed=gxProfileTick()-start;
            auto& e=gxNamedProfiles[index];e.ticks+=elapsed;++e.calls;
            if(gxSectionOwner)gxOwnSection(index,elapsed);
        }
    }
#else
    explicit GxTickProfileScope(unsigned){}
#endif
};

// Explicitly stopped span for regions with early returns.
struct GxNamedProfileSpan {
#if defined(__3DS__)
    unsigned index;std::uint64_t start{};bool active{};
    explicit GxNamedProfileSpan(unsigned value):index(value),active(gxSparseVertexProfiling&&value<gxNamedProfiles.size()){
        if(active)start=gxProfileTick();
    }
    void stop(){if(active){auto& e=gxNamedProfiles[index];e.ticks+=gxProfileTick()-start;++e.calls;active=false;}}
    ~GxNamedProfileSpan(){stop();}
#else
    explicit GxNamedProfileSpan(unsigned){}
    void stop(){}
#endif
};
}
