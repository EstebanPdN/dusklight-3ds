// Host test of the indirect warp sampler (gx_indirect.hpp), its GPU-route
// constant plan and the per-vertex part added to it (gx_indirect_gpu.hpp,
// gx_vertex_warp.hpp). Runner: Tools/3ds/test_gx_indirect.py.
#include "../runtime/gx_indirect_gpu.hpp"
#include "../runtime/gx_vertex_warp.hpp"
#include "../runtime/texture_gx.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace dusk::native;
using namespace dusk::native::gx;
using u8=std::uint8_t;

// The Alpha 7 wave 1 sampler, verbatim (calls qualified), as the reference the refactor must
// reproduce bit for bit on the formats it already admitted.
namespace legacy {
inline bool validIndirectImage(const Texture& t) {
    if(!t.width||!t.height||t.wrapS>2||t.wrapT>2||!t.mipCount||t.mipCount>8||t.minFilter>5||
       t.magFilter>1||t.minLod>t.maxLod||t.maxLod>=t.mipCount||!std::isfinite(t.lodBias))return false;
    if(t.format>3&&t.format!=5)return false; // I4, I8, IA4, IA8, RGB5A3, with exact GX tiling.
    const unsigned tw=(t.format==3||t.format==5)?4:8,th=t.format==0?8:4;
    std::size_t bytes=0;
    for(unsigned level=0;level<t.mipCount;++level)
        bytes+=std::size_t((std::max(1u,unsigned(t.width)>>level)+tw-1)/tw)*((std::max(1u,unsigned(t.height)>>level)+th-1)/th)*32;
    return t.image.size()>=bytes;
}
inline bool sampleIndirectLevel(const Texture& t,float u,float v,std::array<float,3>& abg) {
    if(!std::isfinite(u)||!std::isfinite(v))return false;
    auto axis=[](double value,unsigned size,unsigned wrap){
        if(!wrap)return std::clamp(value,0.0,1.0)*size;
        const double period=wrap==2?2:1;
        value=std::fmod(value,period);if(value<0)value+=period;
        return value*size;
    };
    auto index=[](int value,unsigned size,unsigned wrap){
        if(!wrap)return std::clamp(value,0,int(size)-1);
        const int period=int(size)*(wrap==2?2:1);
        value%=period;if(value<0)value+=period;
        return wrap==2&&value>=int(size)?period-1-value:value;
    };
    auto pixel=[&](int x,int y){
        x=index(x,t.width,t.wrapS);y=index(y,t.height,t.wrapT);
        const unsigned tw=(t.format==3||t.format==5)?4:8,th=t.format==0?8:4;
        const auto tile=((unsigned(y)/th)*((t.width+tw-1)/tw)+unsigned(x)/tw)*32;
        const unsigned at=(unsigned(y)%th)*tw+unsigned(x)%tw;
        float a,i;
        if(t.format==0){const auto byte=t.image[tile+at/2];a=i=((at&1)?byte&15:byte>>4)*17;}
        else if(t.format==1){a=i=t.image[tile+at];}
        else if(t.format==2){const auto byte=t.image[tile+at];a=(byte>>4)*17;i=(byte&15)*17;}
        else if(t.format==5){
            const unsigned value=(unsigned(t.image[tile+at*2])<<8)|t.image[tile+at*2+1];
            if(value&0x8000){const unsigned b=value&31,g=(value>>5)&31;
                return std::array<float,3>{255,float((b<<3)|(b>>2)),float((g<<3)|(g>>2))};}
            const unsigned alpha=value>>12;
            return std::array<float,3>{float((alpha<<5)|(alpha<<2)|(alpha>>1)),float((value&15)*17),float(((value>>4)&15)*17)};
        }
        else {a=t.image[tile+at*2];i=t.image[tile+at*2+1];}
        return std::array<float,3>{a,i,i};
    };
    const double x=axis(u,t.width,t.wrapS),y=axis(v,t.height,t.wrapT);
    if(!t.magFilter){abg=pixel(int(std::floor(x)),int(std::floor(y)));return true;}
    const int ix=int(std::floor(x-.5)),iy=int(std::floor(y-.5));
    const float fx=x-.5-ix,fy=y-.5-iy;
    const auto a=pixel(ix,iy),b=pixel(ix+1,iy),c=pixel(ix,iy+1),d=pixel(ix+1,iy+1);
    for(unsigned n=0;n<3;++n)abg[n]=(a[n]*(1-fx)+b[n]*fx)*(1-fy)+(c[n]*(1-fx)+d[n]*fx)*fy;
    return true;
}
inline bool sampleIndirectImage(const Texture& t,float u,float v,std::array<float,3>& abg) {
    const float lod=t.minFilter<2?0:std::clamp(t.lodBias,float(t.minLod),float(t.maxLod));
    const bool blend=t.minFilter>=4;
    const unsigned low=blend?unsigned(lod):unsigned(std::floor(lod+.5f));
    auto level=[&](unsigned selected,std::array<float,3>& result){
        auto image=t;std::size_t offset=0;const unsigned tw=(t.format==3||t.format==5)?4:8,th=t.format==0?8:4;
        for(unsigned n=0;n<selected;++n){offset+=std::size_t((image.width+tw-1)/tw)*((image.height+th-1)/th)*32;
            image.width=std::max(1u,unsigned(image.width)/2);image.height=std::max(1u,unsigned(image.height)/2);}
        image.image=image.image.subspan(offset);
        image.magFilter=lod<=0?t.magFilter:(t.minFilter&1);
        return legacy::sampleIndirectLevel(image,u,v,result);
    };
    if(!level(low,abg))return false;
    if(blend&&lod>low){std::array<float,3> high;if(!level(low+1,high))return false;
        const float fraction=lod-low;for(unsigned c=0;c<3;++c)abg[c]+=(high[c]-abg[c])*fraction;}
    return true;
}
}

