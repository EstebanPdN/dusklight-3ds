#pragma once
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/uniforms.h>
}
#include <cstring>
namespace dusk::native {
// Compare against Citro3D's actual shared shadow, not a separate last-draw
// cache. Other shaders/writers are therefore visible, and existing dirty flags
// (including frame/context invalidation) remain set even when values match.
inline void setGxVertexUniform(int id,const C3D_FVec& value){
    if(std::memcmp(&C3D_FVUnif[GPU_VERTEX_SHADER][id],&value,sizeof(value)))
        *C3D_FVUnifWritePtr(GPU_VERTEX_SHADER,id,1)=value;
}
inline void setGxVertexMatrix(int id,const C3D_Mtx& value){
    for(unsigned row=0;row<4;++row)setGxVertexUniform(id+row,value.r[row]);
}
inline void setGxVertexLoop(int id,int count,int first,int increment,int unused){
    const auto value=IVec_Pack(count,first,increment,unused);
    if(std::memcmp(&C3D_IVUnif[GPU_VERTEX_SHADER][id-0x60],&value,sizeof(value)))
        *C3D_IVUnifWritePtr(GPU_VERTEX_SHADER,id)=value;
}
}
