// Multi-texture texgen on the GPU against the CPU vertex path. For register
// banks with 2x4 texgens from TEX0, TEX1 and the normal (role -1, the J3D
// environment maps), planRigidMultiVertex and finishCompactRigidUniforms
// build the uniform block exactly as the game does. The block, the compact
// attributes and the CPU route's final coordinates (transformVertex, texture
// scale, vertical flip) are printed as JSON; Tools/3ds/test_gx_rigid_texgen.py
// runs gx_rigid_mt.v.pica on them and compares. Host floats only: PICA
// float24 rounding is an Azahar/console check.
#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_rigid_vertex.hpp"
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
using namespace dusk::native::gx;
namespace {
struct Unit {unsigned coordinate,source,matrix,post;}; // source: GX row (0 position, 1 normal, 5 TEX0, 6 TEX1)
struct Case {const char* name;bool lit;std::array<Unit,3> units;unsigned count;unsigned refused=0;bool normalOff=false;unsigned reverseFog=0;bool sewersFog=false;};
void setf(CommandRegisters& r,unsigned at,float v){r.xf[at]=std::bit_cast<u32>(v);r.xfValid.set(at);}
void row(const std::array<float,4>& v){std::printf("[%.9g,%.9g,%.9g,%.9g]",double(v[0]),double(v[1]),double(v[2]),double(v[3]));}
}
int main(){
    std::mt19937 rng(1116);
    auto uni=[&](float a,float b){return std::uniform_real_distribution<float>(a,b)(rng);};
    // Units sample coordinates 0..2; matrix 60 is identity, 30/33/36 TEXMTX0-2,
    // post 61 identity or a dual-texgen matrix (folded into the rows).
    const Case cases[]{
        {"sword: TEX0, normal on unit 1",true,{{{0,5,60,61},{1,1,33,61},{0,0,0,0}}},2},
        {"freezard: normal on unit 0",true,{{{1,1,33,61},{0,5,60,61},{0,0,0,0}}},2},
        {"effect: TEX0, TEX0, normal on unit 2",true,{{{0,5,30,61},{1,5,33,61},{2,1,36,61}}},3},
        {"normal with a post matrix",true,{{{0,6,60,61},{1,1,30,3},{0,0,0,0}}},2},
        // J3D post-texmtx models: identity first stage, the matrix in post.
        {"normal, identity texgen, post matrix",true,{{{0,5,60,61},{1,1,60,6},{0,0,0,0}}},2},
        {"unlit normal texgen",false,{{{0,1,36,61},{1,6,33,61},{0,0,0,0}}},2},
        {"TEX1 only (role 1 unchanged)",true,{{{0,6,30,61},{1,5,33,6},{0,0,0,0}}},2},
        {"reverse exponential constant fog in unit 2",true,{{{0,5,60,61},{1,6,60,61},{0,0,0,0}}},2,0,false,6},
        {"reverse exponential squared constant fog with range",true,{{{0,5,60,61},{1,1,33,61},{0,0,0,0}}},2,0,false,7},
        {"Castle sewers saturated linear fog with range",true,{{{0,5,60,61},{1,6,60,61},{0,0,0,0}}},2,0,false,0,true},
        // 2x4 from the position (ABC1): the compact origin is not folded
        // into unit rows, so it stays refused (reject 10) and is recorded.
        {"position texgen refused",true,{{{0,5,60,61},{1,0,33,61},{0,0,0,0}}},2,10},
        // A/B switch mt-normal-texgen-off.txt: the normal unit is refused as
        // before (reject 15).
        {"normal texgen switched off",true,{{{0,5,60,61},{1,1,33,61},{0,0,0,0}}},2,15,true},
    };
    std::printf("{\"cases\":[");
    bool firstCase=true;
    for(const auto& test:cases){
        CommandRegisters r{};
        for(unsigned i=0;i<6;++i)setf(r,0x1020+i,std::array{1.2f,0.f,1.6f,0.f,-1.0001f,-0.20001f}[i]);
        r.xf[0x1026]=0;r.xfValid.set(0x1026);
        for(unsigned i=0;i<6;++i)setf(r,0x101a+i,std::array{160.f,-120.f,0.f,502.f,462.f,16777215.f}[i]);
        r.xf[0x103f]=3;r.xfValid.set(0x103f);
        // Position matrix 0 (view space, in front of the camera), normal 3x3.
        const std::array<float,12> position{.8f,.1f,-.2f,3,.05f,.9f,.1f,-2,.2f,-.1f,.85f,-60};
        for(unsigned k=0;k<12;++k)setf(r,k,position[k]);
        for(unsigned k=0;k<9;++k)setf(r,0x400+k,position[k/3*4+k%3]*1.25f);
        // Texture matrices 30..38 (J3D Envmap-like: 0.5 scale plus rotation)
        // and post matrices from 0x500 on.
        for(unsigned at=4*30;at<4*39;++at)setf(r,at,uni(-.6f,.6f));
        for(unsigned at=0x500;at<0x500+4*9;++at)setf(r,at,uni(-1,1));
        std::array<std::uint8_t,3> coordinateGen{};
        for(unsigned u=0;u<test.count;++u){
            const auto& unit=test.units[u];
            const u32 form=unit.source<=4;
            r.xf[0x1040+unit.coordinate]=(form<<2)|(unit.source<<7);r.xfValid.set(0x1040+unit.coordinate);
            r.xf[0x1050+unit.coordinate]=unit.post==61?61:unit.post;r.xfValid.set(0x1050+unit.coordinate);
            coordinateGen[unit.coordinate]=std::uint8_t(unit.matrix);
        }
        // Coordinates nobody reads keep a valid identity texgen from TEX0.
        for(unsigned c=0;c<3;++c)if(!r.xfValid[0x1040+c]){r.xf[0x1040+c]=5u<<7;r.xfValid.set(0x1040+c);r.xf[0x1050+c]=61;r.xfValid.set(0x1050+c);coordinateGen[c]=60;}
        r.matrixIndex[0]=0|(u32(coordinateGen[0])<<6)|(u32(coordinateGen[1])<<12)|(u32(coordinateGen[2])<<18)|(60u<<24);
        r.matrixIndexValid.set(0);
        std::uint8_t mask=0;for(unsigned u=0;u<test.count;++u)mask|=std::uint8_t(1u<<test.units[u].coordinate);
        VertexTransformPlan transform;VertexTransformError error{};
        if(!planVertexTransform(r,mask,transform,&error)){std::fprintf(stderr,"%s: transform plan %d\n",test.name,int(error));return 1;}
        RasterColorPlan color{};
        color.material={1,1,1,1};color.ambient={.2f,.25f,.3f,1};
        if(test.lit){
            color.lit=1;color.required=1;color.channels[0]={1,2,1};
            color.lights[0].color={.6f,.5f,.4f,1};color.lights[0].position={10,20,-30};
            color.lights[0].direction={0,0,1};color.lights[0].cosine={1,0,0};color.lights[0].distance={1,0,0};
        }
        PicaMaterialPlan material{};material.textureCount=std::uint8_t(test.count);
        for(unsigned u=0;u<test.count;++u)material.textures[u]={std::uint8_t(u),std::uint8_t(test.units[u].coordinate)};
        const std::array<std::array<float,2>,3> scales{{{.5f,.25f},{1,1},{.75f,.125f}}};
        DecodedVertex first{};first.present=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_TEX0)|(1u<<GX_VA_TEX1);first.normalCount=1;
        RigidVertexPlan rigid;
        const auto plansBefore=gxRigidNormalTexgenPlans;
        VertexFogPlan fog;
        const int fogUnit=test.reverseFog||test.sewersFog?int(test.count):-1;
        if(test.reverseFog){
            for(unsigned reg=0xe8;reg<=0xf2;++reg)r.bpValid.set(reg);
            r.bp[0xe8]=0x156|(test.reverseFog==7?1024u:0u);
            r.bp[0xe9]=0xfff000;r.bp[0xea]=0x200100;r.bp[0xed]=0x040020;
            r.bp[0xef]=0x40000f;r.bp[0xf0]=1;r.bp[0xf1]=test.reverseFog<<21;r.bp[0xf2]=0x1240ee;
            assert(planVertexFog(r,fog)&&fog.enabled&&fog.zeroReverseRamp&&fog.b==.5);
            assert(appendVertexFog(material,fog)&&material.textureCount==3);
            const auto& step=material.stages[0];
            assert(step.rgb.inputs[0].source==MaterialSource::constant&&step.rgb.inputs[2].source==MaterialSource::texture2);
            assert(step.alpha.inputs[0].source==MaterialSource::previous&&step.alpha.inputs[0].operand==MaterialOperand::alpha);
            const auto refuses=[&](const CommandRegisters& registers,const VertexFogPlan& candidate,int unit){
                const auto before=gxRigidPlanRejects[20];
                assert(!planRigidMultiVertex(registers,first,transform,color,candidate,false,400,material,test.count,scales,unit,rigid));
                assert(gxRigidPlanRejects[20]==before+1);
            };
            rigidConstantFogRamp=false;refuses(r,fog,fogUnit);rigidConstantFogRamp=true;
            refuses(r,fog,-1);
            auto invalidLight=color;invalidLight.lights[0].position[0]=1e9f;
            const auto uniformRejects=gxRigidPlanRejects[25];
            assert(!planRigidMultiVertex(r,first,transform,invalidLight,fog,false,400,material,test.count,scales,fogUnit,rigid));
            assert(gxRigidPlanRejects[25]==uniformRejects+1&&gxRigidLastRefusedUniform.row==19&&
                   gxRigidLastRefusedUniform.component==0&&gxRigidLastRefusedUniform.value==1e9f);
            for(const auto [reg,word]:std::array<std::array<unsigned,2>,4>{{
                    {0xee,1},{0xee,0x38000},{0xf1,(test.reverseFog<<21)|0x80001u},
                    {0xf1,(test.reverseFog<<21)|0xb8000u}}}){
                auto changed=r;changed.bp[reg]=word;VertexFogPlan varying;
                assert(planVertexFog(changed,varying)&&varying.enabled&&!varying.zeroReverseRamp);
                refuses(changed,varying,fogUnit);
            }
            auto extreme=r;setf(extreme,0x101a,std::numeric_limits<float>::denorm_min());
            VertexFogPlan boundary;
            assert(planVertexFog(extreme,boundary)&&boundary.zeroReverseRamp);
            for(float w:{-1.f,0.f,std::numeric_limits<float>::denorm_min(),1.f,std::numeric_limits<float>::max()})
                for(float z:{-std::numeric_limits<float>::max(),std::nextafter(-.5f,-1.f),-.5f,std::nextafter(-.5f,0.f),0.f,1.f})
                    for(float x:{-std::numeric_limits<float>::max(),-1.f,0.f,1.f,std::numeric_limits<float>::max()}){
                        assert(evaluateVertexFog(boundary,{0,x,z,w},400,false)==0);
                        assert(evaluateVertexFog(boundary,{0,x,z,w},400)==1.f/256);
                    }
        }
        if(test.sewersFog){
            // GX BP writes from the room 3 material display lists in dump 049.
            const std::array<u32,11> words{0x696,0x104104,0x11810e,0x12c122,0x140136,0x15414a,
                0x6e18c,0x400092,2,0x4bf800,0x8863c4};
            for(unsigned i=0;i<words.size();++i){r.bp[0xe8+i]=words[i];r.bpValid.set(0xe8+i);}
            assert(planVertexFog(r,fog)&&fog.enabled&&fog.function==2&&fog.rangeEnabled&&fog.c==-1.0);
            assert(appendVertexFog(material,fog)&&material.textureCount==3);
            const auto refuses=[&](const VertexFogPlan& candidate,unsigned row,unsigned component){
                const auto before=gxRigidPlanRejects[25];
                assert(!planRigidMultiVertex(r,first,transform,color,candidate,false,400,material,test.count,scales,fogUnit,rigid));
                assert(gxRigidPlanRejects[25]==before+1&&gxRigidLastRefusedUniform.row==row&&
                       gxRigidLastRefusedUniform.component==component);
            };
            rigidConstantFogRamp=false;
            refuses(fog,11,0);
            assert(gxRigidLastRefusedUniform.value==float(fog.a));
            rigidConstantFogRamp=true;
            auto invalid=fog;invalid.c=0;refuses(invalid,11,0);
            invalid=fog;invalid.a=std::numeric_limits<float>::infinity();refuses(invalid,11,0);
            invalid=fog;invalid.b=1e9;refuses(invalid,11,1);
            invalid=fog;invalid.viewportWidth=1e-20;refuses(invalid,13,0);
            assert(planRigidMultiVertex(r,first,transform,color,fog,true,400,material,test.count,scales,-1,rigid));
            assert(rigid.uniforms[12][0]==2&&rigid.uniforms[12][2]==1);
            for(float w:{-1.f,0.f,std::numeric_limits<float>::denorm_min(),1.f})
                for(float z:{-std::numeric_limits<float>::max(),-1.f,0.f,1.f})
                    for(float x:{-std::numeric_limits<float>::max(),0.f,std::numeric_limits<float>::max()})
                        assert(evaluateVertexFog(fog,{0,x,z,w},400)==1.f);
        }
        if(test.refused){
            const auto before=gxRigidPlanRejects[test.refused];
            multiTextureNormalTexgen=!test.normalOff;
            const bool planned=planRigidMultiVertex(r,first,transform,color,VertexFogPlan{},false,400,material,test.count,scales,-1,rigid);
            multiTextureNormalTexgen=true;
            assert(!planned);
            assert(gxRigidPlanRejects[test.refused]==before+1&&gxRigidLastRefusedTexgen.code==test.refused);
            assert(gxRigidLastRefusedTexgen.gen==r.xf[0x1041]&&gxRigidLastRefusedTexgen.post==r.xf[0x1051]&&gxRigidNormalTexgenPlans==plansBefore);
            continue;
        }
        if(!planRigidMultiVertex(r,first,transform,color,fog,false,400,material,test.count,scales,fogUnit,rigid)){
            std::fprintf(stderr,"%s: planner refused\n",test.name);
            for(unsigned c=0;c<64;++c)if(gxRigidPlanRejects[c])std::fprintf(stderr,"  reject %u=%u\n",c,unsigned(gxRigidPlanRejects[c]));
            return 1;
        }
        bool normalUnit=false;
        for(unsigned u=0;u<test.count;++u){
            const float role=multiRole(rigid,u);
            assert(role==(test.units[u].source==1?-1.f:1.f));normalUnit|=role<0;
        }
        assert(gxRigidNormalTexgenPlans==plansBefore+(normalUnit?1:0));
        if(test.reverseFog||test.sewersFog){
            assert(rigid.uniforms[12][0]==2&&multiRole(rigid,fogUnit)==.5f);
            assert(rigid.uniforms[11][0]==float(test.sewersFog));
        }
        // The compact decoder's part: s16 normals (14 fraction bits), s16
        // TEX0 (10 bits), s16 TEX1 (8 bits), positions relative to the origin.
        rigid.decodedNormalScale=std::ldexp(1.f,-14);rigid.decodedTex0Scale=std::ldexp(1.f,-10);rigid.decodedTex1Scale=std::ldexp(1.f,-8);
        rigid.origin={0,0,0};
        if(!finishCompactRigidUniforms(rigid,{1,1})){std::fprintf(stderr,"%s: finish refused\n",test.name);return 1;}
        std::printf("%s{\"name\":\"%s\",\"lights\":%u,\"units\":%u,\"uniforms\":[",firstCase?"":",",test.name,rigid.lights,test.count);firstCase=false;
        for(unsigned n=0;n<94;++n){if(n)std::printf(",");row(rigid.uniforms[n]);}
        std::printf("],\"vertices\":[");
        for(unsigned i=0;i<48;++i){
            std::array<int,3> normal;
            std::array<float,3> n;
            do{for(unsigned c=0;c<3;++c)n[c]=uni(-1,1);}while(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]<.04f);
            const float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            for(unsigned c=0;c<3;++c)normal[c]=int(std::lround(n[c]/length*16384));
            const std::array<int,2> t0{int(rng()%4096)-2048,int(rng()%4096)-2048},t1{int(rng()%1024)-512,int(rng()%1024)-512};
            const std::array<float,3> p{uni(-20,20),uni(-20,20),uni(-20,20)};
            DecodedVertex v=first;
            for(unsigned c=0;c<3;++c){v.position[c]=p[c];v.normal[0][c]=float(normal[c])*rigid.decodedNormalScale;}
            for(unsigned c=0;c<2;++c){v.texcoord[0][c]=float(t0[c])*rigid.decodedTex0Scale;v.texcoord[1][c]=float(t1[c])*rigid.decodedTex1Scale;}
            TransformedVertex cpu;
            if(!transformVertex(transform,v,cpu,&error,false)){std::fprintf(stderr,"%s: CPU transform %d\n",test.name,int(error));return 1;}
            std::printf("%s{\"v0\":[%.9g,%.9g,%.9g,1],\"v1\":[%d,%d,%d,1],\"v2\":[%d,%d,0,1],\"v3\":[255,255,255,255],\"v4\":[%d,%d,0,1],\"v5\":[%d,%d,0,1],",
                i?",":"",double(p[0]),double(p[1]),double(p[2]),normal[0],normal[1],normal[2],t0[0],t0[1],
                int(rigid.matrixOffsets[0]),int(rigid.normalOffsets[0]),t1[0],t1[1]);
            std::printf("\"clip\":");row(cpu.clip);
            std::printf(",\"expected\":[");
            // The CPU route's final coordinate: uv scaled, vertical flip (q=1).
            for(unsigned u=0;u<test.count;++u){
                const auto& uv=cpu.texcoord[test.units[u].coordinate];
                std::printf("%s[%.9g,%.9g]",u?",":"",double(uv[0]*scales[u][0]),double(1-uv[1]*scales[u][1]));
            }
            if(test.reverseFog||test.sewersFog){
                const float ramp=evaluateVertexFog(fog,cpu.clip,400,false);
                std::printf(",[%.9g,0.5]",double((ramp*255+.5f)/256));
            }
            std::printf("]}");
        }
        std::printf("]}");
    }
    std::printf("]}\n");
    return 0;
}
