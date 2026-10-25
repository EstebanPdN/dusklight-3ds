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
}
extern "C" {
#include <c3d/attribs.h>
#include <c3d/base.h>
}
#include <cstring>
extern "C" void OSReport(const char*,...);
namespace dusk::native {
void dropPendingCapture();
namespace {
// Persistent linear index pool for native shape records; reset only at a
// frame boundary (queued GPU work may still read it).
std::uint16_t* indexPool{};std::size_t indexPoolCapacity{},indexPoolUsed{};std::uint32_t indexPoolEpoch=1;bool indexPoolResetPending{};
constexpr std::size_t budget=512*1024;
void* memory;
gx::GpuGeometryArena arena;
std::size_t flushedBytes{};
struct StaticMesh {void* memory{};gx::GpuVertex* vertices{};u16* indices{};unsigned count{};};
std::array<StaticMesh,64> meshes{};
std::size_t staticBytes;
constexpr std::size_t staticBudget=4*1024*1024;
const char* failure="none";
bool reject(const char* why){failure=why;return false;}
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
    if(indexPoolResetPending){indexPoolUsed=0;indexPoolResetPending=false;++indexPoolEpoch;}
    dropPendingCapture(); // an unconsumed deferred capture is never read back
    flushedBytes=0;return true;
}
bool flushGxGeometry(){
    const auto committed=arena.used();
    if(flushedBytes>committed)return reject("GX geometry flush watermark invalid");
    if(committed==flushedBytes)return true;
    if(!memory||R_FAILED(GSPGPU_FlushDataCache(static_cast<u8*>(memory)+flushedBytes,committed-flushedBytes)))
        return reject("GX geometry cache flush failed");
    flushedBytes=committed;return true;
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
bool cancelGxGeometry(const gx::GpuGeometryBatch& batch){return arena.cancel(batch)||reject("GX geometry cancellation invalid");}
bool shrinkGxGeometry(gx::GpuGeometryBatch& batch,std::size_t vertices,std::size_t indices,bool externalVertices){
    return arena.shrinkPending(batch,vertices,indices,externalVertices)||reject("GX geometry pending shrink invalid");
}
namespace {
bool bindBuffers(const gx::GpuVertex* vertices,bool setAttributes=true){
    if(setAttributes){
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
bool submitGxGeometry(const gx::GpuGeometryBatch& batch){
    if(!arena.owns(batch)||!gx::validateGpuGeometry(batch))return reject("GX indexed geometry invalid");
    if(!drawBuffers(batch.vertices.data(),batch.indices.data(),batch.indices.size()))return false;
    // Reservation validity was checked before submission. No callback or arena
    // mutation occurs between that check and this single-render-thread commit.
    return arena.commit(batch);
}
bool submitGxTrianglePairs(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||batch.vertices.size()%2||!gx::validateGpuGeometry(batch))
        return reject("GX triangle-pair geometry invalid");
    const auto count=batch.vertices.size()/2;
    if(count<3||count%4)return reject("GX triangle-pair vertex count/alignment invalid");
    for(auto index:batch.indices)if(index>=count)return reject("GX triangle-pair index invalid");
    // Commit before callback: a command-buffer completion can publish/replay
    // these immutable vertices in the middle of this large two-pass draw.
    if(!arena.commit(batch))return reject("GX triangle-pair commit failed");
    for(unsigned i=0;i<batch.indices.size();i+=3)for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        // Both immutable halves have the same layout. The callback changes
        // raster/material state only; queue completion preserves attributes.
        // Restore the layout after any previous compact draw, once per batch.
        if(!drawBuffers(batch.vertices.data()+pass*count,batch.indices.data()+i,3,i==0&&pass==0))return false;
    }
    return true;
}
bool submitGxTriangleAlphaPairs(const gx::GpuGeometryBatch& batch,bool (*beforePass)(bool,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||!gx::validateGpuGeometry(batch))
        return reject("GX triangle alpha geometry invalid");
    if(!arena.commit(batch))return reject("GX triangle alpha commit failed");
    for(unsigned i=0;i<batch.indices.size();i+=3)for(unsigned pass=0;pass<2;++pass){
        if(!beforePass(pass!=0,data))return false;
        if(i==0&&pass==0){
            if(!drawBuffers(batch.vertices.data(),batch.indices.data(),3))return false;
        }else C3D_DrawElements(GPU_TRIANGLES,3,C3D_UNSIGNED_SHORT,batch.indices.data()+i);
    }
    return true;
}
bool submitGxTriangleAlphaInterval(const gx::GpuGeometryBatch& batch,bool (*beforePass)(gx::AlphaIntervalPass,void*),void* data){
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly||!beforePass||!arena.owns(batch)||!gx::validateGpuGeometry(batch))
        return reject("GX alpha interval geometry invalid");
    if(!bindBuffers(batch.vertices.data()))return false;
    if(!arena.commit(batch))return reject("GX alpha interval commit failed");
    for(unsigned i=0;i<batch.indices.size();i+=3)for(unsigned pass=0;pass<4;++pass){
        if(!beforePass(static_cast<gx::AlphaIntervalPass>(pass),data))return false;
        C3D_DrawElements(GPU_TRIANGLES,3,C3D_UNSIGNED_SHORT,batch.indices.data()+i);
    }
    return true;
}
bool submitGxCompactRigidGeometry(const gx::GpuGeometryBatch& batch,const void* resident,bool multiTexture){
    // Resident static vertices were validated when decoded and stored.
    if(!arena.owns(batch)||(!resident&&!multiTexture&&!gx::validateCompactRigidGeometry(batch)))return reject("GX compact rigid geometry invalid");
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
    AttrInfo_AddLoader(attributes,4,GPU_BYTE,2);
    AttrInfo_AddLoader(attributes,3,GPU_UNSIGNED_BYTE,4);
    auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
    if(BufInfo_Add(buffers,resident?resident:batch.vertices.data(),sizeof(gx::CompactRigidVertex),5,0x43210)<0)
        return reject("GX compact rigid address invalid");
    C3D_DrawElements(GPU_TRIANGLES,batch.indices.size(),C3D_UNSIGNED_SHORT,batch.indices.data());
    return arena.commit(batch);
}
// Native engine: draw persistent resident vertices/indices (no frame arena).
bool drawResidentRigid(const void* vertices,const std::uint16_t* indices,unsigned indexCount,bool multiTexture){
    if(!vertices||!indices||!indexCount||indexCount%3)return reject("GX resident rigid draw invalid");
    auto* attributes=C3D_GetAttrInfo();AttrInfo_Init(attributes);
    AttrInfo_AddLoader(attributes,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(attributes,1,GPU_SHORT,3);
    AttrInfo_AddLoader(attributes,2,GPU_SHORT,2);
    if(multiTexture)AttrInfo_AddLoader(attributes,5,GPU_SHORT,2);
    AttrInfo_AddLoader(attributes,4,GPU_BYTE,2);
    AttrInfo_AddLoader(attributes,3,GPU_UNSIGNED_BYTE,4);
    auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
    if(BufInfo_Add(buffers,vertices,multiTexture?sizeof(gx::CompactRigidVertexMT):sizeof(gx::CompactRigidVertex),
                   multiTexture?6:5,multiTexture?0x543210:0x43210)<0)return reject("GX resident rigid address invalid");
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
