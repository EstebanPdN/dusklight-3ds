// Compile against installed Citro3D/libctru headers, using only CPU TexEnv
// storage. No mock source/operand/function enums and no GPU execution claim.
#include "../gx_material_pica.hpp"
#include "../gx_blend_pica.hpp"
#include "material_equivalence.hpp"
#include "raster_carrier_fixture.hpp"
#include "../runtime/gx_fog.hpp"
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/texenv.h>
}
#include <cstring>
#include <cstdio>
using namespace dusk::native;
static C3D_TexEnv envs[6];static unsigned resetMask,bufferRgb,bufferAlpha;static u32 bufferColor;
static std::array<unsigned,6> blendRegisters;
void C3D_AlphaBlend(GPU_BLENDEQUATION rgb,GPU_BLENDEQUATION alpha,GPU_BLENDFACTOR src,GPU_BLENDFACTOR dst,GPU_BLENDFACTOR srcAlpha,GPU_BLENDFACTOR dstAlpha){
    blendRegisters={unsigned(rgb),unsigned(alpha),unsigned(src),unsigned(dst),unsigned(srcAlpha),unsigned(dstAlpha)};
}
C3D_TexEnv* C3D_GetTexEnv(int id){assert(id>=0&&id<6);return &envs[id];}
void C3D_TexEnvBufUpdate(int mode,int mask){
    assert(mask>=0&&mask<16);
    if(mode==C3D_Both){assert(mask==0);++resetMask;bufferRgb=bufferAlpha=0;}
    else if(mode==C3D_RGB)bufferRgb=mask;
    else {assert(mode==C3D_Alpha);bufferAlpha=mask;}
}
void C3D_TexEnvBufColor(u32 color){bufferColor=color;}
static gx::MaterialSource decodeSource(unsigned n){
    switch(n){case 0:return gx::MaterialSource::primary;case 3:return gx::MaterialSource::texture0;
    case 4:return gx::MaterialSource::texture1;case 5:return gx::MaterialSource::texture2;
    case 13:return gx::MaterialSource::buffer;case 14:return gx::MaterialSource::constant;case 15:return gx::MaterialSource::previous;}
    assert(false);return {};
}
static gx::MaterialFunction decodeFunction(unsigned n){
    switch(n){case 0:return gx::MaterialFunction::replace;case 1:return gx::MaterialFunction::modulate;
    case 2:return gx::MaterialFunction::add;case 3:return gx::MaterialFunction::addSigned;case 5:return gx::MaterialFunction::subtract;case 4:return gx::MaterialFunction::interpolate;case 8:return gx::MaterialFunction::multiplyAdd;}
    assert(false);return {};
}
static gx::PicaMaterialPlan decode(const gx::PicaMaterialPlan& original){
    auto p=original;p.bufferUpdateRgb=bufferRgb;p.bufferUpdateAlpha=bufferAlpha;for(unsigned c=0;c<4;++c)p.bufferConstant[c]=bufferColor>>(8*c);
    for(unsigned t=0;t<p.stageCount;++t){const auto& e=envs[t];auto& stage=p.stages[t];
        for(unsigned c=0;c<4;++c)stage.constant[c]=e.color>>(8*c);
        for(unsigned a=0;a<2;++a){auto& ch=a?stage.alpha:stage.rgb;
            ch.function=decodeFunction(a?e.funcAlpha:e.funcRgb);ch.scale=a?e.scaleAlpha:e.scaleRgb;
            for(unsigned n=0;n<3;++n){auto& in=ch.inputs[n];in.source=decodeSource(((a?e.srcAlpha:e.srcRgb)>>(4*n))&15);
                const unsigned op=((a?e.opAlpha:e.opRgb)>>(4*n))&15;in.inverse=op&1;
                if(a){assert(op<8);constexpr gx::MaterialOperand ops[]{gx::MaterialOperand::alpha,gx::MaterialOperand::red,gx::MaterialOperand::green,gx::MaterialOperand::blue};in.operand=ops[op/2];}
                else{switch(op&~1u){case 0:in.operand=gx::MaterialOperand::color;break;case 2:in.operand=gx::MaterialOperand::alpha;break;case 4:in.operand=gx::MaterialOperand::red;break;case 8:in.operand=gx::MaterialOperand::green;break;case 12:in.operand=gx::MaterialOperand::blue;break;default:assert(false);}}
            }
        }
    }
    for(unsigned t=p.stageCount;t<6;++t){C3D_TexEnv clean{};C3D_TexEnvInit(&clean);assert(!memcmp(&clean,&envs[t],sizeof(clean)));}
    return p;
}
int main(){
    using namespace material_test;std::uint32_t seed=21;unsigned tested=0;
    // Independent original normalized blend equations versus actual Citro3D
    // call arguments. Include stale physical target alpha for GX RGB8, every
    // source/destination factor, and subtraction with factors it must ignore.
    unsigned blendCases=0;
    for(unsigned mode=0;mode<3;++mode)for(unsigned storage=0;storage<2;++storage)
    for(unsigned src=0;src<8;++src)for(unsigned dst=0;dst<8;++dst){
        gx::RasterPlan p;p.storesAlpha=storage;p.blend={std::uint8_t(mode!=0),std::uint8_t(src),std::uint8_t(dst),0};p.subtractBlend=mode==2;
        applyPicaBlend(p);
        for(unsigned n=0;n<64;++n){
            Color s{},d{};for(auto& c:s)c=double(randomWord(seed)>>16&255)/255;for(auto& c:d)c=double(randomWord(seed)>>16&255)/255;
            for(unsigned c=0;c<(storage?4u:3u);++c){
                auto gxFactor=[&](unsigned f,bool dest){
                    switch(f){case 0:return 0.;case 1:return 1.;case 2:return dest?s[c]:d[c];case 3:return 1-(dest?s[c]:d[c]);
                    case 4:return s[3];case 5:return 1-s[3];case 6:return storage?d[3]:1.;case 7:return storage?1-d[3]:0.;}assert(false);return 0.;
                };
                auto gpuFactor=[&](unsigned f){
                    switch(f){case 0:return 0.;case 1:return 1.;case 2:return s[c];case 3:return 1-s[c];case 4:return d[c];case 5:return 1-d[c];
                    case 6:return s[3];case 7:return 1-s[3];case 8:return d[3];case 9:return 1-d[3];}assert(false);return 0.;
                };
                const double expected=std::clamp(mode==0?s[c]:mode==2?d[c]-s[c]:s[c]*gxFactor(src,false)+d[c]*gxFactor(dst,true),0.,1.);
                const double a=s[c]*gpuFactor(blendRegisters[c==3?4:2]),b=d[c]*gpuFactor(blendRegisters[c==3?5:3]);
                const auto eq=blendRegisters[c==3?1:0];assert(eq==0||eq==2);
                const double actual=std::clamp(eq==2?b-a:a+b,0.,1.);assert(std::abs(expected-actual)<1e-12);++blendCases;
            }
        }
    }
    // Exercise every function, source, operand, inverse and scale combination
    // through installed inline register packers and decode raw numeric fields.
    for(unsigned n=0;n<4096;++n){gx::PicaMaterialPlan p{};p.stageCount=1+n%6;p.textureCount=3;p.textures={{{0,0},{1,1},{2,2}}};
        for(auto& c:p.bufferConstant)c=randomWord(seed)>>16;
        p.bufferUpdateRgb=(randomWord(seed)>>16)&15;
        p.bufferUpdateAlpha=(randomWord(seed)>>16)&15;
        for(unsigned t=0;t<p.stageCount;++t){auto& stage=p.stages[t];for(auto& c:stage.constant)c=randomWord(seed)>>16;
            for(unsigned a=0;a<2;++a){auto& ch=a?stage.alpha:stage.rgb;ch.function=static_cast<gx::MaterialFunction>((randomWord(seed)>>16)%7);ch.scale=(randomWord(seed)>>16)%3;
                for(auto& in:ch.inputs){in.source=static_cast<gx::MaterialSource>((randomWord(seed)>>16)%7);in.operand=static_cast<gx::MaterialOperand>(a?1+(randomWord(seed)>>16)%4:(randomWord(seed)>>16)%5);in.inverse=randomWord(seed)&1;}}
        }
        memset(envs,0xa5,sizeof(envs));applyPicaMaterial(p);const auto actual=decode(p);
        // A local packed snapshot must restore every register after a different
        // material overwrites live state, including unused stages/buffer state.
        const auto prepared=preparePicaMaterial(p);
        C3D_TexEnv packedExpected[6];memcpy(packedExpected,envs,sizeof(envs));
        const auto rgbExpected=bufferRgb,alphaExpected=bufferAlpha,colorExpected=bufferColor;
        memset(envs,0x5a,sizeof(envs));bufferRgb=bufferAlpha=15;bufferColor=0x12345678;
        applyPreparedPicaMaterial(prepared);
        assert(!memcmp(packedExpected,envs,sizeof(envs))&&bufferRgb==rgbExpected&&bufferAlpha==alphaExpected&&bufferColor==colorExpected);
        Samples samples{};for(auto& c:samples.raster)c=double(randomWord(seed)&255)/255;
        for(auto& map:samples.texture)for(auto& uv:map)for(auto& c:uv)c=double(randomWord(seed)&255)/255;
        const auto expected=picaEvaluate(p,samples),result=picaEvaluate(actual,samples);
        for(unsigned c=0;c<4;++c)assert(std::abs(expected[c]-result[c])<1e-12);++tested;
    }
    // Legacy mapping must produce identical register contents to the old J2D
    // backend, including unused operands and reset stages.
    constexpr GPU_TEVSRC oldSources[]{GPU_PRIMARY_COLOR,GPU_TEXTURE0,GPU_PREVIOUS,GPU_CONSTANT};
    for(unsigned n=0;n<512;++n){gx::PicaTevPlan p{};p.count=1+n%2;C3D_TexEnv expected[6]{};
        for(auto& e:expected)C3D_TexEnvInit(&e);
        for(unsigned t=0;t<p.count;++t){auto& s=p.stages[t];s.constant={17,93,151,219};C3D_TexEnvColor(&expected[t],0xdb975d11);
            for(unsigned a=0;a<2;++a){auto& c=a?s.alpha:s.rgb;c.function=static_cast<gx::PicaTevFunction>((randomWord(seed)>>16)%2);
                for(auto& source:c.sources)source=static_cast<gx::PicaTevSource>((randomWord(seed)>>16)%4);
                const auto mode=a?C3D_Alpha:C3D_RGB;C3D_TexEnvFunc(&expected[t],mode,c.function==gx::PicaTevFunction::replace?GPU_REPLACE:GPU_MODULATE);
                C3D_TexEnvSrc(&expected[t],mode,oldSources[unsigned(c.sources[0])],oldSources[unsigned(c.sources[1])],oldSources[unsigned(c.sources[2])]);}
        }
        memset(envs,0xa5,sizeof(envs));applyPicaTev(p);assert(!memcmp(envs,expected,sizeof(envs))&&bufferRgb==0);
    }
    assert(resetMask==8704);
    // Actual CPU register packer, independent raw numeric decoding above.
    gx::PicaMaterialPlan carrier;assert(gx::planRasterCarrierMaterial(carrier_test::capturedMaterial(),carrier));
    assert(carrier.stageCount==3&&carrier.textureCount==1);
    applyPicaMaterial(carrier);const auto packed=decode(carrier);
    assert(packed.stages[0].rgb.inputs[0].source==gx::MaterialSource::primary&&packed.stages[0].rgb.inputs[0].operand==gx::MaterialOperand::color);
    bool scalar=false;for(const auto& in:packed.stages[1].rgb.inputs)scalar|=in.source==gx::MaterialSource::primary&&in.operand==gx::MaterialOperand::alpha;
    assert(scalar&&packed.stages[1].rgb.function==gx::MaterialFunction::modulate&&packed.stages[2].rgb.scale==2);
    assert(packed.stages[2].alpha.function==gx::MaterialFunction::replace&&packed.stages[2].alpha.inputs[0].source==gx::MaterialSource::texture0&&packed.stages[2].alpha.inputs[0].operand==gx::MaterialOperand::alpha);
    gx::VertexFogPlan fog;fog.enabled=true;fog.color={0xab,0xc6,0x9a,255};
    assert(gx::appendVertexFog(carrier,fog,false,false));
    assert(carrier.stageCount==4&&carrier.textureCount==2);
    applyPicaMaterial(carrier);const auto fogPacked=decode(carrier);
    assert(fogPacked.stages[3].rgb.function==gx::MaterialFunction::interpolate);
    assert(fogPacked.stages[3].rgb.inputs[2].source==gx::MaterialSource::texture1&&fogPacked.stages[3].rgb.inputs[2].operand==gx::MaterialOperand::alpha);
    assert(fogPacked.stages[3].alpha.function==gx::MaterialFunction::replace&&fogPacked.stages[3].alpha.inputs[0].source==gx::MaterialSource::previous&&fogPacked.stages[3].alpha.inputs[0].operand==gx::MaterialOperand::alpha);
    assert(fogPacked.stages[2].rgb.scale==2&&fogPacked.stages[2].alpha.inputs[0].source==gx::MaterialSource::texture0);
    const auto prepared=preparePicaMaterial(carrier);C3D_TexEnv frozen[6];memcpy(frozen,envs,sizeof(envs));
    gx::PicaMaterialPlan ordinary;ordinary.stageCount=1;applyPicaMaterial(ordinary);applyPreparedPicaMaterial(prepared);
    assert(!memcmp(frozen,envs,sizeof(envs))&&!bufferRgb);
    std::puts("PASS captured carrier uses native numeric PRIMARY RGB/ALPHA operands, original final texture alpha/scale4, and prepared state restoration (host register packing only)");printf("{\"register_plan_cases\":%u,\"prepared_register_equivalence\":4096,\"legacy_register_equivalence\":512,\"blend_component_cases\":%u,\"gpu_executed\":false}\n",tested,blendCases);
}