namespace {
std::mt19937 rng(0x41197u);
unsigned uniform(unsigned low,unsigned high){return std::uniform_int_distribution<unsigned>(low,high)(rng);}
float real(float low,float high){return std::uniform_real_distribution<float>(low,high)(rng);}
bool sameBits(const std::array<float,3>& a,const std::array<float,3>& b){return !std::memcmp(a.data(),b.data(),sizeof(a));}
std::size_t imageBytes(unsigned format,unsigned width,unsigned height,unsigned mips){
    std::size_t bytes=0;
    for(unsigned level=0;level<mips;++level)bytes+=indirectLevelBytes(format,std::max(1u,width>>level),std::max(1u,height>>level));
    return bytes;
}
Texture image(std::vector<u8>& pixels,unsigned format,unsigned width,unsigned height,unsigned mips=1){
    pixels.resize(imageBytes(format,width,height,mips));
    for(auto& b:pixels)b=u8(uniform(0,255));
    Texture t{};t.handle=0x1000+format;t.image=pixels;t.width=std::uint16_t(width);t.height=std::uint16_t(height);
    t.format=std::uint8_t(format);t.mipCount=std::uint8_t(mips);
    return t;
}

// 1. The refactored sampler equals the wave 1 sampler on I4/I8/IA4/IA8/RGB5A3.
void legacyEquivalence(){
    const unsigned formats[]{0,1,2,3,5};
    const float biases[]{-1.f,0.f,.3f,.5f,1.f,1.5f,2.7f};
    unsigned samples=0,images=0;
    for(unsigned iteration=0;iteration<3000;++iteration){
        const unsigned format=formats[uniform(0,4)];
        const unsigned width=uniform(1,40),height=uniform(1,40),mips=uniform(1,4);
        std::vector<u8> pixels;auto t=image(pixels,format,width,height,mips);
        if(uniform(0,9)==0&&!pixels.empty())t.image=t.image.first(pixels.size()-1);
        t.wrapS=std::uint8_t(uniform(0,2));t.wrapT=std::uint8_t(uniform(0,2));
        t.minFilter=std::uint8_t(uniform(0,5));t.magFilter=std::uint8_t(uniform(0,1));
        t.maxLod=std::uint8_t(uniform(0,mips-1));t.minLod=std::uint8_t(uniform(0,t.maxLod));t.lodBias=biases[uniform(0,6)];
        if(uniform(0,29)==0)t.minFilter=6;
        const bool valid=validIndirectImage(t);
        assert(valid==legacy::validIndirectImage(t));
        if(!valid)continue;
        ++images;
        for(unsigned n=0;n<24;++n){
            float u=real(-3,3),v=real(-3,3);
            if(n==0){u=(uniform(0,width-1)+.5f)/width;v=(uniform(0,height-1)+.5f)/height;}
            if(n==1){u=float(uniform(0,width))/width;v=float(uniform(0,height))/height;}
            std::array<float,3> a{},b{};
            const bool ok=sampleIndirectImage(t,u,v,a),legacyOk=legacy::sampleIndirectImage(t,u,v,b);
            assert(ok==legacyOk&&(!ok||sameBits(a,b)));
            ++samples;
        }
    }
    std::array<float,3> a{};std::vector<u8> pixels;auto t=image(pixels,1,8,8);
    assert(!sampleIndirectImage(t,std::numeric_limits<float>::infinity(),0,a));
    std::printf("PASS sampler refactor: %u images, %u samples bit-identical to the wave 1 sampler (I4/I8/IA4/IA8/RGB5A3)\n",images,samples);
}

// 2. Every texel of the formats admitted now (RGB565, RGBA8, CMPR) and of
// RGB5A3 matches the renderer's own decoder (texture_gx.cpp) channel for channel.
void decoderAgreement(){
    unsigned texels=0;
    for(const unsigned format:{4u,5u,6u,14u})for(const auto size:{std::array<unsigned,2>{8,8},{16,8},{24,16},{12,20},{32,32},{5,3}}){
        const unsigned width=size[0],height=size[1];
        std::vector<u8> bti(32);bti[0]=u8(format);bti[2]=u8(width>>8);bti[3]=u8(width);bti[4]=u8(height>>8);bti[5]=u8(height);
        bti[24]=1;bti[31]=32;
        std::vector<u8> pixels;auto t=image(pixels,format,width,height);
        if(format==14)for(std::size_t b=0;b+8<=pixels.size();b+=8)if(uniform(0,1)){std::swap(pixels[b],pixels[b+2]);std::swap(pixels[b+1],pixels[b+3]);}
        bti.insert(bti.end(),pixels.begin(),pixels.end());
        t.image={bti.data()+32,pixels.size()};
        assert(validIndirectImage(t));
        NativeTexturePlan plan;
        assert(inspectNativeTexture(bti,plan,format==4));
        std::vector<u8> decoded(plan.outputBytes);
        assert(decodeNativeTexture(bti,plan,decoded));
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            const unsigned m=(x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2)|((x&4)<<2)|((y&4)<<3);
            const auto* p=decoded.data()+(((y/8)*(plan.paddedWidth/8)+x/8)*64+m)*4; // a, b, g, r
            const auto abg=indirectTexel(t,x,y);
            assert(abg[0]==p[0]&&abg[1]==p[1]&&abg[2]==p[2]);
            ++texels;
        }
    }
    std::printf("PASS new indirect image formats: %u texels of RGB565/RGB5A3/RGBA8/CMPR equal to the renderer decoder's a, b, g\n",texels);
}

// 3. Statistics: exact mean/range against a direct scan, the warp bound holds
// for every texel, every bilinear sample stays inside the range, and a dense
// uniform sampling of a repeat-wrapped map averages to the mean.
void statisticsAndBound(){
    unsigned cases=0;
    for(unsigned iteration=0;iteration<300;++iteration){
        const unsigned formats[]{0,1,2,3,4,5,6,14};
        const unsigned format=formats[uniform(0,7)],width=8u<<uniform(0,2),height=8u<<uniform(0,2);
        std::vector<u8> pixels;auto t=image(pixels,format,width,height);
        t.wrapS=t.wrapT=1;t.magFilter=1;t.minFilter=1;
        IndirectImageStats s;assert(computeIndirectImageStats(t,s));
        std::array<double,3> sum{};std::array<float,3> low{255,255,255},high{0,0,0};
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto abg=indirectTexel(t,x,y);
            for(unsigned c=0;c<3;++c){sum[c]+=abg[c];low[c]=std::min(low[c],abg[c]);high[c]=std::max(high[c],abg[c]);}}
        for(unsigned c=0;c<3;++c){assert(std::abs(s.mean[c]-float(sum[c]/(width*height)))<1e-3f);assert(s.low[c]==low[c]&&s.high[c]==high[c]);}
        VertexIndirectEffect e{};e.source=t;e.bias=uniform(0,7);
        for(auto& row:e.matrix)for(auto& m:row)m=real(-.05f,.05f);
        const auto w=indirectWarpStats(e,s);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto abg=indirectTexel(t,x,y);
            for(unsigned axis=0;axis<2;++axis){float warp=0;
                for(unsigned c=0;c<3;++c)warp+=e.matrix[axis][c]*(abg[c]-((e.bias&(1u<<c))?128.f:0.f));
                assert(std::abs(warp-w.mean[axis])<=w.deviation[axis]*1.0001f+1e-4f);}}
        // Sample grid: exactly 4 x 4 samples per texel over one period.
        std::array<double,3> grid{};unsigned count=0;
        for(unsigned y=0;y<height*4;++y)for(unsigned x=0;x<width*4;++x){
            std::array<float,3> abg;assert(sampleIndirectImage(t,(x+.5f)/(width*4),(y+.5f)/(height*4),abg));
            for(unsigned c=0;c<3;++c){assert(abg[c]>=s.low[c]-1e-3f&&abg[c]<=s.high[c]+1e-3f);grid[c]+=abg[c];}
            ++count;
        }
        for(unsigned c=0;c<3;++c)assert(std::abs(grid[c]/count-s.mean[c])<1e-2);
        ++cases;
    }
    // Mipmapped map, vertex LOD 1.5: the mean blends levels 1 and 2, the range covers level 0.
    std::vector<u8> pixels;auto t=image(pixels,1,16,16,3);
    t.minFilter=5;t.magFilter=1;t.maxLod=2;t.lodBias=1.5f;t.wrapS=t.wrapT=1;
    IndirectImageStats s;assert(computeIndirectImageStats(t,s));
    auto levelMean=[&](unsigned level){
        const auto l=indirectLevel(t,level);double sum=0;
        for(unsigned y=0;y<l.height;++y)for(unsigned x=0;x<l.width;++x)sum+=indirectTexel(l,x,y)[0];
        return sum/(l.width*l.height);
    };
    assert(std::abs(s.mean[0]-float(levelMean(1)+(levelMean(2)-levelMean(1))*.5))<1e-3f);
    float low=255;for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)low=std::min(low,indirectTexel(t,x,y)[0]);
    assert(s.low[0]<=low);
    std::printf("PASS warp statistics: %u maps of all formats, exact mean/range, warp bound holds per texel, uniform bilinear sampling averages to the mean; mip LOD blend\n",cases);
}

// Shared fixture: one TEV stage sampling map 0 with coordinate 0, warped by
// an I8 map of constant value, so every per-vertex warp equals the mean.
struct Fixture {
    CommandRegisters r;MaterialState original;PicaMaterialPlan material;VertexIndirectPlan indirect;
    std::vector<u8> warpPixels;
    Fixture(unsigned width,unsigned height,u8 value,float scale){
        warpPixels.assign(indirectLevelBytes(1,16,16),value);
        static TextureHandle handles=1000;
        Texture warp{};warp.handle=++handles;warp.image=warpPixels;warp.width=warp.height=16;warp.format=1;warp.wrapS=warp.wrapT=1;
        warp.immutablePixels=true;
        original.stageCount=1;original.texgenCount=2;original.indirectCount=1;
        original.stages[0].textureEnabled=true;original.stages[0].texture=0;original.stages[0].coordinate=0;
        material.textureCount=1;material.textures[0]={0,0};
        auto& e=indirect.stages[0];e.enabled=true;e.source=warp;e.coordinate=1;e.state=0x270;e.bias=7;
        e.matrix={{{scale,0,0},{0,scale,0}}};e.sourceScale={1.f/16,1.f/16};e.baseScale={float(width),float(height)};
        indirect.enabled=true;indirect.coordinates=2;
        r.bpValid.set(0x88);r.bp[0x88]=(width-1)|((height-1)<<10)|(1u<<20);
        // Texgens 0 and 1: 2x4 from TEX0/TEX1 (not projected).
        for(unsigned c=0;c<2;++c){r.xfValid.set(0x1040+c);r.xf[0x1040+c]=(5u+c)<<7;}
    }
};

