#include "../runtime/gx_framebuffer.hpp"
#include "../runtime/gx_copy_pixels.hpp"
#include "../runtime/texture_gx.hpp"
#include "../runtime/gx_pica_math.hpp"
#include "../runtime/alpha_interval_target.hpp"
#include "../gx_pica.hpp"
#include "gx_copy_reference.hpp"
#include <cassert>
#include <vector>
#include <array>
#include <cstdio>
#include <cstring>
using namespace dusk::native;
namespace {unsigned created{},binds{},clears{};bool failBind{},failClear{};}
// Isolated framebuffer/pixel mock: no alpha interval or GPU owner is linked.
// Production interval/ownership safety is exercised by its separate host suite.
bool dusk::native::alphaIntervalCopySafe(){assert(!alphaIntervalTargets.enabled&&!alphaIntervalTargets.owner);return true;}
FramebufferHandle dusk::native::platformCreateFramebuffer(unsigned w,unsigned h){assert(!(w&7)&&!(h&7));return reinterpret_cast<void*>(uintptr_t(++created+1));}
bool dusk::native::platformBindFramebuffer(FramebufferHandle h){assert(h);++binds;return !failBind;}
bool dusk::native::platformClearFramebuffer(FramebufferHandle h,uint32_t,uint32_t){assert(h);++clears;return !failClear;}
unsigned morton(unsigned x,unsigned y){unsigned out=0;for(unsigned i=0;i<3;++i){out|=((x>>i)&1)<<(2*i);out|=((y>>i)&1)<<(2*i+1);}return out;}
void depthCopies(){
    // Nonsquare target, cropped source and asymmetric values independently
    // expose transposition, tiled address, depth reversal and byte-order errors.
    gx::TargetExtent e{16,8,16,8};std::array<std::byte,512> input{};
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x){
        const unsigned px=7-y,py=x,at=(py/8)*64+morton(px,py);
        const unsigned expected=(x+y*16)*0x10001;
        const unsigned pica=0x5a000000|(0xffffff-expected);
        std::memcpy(input.data()+at*4,&pica,4);
    }
    ColorCopyPlan p;assert(planDepthCopy(e,8,0,8,8,4,4,0x13,p));
    std::vector<std::uint8_t> output(p.bytes);assert(encodeDepthCopy(p,input,output));
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){
        const unsigned original=(9+2*x+(1+2*y)*16)*0x10001;
        assert(output[(y*4+x)*2]==original>>16&&output[(y*4+x)*2+1]==((original>>8)&255));
    }
    auto bad=p;++bad.bytes;assert(!encodeDepthCopy(bad,input,output));
    assert(!encodeDepthCopy(p,std::span{input}.first(511),output));
    assert(!encodeDepthCopy(p,input,std::span{output}.first(output.size()-1)));
    assert(!encodeDepthCopy(p,input,{reinterpret_cast<std::uint8_t*>(input.data()),p.bytes}));
    assert(!planDepthCopy(e,8,0,9,8,4,4,0x13,p));
    assert(!planDepthCopy(e,8,0,8,8,4,4,0x16,p));
    assert(!planDepthCopy({16,8,15,8},0,0,16,8,4,4,0x13,p));
    assert(planDepthCopy(e,0,0,16,8,5,3,0x13,p));output.resize(p.bytes);assert(encodeDepthCopy(p,input,output));
    assert(output.size()==64); // Unwritten GX tile padding is initialized.
    for(unsigned i=0;i<4;++i){assert(output[32+i*8+2]==0&&output[32+i*8+3]==0);}
    std::puts("Z16 depth copies: nonsquare rotation, crop/downsample, reversed D24, stencil exclusion, endian packing, padding and bounds passed");
}
int main(){
    // Differential against the corrected per-pixel color contract, including uneven
    // crops/scales, large captures, every packed color format and tile padding.
    unsigned compared=0;std::uint32_t seed=0x719eca43;
    auto random=[&](){seed=1664525*seed+1013904223;return seed;};
    for(unsigned n=0;n<64;++n){
        gx::TargetExtent e{608,448,400,240};
        const unsigned left=random()%300,top=random()%200,sw=1+random()%(608-left),sh=1+random()%(448-top);
        const unsigned sizes[]{1,3,4,5,7,8,9,15,16,17,144,192,224,304,608,1024};
        const unsigned width=n<16?sizes[n]:1+random()%89,height=n<16?sizes[15-n]:1+random()%73;
        std::vector<std::byte> input(400*240*4);for(auto& byte:input)byte=std::byte(random()>>24);
        for(unsigned format:{0u,1u,2u,3u,4u,5u,6u,0x28u,0x13u}){
            ColorCopyPlan p;const bool depth=format==0x13;
            assert((depth?planDepthCopy:planColorCopy)(e,left,top,sw,sh,width,height,format,p));
            std::vector<std::uint8_t> actual(p.bytes,0xcd),expected(p.bytes,0xef);
            assert((depth?encodeDepthCopy:encodeColorCopy)(p,input,actual));
            assert((depth?referenceEncodeDepthCopy:referenceEncodeColorCopy)(p,input,expected));
            assert(actual==expected);++compared;
        }
    }
    std::printf("Exact copy comparisons: %u\n",compared);
    depthCopies();
    assert(!beginFramebufferFrame());
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),400,240));
    assert(configureMainFramebuffer(608,448));assert(beginFramebufferFrame());
    assert(!beginFramebufferFrame());assert(gx::currentTargetExtent.logicalWidth==608);
    assert(beginOffscreenFramebuffer(61,29));assert(created==1&&clears==1&&offscreenFramebufferBytes()==64*32*8);
    assert(gx::currentTargetExtent.pixelWidth==64&&gx::currentTargetExtent.pixelHeight==32);
    assert(!endFramebufferFrame());assert(!configureMainFramebuffer(400,240));
    assert(beginOffscreenFramebuffer(61,29)&&created==1&&clears==2);
    assert(restoreMainFramebuffer());assert(!restoreMainFramebuffer());
    assert(deferMainFramebufferClear(0x11223344,0x123456));assert(endFramebufferFrame());
    assert(beginFramebufferFrame()&&clears==2); // Previous depth survives queue drain.
    assert(beginFramebufferRender()&&clears==3);
    assert(beginFramebufferRender()&&clears==3);assert(endFramebufferFrame());
    assert(beginFramebufferFrame());assert(!beginOffscreenFramebuffer(1024,1024));assert(endFramebufferFrame());
    releaseFramebufferRecords();assert(offscreenFramebufferBytes()==0);
    gx::TargetExtent extent{8,8,8,8};
    std::vector<std::byte> rgba(256);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){const unsigned at=(x*8+7-y)*4;rgba[at]=std::byte(64+x*16);rgba[at+1]=std::byte(32+y*16);rgba[at+2]=std::byte(16+x*16);rgba[at+3]=std::byte(32+y*16);}
    for(unsigned format=0;format<=6;++format){
        ColorCopyPlan copy;assert(planColorCopy(extent,0,0,8,8,8,8,format,copy));
        std::vector<uint8_t> bti(32+copy.bytes);bti[0]=format;bti[3]=bti[5]=8;bti[24]=1;bti[31]=32;
        assert(encodeColorCopy(copy,rgba,std::span{bti}.subspan(32)));
        NativeTexturePlan plan;assert(inspectNativeTexture(bti,plan));std::vector<uint8_t> decoded(plan.outputBytes);
        assert(decodeNativeTexture(bti,plan,decoded));
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
            const unsigned alpha=64+x*16,blue=32+y*16,green=16+x*16,red=32+y*16;
            const auto at=morton(x,y);
            if(format==6){assert(decoded[at*4]==alpha&&decoded[at*4+1]==blue&&decoded[at*4+2]==green&&decoded[at*4+3]==red);}
            if(format==4){
                assert(plan.format==NativeTextureFormat::rgb565&&decoded.size()==128);
                const unsigned packed=((red>>3)<<11)|((green>>2)<<5)|(blue>>3);
                assert(decoded[at*2]==(packed&255)&&decoded[at*2+1]==packed>>8);
            }
            if(format==5){const unsigned a=alpha>>5;assert(decoded[at*4]==((a<<5)|(a<<2)|(a>>1)));}
        }
        auto stale=copy;++stale.bytes;assert(!encodeColorCopy(stale,rgba,std::span{bti}.subspan(32)));
        assert(!encodeColorCopy(copy,std::span{rgba}.first(255),std::span{bti}.subspan(32)));
    }
    ColorCopyPlan invalid;assert(!planColorCopy(extent,7,7,2,2,8,8,6,invalid));
    assert(!planColorCopy(extent,0,0,8,8,8,8,7,invalid));
    std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},projection;
    assert(gx::picaProjection(identity,{0,0,61,29,0,1},projection,{61,29,64,32}));
    auto s=gx::picaScissor({0,0,61,29},{61,29,64,32});assert(!s.empty&&s.left==0&&s.top==0&&s.right==32&&s.bottom==64);
    std::puts("Framebuffer transitions/budget, seven copy formats, rotation, bounds and offscreen projection passed");
}
