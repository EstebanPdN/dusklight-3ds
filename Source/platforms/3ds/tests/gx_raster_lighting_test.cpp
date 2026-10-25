#include "../runtime/gx_raster_color.hpp"
#include "../runtime/gx_vertex_transform.hpp"
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace dusk::native::gx;
int main(){
    CommandRegisters r{};
    auto word=[&](unsigned at,unsigned v){r.xf[at]=v;r.xfValid.set(at);};
    auto scalar=[&](unsigned at,float v){word(at,std::bit_cast<unsigned>(v));};
    PicaMaterialPlan material{};material.stageCount=1;
    material.stages[0].rgb.inputs[0]={MaterialSource::primary,MaterialOperand::color,false};
    material.stages[0].alpha.inputs[0]={MaterialSource::primary,MaterialOperand::alpha,false};
    word(0x1009,1);word(0x100e,2|1024);word(0x1010,0);
    word(0x100a,0x80402010);word(0x100c,0x80808080);
    RasterColorPlan p;RasterColorError error;DecodedVertex v{};std::array<float,4> out;
    auto near=[](float a,float b){assert(std::abs(a-b)<2e-6f);};
    assert(planRasterColor(r,material,p,&error)&&!p.needsPosition&&!p.needsNormal);
    assert(evaluateRasterColor(p,v,out));near(out[0],128.f*128/(255*255));near(out[3],128.f/255);
    // Different sources for ambient and material, independent alpha lighting.
    word(0x100e,2|64|1024);word(0x1010,1|2|1024);
    v.present=1u<<GX_VA_CLR0;v.color[0][0]=.25;v.color[0][1]=.5;v.color[0][2]=.75;v.color[0][3]=.5;
    assert(planRasterColor(r,material,p)&&evaluateRasterColor(p,v,out));near(out[0],.25f*128/255);near(out[3],.5f*16/255);
    v.present=0;assert(!evaluateRasterColor(p,v,out,&error)&&error==RasterColorError::vertex);
    // Constant lights do not require positions, normals, direction or attenuation.
    word(0x100e,2|4|1024);word(0x1010,0);word(0x603,0x402010ff);
    assert(planRasterColor(r,material,p)&&!p.needsPosition&&!p.needsNormal);
    assert(evaluateRasterColor(p,v,out));near(out[0],128.f*192/(255*255));
    // Light 7 exercises the high mask nibble. Sum clamps BEFORE material multiply.
    word(0x100e,2|4|(1u<<14)|1024);word(0x673,0xffffffff);
    assert(planRasterColor(r,material,p)&&evaluateRasterColor(p,v,out));near(out[0],128.f/255);
    // Black RGB light is a dead dependency, even when its alpha is nonzero.
    word(0x100e,2|4|1024|512|(2u<<7));word(0x603,0x000000ff);
    assert(planRasterColor(r,material,p)&&p.channels[0].mask==0&&!p.needsNormal&&!p.needsPosition);
    assert(evaluateRasterColor(p,v,out));near(out[0],128.f*128/(255*255));
    // Front/back diffuse, signed and clamped, versus fixed analytic values.
    word(0x100e,2|4|1024|(2u<<7));word(0x100a,0x80808080);word(0x603,0x40404040);
    for(unsigned c=0;c<3;++c)scalar(0x60a+c,c==2?2:0);
    TransformedVertex t{};t.viewNormal={0,0,1};t.normalValid=true;
    assert(planRasterColor(r,material,p)&&p.needsNormal&&p.needsPosition);
    assert(!evaluateRasterColor(p,v,out,&error)&&error==RasterColorError::transform);
    assert(evaluateRasterColor(p,v,out,&error,&t));near(out[0],128.f*192/(255*255));
    t.viewNormal[2]=-1;assert(evaluateRasterColor(p,v,out,&error,&t));near(out[0],128.f*128/(255*255));
    word(0x100e,2|4|1024|(1u<<7));assert(planRasterColor(r,material,p)&&evaluateRasterColor(p,v,out,&error,&t));near(out[0],128.f*64/(255*255));
    // Spotlight: unit cosine, distance 2, attenuation (1)/(1+d^2)=1/5.
    word(0x100e,2|4|1024|512|(2u<<7));t.viewNormal[2]=1;
    for(unsigned c=0;c<3;++c){scalar(0x60d+c,c==2?1:0);scalar(0x604+c,c==0?1:0);scalar(0x607+c,c==1?0:1);}
    assert(planRasterColor(r,material,p)&&evaluateRasterColor(p,v,out,&error,&t));near(out[0],128.f/255*(128.f/255+64.f/255/5));
    // Specular path, constant denominator and numerator, independent of distance.
    word(0x100e,2|4|512);scalar(0x609,0);
    assert(planRasterColor(r,material,p)&&evaluateRasterColor(p,v,out,&error,&t));near(out[0],128.f*192/(255*255));
    // Missing consumed data, invalid scalar and undefined attenuation fail cleanly.
    r.xfValid.reset(0x60f);assert(!planRasterColor(r,material,p,&error)&&error==RasterColorError::missingState);
    scalar(0x60f,std::numeric_limits<float>::quiet_NaN());assert(!planRasterColor(r,material,p,&error)&&error==RasterColorError::nonfinite);
    scalar(0x60f,1);scalar(0x607,0);assert(planRasterColor(r,material,p));assert(!evaluateRasterColor(p,v,out,&error,&t)&&error==RasterColorError::lighting);
    // No-light channels ignore all unused corrupt light records.
    word(0x100e,2|1024);assert(planRasterColor(r,material,p)&&evaluateRasterColor(p,v,out));
    // Transform the normal by the selected palette matrix, normalize once.
    word(0x1026,1);float projection[]{1,0,1,0,-1,0},viewport[]{200,-120,16777215,540,460,16777215};
    for(unsigned i=0;i<6;++i){scalar(0x1020+i,projection[i]);scalar(0x101a+i,viewport[i]);}
    r.matrixIndex[0]=3;r.matrixIndexValid.set(0);
    for(unsigned i=0;i<12;++i)scalar(12+i,i==0||i==5||i==10?1:0);
    // Rotate +Z to +X with a factor of two, which normalization removes.
    float normal[]{0,0,2,0,1,0,-1,0,0};for(unsigned i=0;i<9;++i)scalar(0x409+i,normal[i]);
    v.present=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM);v.normalCount=1;v.normal[0][2]=1;
    VertexTransformPlan transform;VertexTransformError transformError;
    assert(planVertexTransform(r,0,transform,&transformError));
    assert(transformVertex(transform,v,t,&transformError,true)&&t.normalValid);near(t.viewNormal[0],1);near(t.viewNormal[2],0);
    // A plan borrows immutable registers for one draw and caches matrices.
    // A changed XF validity bit starts a new draw/plan, not the old cache.
    r.xfValid.reset(0x409);assert(planVertexTransform(r,0,transform,&transformError));assert(!transformVertex(transform,v,t,&transformError,true)&&transformError==VertexTransformError::missingState);
    assert(transformVertex(transform,v,t,&transformError,false)&&!t.normalValid);
    std::puts("PASS ambient/material sources, RGB/alpha independence, low/high light masks, diffuse sign/clamp, spot/specular attenuation, missing/nonfinite data and selected normal palette");
}