// 4. Planner: offsets equal the warp mean over the size; limits and refusals.
void planner(){
    setIndirectConstantLimit(3);indirectConstantCounters={};
    {   // Constant warp map 0xa0: warp = 0.0625*(160-128) = 2 texels, deviation 0.
        Fixture f(128,64,0xa0,.0625f);IndirectConstantPlan p;
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(p.enabled&&p.count==1&&p.bindings[0].warped&&p.bindings[0].deviation==0);
        assert(p.bindings[0].offset[0]==2.f/128&&p.bindings[0].offset[1]==2.f/64);
    }
    {   // Random I8 map, matrix 0.0625: deviation up to 8 texels: refused at 3, accepted at 10.
        Fixture f(128,128,0,.0625f);for(auto& b:f.warpPixels)b=u8(uniform(0,255));
        IndirectConstantPlan p;
        assert(!planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p)&&!p.enabled);
        assert(indirectConstantCounters.refused[unsigned(IndirectConstantRefusal::deviation)]==1&&indirectConstantCounters.largestRefused>3);
        setIndirectConstantLimit(10);
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p)&&p.bindings[0].deviation<=.0625f*255);
        // Exact mode: refused by the uniformity probe, without a statistics scan.
        setIndirectConstantLimit(0);const auto scans=indirectStatsCache.scans;
        assert(!planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(indirectStatsCache.scans==scans);
        setIndirectConstantLimit(3);
    }
    {   // The probe: uniform over level 0 and the vertex LOD levels only.
        std::vector<u8> pixels(imageBytes(1,16,16,3),0x90);
        Texture t{};t.image=pixels;t.width=t.height=16;t.format=1;t.mipCount=3;t.maxLod=2;t.minFilter=5;t.lodBias=1.5f;
        assert(indirectImageUniform(t));
        pixels[imageBytes(1,16,16,2)+3]=0x91;assert(!indirectImageUniform(t)); // level 2 (blended at LOD 1.5)
        pixels[imageBytes(1,16,16,2)+3]=0x90;pixels[255]=0x8f;assert(!indirectImageUniform(t)); // level 0
        pixels[255]=0x90;t.lodBias=0;t.minFilter=1;pixels[imageBytes(1,16,16,1)]=0;assert(indirectImageUniform(t)); // level 1 unused
    }
    {   // Scale register differs from the texture size: the CPU route's base*scale/size is no plain offset.
        Fixture f(128,64,0xa0,.0625f);f.indirect.stages[0].baseScale={256,64};IndirectConstantPlan p;
        assert(!planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(indirectConstantCounters.refused[unsigned(IndirectConstantRefusal::scale)]==1);
    }
    {   // Missing image register, more than three units.
        Fixture f(128,64,0xa0,.0625f);f.r.bpValid.reset(0x88);IndirectConstantPlan p;
        assert(!planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(!planIndirectConstant(f.r,f.original,f.material,4,f.indirect,false,false,p));
    }
    {   // Two stages share map/coordinate under different warps: refused, or a plain unit with the fallback.
        Fixture f(128,64,0xa0,.0625f);f.original.stageCount=2;f.original.stages[1]=f.original.stages[0];
        IndirectConstantPlan p;
        assert(!planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(indirectConstantCounters.refused[unsigned(IndirectConstantRefusal::shared)]==1);
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,true,false,p));
        assert(p.enabled&&p.sharedPlain&&!p.bindings[0].warped);
        // Same warp on both stages: one offset, no fallback needed.
        f.indirect.stages[1]=f.indirect.stages[0];
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p)&&p.bindings[0].warped&&!p.sharedPlain);
    }
    {   // An unwarped second unit gets no offset.
        Fixture f(128,64,0xa0,.0625f);f.material.textureCount=2;f.material.textures[1]={1,1};
        f.original.stageCount=2;f.original.stages[1]=f.original.stages[0];f.original.stages[1].texture=1;f.original.stages[1].coordinate=1;
        f.r.bpValid.set(0x89);f.r.bp[0x89]=31|(31u<<10);
        IndirectConstantPlan p;
        assert(planIndirectConstant(f.r,f.original,f.material,2,f.indirect,false,false,p)&&p.count==2&&p.bindings[0].warped&&!p.bindings[1].warped);
    }
    {   // Modes. Exact (0, the default): a uniform map's warp (D = 0) only, no
        // projected unit even with per-pixel division available.
        IndirectConstantPlan p;setIndirectConstantLimit(0);
        Fixture f(128,64,0xa0,.0625f);
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,true,p)&&p.bindings[0].warped&&!p.perPixel);
        auto projected=f;projected.r.xf[0x1040]|=2;
        const auto before=indirectConstantCounters.refused[unsigned(IndirectConstantRefusal::projected)];
        assert(!planIndirectConstant(projected.r,projected.original,projected.material,1,projected.indirect,false,true,p)&&!p.enabled);
        // A texgen register the plan cannot read counts as projected.
        auto unreadable=f;unreadable.r.xfValid.reset(0x1040);
        assert(!planIndirectConstant(unreadable.r,unreadable.original,unreadable.material,1,unreadable.indirect,false,true,p));
        // Normal/binormal texgens (types 2 and 3) are never projected.
        auto normal=f;normal.r.xf[0x1040]=(2u<<4)|2u;
        assert(planIndirectConstant(normal.r,normal.original,normal.material,1,normal.indirect,false,false,p));
        // Approximate (3): projected units only with per-pixel division.
        setIndirectConstantLimit(3);
        assert(!planIndirectConstant(projected.r,projected.original,projected.material,1,projected.indirect,false,false,p));
        assert(planIndirectConstant(projected.r,projected.original,projected.material,1,projected.indirect,false,true,p)&&p.perPixel);
        assert(indirectConstantCounters.refused[unsigned(IndirectConstantRefusal::projected)]==before+3);
        // Off (negative): nothing planned, nothing counted.
        setIndirectConstantLimit(-4);assert(indirectConstantLimit==-1);
        const auto counters=indirectConstantCounters;
        assert(!planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,true,p)&&!p.enabled);
        assert(indirectConstantCounters.planned==counters.planned&&indirectConstantCounters.refused==counters.refused);
        setIndirectConstantLimit(std::numeric_limits<float>::quiet_NaN());assert(indirectConstantLimit==0);
        setIndirectConstantLimit(3);
    }
    // Exact mode: a uniform map is probed and scanned once, then cached.
    indirectStatsCache={};setIndirectConstantLimit(0);
    {Fixture f(128,64,0xa0,.0625f);IndirectConstantPlan p;
     for(unsigned n=0;n<3;++n)assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));}
    assert(indirectStatsCache.scans==1&&indirectStatsCache.hits==2);
    setIndirectConstantLimit(3);
    // Statistics cache: an immutable map is scanned once.
    indirectStatsCache={};
    {Fixture f(128,64,0xa0,.0625f);IndirectConstantPlan p;
     for(unsigned n=0;n<5;++n)assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));}
    assert(indirectStatsCache.scans==1&&indirectStatsCache.hits==4);
    {   // A map that may change is scanned again once per frame.
        Fixture f(128,64,0xa0,.0625f);f.indirect.stages[0].source.immutablePixels=false;
        IndirectConstantPlan p;indirectStatsCache={};
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        f.warpPixels.assign(f.warpPixels.size(),0xc0);++textureFrameCounter;
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,false,p));
        assert(indirectStatsCache.scans==2&&indirectStatsCache.hits==1&&p.bindings[0].offset[0]==4.f/128);
    }
    std::printf("PASS constant plan: offset = mean warp / size, deviation/scale/shared/texture/unit/projected refusals, exact/approximate/off modes, plain shared units, stats cache once per map (per frame when mutable)\n");
}

