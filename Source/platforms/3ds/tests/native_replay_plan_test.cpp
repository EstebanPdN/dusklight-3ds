// Lean native shape replay (gx_command_draw.cpp nativeReplay) against the
// complete replay, at the planner level, over random register banks:
// - replayTexgenMask plus a projection-only planVertexTransform accept
//   exactly what planVertexTransform accepts, with the same projective mask
//   and clip projection;
// - prepareRigidMultiReuse + buildRigidMultiVertex on a plan whose storage
//   holds garbage give planRigidMultiVertex's decision, reject code, fields
//   and every row a submission uploads, also after the palette placement and
//   the finish step;
// - a continuation that restores only the unit rows 81-92 of a finished plan
//   equals one made from a copy of the planned block (the complete replay);
// - planRasterColor, built in place since wave 2, equals the previous
//   implementation (kept below) field by field.
// Run by Tools/3ds/test_gx_rigid_texgen.py. Host floats; the rows are
// compared as bits.
#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_rigid_vertex.hpp"
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dusk::native::gx;
namespace {
// planRasterColor/planRasterColorChannel as they were before wave 2 (a
// second plan built and copied), the reference for the in-place version.
namespace previous {
bool fail(RasterColorError e,RasterColorError* error){if(error)*error=e;return false;}
unsigned arity(MaterialFunction f){
    switch(f){case MaterialFunction::replace:return 1;
    case MaterialFunction::modulate:case MaterialFunction::add:return 2;
    case MaterialFunction::addSigned:case MaterialFunction::subtract:return 2;
    case MaterialFunction::interpolate:case MaterialFunction::multiplyAdd:return 3;}
    return 0;
}
bool planRasterColorChannel(const CommandRegisters& r,unsigned rasterChannel,std::uint8_t required,RasterColorPlan& out,RasterColorError* error,bool strictFinite){
    out={};if(error)*error=RasterColorError::none;RasterColorPlan p;
    if(rasterChannel>1||required>3)return fail(RasterColorError::material,error);
    p.required=required;p.rasterChannel=rasterChannel;p.strictFinite=strictFinite;
    if(p.required){
        if(!r.xfValid[0x1009]||r.xf[0x1009]<=rasterChannel||r.xf[0x1009]>2)return fail(RasterColorError::missingState,error);
        for(unsigned alpha=0;alpha<2;++alpha)if(p.required&(1u<<alpha)){
            const auto reg=(alpha?0x1010:0x100e)+rasterChannel;
            if(!r.xfValid[reg])return fail(RasterColorError::missingState,error);
            const auto control=r.xf[reg];
            if(control&2){
                p.lit|=1u<<alpha;auto& ch=p.channels[alpha];
                ch.mask=((control>>2)&15)|((control>>7)&240);ch.diffuse=(control>>7)&3;
                ch.attenuation=!(control&(1u<<10))?0:(!(control&(1u<<9))?2:1);
                if(ch.diffuse>2)return fail(RasterColorError::lighting,error);
                if(control&64)p.ambientVertex|=1u<<alpha;
                else {
                    if(!r.xfValid[0x100a+rasterChannel])return fail(RasterColorError::missingState,error);
                    for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)p.ambient[c]=float((r.xf[0x100a+rasterChannel]>>(24-8*c))&255)/255;
                }
                auto vector=[&](unsigned at,std::array<float,3>& value){
                    for(unsigned c=0;c<3;++c){
                        if(!r.xfValid[at+c])return fail(RasterColorError::missingState,error);
                        value[c]=std::bit_cast<float>(r.xf[at+c]);
                        if(!std::isfinite(value[c]))return fail(RasterColorError::nonfinite,error);
                    }return true;
                };
                for(unsigned i=0;i<8;++i)if(ch.mask&(1u<<i)){
                    auto& light=p.lights[i];const unsigned at=0x600+16*i;
                    if(!r.xfValid[at+3])return fail(RasterColorError::missingState,error);
                    for(unsigned c=0;c<4;++c)light.color[c]=float((r.xf[at+3]>>(24-8*c))&255)/255;
                    bool contributes=false;
                    for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)contributes|=light.color[c]!=0;
                    if(!contributes){ch.mask&=~(1u<<i);continue;}
                    if(ch.diffuse||ch.attenuation!=2){p.needsPosition=true;if(!vector(at+10,light.position))return false;}
                    if(ch.diffuse||ch.attenuation==0)p.needsNormal=true;
                    if(ch.attenuation!=2&&(!vector(at+13,light.direction)||!vector(at+4,light.cosine)||!vector(at+7,light.distance)))return false;
                }
            }
            if(control&1)p.vertex|=1u<<alpha;
            else{
                if(!r.xfValid[0x100c+rasterChannel])return fail(RasterColorError::missingState,error);
                for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)p.material[c]=float((r.xf[0x100c+rasterChannel]>>(24-8*c))&255)/255;
            }
        }
    }
    out=p;return true;
}
bool planRasterColor(const CommandRegisters& r,const PicaMaterialPlan& material,RasterColorPlan& out,RasterColorError* error){
    out={};if(error)*error=RasterColorError::none;RasterColorPlan p;
    if(material.rasterCarrier!=RasterCarrier::none||!material.stageCount||material.stageCount>6)return fail(RasterColorError::material,error);
    for(unsigned i=0;i<material.stageCount;++i)for(unsigned alpha=0;alpha<2;++alpha){
        const auto& channel=alpha?material.stages[i].alpha:material.stages[i].rgb;
        const auto count=arity(channel.function);if(!count)return fail(RasterColorError::material,error);
        for(unsigned n=0;n<count;++n){
            const auto& in=channel.inputs[n];if(in.source!=MaterialSource::primary)continue;
            if(in.operand==MaterialOperand::alpha||(alpha&&in.operand==MaterialOperand::color))p.required|=2;
            else p.required|=1;
        }
    }
    return previous::planRasterColorChannel(r,0,p.required,out,error,false);
}
}
template<class T>bool sameBits(const T& a,const T& b){return !std::memcmp(&a,&b,sizeof(T));}
// Field by field (RasterColorPlan has padding after `channels`).
bool sameColor(const RasterColorPlan& a,const RasterColorPlan& b){
    for(unsigned i=0;i<8;++i){
        const auto& x=a.lights[i];const auto& y=b.lights[i];
        if(!sameBits(x.color,y.color)||!sameBits(x.position,y.position)||!sameBits(x.direction,y.direction)||
           !sameBits(x.cosine,y.cosine)||!sameBits(x.distance,y.distance))return false;
    }
    for(unsigned c=0;c<2;++c)if(a.channels[c].mask!=b.channels[c].mask||a.channels[c].diffuse!=b.channels[c].diffuse||
        a.channels[c].attenuation!=b.channels[c].attenuation)return false;
    return sameBits(a.material,b.material)&&sameBits(a.ambient,b.ambient)&&a.lit==b.lit&&a.ambientVertex==b.ambientVertex&&
        a.needsPosition==b.needsPosition&&a.needsNormal==b.needsNormal&&a.strictFinite==b.strictFinite&&
        a.rasterChannel==b.rasterChannel&&a.required==b.required&&a.vertex==b.vertex;
}
// Every field a replay or a submission reads, and the uploaded rows.
bool samePlan(const RigidVertexPlan& a,const RigidVertexPlan& b,bool rows){
    if(a.matrixOffsets!=b.matrixOffsets||a.normalOffsets!=b.normalOffsets||a.matrices!=b.matrices||a.compactPalette!=b.compactPalette||
       a.paletteEnd!=b.paletteEnd||a.indexedMatrices!=b.indexedMatrices||a.compact!=b.compact||a.projectedTexture!=b.projectedTexture||
       !sameBits(a.baseTexgen,b.baseTexgen)||a.staticVertices!=b.staticVertices||a.multiTexture!=b.multiTexture||
       a.multiRowUnits!=b.multiRowUnits||a.unitSource!=b.unitSource||a.multiProjected!=b.multiProjected||!sameBits(a.origin,b.origin)||
       !sameBits(a.foldedOrigin,b.foldedOrigin)||a.normalsUnused!=b.normalsUnused||!sameBits(a.decodedNormalScale,b.decodedNormalScale)||
       !sameBits(a.decodedTex0Scale,b.decodedTex0Scale)||!sameBits(a.decodedTex1Scale,b.decodedTex1Scale)||!sameColor(a.color,b.color)||
       a.lights!=b.lights)return false;
    if(!rows)return true;
    const unsigned count=a.uniformCount();
    for(unsigned n=0;n<94;++n)if((n<count||n>=a.paletteLimit())&&!sameBits(a.uniforms[n],b.uniforms[n])){
        std::fprintf(stderr,"row %u differs: %a %a %a %a / %a %a %a %a\n",n,double(a.uniforms[n][0]),double(a.uniforms[n][1]),double(a.uniforms[n][2]),
            double(a.uniforms[n][3]),double(b.uniforms[n][0]),double(b.uniforms[n][1]),double(b.uniforms[n][2]),double(b.uniforms[n][3]));
        return false;
    }
    return true;
}
void setf(CommandRegisters& r,unsigned at,float v){r.xf[at]=std::bit_cast<u32>(v);r.xfValid.set(at);}
}
int main(){
    std::mt19937 rng(20261116);
    auto chance=[&](unsigned percent){return rng()%100<percent;};
    auto uni=[&](float a,float b){return std::uniform_real_distribution<float>(a,b)(rng);};
    unsigned texgenCases=0,texgenAccepted=0,planCases=0,planAccepted=0,continuations=0,colorCases=0,colorAccepted=0;
    std::array<unsigned,64> rejectCodes{};
    static RigidVertexPlan reused; // the replay's storage: garbage from earlier cases and random bytes
    for(unsigned iteration=0;iteration<6000;++iteration){
        CommandRegisters r{};
        // Viewport, projection and its type; now and then a missing or
        // nonfinite word (the projection part then refuses).
        for(unsigned i=0;i<6;++i)setf(r,0x101a+i,std::array{160.f,-120.f,0.f,502.f,462.f,16777215.f}[i]+(i<2?uni(-20,20):0));
        const bool ortho=chance(15);
        for(unsigned i=0;i<6;++i)setf(r,0x1020+i,std::array{1.2f,0.f,1.6f,0.f,-1.0001f,-0.20001f}[i]*uni(.8f,1.25f));
        r.xf[0x1026]=ortho;r.xfValid.set(0x1026);
        if(chance(3))r.xfValid.reset(0x101a+rng()%13);
        if(chance(2))setf(r,0x1020+rng()%6,std::numeric_limits<float>::quiet_NaN());
        // Position matrices 0 and 3 (view space) with their normal rows.
        for(unsigned m=0;m<6;m+=3){
            const std::array<float,12> position{.8f,.1f,-.2f,uni(-5,5),.05f,.9f,.1f,uni(-5,5),.2f,-.1f,.85f,uni(-200,-20)};
            for(unsigned k=0;k<12;++k)setf(r,4*m+k,position[k]);
            const float scale=chance(50)?1.25f:uni(.5f,2);
            for(unsigned k=0;k<9;++k)setf(r,0x400+3*m+k,position[k/3*4+k%3]*scale);
        }
        // Texture matrices in position memory (6..9) and 30..38, post matrices.
        for(unsigned at=4*6;at<4*10;++at)setf(r,at,uni(-1,1));
        for(unsigned at=4*30;at<4*39;++at)setf(r,at,uni(-.6f,.6f));
        for(unsigned at=0x500;at<0x500+4*12;++at)setf(r,at,uni(-1,1));
        if(chance(3))setf(r,4*30+rng()%36,std::numeric_limits<float>::infinity());
        // Texgens of coordinates 0..3: source TEX0/TEX1/normal/position,
        // 2x4 or 3x4, identity or a post matrix, sometimes invalid forms.
        const unsigned count=1+rng()%4;r.xf[0x103f]=chance(5)?9:count;r.xfValid.set(0x103f);
        std::array<unsigned,4> selected{};
        for(unsigned c=0;c<4;++c){
            static constexpr unsigned rows[]{5,6,1,0,5,13,2};
            const unsigned row=rows[rng()%7];
            const unsigned type=chance(90)?0:(chance(50)?2:1+rng()%4);
            const u32 projective=chance(25),form=row<=4;
            r.xf[0x1040+c]=(type<<4)|(row<<7)|(projective<<1)|(form<<2);r.xfValid.set(0x1040+c);
            static constexpr unsigned posts[]{61,61,0,3,6,61,58,1};
            r.xf[0x1050+c]=posts[rng()%8]|(chance(5)?0x100u:0u);r.xfValid.set(0x1050+c);
            if(chance(3))r.xfValid.reset(0x1040+c+(chance(50)?0x10:0));
            static constexpr unsigned matrices[]{60,60,30,33,36,6,9,61};
            selected[c]=matrices[rng()%8];
        }
        const unsigned position=chance(80)?0:3;
        r.matrixIndex[0]=position|(selected[0]<<6)|(selected[1]<<12)|(selected[2]<<18)|(selected[3]<<24);r.matrixIndexValid.set(0);
        r.matrixIndex[1]=60|(60u<<6)|(60u<<12)|(60u<<18);if(chance(70))r.matrixIndexValid.set(1);
        // Material: 0..3 units on coordinates 0..3, sometimes projected.
        PicaMaterialPlan material{};
        const unsigned units=rng()%4;material.textureCount=std::uint8_t(units);
        for(unsigned u=0;u<units;++u)material.textures[u]={std::uint8_t(u),std::uint8_t(rng()%4)};
        material.projectiveTexture0=units&&chance(20);
        std::uint8_t requested=0;for(unsigned u=0;u<units;++u)requested|=std::uint8_t(1u<<material.textures[u].coordinate);
        // planVertexTransform against its replay form.
        VertexTransformPlan full,projection;
        const bool fullOkay=planVertexTransform(r,requested,full);
        const bool projectionOkay=planVertexTransform(r,0,projection);
        std::uint8_t mask=0xff;const bool texgenOkay=replayTexgenMask(r,requested,mask);
        ++texgenCases;
        assert(fullOkay==(projectionOkay&&texgenOkay));
        if(fullOkay){
            ++texgenAccepted;
            assert(mask==full.projectiveMask()&&sameBits(projection.clipProjection(),full.clipProjection()));
        }
        // Lights 0..7 in XF light memory and the channel registers, for the
        // colour planner comparison and a colour plan for the rigid one.
        for(unsigned i=0;i<8;++i){
            const unsigned at=0x600+16*i;
            r.xf[at+3]=chance(15)?0u:u32(rng());r.xfValid.set(at+3);
            for(unsigned k=4;k<16;++k){if(k>=10&&k<13)setf(r,at+k,uni(-200,200));else setf(r,at+k,uni(-1,1.5f));}
            if(chance(2))r.xfValid.reset(at+4+rng()%12);
            if(chance(1))setf(r,at+10,std::numeric_limits<float>::quiet_NaN());
        }
        r.xf[0x1009]=1+rng()%2;r.xfValid.set(0x1009);
        for(unsigned reg:{0x100au,0x100bu,0x100cu,0x100du}){r.xf[reg]=u32(rng());r.xfValid.set(reg);}
        for(unsigned reg:{0x100eu,0x100fu,0x1010u,0x1011u}){r.xf[reg]=u32(rng())&0x7fff;r.xfValid.set(reg);}
        if(chance(3))r.xfValid.reset(0x100a+rng()%8);
        PicaMaterialPlan colorMaterial{};colorMaterial.stageCount=std::uint8_t(1+rng()%3);
        for(unsigned s=0;s<colorMaterial.stageCount;++s)for(auto* channel:{&colorMaterial.stages[s].rgb,&colorMaterial.stages[s].alpha}){
            channel->function=static_cast<MaterialFunction>(rng()%7);
            for(auto& in:channel->inputs)in={static_cast<MaterialSource>(rng()%4),static_cast<MaterialOperand>(rng()%3)};
        }
        if(chance(3))colorMaterial.stageCount=chance(50)?0:7;
        RasterColorPlan color,colorPrevious;RasterColorError e1{},e2{};
        std::memset(static_cast<void*>(&color),0xa5,sizeof(color));
        const bool c1=planRasterColor(r,colorMaterial,color,&e1),c2=previous::planRasterColor(r,colorMaterial,colorPrevious,&e2);
        ++colorCases;colorAccepted+=c1;
        assert(c1==c2&&e1==e2&&sameColor(color,colorPrevious));
        if(!fullOkay)continue;
        // A colour plan for the rigid planner: the planned one when it is
        // usable, otherwise a random one covering the rejects.
        if(!c1||chance(40)){
            color={};color.material={uni(0,1),uni(0,1),uni(0,1),uni(0,1)};color.ambient={uni(0,1),uni(0,1),uni(0,1),uni(0,1)};
            color.required=std::uint8_t(rng()%4);color.lit=std::uint8_t(rng()%4);color.vertex=std::uint8_t(rng()%4);
            color.ambientVertex=chance(10)?std::uint8_t(rng()%4):0;
            color.channels[0]={std::uint8_t(rng()%256),std::uint8_t(chance(85)?2:rng()%3),std::uint8_t(chance(85)?1:rng()%3)};
            if(chance(5))color.channels[1].mask=1;
            for(auto& l:color.lights){
                l.color={uni(0,1),uni(0,1),uni(0,1),1};l.position={uni(-100,100),uni(-100,100),uni(-100,100)};l.direction={uni(-1,1),uni(-1,1),uni(-1,1)};
                l.cosine={uni(0,1),uni(-.5f,.5f),uni(-.5f,.5f)};l.distance={uni(.1f,2),uni(0,.1f),uni(0,.01f)};
                if(chance(5))l.distance={0,0,0};
                if(chance(3))l.distance[0]=-1;
            }
        }
        VertexFogPlan fog{};
        if(chance(40)){
            fog.enabled=true;fog.function=chance(90)?2:4;fog.orthographic=chance(5);fog.a=uni(0,3);fog.b=uni(chance(95)?1.1f:.5f,4);fog.c=uni(-.5f,.5f);
            fog.rangeEnabled=chance(50);fog.viewportWidth=uni(300,700);fog.rangeCenter=uni(-100,100);
            for(auto& k:fog.rangeK)k=uni(.5f,64);
        }
        const bool primaryAlpha=chance(30);
        const int fogUnit=fog.enabled&&chance(70)?int(units):-1;
        std::array<std::array<float,2>,3> scales{};
        for(auto& s:scales)s={uni(.01f,1),uni(.01f,1)};
        if(chance(2))scales[rng()%3][rng()%2]=0;
        DecodedVertex first{};
        first.present=(1u<<GX_VA_POS)|(chance(90)?(1u<<GX_VA_NRM):0u)|(chance(90)?(1u<<GX_VA_TEX0):0u)|(chance(70)?(1u<<GX_VA_TEX1):0u);
        if(chance(8))first.present|=1u<<(GX_VA_TEX0MTXIDX+rng()%3);
        if(chance(5)){first.present|=1;first.matrix[0]=std::uint8_t(3*(rng()%2));}
        first.normalCount=first.present&(1u<<GX_VA_NRM)?1:0;
        const bool allowProjected=chance(80);
        multiTextureWidePalette=chance(85);multiTextureNormalTexgen=chance(95);
        const unsigned width=chance(50)?400:320;
        // The complete replay: a fresh plan.
        auto rejectsBefore=gxRigidPlanRejects;const auto normalBefore=gxRigidNormalTexgenPlans;
        RigidVertexPlan complete;
        const bool okay=planRigidMultiVertex(r,first,full,color,fog,primaryAlpha,width,material,units,scales,fogUnit,complete,allowProjected);
        auto completeRejects=gxRigidPlanRejects;const auto completeNormal=gxRigidNormalTexgenPlans;const auto completeRefused=gxRigidLastRefusedTexgen;
        // The lean replay: the reused storage, random bytes everywhere.
        for(auto& row:reused.uniforms)for(auto& v:row)v=std::bit_cast<float>(u32(rng()));
        {auto* bytes=reinterpret_cast<unsigned char*>(&reused);
         for(std::size_t i=sizeof(reused.uniforms);i<sizeof(reused);++i)bytes[i]=static_cast<unsigned char>(rng());}
        gxRigidPlanRejects=rejectsBefore;gxRigidNormalTexgenPlans=normalBefore;
        prepareRigidMultiReuse(reused);
        const bool leanOkay=buildRigidMultiVertex(r,first,mask,projection.clipProjection(),color,fog,primaryAlpha,width,material,units,scales,fogUnit,reused,allowProjected);
        ++planCases;
        assert(leanOkay==okay&&gxRigidPlanRejects==completeRejects&&gxRigidNormalTexgenPlans==completeNormal);
        assert(gxRigidLastRefusedTexgen.gen==completeRefused.gen&&gxRigidLastRefusedTexgen.post==completeRefused.post&&gxRigidLastRefusedTexgen.code==completeRefused.code);
        for(unsigned c=0;c<64;++c)if(completeRejects[c]!=rejectsBefore[c])++rejectCodes[c];
        // A refused plan is never used (the replay returns at once).
        if(!okay)continue;
        ++planAccepted;
        assert(samePlan(complete,reused,true));
        // The replay's steps after the plan: palette, decoded scales, origin,
        // finish. The complete replay keeps a copy of the planned block for
        // its continuations; the lean one keeps the unit rows 81-92.
        const RigidVertexPlan planned=complete;
        std::array<std::array<float,4>,12> unitRows{};std::copy_n(reused.uniforms.begin()+81,12,unitRows.begin());
        auto place=[&](RigidVertexPlan& p,bool compactPalette,unsigned extra,float normalScale,float tex0,float tex1,std::array<float,3> origin){
            if(compactPalette)restartCompactPalette(p);
            bool placed=true;
            for(unsigned n=0;placed&&n<extra;++n)placed=addRigidMatrix(r,3*n,p);
            p.decodedNormalScale=normalScale;p.decodedTex0Scale=tex0;p.decodedTex1Scale=tex1;p.origin=origin;
            return placed&&finishCompactRigidUniforms(p,scales[0]);
        };
        auto continuation=[&](RigidVertexPlan& p){
            p.matrixOffsets.fill(-1);p.normalOffsets.fill(-1);p.matrices=0;p.compactPalette=false;p.paletteEnd=0;
            return addRigidMatrix(r,position,p);
        };
        const bool compactPalette=chance(30);const unsigned extra=rng()%3;
        const std::array<float,3> origin{chance(50)?0.f:256.f*float(rng()%8),0,chance(50)?0.f:-512.f};
        const float normalScale=std::ldexp(1.f,-14),tex0=std::ldexp(1.f,-int(rng()%11)),tex1=std::ldexp(1.f,-8);
        const bool a=place(complete,compactPalette,extra,normalScale,tex0,tex1,origin),b=place(reused,compactPalette,extra,normalScale,tex0,tex1,origin);
        assert(a==b);
        if(!a)continue;
        assert(samePlan(complete,reused,true));
        // Continuation: the next shape of the same material (other scales,
        // origin and palette). Complete: a copy of the planned block. Lean:
        // the finished plan with its unit rows restored.
        RigidVertexPlan next=planned;
        std::copy(unitRows.begin(),unitRows.end(),reused.uniforms.begin()+81);
        reused.foldedOrigin={};reused.compact=false;reused.staticVertices=nullptr;
        const bool nextCompact=chance(30);const unsigned nextExtra=rng()%3;
        const std::array<float,3> nextOrigin{chance(50)?0.f:-256.f*float(rng()%8),chance(50)?0.f:512.f,0};
        const float nextTex0=std::ldexp(1.f,-int(rng()%11));
        const bool c=continuation(next)&&place(next,nextCompact,nextExtra,normalScale,nextTex0,tex1,nextOrigin);
        const bool d=continuation(reused)&&place(reused,nextCompact,nextExtra,normalScale,nextTex0,tex1,nextOrigin);
        assert(c==d);
        if(c){assert(samePlan(next,reused,true));++continuations;}
    }
    multiTextureWidePalette=true;multiTextureNormalTexgen=true;
    unsigned codes=0;for(auto n:rejectCodes)codes+=n!=0;
    assert(texgenAccepted>texgenCases/4&&texgenAccepted<texgenCases&&planAccepted>planCases/10&&planAccepted<planCases&&continuations>planAccepted/4);
    assert(colorAccepted>colorCases/10&&colorAccepted<colorCases&&codes>=15);
    std::fprintf(stderr,"PASS lean replay planner: %u texgen banks (%u accepted) equal to planVertexTransform; %u rigid plans (%u accepted, %u reject codes seen) "
                 "equal on reused garbage storage, through palette and finish; %u continuations from kept unit rows equal; %u colour plans (%u accepted) "
                 "equal to the previous planRasterColor\n",texgenCases,texgenAccepted,planCases,planAccepted,codes,continuations,colorCases,colorAccepted);
    return 0;
}
