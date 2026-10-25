#include "gx_gpu_geometry.hpp"
#include <cmath>
#include <cstring>
#include <limits>
namespace dusk::native::gx {
bool GpuGeometryArena::attach(std::span<std::byte> bytes){
    if(active||pending.ticket||bytes.empty()||bytes.size()>16*1024*1024||
       reinterpret_cast<std::uintptr_t>(bytes.data())%16)return false;
    storage=bytes;cursor=pendingEnd=0;return true;
}
bool GpuGeometryArena::begin(){
    if(active||pending.ticket||storage.empty())return false;
    active=true;cursor=0;return true;
}
bool GpuGeometryArena::end(){
    if(!active||pending.ticket)return false;
    active=false;return true;
}
bool GpuGeometryArena::reserve(std::size_t vertices,std::size_t indices,GpuGeometryBatch& out){
    out={};
    if(!active||pending.ticket||vertices<3||vertices>65535||!indices||indices%3||indices>196605||
       serial==std::numeric_limits<std::uint64_t>::max())return false;
    // Counts are bounded before arithmetic; align both stream starts to 16 B.
    const auto start=(cursor+15)&~std::size_t(15);
    const auto indexStart=(start+vertices*sizeof(GpuVertex)+15)&~std::size_t(15);
    const auto end=indexStart+indices*sizeof(std::uint16_t);
    if(end>storage.size())return false;
    pending={{reinterpret_cast<GpuVertex*>(storage.data()+start),vertices},
             {reinterpret_cast<std::uint16_t*>(storage.data()+indexStart),indices},++serial};
    pendingEnd=end;out=pending;return true;
}
bool GpuGeometryArena::owns(const GpuGeometryBatch& batch)const{
    return active&&pending.ticket&&batch.ticket==pending.ticket&&
        batch.vertices.data()==pending.vertices.data()&&batch.vertices.size()==pending.vertices.size()&&
        batch.indices.data()==pending.indices.data()&&batch.indices.size()==pending.indices.size();
}
bool GpuGeometryArena::shrinkPending(GpuGeometryBatch& inout,std::size_t newVertices,std::size_t newIndices,bool externalVertices){
    if(!owns(inout)||newVertices<3||newVertices>65535||newIndices<3||
       newIndices%3||newIndices>196605||newVertices>inout.vertices.size()||
       newIndices>inout.indices.size())return false;
    const bool changed=newVertices!=inout.vertices.size()||newIndices!=inout.indices.size();
    if(changed&&serial==std::numeric_limits<std::uint64_t>::max())return false;
    // Recheck the owned layout with bounded arithmetic before touching memory.
    const auto base=reinterpret_cast<std::uintptr_t>(storage.data());
    const auto vertexAddress=reinterpret_cast<std::uintptr_t>(inout.vertices.data());
    if(vertexAddress<base||vertexAddress-base>storage.size())return false;
    const auto start=static_cast<std::size_t>(vertexAddress-base);
    if(start%16||cursor>start||cursor>std::numeric_limits<std::size_t>::max()-15||
       ((cursor+15)&~std::size_t(15))!=start)return false;
    const auto layout=[&](std::size_t vertices,std::size_t indices,
                          std::size_t& indexStart,std::size_t& end){
        if(vertices<3||vertices>65535||indices<3||indices>196605||indices%3||
           vertices>(storage.size()-start)/sizeof(GpuVertex))return false;
        const auto vertexEnd=start+vertices*sizeof(GpuVertex);
        if(vertexEnd>std::numeric_limits<std::size_t>::max()-15)return false;
        indexStart=(vertexEnd+15)&~std::size_t(15);
        if(indexStart>storage.size()||indices>(storage.size()-indexStart)/sizeof(std::uint16_t))return false;
        end=indexStart+indices*sizeof(std::uint16_t);return true;
    };
    std::size_t oldIndexStart{},oldEnd{},newIndexStart{},newEnd{};
    if(!layout(inout.vertices.size(),inout.indices.size(),oldIndexStart,oldEnd)||
       oldEnd!=pendingEnd||inout.indices.data()!=reinterpret_cast<std::uint16_t*>(storage.data()+oldIndexStart)||
       !layout(newVertices,newIndices,newIndexStart,newEnd)||newEnd>oldEnd)return false;
    // External vertices: indices address a resident buffer, not this prefix.
    if(!externalVertices)for(std::size_t i=0;i<newIndices;++i)if(inout.indices[i]>=newVertices)return false;
    // All rejection paths precede the move. Source may overlap destination.
    if(changed){
        std::memmove(storage.data()+newIndexStart,inout.indices.data(),newIndices*sizeof(std::uint16_t));
        inout.vertices=inout.vertices.first(newVertices);
        inout.indices={reinterpret_cast<std::uint16_t*>(storage.data()+newIndexStart),newIndices};
        inout.ticket=++serial;
    }
    pendingEnd=newEnd;pending=inout;return true;
}
bool GpuGeometryArena::commit(const GpuGeometryBatch& batch){
    if(!owns(batch))return false;
    cursor=pendingEnd;pending={};return true;
}
bool GpuGeometryArena::cancel(const GpuGeometryBatch& batch){
    if(!owns(batch))return false;
    pending={};return true;
}
bool validateGpuGeometry(const GpuGeometryBatch& batch){
    if(batch.qPolicy!=GeometryQPolicy::positiveOnly&&batch.qPolicy!=GeometryQPolicy::directTexture0Diagnostic)return false;
    if(batch.vertices.size()<3||batch.vertices.size()>65535||batch.indices.empty()||
       batch.indices.size()%3||batch.indices.size()>196605)return false;
    for(auto index:batch.indices)if(index>=batch.vertices.size())return false;
    for(const auto& v:batch.vertices){
        for(float x:v.position)if(!std::isfinite(x))return false;
        for(float x:v.color)if(!std::isfinite(x)||x<0||x>1)return false;
        for(const auto& uv:v.texcoord)for(float x:uv)if(!std::isfinite(x))return false;
        if(!std::isfinite(v.textureQ)||v.textureQ==0||
           (batch.qPolicy==GeometryQPolicy::positiveOnly&&v.textureQ<0))return false;
    }
    return true;
}
bool validateCompactRigidGeometry(const GpuGeometryBatch& batch){
    if(batch.qPolicy!=GeometryQPolicy::positiveOnly)return false;
    if(batch.vertices.size()<3||batch.vertices.size()>65535||batch.indices.empty()||
       batch.indices.size()%3||batch.indices.size()>196605)return false;
    for(auto index:batch.indices)if(index>=batch.vertices.size())return false;
    const auto bytes=std::as_bytes(batch.vertices);
    for(unsigned i=0;i<batch.vertices.size();++i){
        CompactRigidVertex v;std::memcpy(&v,bytes.data()+i*sizeof(v),sizeof(v));
        for(float x:v.position)if(!std::isfinite(x))return false;
        for(int row:v.matrix)if(row<0||row>90)return false;
    }
    return true;
}

}