// Host execution of gx_rigid_mt.v.pica's texgen for one unit (roles 1 and -1
// on in0=(TEX0,1,1)/in1=(TEX1,third) or (normal,1); role 2 on (position,1)).
// `tex1`: x and y of shader input v5, the record's second coordinate or in
// its place the warp of the vertex; `third`: its z and w, the third
// coordinate of a wide record.
std::array<float,3> shaderUnit(const RigidVertexPlan& p,unsigned unit,const std::array<float,2>& tex0,const std::array<float,2>& tex1,
                               const std::array<float,3>& normal,const std::array<float,3>& position,const std::array<float,2>& third={0,0}){
    const auto& u=p.uniforms;const unsigned base=multiUnitRow(unit);const float role=multiRole(p,unit);
    auto dot=[](const std::array<float,4>& a,const std::array<float,4>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];};
    const std::array<float,4> in0{tex0[0],tex0[1],1,1},in1{tex1[0],tex1[1],third[0],third[1]},
        n{normal[0]*u[11][3],normal[1]*u[11][3],normal[2]*u[11][3],1},pos{position[0],position[1],position[2],1};
    if(role==1)return {dot(u[base],in0)+dot(u[base+1],in1),dot(u[base+2],in0)+dot(u[base+3],in1),1};
    if(role==-1)return {dot(u[base],n)+dot(u[base+1],in1),dot(u[base+2],n)+dot(u[base+3],in1),1};
    float s=dot(u[base],pos),t=dot(u[base+1],pos);const float q=dot(u[base+2],pos);
    s+=q*(u[base+3][3]*tex1[0]);t+=q*(u[base+3][3]*tex1[1]);
    if(u[base+3][2]>0)return {u[base+3][0]*s,q-u[base+3][1]*t,q};
    const float d=1/std::max(.001953125f,q);
    return {u[base+3][0]*s*d,1-u[base+3][1]*t*d,1};
}
bool near(float a,float b){return std::abs(a-b)<=1e-5f*std::max(1.f,std::max(std::abs(a),std::abs(b)));}
// Cases for the runner, which executes gx_rigid_mt.v.pica itself on them
// (test_gx_rigid_texgen.py's interpreter): a complete uniform block with one
// identity matrix and one dark light, the attributes, the expected outtex.
struct ShaderCase {
    std::array<std::array<float,4>,94> uniforms{};
    struct Vertex {std::array<float,3> position,normal;std::array<float,2> tex0,tex1;std::array<float,3> expected;std::array<float,2> third{};};
    std::vector<Vertex> vertices;
    unsigned role{};
};
std::vector<ShaderCase> shaderCases;
void prepareShaderBlock(RigidVertexPlan& p){
    auto& u=p.uniforms;
    for(unsigned r=0;r<3;++r){u[r]={0,0,0,0};u[r][r]=1;u[3+r]={0,0,0,0};u[3+r][r]=1;}
    for(unsigned r=0;r<4;++r){u[6+r]={0,0,0,0};u[6+r][r]=1;}
    u[9]={0,0,-.1f,10};u[10]={1,1,1,0};u[12]={0,0,0,0};u[14][3]=0;u[15][3]=-1;u[16][3]=1;u[17][3]=0;
    u[19]={0,0,0,0};u[20]={0,0,0,0};u[21]={0,0,0,0};u[22]={1,0,0,0};u[93]={1,1,1,1};
}
void writeShaderCases(const char* path){
    auto* file=std::fopen(path,"w");assert(file);
    std::fprintf(file,"{\"cases\":[");
    for(std::size_t n=0;n<shaderCases.size();++n){const auto& c=shaderCases[n];
        std::fprintf(file,"%s{\"name\":\"case%zu-role%u\",\"lights\":1,\"uniforms\":[",n?",":"",n,c.role);
        for(unsigned r=0;r<94;++r)std::fprintf(file,"%s[%.9g,%.9g,%.9g,%.9g]",r?",":"",double(c.uniforms[r][0]),double(c.uniforms[r][1]),double(c.uniforms[r][2]),double(c.uniforms[r][3]));
        std::fprintf(file,"],\"vertices\":[");
        for(std::size_t v=0;v<c.vertices.size();++v){const auto& x=c.vertices[v];
            std::fprintf(file,"%s{\"v0\":[%.9g,%.9g,%.9g,1],\"v1\":[%.9g,%.9g,%.9g,0],\"v2\":[%.9g,%.9g,0,0],\"v3\":[1,1,1,1],\"v4\":[0,3,0,0],"
                "\"v5\":[%.9g,%.9g,%.9g,%.9g],\"expected\":[%.9g,%.9g,%.9g]}",v?",":"",
                double(x.position[0]),double(x.position[1]),double(x.position[2]),double(x.normal[0]),double(x.normal[1]),double(x.normal[2]),
                double(x.tex0[0]),double(x.tex0[1]),double(x.tex1[0]),double(x.tex1[1]),double(x.third[0]),double(x.third[1]),
                double(x.expected[0]),double(x.expected[1]),double(x.expected[2]));}
        std::fprintf(file,"]}");
    }
    std::fprintf(file,"]}\n");std::fclose(file);
}

