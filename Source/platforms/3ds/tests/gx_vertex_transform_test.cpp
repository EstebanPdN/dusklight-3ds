#include "../runtime/gx_vertex_transform.hpp"
#include "../runtime/gx_geometry.hpp"
#include <vector>
#include "../runtime/gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
using Matrix=std::array<float,12>;
using Vector=std::array<double,4>;
static Vector multiply(const Matrix& m,const Vector& v){Vector out{0,0,0,1};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)out[r]+=double(m[4*r+c])*v[c];return out;}
static void close(float value,double expected){assert(std::isfinite(value)&&std::abs(double(value)-expected)<2e-5*(1+std::abs(expected)));}
static Matrix affine(unsigned n){const float f=0.1f*n;return {1+f,.2f,.3f,4+f,.4f,2+f,.5f,-3-f,.6f,.7f,3+f,2-f};}
static DecodedVertex vertex(){DecodedVertex v{};v.present=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_CLR0)|(1u<<GX_VA_CLR1);v.normalCount=3;
    for(unsigned c=0;c<3;++c){v.position[c]=.25f+c;for(unsigned s=0;s<3;++s)v.normal[s][c]=.15f+s*.23f+c*.1f;}
    for(unsigned n=0;n<8;++n){v.present|=1u<<(GX_VA_TEX0+n);v.texcoord[n][0]=.12f+n*.08f;v.texcoord[n][1]=.27f+n*.09f;}
    for(unsigned n=0;n<2;++n)for(unsigned c=0;c<4;++c)v.color[n][c]=.15f+n*.2f+c*.1f;return v;
}
static Vector source(const DecodedVertex& v,GXTexGenSrc src){Vector out{0,0,1,1};
    if(src==GX_TG_POS)for(unsigned c=0;c<3;++c)out[c]=v.position[c];
    else if(src<=GX_TG_TANGENT)for(unsigned c=0;c<3;++c)out[c]=v.normal[unsigned(src)-1][c];
    else if(src>=GX_TG_TEX0&&src<=GX_TG_TEX7){out[0]=v.texcoord[src-GX_TG_TEX0][0];out[1]=v.texcoord[src-GX_TG_TEX0][1];}
    else for(unsigned c=0;c<4;++c)out[c]=v.color[src==GX_TG_COLOR0?0:1][c];return out;
}
static void projectionSetup(bool perspective){
    const float ortho[]{.5f,0,0,-.3f,0,.7f,0,.2f,0,0,-.2f,-.5f,0,0,0,1};
    const float persp[]{.5f,0,-.3f,0,0,.7f,.2f,0,0,0,-.2f,-.5f,0,0,-1,0};
    GXSetProjection(perspective?persp:ortho,perspective?GX_PERSPECTIVE:GX_ORTHOGRAPHIC);
    GXSetViewport(17,29,320,160,.15f,.85f);
}
static void checkClip(const TransformedVertex& output,const Matrix& position,const DecodedVertex& v,bool perspective){
    const auto view=multiply(position,{v.position[0],v.position[1],v.position[2],1});
    const Vector clip{.5*view[0]-.3*(perspective?view[2]:1),.7*view[1]+.2*(perspective?view[2]:1),-.2*view[2]-.5,perspective?-view[2]:1};
    // Independently map through the original viewport into the rotated 3DS
    // screen and depth range, keeping homogeneous coordinates throughout.
    const double px=17+160*(1+clip[0]/clip[3]);const double py=29+80*(1-clip[1]/clip[3]);
    close(output.clip[0],(1-2*py/240)*clip[3]);close(output.clip[1],(1-2*px/400)*clip[3]);
    close(output.clip[2],.7*clip[2]-.15*clip[3]);close(output.clip[3],clip[3]);
    for(unsigned c=0;c<3;++c)close(output.viewPosition[c],view[c]);
}
static void errorCase(const CommandRegisters& r,DecodedVertex v,unsigned mask,VertexTransformError expected){
    VertexTransformPlan p;TransformedVertex output;output.clip.fill(17);VertexTransformError e;
    if(planVertexTransform(r,mask,p,&e)){
        assert(!transformVertex(p,v,output,&e));
        for(auto n:output.clip)assert(n==0);for(auto n:output.viewPosition)assert(n==0);
        for(auto uv:output.texcoord)for(auto n:uv)assert(n==0);
    }
    else assert(p.coordinateMask()==0&&p.projectiveMask()==0);
    assert(e==expected);
}
static void geometryCases(CommandProcessor& processor,CommandStream& stream){
    projectionSetup(false);GXSetCurrentMtx(GX_PNMTX0);
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    assert(stream.finish());
    struct Context {std::vector<u16> expected;unsigned calls=0;} context;
    processor.setSink([](const CommandDraw& draw,void* ptr){
        auto& context=*static_cast<Context*>(ptr);std::array<TransformedVertex,16> vertices{};std::array<u16,48> indices{};
        PreparedGeometry prepared;GeometryError error;
        vertices[0].clip.fill(19);indices.fill(0xabcd);
        assert(!prepareGeometry(draw,1,std::span(vertices).first(1),indices,prepared,&error)&&error==GeometryError::capacity);
        assert(prepared.vertices.empty()&&vertices[0].clip[0]==19&&indices[0]==0xabcd);
        assert(!prepareGeometry(draw,1,vertices,std::span(indices).first(1),prepared,&error)&&error==GeometryError::capacity);
        assert(prepareGeometry(draw,1,vertices,indices,prepared,&error));
        assert(prepared.vertices.size()==draw.vertexCount&&prepared.indices.size()==context.expected.size());
        for(unsigned i=0;i<prepared.indices.size();++i)assert(prepared.indices[i]==context.expected[i]);
        for(unsigned i=0;i<draw.vertexCount;++i){close(prepared.vertices[i].texcoord[0][0],i*.125);close(prepared.vertices[i].texcoord[0][1],i*.25);}
        // Invalid optimized indices and output overlap are refused before writes.
        auto malformed=draw;malformed.indexCount=3;malformed.indices={};malformed.primitive=GX_TRIANGLES;
        assert(!prepareGeometry(malformed,1,vertices,indices,prepared,&error)&&error==GeometryError::index&&prepared.vertices.empty());
        auto truncated=draw;truncated.vertices=draw.vertices.first(draw.vertices.size()-1);
        vertices[0].clip.fill(19);indices.fill(0xabcd);
        assert(!prepareGeometry(truncated,1,vertices,indices,prepared,&error)&&error==GeometryError::decode);
        assert(prepared.vertices.empty()&&vertices[0].clip[0]==19&&indices[0]==0xabcd);
        assert(!prepareGeometry(draw,1,vertices,{reinterpret_cast<u16*>(vertices.data()),indices.size()},prepared,&error)&&error==GeometryError::alias);
        ++context.calls;return true;
    },&context);
    const GXVtxDescList desc[]{{GX_VA_POS,GX_DIRECT},{GX_VA_TEX0,GX_DIRECT},{GX_VA_NULL,GX_NONE}};
    const GXVtxAttrFmtList fmt[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_TEX0,GX_TEX_ST,GX_F32,0},{GX_VA_NULL,GX_POS_XY,GX_U8,0}};
    aurora::gx::dl::VtxFmtLists fmts{};fmts[0]=fmt;
    const GXPrimitive primitive[]{GX_QUADS,GX_TRIANGLES,GX_TRIANGLESTRIP,GX_TRIANGLEFAN};
    const unsigned counts[]{4,6,5,5};
    const std::vector<u16> expected[]{{0,1,2,2,3,0},{0,1,2,3,4,5},{0,1,2,2,1,3,2,3,4},{0,1,2,0,2,3,0,3,4}};
    for(unsigned test=0;test<4;++test){context.expected=expected[test];std::vector<u8> bytes{u8(primitive[test]),0,u8(counts[test])};
        for(unsigned i=0;i<counts[test];++i)for(float value:{float(i),float(i+1),-2.0f,float(i)*.125f,float(i)*.25f}){const auto bits=std::bit_cast<u32>(value);for(int shift=24;shift>=0;shift-=8)bytes.push_back(bits>>shift);}
        assert(processor.execute(bytes));auto optimized=aurora::gx::dl::optimize(bytes.data(),bytes.size(),desc,&fmts);assert(optimized&&processor.execute(*optimized));
    }
    assert(context.calls==8);processor.setSink(nullptr);
}
int main(){
    CommandMemory memory;CommandProcessor processor(memory);u8 scratch[256];CommandStream stream(processor,scratch);assert(setCommandStream(&stream));
    const auto positionA=affine(1),positionB=affine(2),texA=affine(3),texB=affine(4),post=affine(5);
    GXLoadPosMtxImm(positionA.data(),GX_PNMTX0);GXLoadPosMtxImm(positionB.data(),GX_PNMTX1);GXSetCurrentMtx(GX_PNMTX0);
    GXLoadTexMtxImm(texA.data(),GX_TEXMTX0,GX_MTX3x4);GXLoadTexMtxImm(texB.data(),GX_TEXMTX1,GX_MTX3x4);GXLoadTexMtxImm(post.data(),GX_PTTEXMTX2,GX_MTX3x4);GXSetNumTexGens(8);
    constexpr GXTexGenSrc sources[]{GX_TG_POS,GX_TG_NRM,GX_TG_BINRM,GX_TG_TANGENT,GX_TG_TEX0,GX_TG_TEX1,GX_TG_TEX2,GX_TG_TEX3,GX_TG_TEX4,GX_TG_TEX5,GX_TG_TEX6,GX_TG_TEX7,GX_TG_COLOR0};
    unsigned compared=0;
    for(bool perspective:{false,true})for(unsigned coord=0;coord<8;++coord)for(auto src:sources)
    for(bool projective:{false,true})for(bool normalize:{false,true})for(bool postEnabled:{false,true})for(unsigned selector=0;selector<3;++selector){
        projectionSetup(perspective);const auto type=projective?GX_TG_MTX3x4:GX_TG_MTX2x4;
        GXSetTexCoordGen2(static_cast<GXTexCoordID>(coord),type,src,selector==0?GX_IDENTITY:GX_TEXMTX0,normalize,postEnabled?GX_PTTEXMTX2:GX_PTIDENTITY);
        assert(stream.finish());auto v=vertex();if(selector==2){v.present|=1u<<GX_VA_PNMTXIDX;v.matrix[0]=3;v.present|=1u<<(GX_VA_TEX0MTXIDX+coord);v.matrix[coord+1]=33;}
        VertexTransformPlan plan;assert(planVertexTransform(processor.registers(),1u<<coord,plan));TransformedVertex output;assert(transformVertex(plan,v,output));
        assert(plan.projectiveMask()==(projective?1u<<coord:0));checkClip(output,selector==2?positionB:positionA,v,perspective);
        auto expected=source(v,src);if(selector)expected=multiply(selector==2?texB:texA,expected);if(!projective)expected[2]=1;
        if(normalize){const double length=std::sqrt(expected[0]*expected[0]+expected[1]*expected[1]+expected[2]*expected[2]);for(unsigned c=0;c<3;++c)expected[c]/=length;}
        expected[3]=1;if(postEnabled)expected=multiply(post,expected);if(!projective)expected[2]=1;
        for(unsigned c=0;c<3;++c)close(output.texcoord[coord][c],expected[c]);++compared;
        // Exercise a second vertex through the same validated matrix cache,
        // including a position palette switch and returning to the first one.
        v.position[0]+=.37f;v.present|=1u<<GX_VA_PNMTXIDX;
        for(unsigned position:{0u,3u,0u}){v.matrix[0]=position;
            assert(transformVertex(plan,v,output));checkClip(output,position?positionB:positionA,v,perspective);}
    }
    projectionSetup(false);
    for(unsigned color=0;color<2;++color){GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_SRTG,color?GX_TG_COLOR1:GX_TG_COLOR0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);assert(stream.finish());
        VertexTransformPlan p;TransformedVertex out;auto v=vertex();assert(planVertexTransform(processor.registers(),1,p)&&transformVertex(p,v,out));close(out.texcoord[0][0],v.color[color][0]);close(out.texcoord[0][1],v.color[color][1]);close(out.texcoord[0][2],1);}
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX3x4,GX_TG_TEX0,GX_TEXMTX0,GX_FALSE,GX_PTIDENTITY);assert(stream.finish());
    const auto good=processor.registers();auto v=vertex();VertexTransformPlan plan;assert(planVertexTransform(good,1,plan));TransformedVertex out;assert(transformVertex(plan,v,out));
    // q must survive, rather than being divided per vertex and interpolated incorrectly.
    const auto uv=multiply(texA,source(v,GX_TG_TEX0));assert(std::abs(uv[2]-1)>.1);close(out.texcoord[0][2],uv[2]);close(out.texcoord[0][0],uv[0]);
    auto r=good;r.xfValid.reset(0x1020);errorCase(r,v,1,VertexTransformError::missingState);
    r=good;r.xf[0x1026]=2;errorCase(r,v,1,VertexTransformError::projection);
    r=good;r.xf[0x101a]=0;errorCase(r,v,1,VertexTransformError::viewport);
    r=good;r.xf[120]=std::bit_cast<u32>(std::numeric_limits<float>::infinity());errorCase(r,v,1,VertexTransformError::nonfinite);
    r=good;r.xfValid.reset(120);errorCase(r,v,1,VertexTransformError::missingState);
    r=good;r.matrixIndexValid.reset(0);errorCase(r,v,1,VertexTransformError::missingState);
    v.matrix[0]=1;v.present|=1u<<GX_VA_PNMTXIDX;errorCase(good,v,1,VertexTransformError::matrixIndex);v=vertex();
    v.matrix[1]=63;v.present|=1u<<GX_VA_TEX0MTXIDX;errorCase(good,v,1,VertexTransformError::matrixIndex);v=vertex();
    v.present&=~(1u<<GX_VA_TEX0);errorCase(good,v,1,VertexTransformError::missingAttribute);v=vertex();
    v.position[0]=std::numeric_limits<float>::quiet_NaN();errorCase(good,v,1,VertexTransformError::nonfinite);v=vertex();
    r=good;r.xf[0x1040]=(1u<<4);errorCase(r,v,1,VertexTransformError::texgen);
    r=good;r.xf[0x1050]=60;errorCase(r,v,1,VertexTransformError::matrixIndex);
    r=good;r.xf[0x103f]=0;errorCase(r,v,1,VertexTransformError::texgen);
    r=good;for(unsigned n=0;n<12;++n)r.xf[120+n]=0;r.xf[0x1050]=61|256;errorCase(r,v,1,VertexTransformError::normalization);
    // Missing unused texgen state and unused palette slots must not reject position-only draws.
    r=good;r.xfValid.reset(0x1040);assert(planVertexTransform(r,0,plan)&&transformVertex(plan,v,out));
    // A normal-only vertex cannot supply a binormal or tangent.
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_BINRM,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);assert(stream.finish());v.normalCount=1;
    errorCase(processor.registers(),v,1,VertexTransformError::missingAttribute);
    geometryCases(processor,stream);
    assert(setCommandStream(nullptr));
    std::printf("{\"transform_cases\":%u,\"plan_bytes\":%zu,\"decoded_vertex_bytes\":%zu,\"transformed_vertex_bytes\":%zu,\"gpu_executed\":false}\n",compared,sizeof(VertexTransformPlan),sizeof(DecodedVertex),sizeof(TransformedVertex));
}
