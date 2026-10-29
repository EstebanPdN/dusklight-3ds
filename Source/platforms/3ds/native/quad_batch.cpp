#include "quad_batch.hpp"
namespace dusk::pica {
bool QuadBatch::begin(MaterialHandle m,const std::array<TextureHandle,3>& t,const DrawUniforms& u){
    if(active||!renderer.frameActive()||!m||!validateUniforms(u,0))return false;
    material=m;textures=t;uniforms=u;frame=renderer.statistics().frames;state=renderer.stateVersion();expectedDraws=renderer.statistics().draws;count=emitted=0;poisoned=false;active=true;return true;
}
bool QuadBatch::append(const Quad& q){
    if(!current()||poisoned){poisoned=true;return false;}
    if(count==capacity&&!flush())return false;
    const unsigned base=count*4,index=count*6;
    for(unsigned i=0;i<4;++i){auto& v=vertices[base+i];v={};v.position=q.corners[i];v.normal={0,0,1};v.uv=q.uv[i];v.color=q.colors[i];}
    // TL,TR,BR,BL. Preserve both painter order and provoking-vertex identity.
    constexpr unsigned order[]{0,1,2,2,3,0};
    for(unsigned i=0;i<6;++i)indices[index+i]=std::uint16_t(base+order[i]);
    ++count;return true;
}
bool QuadBatch::flush(){
    if(!current()||poisoned){poisoned=true;return false;}
    if(!count)return true;
    if(!renderer.drawTransient({vertices.data(),count*4},{indices.data(),count*6},material,textures,uniforms)){poisoned=true;return false;}
    ++emitted;expectedDraws=renderer.statistics().draws;count=0;return true;
}
bool QuadBatch::end(){
    if(!active)return false;
    const bool result=flush();active=false;return result;
}
}