// 5. Folded offsets: with a constant warp map, the GPU route's coordinates
// equal the CPU route's evaluateVertexIndirect output (scaled and flipped as
// the CPU route writes them, projection mode for the projected unit).
void foldMatchesCpuRoute(){
    unsigned vertices=0;
    for(unsigned iteration=0;iteration<400;++iteration){
        const float scale=real(-.12f,.12f);
        Fixture f(32u<<uniform(0,3),32u<<uniform(0,3),u8(uniform(0,255)),scale);
        const unsigned width=(f.r.bp[0x88]&1023)+1,height=((f.r.bp[0x88]>>10)&1023)+1;
        IndirectConstantPlan plan;
        setIndirectConstantLimit(3);
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,true,plan));
        const unsigned role=uniform(0,2); // 0: TEX texgen, 1: normal texgen, 2: projected
        RigidVertexPlan p{};p.multiTexture=true;prepareShaderBlock(p);p.uniforms[11][3]=real(.5f,2);
        std::array<std::array<float,2>,3> scales{};scales[0]={real(.25f,1),real(.25f,1)};
        const float su=scales[0][0],sv=scales[0][1];
        const unsigned base=multiUnitRow(0);
        std::array<std::array<float,4>,3> rows;for(auto& row:rows)for(auto& v:row)v=real(-2,2);
        if(role<2){
            // As planRigidMultiVertex places them (TEX0 source, or the normal).
            p.uniforms[base]={su*rows[0][0],su*rows[0][1],su*rows[0][2],su*rows[0][3]};
            p.uniforms[base+2]={-sv*rows[1][0],-sv*rows[1][1],-sv*rows[1][2],1-sv*rows[1][3]};
            multiRole(p,0)=role==0?1.f:-1.f;
        }else{
            rows[2][3]=real(40,60); // q of at least 10 at every vertex below
            for(unsigned k=0;k<3;++k)p.uniforms[base+k]=rows[k];
            p.uniforms[base+3]={su,sv,0,0};multiRole(p,0)=2;p.multiProjected=true;
        }
        auto planned=p;
        assert(applyIndirectConstant(p,plan,f.material,1,scales));
        assert(indirectConstantPerPixel(p)==(role==2));
        const bool collect=iteration<60;
        ShaderCase folded,plainCase;
        if(collect){folded.uniforms=p.uniforms;folded.role=role;plainCase.uniforms=planned.uniforms;plainCase.role=role+10;}
        for(unsigned n=0;n<8;++n){
            const std::array<float,2> tex0{real(-4,4),real(-4,4)},tex1{real(-4,4),real(-4,4)};
            const std::array<float,3> normal{real(-1,1),real(-1,1),real(-1,1)},position{real(-5,5),real(-5,5),real(-5,5)};
            // CPU route: the original texgen value, the warp, then the GPU vertex form.
            std::array<float,3> baseCoord,source{.3f,.7f,1};
            const float k=p.uniforms[11][3];
            if(role==0)for(unsigned a=0;a<2;++a)baseCoord[a]=rows[a][0]*tex0[0]+rows[a][1]*tex0[1]+rows[a][2]+rows[a][3];
            else if(role==1)for(unsigned a=0;a<2;++a)baseCoord[a]=rows[a][0]*normal[0]*k+rows[a][1]*normal[1]*k+rows[a][2]*normal[2]*k+rows[a][3];
            else for(unsigned a=0;a<3;++a)baseCoord[a]=rows[a][0]*position[0]+rows[a][1]*position[1]+rows[a][2]*position[2]+rows[a][3];
            if(role<2)baseCoord[2]=1;
            Texture destination{};destination.width=std::uint16_t(width);destination.height=std::uint16_t(height);
            std::array<float,2> warped;
            assert(evaluateVertexIndirect(f.indirect.stages[0],source,baseCoord,false,role==2,destination,warped));
            const float q=role==2?baseCoord[2]:1;
            const std::array<float,3> cpu{warped[0]*su,q-warped[1]*sv,q};
            const auto gpu=shaderUnit(p,0,tex0,tex1,normal,position);
            for(unsigned c=0;c<3;++c)assert(near(gpu[c],cpu[c]));
            // Without the plan the rows give the unwarped coordinate.
            const auto plain=shaderUnit(planned,0,tex0,tex1,normal,position);
            if(role<2)assert(near(plain[0],baseCoord[0]*su)&&near(plain[1],1-baseCoord[1]*sv));
            if(collect&&n<3){
                folded.vertices.push_back({position,normal,tex0,tex1,cpu});
                // Without the plan: the shader's own per-vertex divide for role 2.
                const float q0=std::max(.001953125f,baseCoord[2]);
                const std::array<float,3> before=role<2?std::array<float,3>{baseCoord[0]*su,1-baseCoord[1]*sv,1}:
                    std::array<float,3>{su*baseCoord[0]/q0,1-sv*baseCoord[1]/q0,1};
                plainCase.vertices.push_back({position,normal,tex0,tex1,before});
            }
            ++vertices;
        }
        if(collect){shaderCases.push_back(folded);shaderCases.push_back(plainCase);}
    }
    // Refusals: a role-less unit, a missing binding, a unit count mismatch.
    Fixture f(64,64,0x90,.0625f);IndirectConstantPlan plan;setIndirectConstantLimit(3);
    assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,true,plan));
    RigidVertexPlan p{};p.multiTexture=true;std::array<std::array<float,2>,3> scales{{{1,1},{1,1},{1,1}}};
    multiRole(p,0)=.5f;assert(!applyIndirectConstant(p,plan,f.material,1,scales));
    multiRole(p,0)=1;auto other=f.material;other.textures[0]={2,0};assert(!applyIndirectConstant(p,plan,other,1,scales));
    assert(!applyIndirectConstant(p,plan,f.material,2,scales));
    RigidVertexPlan single{};assert(!applyIndirectConstant(single,plan,f.material,1,scales));
    // A projected unit planned without per-pixel division: no fold.
    {auto exact=plan;exact.perPixel=false;RigidVertexPlan projected{};projected.multiTexture=projected.multiProjected=true;multiRole(projected,0)=2;
     assert(!applyIndirectConstant(projected,exact,f.material,1,scales)&&applyIndirectConstant(projected,plan,f.material,1,scales));}
    // Units swapped after planning (routeProjectiveTexture): matched by binding.
    f.material.textureCount=2;f.material.textures[1]={1,1};
    f.original.stageCount=2;f.original.stages[1]=f.original.stages[0];f.original.stages[1].texture=1;f.original.stages[1].coordinate=1;
    f.r.bpValid.set(0x89);f.r.bp[0x89]=63|(63u<<10);
    assert(planIndirectConstant(f.r,f.original,f.material,2,f.indirect,false,true,plan));
    auto swapped=f.material;std::swap(swapped.textures[0],swapped.textures[1]);
    RigidVertexPlan two{};two.multiTexture=true;multiRole(two,0)=1;multiRole(two,1)=1;
    assert(applyIndirectConstant(two,plan,swapped,2,scales));
    assert(two.uniforms[multiUnitRow(0)][3]==0&&two.uniforms[multiUnitRow(1)][3]==plan.bindings[0].offset[0]);
    std::printf("PASS folded offsets: %u vertices, GPU texgen (TEX, normal, projected per pixel) equals the CPU route's warped coordinate; role/binding/count refusals; units matched by binding after a swap\n",vertices);
}

