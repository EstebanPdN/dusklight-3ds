#pragma once
// Wide matrix palettes for skinned J3D shapes (features::Id::skinPalette).
//
// GX has ten position/normal matrix slots, so J3D draws a skinned shape as a
// row of matrix groups: each group loads up to ten matrices into the slots
// (a slot it does not load keeps the previous group's matrix) and calls its
// display list, whose vertices name a slot each. Every group was its own
// draw: a matrix load and a queue entry on the game thread, a replay on the
// second core, a job on the GPU. Link is 52 of them per picture.
//
// The multi-texture vertex program (gx_rigid_mt.v.pica) reads the rows of a
// vertex's matrix from its record (v4), so it takes any palette the uniform
// rows hold. Here a matrix takes three rows, its position rows, which are also
// its normal rows (the shader normalizes the transformed normal):
// - exact: J3D loads the position matrix itself as the normal matrix when its
//   scale flag is 1 (J3DShapeMtxConcatView::loadNrmMtx), so those rows hold
//   the values a group draw uploads as normal rows;
// - bounded: otherwise J3D loads the inverse transpose of its 3x3 (weighted
//   envelopes, scaled bones). The merged draw then lights with the direction
//   of A n instead of A^-T n (A the 3x3), at most normalAngleLimit apart
//   (normalSineBound); a run with an entry over the limit draws its groups
//   the original way that picture, with J3D's own normals.
// A run of consecutive groups whose matrices fit becomes one draw:
// - its display list is the groups' lists one after the other (primitives
//   only), every vertex's matrix byte naming its palette entry k as 3k;
// - the game thread computes each matrix once, as the group loads would, and
//   writes them into the port's skin palette in the XF bank (xfMatrices),
//   which the queue carries to the second core with the other register words;
// - the drawing side places entry k at row(k) of the program's palette rows.
// Same vertices, same matrices, same triangles in the same order; the same
// normals for exact entries, normals within the limit for bounded ones.
//
// Measured on the disc (Tools/3ds/model_prep.py --skin): with three rows per
// matrix the existing program's free rows take 17-20 matrices, which leaves
// 6,850 of the 12,156 skinned group draws (Link's 51 -> 26). A new program
// with a slimmer fixed layout would take 6,047, one to three draws fewer per
// model, while every switch between two programs re-sends the uniform rows:
// no new program. Six rows per matrix (the Alpha 14 merge) leave 11,528.
//
// Pure code only: the game side (J3DNativeSkinPalette.h), the drawing side
// (gx_command_draw.cpp) and the Mac survey (Tools/3ds/model_prep_skin.cpp)
// plan with the same functions, so the survey's draw counts are the runtime's.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace dusk::native::skin {
constexpr unsigned slots=10;
constexpr unsigned maxMatrices=32;
constexpr unsigned maxGroups=96;
constexpr unsigned maxRuns=48;
// Groups one merged list holds (its header lists them for the group-by-group
// fallback of the drawing side).
constexpr unsigned maxRunGroups=16;
// The command processor's batch caps (runtime/gx_commands.cpp drawBatch, as
// Tools/3ds/model_prep_bmd.hpp simulateDraws counts them): a merged list is
// planned to stay one batch, so it is one draw.
constexpr std::uint32_t batchVertices=4096,batchBytes=64*1024,batchIndices=16*1024;

// XF words of the skin palette. The bank's XF array has 0x1058 words; GX has
// no register or matrix memory at 0x100-0x3ff, so the port keeps the palette
// there. Like the slot matrices these words are written for each draw and
// read by that draw: they stay out of the state epoch and the state key
// (gx_commands.cpp writeXf), and the queue's bank log carries them.
constexpr unsigned xfHeader=0x100;     // headerMagic<<16 | entries
constexpr unsigned xfList=0x101;       // generation of the merged list the palette belongs to
constexpr unsigned xfMatrices=0x104;   // 12 words (the 3x4 position rows) per entry
constexpr unsigned xfEnd=xfMatrices+12*maxMatrices;
constexpr std::uint32_t headerMagic=0x534bu;
static_assert(xfEnd<=0x400);
constexpr bool inPaletteXf(std::uint32_t address,std::uint32_t count){return address>=xfHeader&&address+count<=xfEnd;}
// Generations of merged lists: their own space (vertex data counts up from 1,
// instance-batch lists have the top bit), never reused.
constexpr std::uint32_t generationBit=0x40000000u;
constexpr bool mergedGeneration(std::uint32_t generation){return (generation&0xc0000000u)==generationBit;}

