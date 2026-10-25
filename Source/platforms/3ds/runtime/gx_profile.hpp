#pragma once
#include <array>
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
inline std::array<GxNamedProfileEntry,96> gxNamedProfiles{};
// Draw classification on sparse frames: 0 static rigid GPU, 1 dynamic rigid
// GPU, 2 orthographic CPU (2D), 3 perspective CPU, 4 culled/rejected/other.
inline std::array<std::uint64_t,5> gxDrawClassTicks{},gxDrawClassCount{};
// Serialized FIFO commands executed through the incremental stream, by opcode.
inline std::array<std::uint32_t,256> gxFifoOpcodeCounts{},gxFifoOpcodeBytes{};
inline unsigned gxNamedProfileCount{};
// Diagnostic: first gate that kept a perspective draw off the GPU paths.
inline std::array<std::uint64_t,16> gxPerspCpuGate{},gxPerspCpuGateVertices{};
inline unsigned registerGxNamedProfile(const char* name){
    const unsigned index=gxNamedProfileCount;
    if(index<gxNamedProfiles.size()){gxNamedProfiles[index].name=name;++gxNamedProfileCount;}
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