// 7. The per-vertex part. With a map that varies, the GPU route's coordinate
// (rows after applyIndirectConstant and planVertexWarp, the vertex's warp as
// VertexWarpSampler stores it) equals the CPU route's evaluateVertexIndirect
// output at every vertex, within what 16-bit storage and the integer
// sampler round away.
struct WarpCase {
    Fixture f;
    std::vector<u8> pixels;
    std::array<std::array<float,4>,3> mapRows{}; // the map's coordinate before the indirect scale
    bool projectedMap{};
    DecodedVertex first{};
    // format, size, wrap and filter of the map; the map's coordinate from
    // TEX1 (2x4, optional texture and post matrices) or the position (3x4).
    WarpCase(unsigned width,unsigned height,unsigned format,unsigned mapSize,unsigned wrap,bool linear,unsigned mips,float lodBias,
             bool matrix,bool post,bool fromPosition):f(width,height,0,real(-.12f,.12f)){
        auto t=image(pixels,format,mapSize,mapSize==24?16:mapSize,mips);
        static TextureHandle handles=5000;t.handle=++handles;t.immutablePixels=true;
        t.wrapS=t.wrapT=std::uint8_t(wrap);t.magFilter=linear;t.minFilter=std::uint8_t(mips>1?(linear?5:4):linear);
        t.maxLod=std::uint8_t(mips-1);t.lodBias=lodBias;
        auto& e=f.indirect.stages[0];e.source=t;
        const float scale=std::ldexp(1.f,-int(uniform(0,3)));e.sourceScale={scale,scale*.5f};
        for(auto& row:e.matrix)for(auto& m:row)m=real(-.05f,.05f);
        e.bias=uniform(0,7);
        auto& r=f.r;
        projectedMap=fromPosition;
        r.matrixIndexValid.set(0);r.matrixIndexValid.set(1);
        const unsigned index=matrix||fromPosition?33u:60u;
        r.matrixIndex[0]=(60u<<6)|(index<<12)|(60u<<18)|(60u<<24);r.matrixIndex[1]=60u|(60u<<6)|(60u<<12)|(60u<<18);
        // Texgen 1: AB11 from TEX1, or ABC1 from the position with its divisor.
        r.xf[0x1041]=fromPosition?(2u|4u):(6u<<7);
        std::array<std::array<float,4>,3> rows{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
        if(index!=60)for(unsigned row=0;row<(fromPosition?3u:2u);++row)for(unsigned k=0;k<4;++k){
            rows[row][k]=row==2?(k==3?50.f:real(-1,1)):real(-2,2);
            r.xfValid.set(4*index+4*row+k);r.xf[4*index+4*row+k]=std::bit_cast<std::uint32_t>(rows[row][k]);
        }
        for(unsigned c=0;c<2;++c){r.xfValid.set(0x1050+c);r.xf[0x1050+c]=61;}
        if(!fromPosition)for(auto& row:rows){row[3]+=row[2];row[2]=0;}
        if(post){
            constexpr unsigned postRow=6;r.xf[0x1051]=postRow;
            std::array<std::array<float,4>,3> composed{{{0,0,0,0},{0,0,0,0},{0,0,1,0}}};
            for(unsigned row=0;row<(fromPosition?3u:2u);++row){
                float pr[4];
                for(unsigned k=0;k<4;++k){
                    pr[k]=row==2?(k==2?1.f:0.f):real(-1.5f,1.5f);
                    r.xfValid.set(0x500+4*(postRow+row)+k);r.xf[0x500+4*(postRow+row)+k]=std::bit_cast<std::uint32_t>(pr[k]);
                }
                for(unsigned k=0;k<4;++k)composed[row][k]=pr[0]*rows[0][k]+pr[1]*rows[1][k]+(fromPosition?pr[2]*rows[2][k]:0.f);
                composed[row][3]+=fromPosition?pr[3]:pr[2]+pr[3];
            }
            rows=composed;
        }
        mapRows=rows;
        first.present=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_TEX0)|(1u<<GX_VA_TEX1);
    }
    // The map's coordinate of a vertex as the transform makes it (s, t, q).
    std::array<float,3> mapCoordinate(const std::array<float,2>& tex1,const std::array<float,3>& position)const{
        const std::array<float,4> in=projectedMap?std::array<float,4>{position[0],position[1],position[2],1}:
            std::array<float,4>{tex1[0],tex1[1],1,1};
        std::array<float,3> out{0,0,1};
        for(unsigned row=0;row<(projectedMap?3u:2u);++row)out[row]=mapRows[row][0]*in[0]+mapRows[row][1]*in[1]+mapRows[row][2]*in[2]+mapRows[row][3];
        return out;
    }
};
void vertexWarp(){
    setIndirectConstantLimit(indirectConstantEveryWarp);vertexWarpEnabled=true;vertexWarpCounters={};
    unsigned vertices=0,integerDraws=0,generalDraws=0,projectedMaps=0,turnsDraws=0;
    float worst=0,constantMiss=0;
    for(unsigned iteration=0;iteration<600;++iteration){
        const unsigned role=uniform(0,2); // 0: TEX texgen, 1: normal texgen, 2: projected
        // Two draws in three take the forms the game's maps have (the
        // integer sampler); the rest every other format, wrap and filter.
        const bool common=iteration%3!=0;
        const unsigned formats[]{0,1,2,3,4,5,6,14};
        const unsigned format=common?uniform(0,3):formats[uniform(0,7)];
        const unsigned mapSize=common?(16u<<uniform(0,2)):(uniform(0,3)?(8u<<uniform(0,2)):24u);
        const unsigned mips=common?1u:uniform(1,3),wrap=common?1u:uniform(0,2);
        const bool linear=common||uniform(0,1),fromPosition=uniform(0,4)==0;
        // The integer sampler's conditions (gx_vertex_warp.hpp).
        const bool integer=format<=3&&wrap==1&&linear&&mips==1&&mapSize!=24;
        WarpCase w(32u<<uniform(0,3),32u<<uniform(0,3),format,mapSize,wrap,linear,mips,
                   mips>1?real(0,float(mips-1)):0.f,uniform(0,1),uniform(0,1),fromPosition);
        auto& f=w.f;
        const unsigned width=(f.r.bp[0x88]&1023)+1,height=((f.r.bp[0x88]>>10)&1023)+1;
        IndirectConstantPlan plan;IndirectWarpSources sources;
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,true,plan,&sources));
        RigidVertexPlan p{};p.multiTexture=true;prepareShaderBlock(p);p.uniforms[11][3]=real(.5f,2);
        std::array<std::array<float,2>,3> scales{};scales[0]={real(.25f,1),real(.25f,1)};
        const float su=scales[0][0],sv=scales[0][1];
        const unsigned base=multiUnitRow(0);
        std::array<std::array<float,4>,3> rows;for(auto& row:rows)for(auto& v:row)v=real(-2,2);
        if(role<2){
            p.uniforms[base]={su*rows[0][0],su*rows[0][1],su*rows[0][2],su*rows[0][3]};
            p.uniforms[base+2]={-sv*rows[1][0],-sv*rows[1][1],-sv*rows[1][2],1-sv*rows[1][3]};
            multiRole(p,0)=role==0?1.f:-1.f;
        }else{
            rows[2][3]=real(40,60);
            for(unsigned k=0;k<3;++k)p.uniforms[base+k]=rows[k];
            p.uniforms[base+3]={su,sv,0,0};multiRole(p,0)=2;p.multiProjected=true;
        }
        assert(applyIndirectConstant(p,plan,f.material,1,scales));
        const auto constantOnly=p;
        assert(planVertexWarp(f.r,w.first,p,plan,sources,f.material,1,scales));
        assert(!p.wideRecord&&p.vertexWarp.enabled&&p.texcoordAttribute[1]==0xff&&p.vertexWarp.projected==fromPosition);
        assert(p.vertexWarp.sourceAttribute==(fromPosition?unsigned(GX_VA_POS):unsigned(GX_VA_TEX1)));
        const VertexWarpSampler sampler(p.vertexWarp);
        assert(sampler.integerForm()==integer);
        ++(integer?integerDraws:generalDraws);projectedMaps+=fromPosition;
        // What storage and the integer sampler may round away of each stored
        // number, in its own steps: half a step, and a quarter of a byte of
        // the sample through the reference unit's offsets.
        const auto& e=f.indirect.stages[0];
        std::array<float,2> step;
        for(unsigned axis=0;axis<2;++axis){
            float perByte=0;for(float m:e.matrix[axis])perByte+=std::abs(m);
            step[axis]=.51f+(integer?.26f:2e-3f)*perByte/float(axis?height:width)*p.vertexWarp.recordScale;
        }
        if(integer&&!fromPosition){
            // The pass without floats (runTurns) on an indexed S16 coordinate
            // gives what store gives for the same elements, within two of
            // the coordinate's 1024 steps of a texel.
            constexpr unsigned elementCount=40,named=96;
            std::vector<std::int16_t> elements(2*elementCount);for(auto& v:elements)v=std::int16_t(int(uniform(0,3072))-1536);
            std::vector<u8> indices;std::vector<unsigned> order;
            for(unsigned n=0;n<named;++n){const unsigned element=uniform(0,elementCount-1);order.push_back(element);
                indices.push_back(u8(element>>8));indices.push_back(u8(element));}
            std::vector<std::int16_t> out(2*named,0x7777);
            const VertexWarpSampler pass(p.vertexWarp);
            const auto* data=reinterpret_cast<const u8*>(elements.data());auto* target=reinterpret_cast<std::byte*>(out.data());
            assert(pass.runTurns(GX_S16,1.f/256,data,4,elementCount-1,true,indices.data(),2,named,target,4)==0);
            for(unsigned n=0;n<named;++n){
                const float in[3]{elements[2*order[n]]/256.f,elements[2*order[n]+1]/256.f,1};
                std::byte reference[4];sampler.store(in,reference);
                std::int16_t value[2];std::memcpy(value,reference,4);
                for(unsigned i=0;i<2;++i){
                    float perByte=0;for(float v:p.vertexWarp.functional[i])perByte+=std::abs(v);
                    assert(std::abs(float(out[2*n+i])-float(value[i]))<=2+.55f*perByte*p.vertexWarp.recordScale);
                }
            }
            // An index beyond its array stops the pass; another type is not this form.
            assert(pass.runTurns(GX_S16,1.f/256,data,4,0,true,indices.data(),2,named,target,4)==(*std::max_element(order.begin(),order.end())>0?1:0));
            assert(pass.runTurns(GX_F32,1,data,4,elementCount-1,true,indices.data(),2,named,target,4)==-1);
            ++turnsDraws;
        }
        const bool collect=iteration<90;
        ShaderCase warped;
        if(collect){warped.uniforms=p.uniforms;warped.role=20+role;}
        for(unsigned n=0;n<8;++n){
            const std::array<float,2> tex0{real(-4,4),real(-4,4)},tex1{real(-6,6),real(-6,6)};
            const std::array<float,3> normal{real(-1,1),real(-1,1),real(-1,1)},position{real(-5,5),real(-5,5),real(-5,5)};
            std::array<float,3> baseCoord;
            const float k=p.uniforms[11][3];
            if(role==0)for(unsigned a=0;a<2;++a)baseCoord[a]=rows[a][0]*tex0[0]+rows[a][1]*tex0[1]+rows[a][2]+rows[a][3];
            else if(role==1)for(unsigned a=0;a<2;++a)baseCoord[a]=rows[a][0]*normal[0]*k+rows[a][1]*normal[1]*k+rows[a][2]*normal[2]*k+rows[a][3];
            else for(unsigned a=0;a<3;++a)baseCoord[a]=rows[a][0]*position[0]+rows[a][1]*position[1]+rows[a][2]*position[2]+rows[a][3];
            if(role<2)baseCoord[2]=1;
            const auto source=w.mapCoordinate(tex1,position);
            Texture destination{};destination.width=std::uint16_t(width);destination.height=std::uint16_t(height);
            std::array<float,2> cpuWarped;
            assert(evaluateVertexIndirect(e,source,baseCoord,fromPosition,role==2,destination,cpuWarped));
            const float q=role==2?baseCoord[2]:1;
            const std::array<float,3> cpu{cpuWarped[0]*su,q-cpuWarped[1]*sv,q};
            // The decoder's part: the source element of the vertex.
            const std::array<float,3> element=fromPosition?position:std::array<float,3>{tex1[0],tex1[1],1};
            std::byte stored[4];sampler.store(element.data(),stored);
            std::int16_t value[2];std::memcpy(value,stored,4);
            assert(std::abs(value[0])<32767&&std::abs(value[1])<32767);
            // The record's second coordinate holds the stored numbers.
            const std::array<float,2> warp{float(value[0]),float(value[1])};
            const auto gpu=shaderUnit(p,0,tex0,warp,normal,position);
            // Through the coefficients the shader applies to the stored numbers.
            const auto& u=p.uniforms;
            const float limits[3]{
                role==2?su*q*u[base+3][3]*step[0]:std::abs(u[base+1][0])*step[0]+std::abs(u[base+1][1])*step[1],
                role==2?sv*q*u[base+3][3]*step[1]:std::abs(u[base+3][0])*step[0]+std::abs(u[base+3][1])*step[1],0};
            for(unsigned c=0;c<3;++c){
                const float error=std::abs(gpu[c]-cpu[c]);
                assert(error<=limits[c]+2e-5f*std::max(1.f,std::abs(cpu[c])));
                if(c<2)worst=std::max(worst,error/(c?sv:su)/q*float(c?height:width));
            }
            // The constant alone misses what the map adds at this vertex.
            const auto flat=shaderUnit(constantOnly,0,tex0,{0,0},normal,position);
            constantMiss=std::max(constantMiss,std::abs(flat[0]-cpu[0])/su/q*float(width));
            if(collect&&n<3)warped.vertices.push_back({position,normal,tex0,warp,gpu});
            ++vertices;
        }
        if(collect)shaderCases.push_back(warped);
    }
    assert(integerDraws&&generalDraws&&projectedMaps&&turnsDraws&&constantMiss>10*worst);
    // Two warped units of one map with their own matrices and sizes: both
    // carry the per-vertex part through K when the map has one or two
    // channels; with three channels and another matrix the second unit keeps
    // the constant.
    unsigned combined=0,left=0;
    for(unsigned iteration=0;iteration<200;++iteration){
        const unsigned format=iteration%4==3?4u:uniform(0,3);
        WarpCase w(64,32,format,32,1,true,1,0,true,false,false);
        auto& f=w.f;
        f.material.textureCount=2;f.material.textures[1]={1,0};
        f.original.stageCount=2;f.original.stages[1]=f.original.stages[0];f.original.stages[1].texture=1;
        f.r.bpValid.set(0x89);f.r.bp[0x89]=127|(63u<<10);
        f.indirect.stages[1]=f.indirect.stages[0];f.indirect.stages[1].baseScale={128,64};
        for(auto& row:f.indirect.stages[1].matrix)for(auto& m:row)m=real(-.05f,.05f);
        IndirectConstantPlan plan;IndirectWarpSources sources;
        assert(planIndirectConstant(f.r,f.original,f.material,2,f.indirect,false,true,plan,&sources));
        RigidVertexPlan p{};p.multiTexture=true;prepareShaderBlock(p);
        std::array<std::array<float,2>,3> scales{{{real(.25f,1),real(.25f,1)},{real(.25f,1),real(.25f,1)},{1,1}}};
        std::array<std::array<float,4>,2> rows;for(auto& row:rows)for(auto& v:row)v=real(-2,2);
        for(unsigned unit=0;unit<2;++unit){
            const unsigned base=multiUnitRow(unit);const float su=scales[unit][0],sv=scales[unit][1];
            p.uniforms[base]={su*rows[0][0],su*rows[0][1],su*rows[0][2],su*rows[0][3]};
            p.uniforms[base+2]={-sv*rows[1][0],-sv*rows[1][1],-sv*rows[1][2],1-sv*rows[1][3]};
            multiRole(p,unit)=1;
        }
        assert(applyIndirectConstant(p,plan,f.material,2,scales));
        const auto before=vertexWarpCounters;
        assert(planVertexWarp(f.r,w.first,p,plan,sources,f.material,2,scales));
        const bool both=format<=3;
        assert(vertexWarpCounters.units-before.units==(both?2u:1u)&&vertexWarpCounters.constantUnits-before.constantUnits==(both?0u:1u));
        ++(both?combined:left);
        const VertexWarpSampler sampler(p.vertexWarp);
        for(unsigned n=0;n<6;++n){
            const std::array<float,2> tex0{real(-4,4),real(-4,4)},tex1{real(-6,6),real(-6,6)};
            const std::array<float,3> none{};
            const std::array<float,3> baseCoord{rows[0][0]*tex0[0]+rows[0][1]*tex0[1]+rows[0][2]+rows[0][3],
                rows[1][0]*tex0[0]+rows[1][1]*tex0[1]+rows[1][2]+rows[1][3],1};
            const std::array<float,3> element{tex1[0],tex1[1],1};
            std::byte stored[4];sampler.store(element.data(),stored);
            std::int16_t value[2];std::memcpy(value,stored,4);
            // The rounding of the stored numbers (those of unit 0), as above.
            std::array<float,2> step;
            for(unsigned axis=0;axis<2;++axis){
                float perByte=0;for(float m:f.indirect.stages[0].matrix[axis])perByte+=std::abs(m);
                step[axis]=.51f+.26f*perByte/f.indirect.stages[0].baseScale[axis]*p.vertexWarp.recordScale;
            }
            for(unsigned unit=0;unit<(both?2u:1u);++unit){
                const auto& e=f.indirect.stages[unit];
                Texture destination{};destination.width=std::uint16_t(e.baseScale[0]);destination.height=std::uint16_t(e.baseScale[1]);
                std::array<float,2> cpuWarped;
                assert(evaluateVertexIndirect(e,w.mapCoordinate(tex1,none),baseCoord,false,false,destination,cpuWarped));
                const std::array<float,2> cpu{cpuWarped[0]*scales[unit][0],1-cpuWarped[1]*scales[unit][1]};
                const auto gpu=shaderUnit(p,unit,tex0,{float(value[0]),float(value[1])},none,none);
                for(unsigned c=0;c<2;++c){
                    // The second unit's offsets come through K from numbers
                    // rounded for the first.
                    const auto& row=p.uniforms[multiUnitRow(unit)+(c?3:1)];
                    const float limit=std::abs(row[0])*step[0]+std::abs(row[1])*step[1];
                    assert(std::abs(gpu[c]-cpu[c])<=limit+2e-5f*std::max(1.f,std::abs(cpu[c])));
                }
                ++vertices;
            }
        }
    }
    assert(combined&&left);
    // Refusals. Each leaves the plan as the constant plan made it.
    {
        WarpCase w(64,64,1,16,1,true,1,0,true,false,false);auto& f=w.f;
        IndirectConstantPlan plan;IndirectWarpSources sources;
        assert(planIndirectConstant(f.r,f.original,f.material,1,f.indirect,false,true,plan,&sources));
        std::array<std::array<float,2>,3> scales{{{1,1},{1,1},{1,1}}};
        auto fresh=[&]{RigidVertexPlan p{};p.multiTexture=true;multiRole(p,0)=1;return p;};
        auto refusedBy=[&](VertexWarpRefusal why,auto&& attempt,float role=1){
            const auto before=vertexWarpCounters.refused[unsigned(why)];
            auto p=fresh();multiRole(p,0)=role;const auto untouched=p.uniforms;
            assert(!attempt(p)&&!p.vertexWarp.enabled&&p.texcoordAttribute[1]==GX_VA_TEX1&&p.uniforms==untouched);
            assert(vertexWarpCounters.refused[unsigned(why)]==before+1);
        };
        // The record's second coordinate is taken: a wide record (three
        // coordinate sets), or a unit generated from the second set.
        for(const bool wide:{true,false}){
            const auto before=vertexWarpCounters.refused[unsigned(VertexWarpRefusal::record)];
            auto p=fresh();p.wideRecord=wide;if(!wide)p.unitSource[0]=1;
            assert(!planVertexWarp(f.r,w.first,p,plan,sources,f.material,1,scales)&&!p.vertexWarp.enabled&&p.texcoordAttribute[1]==GX_VA_TEX1);
            assert(vertexWarpCounters.refused[unsigned(VertexWarpRefusal::record)]==before+1);
        }
        // A fog ramp unit takes no offset.
        refusedBy(VertexWarpRefusal::role,[&](RigidVertexPlan& p){return planVertexWarp(f.r,w.first,p,plan,sources,f.material,1,scales);},.5f);
        // The map's coordinate projected from a texture coordinate, a texgen
        // of another type, a normalising post matrix.
        for(const std::uint32_t gen:{(6u<<7)|2u,(6u<<7)|(1u<<4)}){
            auto r=f.r;r.xf[0x1041]=gen;
            refusedBy(VertexWarpRefusal::texgen,[&](RigidVertexPlan& p){return planVertexWarp(r,w.first,p,plan,sources,f.material,1,scales);});
        }
        {auto r=f.r;r.xf[0x1051]=61|0x100;
         refusedBy(VertexWarpRefusal::texgen,[&](RigidVertexPlan& p){return planVertexWarp(r,w.first,p,plan,sources,f.material,1,scales);});}
        // Its source attribute absent from the vertex; a matrix chosen per vertex.
        for(const std::uint32_t present:{w.first.present&~(1u<<GX_VA_TEX1),w.first.present|(1u<<(GX_VA_TEX0MTXIDX+1))}){
            auto first=w.first;first.present=present;
            refusedBy(VertexWarpRefusal::attribute,[&](RigidVertexPlan& p){return planVertexWarp(f.r,first,p,plan,sources,f.material,1,scales);});
        }
        // A map without variation: the constant is the whole warp.
        {Fixture flat(64,64,0x90,.0625f);flat.r=f.r;
         IndirectConstantPlan flatPlan;IndirectWarpSources flatSources;
         assert(planIndirectConstant(flat.r,flat.original,flat.material,1,flat.indirect,false,true,flatPlan,&flatSources));
         refusedBy(VertexWarpRefusal::uniform,[&](RigidVertexPlan& p){return planVertexWarp(flat.r,w.first,p,flatPlan,flatSources,flat.material,1,scales);});}
        // The switch: nothing planned, nothing counted.
        vertexWarpEnabled=false;const auto counters=vertexWarpCounters;
        {auto p=fresh();assert(!planVertexWarp(f.r,w.first,p,plan,sources,f.material,1,scales)&&!p.vertexWarp.enabled);}
        assert(vertexWarpCounters.planned==counters.planned&&vertexWarpCounters.refused==counters.refused);
        vertexWarpEnabled=true;
    }
    std::printf("PASS per-vertex warp: %u vertices on %u integer-form and %u general-form draws (%u maps projected from the position), GPU coordinate equals "
        "the CPU route's warped coordinate within storage rounding (largest difference %.4f destination texels); two units through K; "
        "record/role/texgen/attribute/uniform refusals and the switch; the pass without floats equals it on %u draws\n",vertices,integerDraws,
        generalDraws,projectedMaps,double(worst),turnsDraws);
}