// Uniform rows of entry k in the multi-texture program's layout (gx_rigid_
// vertex.hpp): rows 0-5 (the first matrix of a group draw) hold entries 0 and
// 1, then three rows each from paletteRow up to the lowest texture unit's
// rows (RigidVertexPlan::paletteLimit). paletteRow is the first row after
// four lights (from row 19, four rows each), fixed so that the rows baked into
// the resident vertices do not move when lights come and go; a draw with more
// lights is refused (drawn group by group). Rows 6-18 (projection, colours,
// fog) and the lights are never palette rows; the shadow caster pass uploads
// its tile projection to rows 6-9.
// The angle between a bounded entry's lighting normal and J3D's: with
// A = U S V^T and m = V^T n, cos t = 1/(|S m| |S^-1 m|) >= 2k/(1+k^2) for
// k = s_max/s_min (Kantorovich), so sin t <= (k^2-1)/(k^2+1). With
// G = A^T A = c (I + E), c = trace(G)/3, every eigenvalue of G (s^2) is in
// c [1-e, 1+e] for e >= |E|_2, so sin t <= e; E is symmetric with trace 0,
// so |E|_2 <= sqrt(2/3) |E|_F. normalSineSquared returns that e^2 (>= 1: no
// bound, the matrix is singular or far from a scaled rotation). About 30
// multiplications, no root. A 50/50 blend of two bones bent by b keeps
// s_max = 1 and s_min = cos(b/2): e = 2(1-c^2)/(1+2c^2) with c = cos(b/2), so
// 4 degrees allows b up to 36 degrees (the true angle is then 3 degrees).
inline float normalSineSquared(const float (*m)[4]){
    float g[3][3];
    for(unsigned i=0;i<3;++i)for(unsigned j=i;j<3;++j)g[i][j]=m[0][i]*m[0][j]+m[1][i]*m[1][j]+m[2][i]*m[2][j];
    const float c=(g[0][0]+g[1][1]+g[2][2])*(1.0f/3.0f);
    if(!(c>0.0f))return 1.0f;
    const float d0=g[0][0]-c,d1=g[1][1]-c,d2=g[2][2]-c;
    const float f2=d0*d0+d1*d1+d2*d2+2.0f*(g[0][1]*g[0][1]+g[0][2]*g[0][2]+g[1][2]*g[1][2]);
    const float e2=f2*(2.0f/3.0f)/(c*c);
    return e2==e2?e2:1.0f;
}
// The largest angle a bounded entry may light with, as its sine (4 degrees:
// see the census line of J3DNativeSkinPalette.h and the Alpha 21 report).
constexpr float normalSineLimit=0.06976f;
constexpr unsigned maxLights=4;
constexpr unsigned paletteRow=19+4*maxLights;
constexpr unsigned row(unsigned k){return k<2?3*k:paletteRow+3*(k-2);}
// Entries that fit below `limit` (paletteLimit: 89, 85 or 81 for one, two or
// three texture units with rows).
constexpr unsigned capacity(unsigned limit){
    const unsigned n=limit>=paletteRow?2+(limit-paletteRow)/3:2;
    return n<maxMatrices?n:maxMatrices;
}
constexpr unsigned capacityForUnits(unsigned units){
    units=units<1?1:units>3?3:units;
    return capacity(81+4*(3-units));
}
static_assert(capacityForUnits(1)==20&&capacityForUnits(2)==18&&capacityForUnits(3)==17);
// Uniform rows a draw of n entries reaches (the budget check: never past the
// unit rows, never past the 94 rows the programs declare).
constexpr unsigned rowsReached(unsigned n){return n?row(n-1)+3:0;}
static_assert(rowsReached(capacityForUnits(1))<=89&&rowsReached(capacityForUnits(3))<=81);

// One matrix group of a shape: its display list and what its load puts in
// each slot (a use-matrix index, 0xffff: the slot keeps the previous group's).
struct GroupInput {
    const std::uint8_t* list{};std::uint32_t bytes{};
    std::array<std::uint16_t,slots> loads{};
};
// The vertex of the shape's lists: bytes per vertex and where its matrix
// byte is (every other attribute indexed: J3DShape vertex descriptors).
struct VertexLayout {std::uint32_t stride{},matrixAt{};};

struct Run {
    std::uint16_t firstGroup{},groups{};
    std::uint8_t matrices{};
    std::uint32_t vertices{},indices{},listBytes{}; // raw vertices, triangle indices, merged list bytes before padding
    std::array<std::uint32_t,maxMatrices> keys{};    // the matrix of each palette entry
};
struct ShapePlan {
    unsigned groups{},runs{};
    std::array<Run,maxRuns> run{};
    std::array<std::uint8_t,maxGroups> runOf{};
    std::array<std::array<std::uint8_t,slots>,maxGroups> entry{}; // slot -> palette entry in its run, 0xff unused
};
enum class Refusal : std::uint8_t {none,groups,layout,list,unloaded,capacity,runs,count};
inline const char* refusalName(Refusal r){
    static constexpr const char* names[unsigned(Refusal::count)]{"none","groups","layout","list","unloaded","capacity","runs"};
    return names[unsigned(r)<unsigned(Refusal::count)?unsigned(r):0];
}

