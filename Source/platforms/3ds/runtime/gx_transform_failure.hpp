#pragma once
#include "gx_vertex_transform_diagnostic.hpp"
#include "gx_pica_math.hpp"
#include <bit>
#include <cstring>
namespace dusk::native::gx {
// Failure-only bounded position survey. Never changes acceptance or GPU data.
// Extents are checked here: CommandDraw accessors assume validated extents.
inline void surveyTransformFailure(const CommandDraw& draw,TransformDiagnosticSink sink){
    if(!sink)return;
    constexpr unsigned vertexCap=256,indexCap=768;
    const auto stride=draw.plan.packedStride();
    const bool extent=draw.vertexCount&&stride&&draw.vertexCount<=draw.vertices.size()/stride;
    bool topology=true;
    unsigned checkedIndices=0;
    if(draw.indexCount){
        topology=draw.primitive==GX_TRIANGLES&&draw.indexCount%3==0&&draw.indexCount<=draw.indices.size()/2;
        if(topology){
            for(;checkedIndices<draw.indexCount&&checkedIndices<indexCap;++checkedIndices){
                u16 index;std::memcpy(&index,draw.indices.data()+2*checkedIndices,2);
                if(index>=draw.vertexCount){topology=false;break;}
            }
            topology=topology&&checkedIndices==draw.indexCount;
        }
    }else{
        switch(draw.primitive){
        case GX_TRIANGLES:topology=draw.vertexCount>=3&&draw.vertexCount%3==0;break;
        case GX_QUADS:topology=draw.vertexCount>=4&&draw.vertexCount%4==0;break;
        case GX_TRIANGLESTRIP:case GX_TRIANGLEFAN:topology=draw.vertexCount>=3;break;
        default:topology=false;
        }
    }
    sink("survey",draw.vertexCount,draw.indexCount,extent,topology);
    VertexTransformPlan p;
    bool positions=extent&&planVertexTransform(draw.registers,0,p);
    unsigned checked=0,common=15;
    bool identical=true;
    std::array<unsigned,4> first{};
    if(positions)for(;checked<draw.vertexCount&&checked<vertexCap;++checked){
        DecodedVertex v;TransformedVertex out;
        if(!draw.vertex(checked,v)||!transformVertex(p,v,out)){positions=false;break;}
        std::array<unsigned,4> bits{};
        for(unsigned c=0;c<4;++c)bits[c]=std::bit_cast<unsigned>(out.clip[c]);
        if(!checked)first=bits;else identical=identical&&bits==first;
        common&=picaClipSideMask(out.clip);
        // Implicit sequential vertex index: one exact 4-word record per success.
        sink("survey_clip",bits[0],bits[1],bits[2],bits[3]);
    }
    const bool complete=positions&&checked==draw.vertexCount;
    sink("survey_end",checked,complete,complete?common:0,complete&&identical);
    sink("survey_bounds",vertexCap,indexCap,checkedIndices,topology&&complete);
}
}