// 8. A projected unit that follows the camera (projectedUnitFollowsCamera):
// q = k w for the rows of one position matrix, whatever the compact origin.
void cameraDivisor(){
    unsigned accepted=0,refused=0;
    for(unsigned iteration=0;iteration<400;++iteration){
        RigidVertexPlan p{};p.multiTexture=p.multiProjected=true;p.matrices=1;multiRole(p,0)=2;
        auto& u=p.uniforms;
        // A view matrix: rotation about two axes and a translation of world size.
        const float a=real(0,6.28f),b=real(0,6.28f);
        const float ca=std::cos(a),sa=std::sin(a),cb=std::cos(b),sb=std::sin(b);
        u[0]={ca,0,sa,real(-40000,40000)};u[1]={sa*sb,cb,-ca*sb,real(-40000,40000)};u[2]={-sa*cb,sb,ca*cb,real(-40000,40000)};
        // Perspective projection: w = -z of the view.
        u[6]={real(.5f,2),0,0,0};u[7]={0,real(.5f,2),0,0};u[8]={0,0,real(-.6f,-.4f),real(-2,-.5f)};u[9]={0,0,-1,0};
        const float k=std::ldexp(1.f,int(uniform(0,6))-3);
        const unsigned base=multiUnitRow(0);
        for(unsigned c=0;c<4;++c){u[base][c]=real(-2,2);u[base+1][c]=real(-2,2);u[base+2][c]=-k*u[2][c];}
        assert(projectedUnitFollowsCamera(p));++accepted;
        // The compact origin folded into both sets of rows changes nothing.
        {auto folded=p;const float o[3]{real(-30000,30000),real(-500,500),real(-30000,30000)};
         for(const unsigned row:{0u,1u,2u,base,base+1,base+2}){auto& m=folded.uniforms[row];
            m[3]=float(double(m[3])+double(m[0])*o[0]+double(m[1])*o[1]+double(m[2])*o[2]);}
         assert(projectedUnitFollowsCamera(folded));}
        // Another direction, another depth, the far side, several matrices,
        // a matrix chosen per vertex, an orthographic projection.
        {auto other=p;other.uniforms[base+2][iteration%3]+=k*(iteration%2?.01f:-.01f);assert(!projectedUnitFollowsCamera(other));}
        {auto other=p;other.uniforms[base+2][3]+=k*2;assert(!projectedUnitFollowsCamera(other));}
        {auto other=p;for(auto& v:other.uniforms[base+2])v=-v;assert(!projectedUnitFollowsCamera(other));}
        {auto other=p;other.matrices=2;assert(!projectedUnitFollowsCamera(other));}
        {auto other=p;other.indexedMatrices=true;assert(!projectedUnitFollowsCamera(other));}
        {auto other=p;other.uniforms[9]={0,0,0,1};assert(!projectedUnitFollowsCamera(other));}
        {auto other=p;other.multiProjected=false;assert(!projectedUnitFollowsCamera(other));}
        refused+=7;
    }
    std::printf("PASS camera divisor: %u projected units with q = k w accepted (origin folded or not), %u with another direction, depth, sign, "
        "matrix count, per-vertex matrix or an orthographic projection refused\n",accepted,refused);
}