// What one group's list holds: triangle-kind primitives of VAT 0 and NOP
// padding (anything else keeps the shape apart), and the slots its vertices
// name.
struct GroupFacts {
    std::uint32_t vertices{},indices{},primitiveBytes{};
    std::uint16_t usedSlots{};
};
inline bool triangleKind(std::uint8_t op,std::uint32_t count,std::uint32_t& indices){
    switch(op&0xf8){
    case 0x90:if(count<3||count%3)return false;indices=count;return true;
    case 0x80:if(count<4||count%4)return false;indices=count/4*6;return true;
    case 0x98:case 0xa0:if(count<3)return false;indices=(count-2)*3;return true;
    }
    return false;
}
inline bool readGroup(const std::uint8_t* list,std::uint32_t bytes,const VertexLayout& layout,GroupFacts& out){
    out={};
    if(!list||!layout.stride||layout.matrixAt>=layout.stride)return false;
    for(std::uint32_t at=0;at<bytes;){
        const std::uint8_t op=list[at];
        if(op==0){++at;continue;}
        if((op&7)||at+3>bytes)return false;
        const std::uint32_t count=(std::uint32_t(list[at+1])<<8)|list[at+2];
        const std::uint64_t size=3+std::uint64_t(count)*layout.stride;
        std::uint32_t indices=0;
        if(!triangleKind(op,count,indices)||size>bytes-at)return false;
        for(std::uint32_t v=0;v<count;++v){
            const unsigned index=list[at+3+v*layout.stride+layout.matrixAt];
            if(index%3||index/3>=slots)return false;
            out.usedSlots|=std::uint16_t(1u<<(index/3));
        }
        out.vertices+=count;out.indices+=indices;out.primitiveBytes+=std::uint32_t(size);
        at+=std::uint32_t(size);
    }
    return out.vertices!=0&&out.vertices<=batchVertices&&out.vertices*std::uint64_t(layout.stride)<=batchBytes&&out.indices<=batchIndices;
}

