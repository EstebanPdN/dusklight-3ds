#include "gx_pica.hpp"
#include "gx_geometry_pica.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gx_compact_cache.hpp"
#include "runtime/gx_fog_visibility.hpp"
#include "runtime/gx_rigid_vertex.hpp"
#include "runtime/gx_texture_commands.hpp"
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/gpu/enums.h>
// gpu.h declares libctru's C command-buffer functions (GPUCMD_Add). shbin.h,
// below, includes it too: include it first, with C linkage.
extern "C" {
#include <3ds/gpu/gpu.h>
#include <3ds/gpu/registers.h>
}
#include <3ds/gpu/shaderProgram.h>
extern "C" {
#include <3ds/allocator/linear.h>
#include <3ds/services/gspgpu.h>
#if defined(__3DS__)
#include <3ds/svc.h>
#endif
}
extern "C" {
#include <c3d/attribs.h>
#include <c3d/buffers.h>
#include <c3d/base.h>
}
#include <cstring>
#include <cmath>
#include <algorithm>
#include <span>
#include <array>
#include <new>
#include <vector>
extern "C" void OSReport(const char*,...);
namespace dusk::native {
void dropPendingCapture();
namespace {
// Vertex attribute and buffer configuration. Citro3D keeps the configuration
// it last sent in its context; C3D_GetAttrInfo and C3D_GetBufInfo return that
// copy and mark it for a complete re-send at the next draw (about 10 command
// words for the attributes and 40 for the buffers, each parsed again by the
// GPU guard), also when nothing changed. Alpha 6 rebuilt and re-sent both for
// nearly every draw. Here the wanted configuration is compared with Citro3D's
// copy instead: an equal one is left alone, and buffers that differ only in
// the first one (each draw's vertices) send that buffer's three registers and
// store them in the copy, so Citro3D's record still equals the hardware and a
// later complete re-send writes the same values. Comparing the copy itself,
// not a record of our own, also sees any other writer. After
// invalidateGxGeometryBindings (frame begin, render target, captures) the
// first draw goes through the getters again, as every draw did before.
// The two layouts: clip-space GpuVertex, and the compact rigid vertex (single
// and multi-texture records share it; only the stride differs).
std::array<C3D_AttrInfo,3> layouts{};bool layoutsBuilt{};
C3D_AttrInfo* attributeContext{};C3D_BufInfo* bufferContext{};
bool resendAttributes=true,resendBuffers=true;
// Citro3D's own encoding of the wanted buffers; only entry 0 is ever added,
// entries 1-11 stay as BufInfo_Init leaves them.
C3D_BufInfo wantedBuffers{};bool wantedBuffersReady{};
GxBindingStatistics bindings{};
void buildLayouts(){
    auto& clip=layouts[0];AttrInfo_Init(&clip);
    AttrInfo_AddLoader(&clip,0,GPU_FLOAT,4);AttrInfo_AddLoader(&clip,1,GPU_FLOAT,4);
    for(unsigned i=2;i<5;++i)AttrInfo_AddLoader(&clip,i,GPU_FLOAT,2);
    AttrInfo_AddLoader(&clip,5,GPU_FLOAT,1);
    // CompactRigidVertex(MT): position, normal, TEX0, TEX1 (the unused padding
    // of the single layout), matrix, color.
    auto& compact=layouts[1];AttrInfo_Init(&compact);
    AttrInfo_AddLoader(&compact,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&compact,1,GPU_SHORT,3);
    AttrInfo_AddLoader(&compact,2,GPU_SHORT,2);
    AttrInfo_AddLoader(&compact,5,GPU_SHORT,2);
    AttrInfo_AddLoader(&compact,4,GPU_BYTE,2);
    AttrInfo_AddLoader(&compact,3,GPU_UNSIGNED_BYTE,4);
    // CompactRigidVertexMTWide: the same six attributes, the second
    // coordinate four shorts wide (TEX1.st, third coordinate.st). The count
    // stays six: the multi-texture program reads six inputs, and a layout
    // with another count stopped the console PICA (2026-11-13).
    auto& wide=layouts[2];AttrInfo_Init(&wide);
    AttrInfo_AddLoader(&wide,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&wide,1,GPU_SHORT,3);
    AttrInfo_AddLoader(&wide,2,GPU_SHORT,2);
    AttrInfo_AddLoader(&wide,5,GPU_SHORT,4);
    AttrInfo_AddLoader(&wide,4,GPU_BYTE,2);
    AttrInfo_AddLoader(&wide,3,GPU_UNSIGNED_BYTE,4);
    layoutsBuilt=true;
}
// 0: clip-space GpuVertex, 1: compact records, 2: wide compact records.
bool setVertexLayout(unsigned kind){
    if(!layoutsBuilt)buildLayouts();
    const auto& want=layouts[kind];
    if(!resendAttributes&&attributeContext&&!std::memcmp(attributeContext,&want,sizeof(want))){++bindings.layoutsKept;return true;}
    auto* context=C3D_GetAttrInfo();
    if(!context)return false;
    *context=want;attributeContext=context;resendAttributes=false;++bindings.layoutsSent;return true;
}
bool setVertexBuffer(const void* vertices,std::size_t stride){
    auto& want=wantedBuffers;
    if(!wantedBuffersReady){BufInfo_Init(&want);wantedBuffersReady=true;}
    want.bufCount=0;
    if(BufInfo_Add(&want,vertices,ptrdiff_t(stride),6,0x543210)<0)return false;
    if(!resendBuffers&&bufferContext){
        auto& context=*bufferContext;
        if(!std::memcmp(&context,&want,sizeof(want))){++bindings.buffersKept;return true;}
        if(context.base_paddr==want.base_paddr&&context.bufCount==want.bufCount&&
           !std::memcmp(context.buffers+1,want.buffers+1,sizeof(want.buffers)-sizeof(want.buffers[0]))){
            // The first three words of Citro3D's own buffer block (C3Di_BufInfoBind).
            static_assert(sizeof(C3D_BufCfg)==3*sizeof(u32));
            GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET,reinterpret_cast<const u32*>(&want.buffers[0]),3);
            context.buffers[0]=want.buffers[0];++bindings.buffersFirstOnly;return true;
        }
    }
    auto* context=C3D_GetBufInfo();
    if(!context)return false;
    *context=want;bufferContext=context;resendBuffers=false;++bindings.buffersSent;return true;
}
// Persistent linear index pool for native shape records; reset only at a
// frame boundary (queued GPU work may still read it).
std::uint16_t* indexPool{};std::size_t indexPoolCapacity{},indexPoolUsed{};std::uint32_t indexPoolEpoch=1;bool indexPoolResetPending{};
// 1 MiB since Alpha 12 (512 KiB before). A full arena mid-picture waits for
// the whole GPU queue before reuse: console Alpha 11, F_SP104 r1, 6 waits per
// 300 pictures at about 27 ms each. The lowest linear free in that run was
// 3.3 MB, and compact textures (Alpha 12) give linear memory back.
constexpr std::size_t budget=1024*1024;
void* memory;
gx::GpuGeometryArena arena;
std::size_t flushedBytes{};
// HUD tape geometry (runtime/hud_tape.cpp): while a 2D pass is recorded, its
// reservations come from this second buffer, which no frame begin resets, so
// the recorded GPU commands can be run again in later pictures. It is written
// only at a recording, after every queued read of it completed (frame begin
// waits for the previous picture's GPU work). `active` is the arena the
// reservation in progress belongs to.
constexpr std::size_t tapeBudget=128*1024;
void* tapeMemory{};
gx::GpuGeometryArena tapeArena;
gx::GpuGeometryArena* active=&arena;
gx::FogVisibility fogVisibility;
// Where a screen recording returns to: the frame arena, or the HUD tape's
// when the screen is recorded inside a HUD tape recording.
gx::GpuGeometryArena* screenOuter=&arena;
std::size_t tapeFlushed{};
bool tapeOverflow{};
// Retained 2D screens: one buffer for every kept recording. screenArena is
// attached to the bytes after the last recording for the time of one
// recording (screenBase is where it starts in the buffer).
constexpr std::size_t screenBudget=512*1024;
void* screenMemory{};
gx::GpuGeometryArena screenArena;
// screenBase: end of the appended recordings. screenAt: where the recording
// in progress writes (screenBase, or a reused region).
std::size_t screenBase{},screenAt{},screenFlushed{};
bool screenOverflow{},screenAppending{};
bool kernelFlush{};
// Caster snapshots can outlive several reclaims of the frame arena. Each
// page has a stable GPU address, even when another page has to be allocated.
struct alignas(16) CasterGeometryPage {
    CasterGeometryPage* next{};
    std::size_t capacity{},used{},published{};
    std::byte* data(){return reinterpret_cast<std::byte*>(this+1);}
};
CasterGeometryPage* casterPages{};
struct StaticMesh {void* memory{};gx::GpuVertex* vertices{};u16* indices{};unsigned count{};};
std::array<StaticMesh,64> meshes{};
std::size_t staticBytes;
constexpr std::size_t staticBudget=4*1024*1024;
const char* failure="none";
bool reject(const char* why){failure=why;return false;}
// Hardware clip guard. The console PICA hung on the first title frame on
// CPU-transformed meshes that reached it with vertices behind the camera and
// screen coordinates up to two million viewports wide (a clamped tiny w).
// Azahar clips those on the host GPU and never showed it. Every CPU triangle
// is clipped here, in clip space, to w >= clipMinW and |x|,|y| <= guard * w;
// all attributes are linear in clip space, so visible pixels are unchanged.
constexpr float clipMinW=1.0f/1024,clipGuard=8;
struct ClipStats {unsigned triangles=0,clipped=0,dropped=0,added=0;};
ClipStats clipStats{};
float planeDistance(const gx::GpuVertex& v,unsigned plane){
    const float x=v.position[0],y=v.position[1],w=v.position[3];
    switch(plane){
        case 0:return w-clipMinW;
        case 1:return clipGuard*w-x;
        case 2:return clipGuard*w+x;
        case 3:return clipGuard*w-y;
        default:return clipGuard*w+y;
    }
}
gx::GpuVertex lerpVertex(const gx::GpuVertex& a,const gx::GpuVertex& b,float t){
    gx::GpuVertex r;
    for(unsigned k=0;k<4;++k){r.position[k]=a.position[k]+(b.position[k]-a.position[k])*t;r.color[k]=a.color[k]+(b.color[k]-a.color[k])*t;}
    for(unsigned u=0;u<3;++u)for(unsigned k=0;k<2;++k)r.texcoord[u][k]=a.texcoord[u][k]+(b.texcoord[u][k]-a.texcoord[u][k])*t;
    r.textureQ=a.textureQ+(b.textureQ-a.textureQ)*t;
    for(auto& c:r.color)c=std::min(1.0f,std::max(0.0f,c));
    return r;
}
// One bit per guard plane the vertex is beyond (the tests of planeDistance;
// a NaN counts as beyond).
inline std::uint8_t guardCode(const gx::GpuVertex& v){
    const float x=v.position[0],y=v.position[1],w=v.position[3],g=clipGuard*w;
    return std::uint8_t(unsigned(!(w-clipMinW>=0))|unsigned(!(g-x>=0))<<1|unsigned(!(g+x>=0))<<2|unsigned(!(g-y>=0))<<3|unsigned(!(g+y>=0))<<4);
}
// One polygon corner: an original vertex (index) or a new one per half.
struct ClipCorner {int original;gx::GpuVertex v[2];};
std::vector<gx::GpuVertex> clipExtra[2];
std::vector<std::uint16_t> clipIndices;
std::vector<std::uint8_t> clipCodes;
// 0: nothing to clip (draw the batch as is). 1: `out` holds the clipped mesh
// and the original reservation is committed unused. 2: nothing visible
// remains (original committed). -1: failure.
int clipForHardware(const gx::GpuGeometryBatch& batch,unsigned halves,gx::GpuGeometryBatch& out,unsigned indexCopies=1){
    const std::size_t count=batch.vertices.size()/halves;
    // One pass over the vertices; triangles are then sorted by their codes.
    clipCodes.resize(count);
    unsigned any=0;
    for(std::size_t i=0;i<count;++i)any|=clipCodes[i]=guardCode(batch.vertices[i]);
    if(!any)return 0;
    auto code=[&](std::uint16_t index){return index<count?clipCodes[index]:guardCode(batch.vertices[index]);};
    clipExtra[0].clear();clipExtra[1].clear();clipIndices.clear();
    std::array<ClipCorner,12> polygon,next;
    const auto sourceIndices=batch.indices.size()/indexCopies;
    for(std::size_t i=0;i+2<sourceIndices;i+=3){
        ++clipStats.triangles;
        const std::uint16_t* tri=batch.indices.data()+i;
        const unsigned a=code(tri[0]),b=code(tri[1]),c=code(tri[2]);
        if(!(a|b|c)){clipIndices.insert(clipIndices.end(),tri,tri+3);continue;}
        ++clipStats.clipped;
        // All three corners beyond one plane: nothing of the triangle is
        // inside (most clipped triangles; the plane-by-plane clip below would
        // reach the same empty polygon after interpolating whole vertices).
        if(a&b&c){++clipStats.dropped;continue;}
        unsigned n=3;
        for(unsigned k=0;k<3;++k){polygon[k].original=tri[k];for(unsigned h=0;h<halves;++h)polygon[k].v[h]=batch.vertices[h*count+tri[k]];}
        for(unsigned plane=0;plane<5&&n;++plane){
            unsigned m=0;
            for(unsigned k=0;k<n;++k){
                const auto& a=polygon[k];const auto& b=polygon[(k+1)%n];
                const float da=planeDistance(a.v[0],plane),db=planeDistance(b.v[0],plane);
                if(da>=0&&m<next.size())next[m++]=a;
                if((da>=0)!=(db>=0)&&m<next.size()){
                    const float t=da/(da-db);
                    ClipCorner c{-1,{}};for(unsigned h=0;h<halves;++h)c.v[h]=lerpVertex(a.v[h],b.v[h],t);
                    next[m++]=c;
                }
            }
            std::copy(next.begin(),next.begin()+m,polygon.begin());n=m;
        }
        if(n<3){++clipStats.dropped;continue;}
        std::array<std::size_t,12> index{};
        for(unsigned k=0;k<n;++k){
            if(polygon[k].original>=0){index[k]=std::size_t(polygon[k].original);continue;}
            index[k]=count+clipExtra[0].size();++clipStats.added;
            for(unsigned h=0;h<halves;++h)clipExtra[h].push_back(polygon[k].v[h]);
        }
        for(unsigned k=1;k+1<n;++k){clipIndices.push_back(std::uint16_t(index[0]));clipIndices.push_back(std::uint16_t(index[k]));clipIndices.push_back(std::uint16_t(index[k+1]));}
    }
    // Pairs keep each half 16-byte aligned: a vertex count multiple of four.
    std::size_t total=count+clipExtra[0].size();
    while(halves>1&&total%4){for(unsigned h=0;h<halves;++h)clipExtra[h].push_back(clipExtra[h].empty()?batch.vertices[h*count]:clipExtra[h].back());++total;}
    if(total*halves>65535||clipIndices.size()>196605/indexCopies)return -1;
    if(!active->commit(batch))return -1;
    if(clipIndices.empty())return 2;
    if(!reserveGxGeometry(total*halves,clipIndices.size()*indexCopies,out))return -1;
    for(unsigned h=0;h<halves;++h){
        std::copy(batch.vertices.begin()+h*count,batch.vertices.begin()+(h+1)*count,out.vertices.begin()+h*total);
        std::copy(clipExtra[h].begin(),clipExtra[h].end(),out.vertices.begin()+h*total+count);
    }
    std::copy(clipIndices.begin(),clipIndices.end(),out.indices.begin());
    out.qPolicy=batch.qPolicy;return 1;
}
}
bool initializeGxGeometry(){
    if(memory)return true;
    memory=linearMemAlign(budget,16);
    if(!memory)return reject("GX geometry arena allocation failed");
    if(!arena.attach({static_cast<std::byte*>(memory),budget})){
        linearFree(memory);memory=nullptr;return reject("GX geometry arena alignment invalid");
    }
    return true;
}
void destroyGxGeometry(){
    fogVisibility={};
    while(casterPages){
        auto* page=casterPages;
        casterPages=page->next;
        linearFree(page);
    }
    for(auto& mesh:meshes){if(mesh.memory)linearFree(mesh.memory);mesh={};}staticBytes=0;
    if(memory)linearFree(memory);
    memory=nullptr;arena={};flushedBytes=0;
    if(tapeMemory)linearFree(tapeMemory);
    tapeMemory=nullptr;tapeArena={};active=&arena;
    if(screenMemory)linearFree(screenMemory);
    screenMemory=nullptr;screenArena={};screenBase=0;
    attributeContext=nullptr;bufferContext=nullptr;resendAttributes=resendBuffers=true;
}
// Changes whenever the arena starts again from its beginning (new frame or a
// reclaim within the frame): a pointer into the arena taken under another
// number may now hold other geometry.
static std::uint32_t arenaGeneration{};
bool beginGxGeometryFrame(){
    if(!platformBeginGpuCommands()||!arena.begin())return reject("GX geometry frame begin invalid");
    ++arenaGeneration;
    for(auto* page=casterPages;page;page=page->next)page->used=page->published=0;
    // The previous queue completed: resident static geometry may be recycled.
    // A new play scene empties the pool (StaticCompactCache::clearAtStageChange)
    // and the resident indices with it, so neither fills across stages and
    // resets in play; every record using them carries an old epoch.
    bool stageClear=false;
    if(auto* cache=gx::staticCompactCache()){stageClear=cache->frameBegin();fogVisibility.reset(cache->epoch);}
    if(stageClear)indexPoolResetPending=true;
    invalidatePicaCommandState();
    ++gx::textureFrameCounter;
    if(clipStats.clipped&&gx::textureFrameCounter%30==0){
        static unsigned reports;
        if(reports<40){++reports;OSReport("Native hardware clip guard: triangles=%u clipped=%u dropped=%u added-vertices=%u\n",clipStats.triangles,clipStats.clipped,clipStats.dropped,clipStats.added);}
    }
    clipStats={};
    if(indexPoolResetPending){
        static unsigned lines; // diagnostics, at most 160 lines
        if(lines<160){++lines;OSReport("Native resident indices cleared reason=%s bytes-before=%u/%u\n",stageClear?"stage":"full",
            unsigned(indexPoolUsed*sizeof(std::uint16_t)),unsigned(indexPoolCapacity*sizeof(std::uint16_t)));}
        indexPoolUsed=0;indexPoolResetPending=false;++indexPoolEpoch;
    }
    dropPendingCapture(); // an unconsumed deferred capture is never read back
    flushedBytes=0;return true;
}
void setGxGeometryKernelFlush(bool enabled){kernelFlush=enabled;}
// The committed bytes of a tape recording in progress: its draws are in the
// command list like any others, so every submission publishes them too.
static bool flushTapeGeometry(){
    if(screenArena.inFrame()){
        const auto used=screenArena.used();
        if(used>screenFlushed){
            if(R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(screenMemory)+screenAt+screenFlushed,used-screenFlushed)))return reject("GX geometry cache flush failed");
            screenFlushed=used;
        }
    }
    if(!tapeArena.inFrame())return true;
    const auto used=tapeArena.used();
    if(used<=tapeFlushed)return true;
    if(R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(tapeMemory)+tapeFlushed,used-tapeFlushed)))return reject("GX geometry cache flush failed");
    tapeFlushed=used;return true;
}
bool flushGxGeometry(){
    if(!flushTapeGeometry())return false;
    const auto committed=arena.used();
    if(flushedBytes>committed)return reject("GX geometry flush watermark invalid");
    if(committed==flushedBytes)return true;
    if(!memory)return reject("GX geometry cache flush failed");
    auto* const first=static_cast<u8*>(memory)+flushedBytes;const auto bytes=committed-flushedBytes;
#if defined(__3DS__)
    const Result flushed=kernelFlush?svcFlushProcessDataCache(CUR_PROCESS_HANDLE,reinterpret_cast<u32>(first),bytes):
        GSPGPU_FlushDataCache(first,bytes);
#else
    const Result flushed=GSPGPU_FlushDataCache(first,bytes);
#endif
    if(R_FAILED(flushed))return reject("GX geometry cache flush failed");
    flushedBytes=committed;return true;
}
// A draw submitted before its reservation is committed (the console GPU guard
// runs a new kind of draw alone, at once): publish the reserved bytes too. The
// committed watermark is unchanged; they are flushed again with their block.
bool flushPendingGxGeometry(){
    if(!flushGxGeometry())return false;
    if(tapeArena.inFrame()){
        const auto tapeEnd=tapeArena.reservedEnd();
        if(tapeEnd>tapeFlushed){
            if(R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(tapeMemory)+tapeFlushed,tapeEnd-tapeFlushed)))return reject("GX geometry cache flush failed");
            // The committed part only: a pending reservation is flushed again.
            tapeFlushed=tapeArena.used();
        }
    }
    if(screenArena.inFrame()){
        const auto screenEnd=screenArena.reservedEnd();
        if(screenEnd>screenFlushed){
            if(R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(screenMemory)+screenAt+screenFlushed,screenEnd-screenFlushed)))return reject("GX geometry cache flush failed");
            screenFlushed=screenArena.used();
        }
    }
    const auto end=arena.reservedEnd();
    if(end<=flushedBytes||!memory)return true;
    return R_SUCCEEDED(GSPGPU_FlushDataCache(static_cast<u8*>(memory)+flushedBytes,end-flushedBytes))||reject("GX geometry cache flush failed");
}
bool endGxGeometryFrame(){
    if(!arena.end())return reject("GX geometry frame has pending draw");
    return flushGxGeometry();
}
bool beginGxTapeGeometry(){
    if(active!=&arena)return false;
    if(!tapeMemory){
        tapeMemory=linearMemAlign(tapeBudget,16);
        if(!tapeMemory)return false;
        if(!tapeArena.attach({static_cast<std::byte*>(tapeMemory),tapeBudget})){linearFree(tapeMemory);tapeMemory=nullptr;return false;}
    }
    if(!tapeArena.begin())return false;
    active=&tapeArena;tapeFlushed=0;tapeOverflow=false;return true;
}
bool endGxTapeGeometry(){
    if(active!=&arena&&active!=&tapeArena)return false;
    const bool recording=tapeArena.inFrame();
    active=&arena;
    if(!recording)return false;
    bool okay=!tapeOverflow;
    const auto used=tapeArena.used();
    if(used>tapeFlushed){
        if(R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(tapeMemory)+tapeFlushed,used-tapeFlushed)))okay=false;
        tapeFlushed=used;
    }
    // A reservation left pending belongs to a draw that failed: no tape, and
    // the arena starts clean again.
    if(!tapeArena.end()){
        okay=false;tapeArena={};
        if(!tapeArena.attach({static_cast<std::byte*>(tapeMemory),tapeBudget})){linearFree(tapeMemory);tapeMemory=nullptr;}
    }
    return okay;
}
bool beginGxScreenGeometry(std::size_t at,std::size_t capacity){
    if(active!=&arena&&active!=&tapeArena)return false;
    if(!screenMemory){
        screenMemory=linearMemAlign(screenBudget,16);
        if(!screenMemory)return false;
        screenBase=0;
    }
    const bool append=at==gxScreenGeometryAppend;
    if(append){
        // Room for at least a few quads; a full buffer is recycled by its owner.
        if(screenBudget-screenBase<4096)return false;
        at=screenBase;capacity=screenBudget-screenBase;
    }else if(at%16||at>screenBase||capacity>screenBase-at||capacity<256)return false;
    screenArena={};
    if(!screenArena.attach({static_cast<std::byte*>(screenMemory)+at,capacity})||!screenArena.begin()){screenArena={};return false;}
    screenOuter=active;
    active=&screenArena;screenAt=at;screenAppending=append;screenFlushed=0;screenOverflow=false;return true;
}
bool endGxScreenGeometry(std::size_t& at,std::size_t& bytes){
    at=bytes=0;
    if(active!=&arena&&active!=&screenArena&&active!=&tapeArena)return false;
    const bool recording=screenArena.inFrame();
    // An overflow already moved the draws to the outer arena.
    if(active==&screenArena)active=screenOuter;
    if(!recording)return false;
    bool okay=!screenOverflow;
    const auto used=screenArena.used();
    if(used>screenFlushed){
        if(R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(screenMemory)+screenAt+screenFlushed,used-screenFlushed)))okay=false;
        screenFlushed=used;
    }
    // A reservation left pending belongs to a draw that failed.
    if(!screenArena.end())okay=false;
    screenArena={};
    at=screenAt;bytes=(used+15)&~std::size_t(15);
    // Appended bytes stay taken: draws of this frame already point at them.
    if(screenAppending)screenBase=std::min(screenBudget,screenBase+bytes);
    return okay;
}
void recycleGxScreenGeometry(){if(active!=&screenArena){screenBase=0;screenArena={};}}
std::size_t gxScreenGeometryUsed(){return screenBase;}
std::size_t gxScreenGeometryCapacity(){return screenBudget;}
bool reserveGxGeometry(std::size_t vertices,std::size_t indices,gx::GpuGeometryBatch& out){
    if(active==&screenArena){
        if(screenArena.reserve(vertices,indices,out))return true;
        // The buffer is full: the rest of the screen uses the arena it was
        // begun in and this recording is dropped (endGxScreenGeometry
        // returns false).
        screenOverflow=true;active=screenOuter;
    }
    if(active==&tapeArena){
        if(tapeArena.reserve(vertices,indices,out))return true;
        // The tape buffer is full: the rest of the pass uses the frame arena
        // and this recording is dropped (endGxTapeGeometry returns false).
        tapeOverflow=true;active=&arena;
    }
    if(arena.reserve(vertices,indices,out))return true;
    // Every committed batch references this fixed linear arena. Reuse it only
    // after the actual GPU queue has consumed all earlier submissions. end()
    // also refuses recycling if a caller still has a pending reservation.
    if(arena.inFrame()&&arena.used()&&platformCompleteGpu(500000000)==GpuCompletion::complete&&
       arena.end()&&arena.begin()) {
        flushedBytes=0;++arenaGeneration;
        static unsigned reclaims{};
        if(reclaims++<4)OSReport("Native GPU completed geometry block reclaimed (%lu bytes)\n",static_cast<unsigned long>(budget));
        if(arena.reserve(vertices,indices,out))return true;
    }
    return reject("GX geometry capacity/protocol or completion failed");
}
void invalidateGxGeometryBindings(){resendAttributes=resendBuffers=true;}
GxBindingStatistics gxBindingStatistics(){return bindings;}
std::uint32_t gxGeometryArenaGeneration(){return arenaGeneration;}
namespace {
std::size_t transientGeometryRemaining(const void* pointer){
    const auto at=reinterpret_cast<std::uintptr_t>(pointer);
    const auto remaining=[at](const void* storage,std::size_t bytes){
        const auto base=reinterpret_cast<std::uintptr_t>(storage);
        return storage&&at>=base&&at-base<bytes?bytes-(at-base):0;
    };
    return remaining(memory,budget)+remaining(tapeMemory,tapeBudget)+remaining(screenMemory,screenBudget);
}
}
bool retainGxCasterGeometry(const void*& vertices,std::size_t availableVertexBytes,
                            const std::uint16_t*& indices,unsigned indexCount){
    if(!vertices||!indices||!indexCount||indexCount%3)return false;
    const auto vertexRemaining=transientGeometryRemaining(vertices);
    const auto indexRemaining=transientGeometryRemaining(indices);
    if(!vertexRemaining&&!indexRemaining)return true;
    if(indexRemaining&&indexCount>indexRemaining/sizeof(*indices))return false;
    const std::size_t indexBytes=indexRemaining?std::size_t(indexCount)*sizeof(*indices):0;
    std::size_t vertexBytes=0;
    if(vertexRemaining){
        const auto used=std::size_t(*std::max_element(indices,indices+indexCount))+1;
        constexpr auto stride=sizeof(gx::CompactRigidVertex);
        if(used>availableVertexBytes/stride||used>vertexRemaining/stride)return false;
        vertexBytes=used*stride;
    }
    const std::size_t bytes=(vertexBytes+indexBytes+15)&~std::size_t(15);
    auto* page=casterPages;
    while(page&&bytes>page->capacity-page->used)page=page->next;
    if(!page){
        // Spill without moving earlier snapshots. On a fragmented linear
        // heap, retry with only the space this draw requires.
        auto capacity=std::max(bytes,std::size_t(64*1024));
        auto* allocation=linearMemAlign(sizeof(CasterGeometryPage)+capacity,16);
        if(!allocation&&capacity!=bytes){
            capacity=bytes;
            allocation=linearMemAlign(sizeof(CasterGeometryPage)+capacity,16);
        }
        if(!allocation)return false;
        page=new(allocation) CasterGeometryPage{casterPages,capacity};
        casterPages=page;
    }
    auto* at=page->data()+page->used;
    if(vertexBytes){
        std::memcpy(at,vertices,vertexBytes);
        vertices=at;
        at+=vertexBytes;
    }
    if(indexBytes){
        std::memcpy(at,indices,indexBytes);
        indices=reinterpret_cast<const std::uint16_t*>(at);
    }
    page->used+=bytes;
    return true;
}
bool publishGxCasterGeometry(){
    for(auto* page=casterPages;page;page=page->next){
        if(page->used==page->published)continue;
        if(R_FAILED(GSPGPU_FlushDataCache(page->data()+page->published,page->used-page->published)))
            return reject("GX caster geometry cache flush failed");
        page->published=page->used;
    }
    return true;
}
bool cancelGxGeometry(const gx::GpuGeometryBatch& batch){return active->cancel(batch)||reject("GX geometry cancellation invalid");}
bool shrinkGxGeometry(gx::GpuGeometryBatch& batch,std::size_t vertices,std::size_t indices,bool externalVertices){
    return active->shrinkPending(batch,vertices,indices,externalVertices)||reject("GX geometry pending shrink invalid");
}
namespace {
bool bindBuffers(const gx::GpuVertex* vertices){
    if(!setVertexLayout(0))return reject("GX attribute state unavailable");
    if(!setVertexBuffer(vertices,sizeof(gx::GpuVertex)))return reject("GX geometry GPU address invalid");
    return true;
}
bool drawBuffers(const gx::GpuVertex* vertices,const u16* indices,unsigned count){
    if(!bindBuffers(vertices))return false;
    C3D_DrawElements(GPU_TRIANGLES,count,C3D_UNSIGNED_SHORT,indices);return true;
}
}
bool uploadStaticGxGeometry(const gx::GpuGeometryBatch& batch,unsigned& handle){
    handle=0;if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!gx::validateGpuGeometry(batch))return reject("Static mesh invalid");
    const auto vertexBytes=batch.vertices.size_bytes();
    const auto indexOffset=(vertexBytes+15)&~std::size_t(15);
    const auto bytes=(indexOffset+batch.indices.size_bytes()+15)&~std::size_t(15);
    if(bytes>staticBudget-staticBytes)return reject("Static mesh budget exceeded");
    unsigned slot=0;while(slot<meshes.size()&&meshes[slot].memory)++slot;
    if(slot==meshes.size())return reject("Static mesh slots exhausted");
    auto* allocation=static_cast<u8*>(linearMemAlign(bytes,16));
    if(!allocation)return reject("Static mesh GPU allocation failed");
    std::memcpy(allocation,batch.vertices.data(),vertexBytes);
    std::memcpy(allocation+indexOffset,batch.indices.data(),batch.indices.size_bytes());
    if(R_FAILED(GSPGPU_FlushDataCache(allocation,bytes))){linearFree(allocation);return reject("Static mesh flush failed");}
    meshes[slot]={allocation,reinterpret_cast<gx::GpuVertex*>(allocation),reinterpret_cast<u16*>(allocation+indexOffset),unsigned(batch.indices.size())};
    staticBytes+=bytes;handle=slot+1;return true;
}
bool submitStaticGxGeometry(unsigned handle){
    if(!arena.inFrame()||!handle||handle>meshes.size()||!meshes[handle-1].memory)return reject("Static mesh handle/frame invalid");
    const auto& mesh=meshes[handle-1];return drawBuffers(mesh.vertices,mesh.indices,mesh.count);
}
std::size_t staticGxGeometryBytes(){return staticBytes;}
bool submitGxGeometry(const gx::GpuGeometryBatch& batch,bool clipSpace){
    if(!active->owns(batch)||!gx::validateGpuGeometry(batch))return reject("GX indexed geometry invalid");
    gx::GpuGeometryBatch clipped;
    // The guard reads positions as clip coordinates. The single-texture rigid
    // shader's float layout carries object-space positions (w is a normal
    // component): from the guard's first build (2026-11-13) until 2026-11-14
    // most triangles of those draws were dropped as if outside the screen.
    const int clip=clipSpace?clipForHardware(batch,1,clipped):0;
    if(clip<0)return reject("GX hardware clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;
    if(!drawBuffers(use.vertices.data(),use.indices.data(),use.indices.size()))return false;
    // Reservation validity was checked before submission. No callback or arena
    // mutation occurs between that check and this single-render-thread commit.
    return active->commit(use);
}
bool submitGxTrianglePairs(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(!beforePass)return reject("GX triangle-pair callback missing");
    struct Callback {bool (*before)(bool,void*);void* data;} callback{beforePass,data};
    return submitGxTrianglePasses(batch,false,false,+[](unsigned pass,void* opaque){
        const auto& cb=*static_cast<const Callback*>(opaque);
        return cb.before(pass!=0,cb.data);
    },&callback);
}
bool submitGxTrianglePasses(const gx::GpuGeometryBatch& batch,bool applyFog,bool mesh,
                            bool (*beforePass)(unsigned,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!active->owns(batch)||batch.vertices.size()%2||
       (mesh&&batch.indices.size()%6))
        return reject("GX triangle-pair geometry invalid");
    auto source=batch;
    if(mesh)source.indices=batch.indices.first(batch.indices.size()/2);
    if(!gx::validateGpuGeometry(source))return reject("GX triangle-pair geometry invalid");
    const auto count=batch.vertices.size()/2;
    if(count<3||count%4)return reject("GX triangle-pair vertex count/alignment invalid");
    for(auto index:source.indices)if(index>=count)return reject("GX triangle-pair index invalid");
    // Commit before callback: a command-buffer completion can publish/replay
    // these immutable vertices in the middle of this large two-pass draw.
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,2,clipped,mesh?2:1);
    if(clip<0)return reject("GX triangle-pair clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;const auto half=use.vertices.size()/2;
    const unsigned passes=applyFog?3:2;
    if(mesh){
        // LEQUAL makes the last triangle win a depth tie. The additional
        // passes consume their marks in reverse order, so the same triangle
        // supplies its UVs and fog even when coplanar faces overlap.
        const auto indices=use.indices.size()/2;
        for(std::size_t i=0;i<indices;i+=3)
            std::copy_n(use.indices.data()+indices-i-3,3,use.indices.data()+indices+i);
        if(!active->commit(use))return reject("GX triangle-pair commit failed");
        for(unsigned pass=0;pass<passes;++pass)
            if(!beforePass(pass,data)||!drawBuffers(use.vertices.data()+(pass?half:0),
                use.indices.data()+(pass?indices:0),unsigned(indices)))return false;
        return true;
    }
    if(!active->commit(use))return reject("GX triangle-pair commit failed");
    for(unsigned i=0;i<use.indices.size();i+=3)for(unsigned pass=0;pass<passes;++pass){
        if(!beforePass(pass,data))return false;
        // Both immutable halves have the same layout. The callback changes
        // raster/material state only; queue completion preserves attributes.
        // Only the buffer address alternates between the halves.
        if(!drawBuffers(use.vertices.data()+(pass?half:0),use.indices.data()+i,3))return false;
    }
    return true;
}
// Whole-mesh form of the pair above: the original half, then the fog half,
// two draws in total. The caller's passes make it equivalent (see
// submitGxTriangleFogGeometry).
bool submitGxMeshPair(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!active->owns(batch)||batch.vertices.size()%2||!gx::validateGpuGeometry(batch))
        return reject("GX mesh-pair geometry invalid");
    const auto count=batch.vertices.size()/2;
    if(count<3||count%4||batch.indices.size()%3)return reject("GX mesh-pair vertex count/alignment invalid");
    for(auto index:batch.indices)if(index>=count)return reject("GX mesh-pair index invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,2,clipped);
    if(clip<0)return reject("GX mesh-pair clip guard failed");
    if(clip==2)return true;
    if(!active->commit(clip?clipped:batch))return reject("GX mesh-pair commit failed");
    const auto& use=clip?clipped:batch;const auto half=use.vertices.size()/2;
    for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        if(!drawBuffers(use.vertices.data()+pass*half,use.indices.data(),unsigned(use.indices.size())))return false;
    }
    return true;
}
bool submitGxAlphaUnionMesh(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!active->owns(batch)||batch.indices.size()%3||!gx::validateGpuGeometry(batch))
        return reject("GX alpha-union mesh geometry invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,1,clipped);
    if(clip<0)return reject("GX alpha-union mesh clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;
    // Committed before the first callback: a queue flush inside it publishes them.
    if(!active->commit(use))return reject("GX alpha-union mesh commit failed");
    for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        if(!drawBuffers(use.vertices.data(),use.indices.data(),unsigned(use.indices.size())))return false;
    }
    return true;
}
bool submitGxTriangleAlphaPairs(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!active->owns(batch)||!gx::validateGpuGeometry(batch))
        return reject("GX triangle alpha geometry invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,1,clipped);
    if(clip<0)return reject("GX triangle alpha clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;
    if(!active->commit(use))return reject("GX triangle alpha commit failed");
    for(unsigned i=0;i<use.indices.size();i+=3)for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        if(i==0&&pass==0){
            if(!drawBuffers(use.vertices.data(),use.indices.data(),3))return false;
        }else C3D_DrawElements(GPU_TRIANGLES,3,C3D_UNSIGNED_SHORT,use.indices.data()+i);
    }
    return true;
}
bool submitGxTriangleAlphaInterval(const gx::GpuGeometryBatch& batch,bool (*beforePass)(gx::AlphaIntervalPass,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!active->owns(batch)||!gx::validateGpuGeometry(batch))
        return reject("GX alpha interval geometry invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,1,clipped);
    if(clip<0)return reject("GX alpha interval clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;
    if(!bindBuffers(use.vertices.data()))return false;
    if(!active->commit(use))return reject("GX alpha interval commit failed");
    for(unsigned i=0;i<use.indices.size();i+=3)for(unsigned pass=0;pass<4;++pass){
        if(!beforePass(static_cast<gx::AlphaIntervalPass>(pass),data))return false;
        C3D_DrawElements(GPU_TRIANGLES,3,C3D_UNSIGNED_SHORT,use.indices.data()+i);
    }
    return true;
}
static bool submitCompact(const gx::GpuGeometryBatch& batch,const void* resident,bool multiTexture,bool wide,
                          const gx::RigidVertexPlan* fog,unsigned& indexCount){
    // Resident static vertices were validated when decoded and stored. Wide
    // records (a third texture coordinate) belong to the multi-texture
    // program and are never resident.
    if(wide&&(!multiTexture||resident))return reject("GX wide rigid records invalid");
    if(!active->owns(batch)||(!resident&&!multiTexture&&!gx::validateCompactRigidGeometry(batch)))return reject("GX compact rigid geometry invalid");
    if(!setVertexLayout(wide?2:1))return reject("GX attribute state unavailable");
    if(!setVertexBuffer(resident?resident:batch.vertices.data(),wide?sizeof(gx::CompactRigidVertexMTWide):
        multiTexture?sizeof(gx::CompactRigidVertexMT):sizeof(gx::CompactRigidVertex)))
        return reject(multiTexture?"GX multi-texture rigid address invalid":"GX compact rigid address invalid");
    indexCount=batch.indices.size();
    // A retained recording can run under a later camera. Its index list must stay complete.
    if(fog && active==&arena && resident && multiTexture && !wide && fog->matrices==1)
        indexCount=fogVisibility.select(fog->staticVertices,batch.indices,fog->uniforms);
    if(indexCount)C3D_DrawElements(GPU_TRIANGLES,indexCount,C3D_UNSIGNED_SHORT,batch.indices.data());
    return active->commit(batch);
}
bool submitGxCompactRigidGeometry(const gx::GpuGeometryBatch& batch,const void* resident,bool multiTexture,bool wide){
    unsigned count;
    return submitCompact(batch,resident,multiTexture,wide,nullptr,count);
}
bool submitGxCompactFogGeometry(const gx::GpuGeometryBatch& batch,const gx::RigidVertexPlan& rigid,unsigned& indexCount){
    return submitCompact(batch,rigid.staticVertices,rigid.multiTexture,rigid.wideRecord,&rigid,indexCount);
}
// Second pass over the batch just submitted: same buffers, attributes and
// indices (the caller changed raster/material/uniform state only).
void redrawGxGeometry(const gx::GpuGeometryBatch& batch){
    C3D_DrawElements(GPU_TRIANGLES,batch.indices.size(),C3D_UNSIGNED_SHORT,batch.indices.data());
}
// Native engine: draw persistent resident vertices/indices (no frame arena).
bool drawResidentRigid(const void* vertices,const std::uint16_t* indices,unsigned indexCount,bool multiTexture){
    if(!vertices||!indices||!indexCount||indexCount%3)return reject("GX resident rigid draw invalid");
    if(!setVertexLayout(1))return reject("GX attribute state unavailable");
    if(!setVertexBuffer(vertices,multiTexture?sizeof(gx::CompactRigidVertexMT):sizeof(gx::CompactRigidVertex)))
        return reject("GX resident rigid address invalid");
    C3D_DrawElements(GPU_TRIANGLES,indexCount,C3D_UNSIGNED_SHORT,indices);
    return true;
}
std::uint16_t* allocateResidentIndices(const std::uint16_t* source,unsigned count){
    if(!indexPool){
        // 1 MiB: with 256 KiB the pool filled up to five times per zone in
        // Azahar (D_MN11 r6), and each full reset retires every record using
        // it, so the next frames record again. 256 KiB if linear memory is short.
        static bool unavailable;
        if(unavailable)return nullptr;
        for(const std::size_t bytes:{std::size_t(1024*1024),std::size_t(256*1024)}){
            indexPool=static_cast<std::uint16_t*>(linearMemAlign(bytes,16));
            if(indexPool){indexPoolCapacity=bytes/sizeof(std::uint16_t);break;}
        }
        if(!indexPool){indexPoolCapacity=0;unavailable=true;return nullptr;}
        OSReport("Native resident index pool: %u KiB\n",unsigned(indexPoolCapacity*sizeof(std::uint16_t)/1024));
    }
    const std::size_t aligned=(count+7)&~std::size_t(7);
    if(indexPoolResetPending||aligned>indexPoolCapacity-indexPoolUsed){indexPoolResetPending=true;return nullptr;}
    auto* out=indexPool+indexPoolUsed;std::memcpy(out,source,count*sizeof(std::uint16_t));
    GSPGPU_FlushDataCache(out,count*sizeof(std::uint16_t));
    indexPoolUsed+=aligned;return out;
}
std::uint32_t residentIndexEpoch(){return indexPoolEpoch;}
bool attachResidentStaticGeometry(gx::StaticCompactCache& cache,std::size_t bytes){
    auto* pool=static_cast<u8*>(linearMemAlign(bytes,16));
    if(!pool)return false;
    if(cache.attach({pool,bytes},true,[](void* at,std::size_t size){GSPGPU_FlushDataCache(at,size);}))return true;
    linearFree(pool);return false;
}
const char* gxGeometryFailure(){return failure;}
std::size_t gxGeometryBytes(){return arena.used();}
}
