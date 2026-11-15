#include "gx_pica.hpp"
#include "gx_geometry_pica.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gx_compact_cache.hpp"
#include "runtime/gx_texture_commands.hpp"
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/gpu/enums.h>
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
#include <c3d/base.h>
}
#include <cstring>
#include <cmath>
#include <algorithm>
#include <span>
#include <array>
#include <vector>
extern "C" void OSReport(const char*,...);
namespace dusk::native {
void dropPendingCapture();
namespace {
// Attribute layout last given to Citro3D by the resident draw (1 single
// texture, 2 multi-texture; 0 anything else): consecutive resident draws of
// one layout do not rebuild and re-send it.
unsigned residentAttributes;
// Persistent linear index pool for native shape records; reset only at a
// frame boundary (queued GPU work may still read it).
std::uint16_t* indexPool{};std::size_t indexPoolCapacity{},indexPoolUsed{};std::uint32_t indexPoolEpoch=1;bool indexPoolResetPending{};
constexpr std::size_t budget=512*1024;
void* memory;
gx::GpuGeometryArena arena;
std::size_t flushedBytes{};
bool kernelFlush{};
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
int clipForHardware(const gx::GpuGeometryBatch& batch,unsigned halves,gx::GpuGeometryBatch& out){
    const std::size_t count=batch.vertices.size()/halves;
    // One pass over the vertices; triangles are then sorted by their codes.
    clipCodes.resize(count);
    unsigned any=0;
    for(std::size_t i=0;i<count;++i)any|=clipCodes[i]=guardCode(batch.vertices[i]);
    if(!any)return 0;
    auto code=[&](std::uint16_t index){return index<count?clipCodes[index]:guardCode(batch.vertices[index]);};
    clipExtra[0].clear();clipExtra[1].clear();clipIndices.clear();
    std::array<ClipCorner,12> polygon,next;
    for(std::size_t i=0;i+2<batch.indices.size();i+=3){
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
    if(total*halves>65535||clipIndices.size()>196605)return -1;
    if(!arena.commit(batch))return -1;
    if(clipIndices.empty())return 2;
    if(!reserveGxGeometry(total*halves,clipIndices.size(),out))return -1;
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
    for(auto& mesh:meshes){if(mesh.memory)linearFree(mesh.memory);mesh={};}staticBytes=0;
    if(memory)linearFree(memory);
    memory=nullptr;arena={};flushedBytes=0;
}
bool beginGxGeometryFrame(){
    if(!platformBeginGpuCommands()||!arena.begin())return reject("GX geometry frame begin invalid");
    // The previous queue completed: resident static geometry may be recycled.
    if(auto* cache=gx::staticCompactCache())cache->frameBegin();
    invalidatePicaCommandState();
    ++gx::textureFrameCounter;
    if(clipStats.clipped&&gx::textureFrameCounter%30==0){
        static unsigned reports;
        if(reports<40){++reports;OSReport("Native hardware clip guard: triangles=%u clipped=%u dropped=%u added-vertices=%u\n",clipStats.triangles,clipStats.clipped,clipStats.dropped,clipStats.added);}
    }
    clipStats={};
    if(indexPoolResetPending){indexPoolUsed=0;indexPoolResetPending=false;++indexPoolEpoch;}
    dropPendingCapture(); // an unconsumed deferred capture is never read back
    flushedBytes=0;return true;
}
void setGxGeometryKernelFlush(bool enabled){kernelFlush=enabled;}
bool flushGxGeometry(){
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
    const auto end=arena.reservedEnd();
    if(end<=flushedBytes||!memory)return true;
    return R_SUCCEEDED(GSPGPU_FlushDataCache(static_cast<u8*>(memory)+flushedBytes,end-flushedBytes))||reject("GX geometry cache flush failed");
}
bool endGxGeometryFrame(){
    if(!arena.end())return reject("GX geometry frame has pending draw");
    return flushGxGeometry();
}
bool reserveGxGeometry(std::size_t vertices,std::size_t indices,gx::GpuGeometryBatch& out){
    if(arena.reserve(vertices,indices,out))return true;
    // Every committed batch references this fixed linear arena. Reuse it only
    // after the actual GPU queue has consumed all earlier submissions. end()
    // also refuses recycling if a caller still has a pending reservation.
    if(arena.inFrame()&&arena.used()&&platformCompleteGpu(500000000)==GpuCompletion::complete&&
       arena.end()&&arena.begin()&&arena.reserve(vertices,indices,out)) {
        flushedBytes=0;
        static unsigned reclaims{};
        if(reclaims++<4)OSReport("Native GPU completed geometry block reclaimed (%lu bytes)\n",static_cast<unsigned long>(budget));
        return true;
    }
    return reject("GX geometry capacity/protocol or completion failed");
}
void invalidateGxGeometryBindings(){residentAttributes=0;}
bool cancelGxGeometry(const gx::GpuGeometryBatch& batch){return arena.cancel(batch)||reject("GX geometry cancellation invalid");}
bool shrinkGxGeometry(gx::GpuGeometryBatch& batch,std::size_t vertices,std::size_t indices,bool externalVertices){
    return arena.shrinkPending(batch,vertices,indices,externalVertices)||reject("GX geometry pending shrink invalid");
}
namespace {
bool bindBuffers(const gx::GpuVertex* vertices,bool setAttributes=true){
    if(setAttributes){
        residentAttributes=0;
        auto* attributes=C3D_GetAttrInfo();
        AttrInfo_Init(attributes);
        AttrInfo_AddLoader(attributes,0,GPU_FLOAT,4);AttrInfo_AddLoader(attributes,1,GPU_FLOAT,4);
        for(unsigned i=2;i<5;++i)AttrInfo_AddLoader(attributes,i,GPU_FLOAT,2);
        AttrInfo_AddLoader(attributes,5,GPU_FLOAT,1);
    }
    auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
    if(BufInfo_Add(buffers,vertices,sizeof(gx::GpuVertex),6,0x543210)<0)
        return reject("GX geometry GPU address invalid");
    return true;
}
bool drawBuffers(const gx::GpuVertex* vertices,const u16* indices,unsigned count,bool setAttributes=true){
    if(!bindBuffers(vertices,setAttributes))return false;
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
    if(!arena.owns(batch)||!gx::validateGpuGeometry(batch))return reject("GX indexed geometry invalid");
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
    return arena.commit(use);
}
bool submitGxTrianglePairs(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||batch.vertices.size()%2||!gx::validateGpuGeometry(batch))
        return reject("GX triangle-pair geometry invalid");
    const auto count=batch.vertices.size()/2;
    if(count<3||count%4)return reject("GX triangle-pair vertex count/alignment invalid");
    for(auto index:batch.indices)if(index>=count)return reject("GX triangle-pair index invalid");
    // Commit before callback: a command-buffer completion can publish/replay
    // these immutable vertices in the middle of this large two-pass draw.
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,2,clipped);
    if(clip<0)return reject("GX triangle-pair clip guard failed");
    if(clip==2)return true;
    if(!arena.commit(clip?clipped:batch))return reject("GX triangle-pair commit failed");
    const auto& use=clip?clipped:batch;const auto half=use.vertices.size()/2;
    for(unsigned i=0;i<use.indices.size();i+=3)for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        // Both immutable halves have the same layout. The callback changes
        // raster/material state only; queue completion preserves attributes.
        // Restore the layout after any previous compact draw, once per batch.
        if(!drawBuffers(use.vertices.data()+pass*half,use.indices.data()+i,3,i==0&&pass==0))return false;
    }
    return true;
}
// Whole-mesh form of the pair above: the original half, then the fog half,
// two draws in total. The caller's passes make it equivalent (see
// submitGxTriangleFogGeometry).
bool submitGxMeshPair(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||batch.vertices.size()%2||!gx::validateGpuGeometry(batch))
        return reject("GX mesh-pair geometry invalid");
    const auto count=batch.vertices.size()/2;
    if(count<3||count%4||batch.indices.size()%3)return reject("GX mesh-pair vertex count/alignment invalid");
    for(auto index:batch.indices)if(index>=count)return reject("GX mesh-pair index invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,2,clipped);
    if(clip<0)return reject("GX mesh-pair clip guard failed");
    if(clip==2)return true;
    if(!arena.commit(clip?clipped:batch))return reject("GX mesh-pair commit failed");
    const auto& use=clip?clipped:batch;const auto half=use.vertices.size()/2;
    for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        if(!drawBuffers(use.vertices.data()+pass*half,use.indices.data(),unsigned(use.indices.size()),pass==0))return false;
    }
    return true;
}
bool submitGxTriangleAlphaPairs(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||!gx::validateGpuGeometry(batch))
        return reject("GX triangle alpha geometry invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,1,clipped);
    if(clip<0)return reject("GX triangle alpha clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;
    if(!arena.commit(use))return reject("GX triangle alpha commit failed");
    for(unsigned i=0;i<use.indices.size();i+=3)for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        if(i==0&&pass==0){
            if(!drawBuffers(use.vertices.data(),use.indices.data(),3))return false;
        }else C3D_DrawElements(GPU_TRIANGLES,3,C3D_UNSIGNED_SHORT,use.indices.data()+i);
    }
    return true;
}
bool submitGxTriangleAlphaInterval(const gx::GpuGeometryBatch& batch,bool (*beforePass)(gx::AlphaIntervalPass,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||!gx::validateGpuGeometry(batch))
        return reject("GX alpha interval geometry invalid");
    gx::GpuGeometryBatch clipped;
    const int clip=clipForHardware(batch,1,clipped);
    if(clip<0)return reject("GX alpha interval clip guard failed");
    if(clip==2)return true;
    const auto& use=clip?clipped:batch;
    if(!bindBuffers(use.vertices.data()))return false;
    if(!arena.commit(use))return reject("GX alpha interval commit failed");
    for(unsigned i=0;i<use.indices.size();i+=3)for(unsigned pass=0;pass<4;++pass){
        if(!beforePass(static_cast<gx::AlphaIntervalPass>(pass),data))return false;
        C3D_DrawElements(GPU_TRIANGLES,3,C3D_UNSIGNED_SHORT,use.indices.data()+i);
    }
    return true;
}
bool submitGxCompactRigidGeometry(const gx::GpuGeometryBatch& batch,const void* resident,bool multiTexture){
    // Resident static vertices were validated when decoded and stored.
    if(!arena.owns(batch)||(!resident&&!multiTexture&&!gx::validateCompactRigidGeometry(batch)))return reject("GX compact rigid geometry invalid");
    residentAttributes=0;
    auto* attributes=C3D_GetAttrInfo();AttrInfo_Init(attributes);
    if(multiTexture){
        // CompactRigidVertexMT: position, normal, TEX0, TEX1, matrix, color.
        AttrInfo_AddLoader(attributes,0,GPU_FLOAT,3);
        AttrInfo_AddLoader(attributes,1,GPU_SHORT,3);
        AttrInfo_AddLoader(attributes,2,GPU_SHORT,2);
        AttrInfo_AddLoader(attributes,5,GPU_SHORT,2);
        AttrInfo_AddLoader(attributes,4,GPU_BYTE,2);
        AttrInfo_AddLoader(attributes,3,GPU_UNSIGNED_BYTE,4);
        auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
        if(BufInfo_Add(buffers,resident?resident:batch.vertices.data(),sizeof(gx::CompactRigidVertexMT),6,0x543210)<0)
            return reject("GX multi-texture rigid address invalid");
        C3D_DrawElements(GPU_TRIANGLES,batch.indices.size(),C3D_UNSIGNED_SHORT,batch.indices.data());
        return arena.commit(batch);
    }
    AttrInfo_AddLoader(attributes,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(attributes,1,GPU_SHORT,3);
    AttrInfo_AddLoader(attributes,2,GPU_SHORT,2);
    AttrInfo_AddLoader(attributes,5,GPU_SHORT,2); // unused padding attribute (six-attribute shape)
    AttrInfo_AddLoader(attributes,4,GPU_BYTE,2);
    AttrInfo_AddLoader(attributes,3,GPU_UNSIGNED_BYTE,4);
    auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
    if(BufInfo_Add(buffers,resident?resident:batch.vertices.data(),sizeof(gx::CompactRigidVertex),6,0x543210)<0)
        return reject("GX compact rigid address invalid");
    C3D_DrawElements(GPU_TRIANGLES,batch.indices.size(),C3D_UNSIGNED_SHORT,batch.indices.data());
    return arena.commit(batch);
}
// Second pass over the batch just submitted: same buffers, attributes and
// indices (the caller changed raster/material/uniform state only).
void redrawGxGeometry(const gx::GpuGeometryBatch& batch){
    C3D_DrawElements(GPU_TRIANGLES,batch.indices.size(),C3D_UNSIGNED_SHORT,batch.indices.data());
}
// Native engine: draw persistent resident vertices/indices (no frame arena).
bool drawResidentRigid(const void* vertices,const std::uint16_t* indices,unsigned indexCount,bool multiTexture){
    if(!vertices||!indices||!indexCount||indexCount%3)return reject("GX resident rigid draw invalid");
    if(residentAttributes!=(multiTexture?2u:1u)){
        auto* attributes=C3D_GetAttrInfo();AttrInfo_Init(attributes);
        AttrInfo_AddLoader(attributes,0,GPU_FLOAT,3);
        AttrInfo_AddLoader(attributes,1,GPU_SHORT,3);
        AttrInfo_AddLoader(attributes,2,GPU_SHORT,2);
        AttrInfo_AddLoader(attributes,5,GPU_SHORT,2); // TEX1, or the unused padding of the single layout
        AttrInfo_AddLoader(attributes,4,GPU_BYTE,2);
        AttrInfo_AddLoader(attributes,3,GPU_UNSIGNED_BYTE,4);
        residentAttributes=multiTexture?2:1;
    }
    auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
    if(BufInfo_Add(buffers,vertices,multiTexture?sizeof(gx::CompactRigidVertexMT):sizeof(gx::CompactRigidVertex),
                   6,0x543210)<0)return reject("GX resident rigid address invalid");
    C3D_DrawElements(GPU_TRIANGLES,indexCount,C3D_UNSIGNED_SHORT,indices);
    return true;
}
std::uint16_t* allocateResidentIndices(const std::uint16_t* source,unsigned count){
    if(!indexPool){
        indexPoolCapacity=256*1024/sizeof(std::uint16_t);
        indexPool=static_cast<std::uint16_t*>(linearMemAlign(indexPoolCapacity*sizeof(std::uint16_t),16));
        if(!indexPool){indexPoolCapacity=0;return nullptr;}
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