// Runs of consecutive groups: a group joins the open run while the run stays
// one batch, holds at most maxRunGroups groups and the distinct matrices its
// vertices name (keyOf: use-matrix index -> matrix identity) fit `capacity`.
// Every group is in a run (a run of one group is drawn as the group was), so
// no group depends on slots another run loaded. A shape whose lists hold
// anything but triangle primitives, or whose vertices name a slot no earlier
// load of the shape filled (the original would read whatever the previous
// shape left there), is refused whole: it stays on the original path.
template<class KeyOf>
Refusal planShape(std::span<const GroupInput> groups,const VertexLayout& layout,KeyOf keyOf,unsigned capacity,ShapePlan& out){
    out.groups=unsigned(groups.size());out.runs=0;
    if(groups.size()<2||groups.size()>maxGroups)return Refusal::groups;
    if(!layout.stride||layout.matrixAt>=layout.stride)return Refusal::layout;
    if(capacity<slots||capacity>maxMatrices)return Refusal::capacity;
    std::array<GroupFacts,maxGroups> facts{};
    std::array<std::array<std::uint32_t,slots>,maxGroups> keys{};
    std::array<std::uint32_t,slots> current{};
    std::uint16_t filled=0;
    for(std::size_t g=0;g<groups.size();++g){
        if(!readGroup(groups[g].list,groups[g].bytes,layout,facts[g]))return Refusal::list;
        for(unsigned s=0;s<slots;++s)if(groups[g].loads[s]!=0xffff){current[s]=keyOf(groups[g].loads[s]);filled|=std::uint16_t(1u<<s);}
        if(facts[g].usedSlots&~filled)return Refusal::unloaded;
        keys[g]=current;
    }
    // Adds group g's matrices to the run's set; false (set unchanged) when
    // they do not fit.
    auto join=[&](std::array<std::uint32_t,maxMatrices>& set,unsigned& n,std::size_t g){
        auto grown=set;unsigned count=n;
        for(unsigned s=0;s<slots;++s)if(facts[g].usedSlots&(1u<<s)){
            bool seen=false;for(unsigned i=0;i<count&&!seen;++i)seen=grown[i]==keys[g][s];
            if(seen)continue;
            if(count>=capacity)return false;
            grown[count++]=keys[g][s];
        }
        set=grown;n=count;
        return true;
    };
    out.runOf.fill(0xff);
    for(auto& e:out.entry)e.fill(0xff);
    for(std::size_t g=0;g<groups.size();){
        if(out.runs==maxRuns)return Refusal::runs;
        Run r{};r.firstGroup=std::uint16_t(g);
        unsigned matrices=0;
        std::size_t h=g;
        for(;h<groups.size()&&h-g<maxRunGroups;++h){
            const auto& f=facts[h];
            if(r.vertices+f.vertices>batchVertices||(std::uint64_t(r.vertices)+f.vertices)*layout.stride>batchBytes||
               r.indices+f.indices>batchIndices)break;
            if(!join(r.keys,matrices,h))break;
            r.vertices+=f.vertices;r.indices+=f.indices;r.listBytes+=f.primitiveBytes;
        }
        if(h==g)return Refusal::capacity; // one group alone does not fit
        r.groups=std::uint16_t(h-g);r.matrices=std::uint8_t(matrices);
        for(std::size_t k=g;k<h;++k){
            out.runOf[k]=std::uint8_t(out.runs);
            for(unsigned s=0;s<slots;++s)if(facts[k].usedSlots&(1u<<s))
                for(unsigned i=0;i<matrices;++i)if(r.keys[i]==keys[k][s]){out.entry[k][s]=std::uint8_t(i);break;}
        }
        out.run[out.runs++]=r;
        g=h;
    }
    return Refusal::none;
}
// Bytes of a run's merged list: its primitives, padded with NOPs to 32.
inline std::uint32_t mergedBytes(const Run& r){return (r.listBytes+31)&~31u;}
// The merged list of run `run` into `out` (mergedBytes bytes): each group's
// primitives in order, every vertex's matrix byte naming its palette entry.
inline bool writeMerged(std::span<const GroupInput> groups,const VertexLayout& layout,const ShapePlan& plan,unsigned run,std::uint8_t* out){
    const Run& r=plan.run[run];
    std::uint8_t* at=out;
    for(unsigned g=r.firstGroup;g<unsigned(r.firstGroup+r.groups);++g){
        const auto* list=groups[g].list;const std::uint32_t bytes=groups[g].bytes;
        for(std::uint32_t i=0;i<bytes;){
            const std::uint8_t op=list[i];
            if(op==0){++i;continue;}
            const std::uint32_t count=(std::uint32_t(list[i+1])<<8)|list[i+2];
            const std::uint32_t size=3+count*layout.stride;
            if(std::uint32_t(at-out)+size>r.listBytes)return false;
            std::memcpy(at,list+i,size);
            for(std::uint32_t v=0;v<count;++v){
                auto& index=at[3+v*layout.stride+layout.matrixAt];
                const std::uint8_t k=plan.entry[g][index/3];
                if(k==0xff)return false;
                index=std::uint8_t(3*k);
            }
            at+=size;i+=size;
        }
    }
    if(std::uint32_t(at-out)!=r.listBytes)return false;
    std::memset(at,0,mergedBytes(r)-r.listBytes);
    return true;
}

// Written by the game thread in front of every merged list (its memory is
// never written again while a picture may still draw it), read by the
// drawing side: what the group-by-group fallback draws when a merged list
// cannot be drawn whole (a plan or decode the merged form refuses), and the
// capacity that drawing side found, which the game thread plans with next.
struct MergedHeader {
    std::uint32_t magic{},generation{};
    std::uint8_t groups{},entries{},pad[2]{};
    std::uint32_t listBytes{};
    // Drawing side: refusals of this list, and the most entries a draw of
    // its material held (0: none known yet). Relaxed: a refusal seen one
    // picture late costs one more fallback.
    std::atomic<std::uint32_t> refusals{};
    std::atomic<std::uint32_t> capacity{};
    struct Group {const std::uint8_t* list{};std::uint32_t bytes{};std::uint8_t entry[slots]{};std::uint8_t pad[2]{};};
    std::array<Group,maxRunGroups> group{};
    std::array<std::uint32_t,maxMatrices> keys{};
};
constexpr std::uint32_t mergedMagic=0x534b4d4cu;
// The header of a merged list, or null (not a merged list, or not this one).
inline MergedHeader* headerOf(const void* list,std::uint32_t generation){
    if(!list||!mergedGeneration(generation))return nullptr;
    auto* h=reinterpret_cast<MergedHeader*>(const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(list))-sizeof(MergedHeader));
    return h->magic==mergedMagic&&h->generation==generation?h:nullptr;
}
}
