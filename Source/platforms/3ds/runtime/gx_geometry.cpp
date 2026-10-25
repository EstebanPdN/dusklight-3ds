#include "gx_geometry.hpp"
namespace dusk::native::gx {
namespace {
bool overlap(std::span<const std::byte> a,std::span<const std::byte> b){
    if(a.empty()||b.empty())return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
    return x<=y?y-x<a.size():x-y<b.size();
}
}
bool prepareGeometry(const CommandDraw& draw,std::uint8_t coordinates,std::span<TransformedVertex> vertices,
                     std::span<u16> indices,PreparedGeometry& out,GeometryError* error,VertexTransformError* transformError){
    out={};if(error)*error=GeometryError::none;if(transformError)*transformError=VertexTransformError::none;
    auto fail=[&](GeometryError value){if(error)*error=value;return false;};
    const auto count=draw.vertexCount;std::size_t needed=0;
    if(draw.indexCount){
        if(draw.primitive!=GX_TRIANGLES||draw.indexCount%3||draw.indices.size()/2<draw.indexCount)return fail(GeometryError::index);
        needed=draw.indexCount;
    }else{
        switch(draw.primitive){
        case GX_TRIANGLES:if(count<3||count%3)return fail(GeometryError::primitive);needed=count;break;
        case GX_QUADS:if(count<4||count%4)return fail(GeometryError::primitive);needed=std::size_t(count/4)*6;break;
        case GX_TRIANGLESTRIP:case GX_TRIANGLEFAN:if(count<3)return fail(GeometryError::primitive);needed=std::size_t(count-2)*3;break;
        default:return fail(GeometryError::primitive);
        }
    }
    if(!count||vertices.size()<count||indices.size()<needed)return fail(GeometryError::capacity);
    // CommandProcessor validates this extent, but standalone callers may not.
    // Check before CommandDraw::vertex creates its per-vertex subspan.
    const auto stride=draw.plan.packedStride();
    if(!stride||count>draw.vertices.size()/stride)return fail(GeometryError::decode);
    const auto vertexBytes=std::as_bytes(vertices.first(count)),indexBytes=std::as_bytes(indices.first(needed));
    if(overlap(vertexBytes,indexBytes))return fail(GeometryError::alias);
    auto aliases=[&](auto bytes){return overlap(vertexBytes,bytes)||overlap(indexBytes,bytes);};
    if(aliases(std::as_bytes(draw.vertices))||aliases(std::as_bytes(draw.indices))||
       aliases(std::as_bytes(std::span(&draw.registers,1)))||aliases(std::as_bytes(std::span(&draw.plan,1)))||
       aliases(std::as_bytes(std::span(&draw.arrays,1))))return fail(GeometryError::alias);
    for(const auto& array:draw.arrays)if(aliases(std::as_bytes(array.bytes)))return fail(GeometryError::alias);
    // Validate all optimized indices before publishing or transforming anything.
    for(u32 i=0;i<draw.indexCount;++i){u16 index;if(!draw.triangleIndex(i,index))return fail(GeometryError::index);}
    VertexTransformPlan plan;if(!planVertexTransform(draw.registers,coordinates,plan,transformError))return fail(GeometryError::transform);
    for(unsigned i=0;i<count;++i){
        DecodedVertex decoded;if(!draw.vertex(i,decoded))return fail(GeometryError::decode);
        if(!transformVertex(plan,decoded,vertices[i],transformError))return fail(GeometryError::transform);
    }
    if(draw.indexCount){for(u32 i=0;i<draw.indexCount;++i){u16 index;draw.triangleIndex(i,index);indices[i]=index;}}
    else{
        std::size_t at=0;
        if(!aurora::gx::dl::expand_triangles(draw.primitive,count,[&](u16 a,u16 b,u16 c){indices[at++]=a;indices[at++]=b;indices[at++]=c;}))return fail(GeometryError::primitive);
    }
    out={vertices.first(count),indices.first(needed),coordinates,plan.projectiveMask()};return true;
}
}