// 6. Report lines stay under the console's 255 characters.
void report(){
    auto& c=indirectConstantCounters;
    // Twelve-digit counts (a year of frames) and clamped extreme values.
    constexpr std::uint64_t large=999999999999ull;
    c.planned=c.gpu=c.vertices=c.perPixel=c.restarts=large;
    for(auto& v:c.refused)v=large;
    c.largestAccepted=c.largestRefused=1e30f;c.last.warped=true;c.last.offset={-1e30f,-1e30f};c.last.deviation=1e30f;
    c.lastWidth=c.lastHeight=65535;c.last.map=255;c.last.coordinate=255;
    indirectStatsCache.scans=indirectStatsCache.hits=large;
    {auto& w=vertexWarpCounters;w.planned=w.draws=w.vertices=w.units=w.constantUnits=large;for(auto& v:w.refused)v=large;}
    vertexWarpIntegerVertices=vertexWarpRepeatVertices=large;
    auto& f=indirectFormatAdmissions;f={};
    // Counted at submission from the plan's note: one per draw, newest formats only.
    {VertexIndirectPlan plan;plan.enabled=true;plan.stages[3].enabled=true;plan.stages[3].source.format=14;
     plan.stages[3].source.width=64;plan.stages[3].source.height=32;
     const auto note=indirectFormatNote(plan);assert(note.format==14&&note.width==64&&note.height==32);
     countIndirectFormat(note);countIndirectFormat(indirectFormatNote({}));
     plan.stages[3].source.format=1;countIndirectFormat(indirectFormatNote(plan));
     assert(f.cmpr==1&&f.rgb565==0&&f.rgba8==0&&f.lastFormat==14&&f.lastWidth==64);}
    f.rgb565=f.rgba8=f.cmpr=large;f.lastWidth=f.lastHeight=65535;f.lastFormat=255;
    setIndirectConstantLimit(1e30f); // the longest mode text and limit
    static unsigned lines,longest;lines=longest=0;
    reportIndirectConstant(+[](const char* line){++lines;longest=std::max<unsigned>(longest,unsigned(std::strlen(line)));});
    assert(lines==6&&longest<255);
    reportIndirectConstant(+[](const char*){assert(false);}); // unchanged: silent
    std::printf("PASS report: %u lines, longest %u characters with the largest values, silent when unchanged\n",lines,longest);
}
}

int main(int argc,char** argv){
    legacyEquivalence();
    decoderAgreement();
    statisticsAndBound();
    planner();
    foldMatchesCpuRoute();
    vertexWarp();
    cameraDivisor();
    report();
    if(argc==3&&!std::strcmp(argv[1],"--shader-cases")){
        writeShaderCases(argv[2]);
        std::printf("PASS shader cases written: %zu uniform blocks for gx_rigid_mt.v.pica\n",shaderCases.size());
    }
    std::printf("gx_indirect_gpu_test: all passed\n");
    return 0;
}
