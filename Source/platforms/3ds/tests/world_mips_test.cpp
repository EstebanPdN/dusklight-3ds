// worldMips host test (Tools/3ds/test_world_mips.py): generated levels against
// an independent box average, alpha-tested coverage kept per level where a
// plain average loses it, the refusals, and the register rules that decide
// which draws may sample generated levels.
#include "../runtime/texture_gx.hpp"
#include "../runtime/gx_world_mips.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dusk::native;
using u8=std::uint8_t;
namespace {
std::vector<u8> header(unsigned format,unsigned w,unsigned h,std::size_t payload){
    std::vector<u8> b(32+payload,0);
    b[0]=u8(format);b[2]=u8(w>>8);b[3]=u8(w);b[4]=u8(h>>8);b[5]=u8(h);b[20]=b[21]=1;b[24]=1;b[31]=32;
    return b;
}
// GX RGB565 (4x4 blocks of big-endian words) from a function of (x,y).
template<class F> std::vector<u8> rgb565(unsigned w,unsigned h,F f){
    auto b=header(4,w,h,std::size_t(w)*h*2);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned v=f(x,y),at=32+((y/4)*(w/4)+x/4)*32+((y%4)*4+x%4)*2;
        b[at]=u8(v>>8);b[at+1]=u8(v);
    }
    return b;
}
// GX IA8 (4x4 blocks of alpha, intensity) from functions of (x,y).
template<class A> std::vector<u8> ia8(unsigned w,unsigned h,A alpha){
    auto b=header(3,w,h,std::size_t(w)*h*2);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned at=32+((y/4)*(w/4)+x/4)*32+((y%4)*4+x%4)*2;
        b[at]=u8(alpha(x,y));b[at+1]=u8((x*7+y*3)&255);
    }
    return b;
}
// GX CMPR in 3-colour mode: a texel is opaque green or transparent (index 3).
template<class O> std::vector<u8> cmpr(unsigned w,unsigned h,O opaque){
    auto b=header(14,w,h,std::size_t(w)*h/2);
    for(unsigned ty=0;ty<h;ty+=8)for(unsigned tx=0;tx<w;tx+=8)for(unsigned sub=0;sub<4;++sub){
        u8* block=b.data()+32+((ty/8)*(w/8)+tx/8)*32+sub*8;
        block[0]=0x07;block[1]=0xE0;block[2]=0x07;block[3]=0xE0; // equal endpoints: 3-colour mode
        for(unsigned y=0;y<4;++y){
            u8 row=0;
            for(unsigned x=0;x<4;++x)row|=u8((opaque(tx+(sub%2)*4+x,ty+(sub/2)*4+y)?0:3)<<(6-2*x));
            block[4+y]=row;
        }
    }
    return b;
}
unsigned picaTexel(unsigned x,unsigned y,unsigned width){
    return ((y/8)*(width/8)+x/8)*64+((x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2)|((x&4)<<2)|((y&4)<<3));
}
struct Levels {NativeTexturePlan plan;std::vector<u8> bytes;std::vector<std::size_t> offset;};
Levels make(const std::vector<u8>& b,u8 reference){
    Levels l;
    assert(inspectNativeTexture(b,l.plan,false,false,true));
    const auto plain=l.plan;
    std::vector<u8> base(plain.outputBytes);
    assert(decodeNativeTexture(b,plain,base));
    assert(planGeneratedMips(l.plan,reference)&&nativeTextureGeneratedMips(l.plan));
    l.bytes.assign(l.plan.outputBytes,0xCD);
    assert(decodeNativeTexture(b,l.plan,l.bytes));
    // The base level is the one a plain decode writes.
    assert(std::equal(base.begin(),base.end(),l.bytes.begin()));
    std::size_t at=0;
    for(unsigned n=0;n<l.plan.mipCount;++n){l.offset.push_back(at);at+=std::size_t(l.plan.width>>n)*(l.plan.height>>n)*nativeTextureBytesPerPixel(l.plan.format);}
    assert(at==l.plan.outputBytes&&nativeTextureGeneratedBytes(l.plan)==at-base.size());
    return l;
}
unsigned word(const Levels& l,unsigned level,unsigned x,unsigned y){
    const auto* t=l.bytes.data()+l.offset[level]+picaTexel(x,y,l.plan.width>>level)*2;
    return unsigned(t[0])|(unsigned(t[1])<<8);
}
void colourLevels(){
    // Every level against the box average of its whole base footprint.
    const auto b=rgb565(64,32,[](unsigned x,unsigned y){return ((x*13+y*7)%32)<<11|((x*x+y)%64)<<5|((x^y)%32);});
    const auto l=make(b,0);
    assert(l.plan.mipCount==3); // 64x32, 32x16, 16x8: the last level 8 high
    unsigned off=0,checked=0;
    for(unsigned n=1;n<l.plan.mipCount;++n){
        const unsigned w=64>>n,h=32>>n,s=1u<<n;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            double sum[3]{};
            for(unsigned v=0;v<s;++v)for(unsigned u=0;u<s;++u){
                const unsigned c=word(l,0,x*s+u,y*s+v),r=c>>11,g=(c>>5)&63,bl=c&31;
                sum[0]+=(r<<3)|(r>>2);sum[1]+=(g<<2)|(g>>4);sum[2]+=(bl<<3)|(bl>>2);
            }
            const unsigned c=word(l,n,x,y);
            const unsigned got[3]{c>>11,(c>>5)&63,c&31},top[3]{31,63,31};
            for(unsigned k=0;k<3;++k){
                const long want=std::lround(sum[k]/(s*s)*top[k]/255.0);
                assert(std::labs(long(got[k])-want)<=1);off+=got[k]!=unsigned(want);++checked;
            }
        }
    }
    assert(off*100<checked);
    std::printf("colour: %u channel values in 2 levels, %u one step off the exact box average (rounding)\n",checked,off);
}
double coverage1(const Levels& l,unsigned level){
    const unsigned w=l.plan.width>>level,h=l.plan.height>>level;unsigned on=0;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)on+=word(l,level,x,y)&1;
    return double(on)/(w*h);
}
void fenceCoverage(){
    // A fence: 1-texel posts every 4 columns, 25 % coverage (CMPR -> RGBA5551).
    const auto b=cmpr(64,64,[](unsigned x,unsigned){return x%4==0;});
    const auto l=make(b,0);
    assert(l.plan.format==NativeTextureFormat::rgba5551&&l.plan.mipCount==4);
    std::printf("fence 25%%, 1-bit alpha, coverage per level:");
    for(unsigned n=0;n<4;++n){const double c=coverage1(l,n);std::printf(" %.3f",c);assert(std::fabs(c-0.25)<0.02);}
    // A plain average thresholded at one half (what a box filter alone gives
    // a 1-bit format): the posts double, then vanish.
    std::printf("; a plain box at 1/2 keeps:");
    for(unsigned n=0;n<4;++n){
        const unsigned s=1u<<n,w=64>>n;unsigned on=0;
        for(unsigned y=0;y<w;++y)for(unsigned x=0;x<w;++x){
            unsigned opaque=0;for(unsigned v=0;v<s;++v)for(unsigned u=0;u<s;++u)opaque+=word(l,0,x*s+u,y*s+v)&1;
            on+=2*opaque>=s*s;
        }
        std::printf(" %.3f",double(on)/(w*w));
    }
    std::printf("\n");
    // Opaque colour is not darkened by the transparent black texels around it.
    for(unsigned n=1;n<4;++n)for(unsigned y=0;y<(64u>>n);++y)for(unsigned x=0;x<(64u>>n);++x){
        const unsigned c=word(l,n,x,y);if(c&1)assert((c>>6&31)==31&&!(c>>11)&&!((c>>1)&31));
    }
}
void testedCoverage(){
    // Multi-bit alpha with an alpha test at 128: posts every 4 columns.
    const auto b=ia8(64,64,[](unsigned x,unsigned){return x%4==0?255u:0u;});
    const auto l=make(b,128);
    assert(l.plan.format==NativeTextureFormat::la8&&l.plan.mipAlphaReference==128);
    std::printf("fence 25%%, LA8 tested at 128, passing per level:");
    for(unsigned n=0;n<4;++n){
        const unsigned w=64>>n;unsigned on=0;
        for(unsigned y=0;y<w;++y)for(unsigned x=0;x<w;++x)on+=l.bytes[l.offset[n]+picaTexel(x,y,w)*2]>=128;
        const double c=double(on)/(w*w);std::printf(" %.3f",c);assert(std::fabs(c-0.25)<0.02);
    }
    std::printf("\n");
    // Untested (reference 0): the plain average, 0.25 * 255 everywhere from level 2.
    const auto plain=make(b,0);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)assert(plain.bytes[plain.offset[2]+picaTexel(x,y,16)*2]==64);
}
void refusals(){
    NativeTexturePlan p;
    auto check=[&](const std::vector<u8>& b){assert(inspectNativeTexture(b,p,false,false,true));const auto before=p;
        assert(nativeTextureChainLevels(p)==1&&!planGeneratedMips(p,0)&&p==before);};
    check(rgb565(24,16,[](unsigned,unsigned){return 0u;}));   // not a power of two (clamped padding)
    check(rgb565(8,64,[](unsigned,unsigned){return 0u;}));    // a level would be under 8 wide
    auto authored=rgb565(32,32,[](unsigned,unsigned){return 0u;});
    authored.resize(32+(32*32+16*16)*2);authored[24]=2;check(authored); // authored levels kept as they are
    // A plan altered after planning is refused by the decoder.
    auto b=rgb565(16,16,[](unsigned x,unsigned){return x;});
    assert(inspectNativeTexture(b,p,false,false,true)&&planGeneratedMips(p,0));
    std::vector<u8> out(p.outputBytes+64);auto bad=p;bad.outputBytes+=2;
    assert(!decodeNativeTexture(b,bad,out));
    bad=p;bad.mipCount=3;assert(!decodeNativeTexture(b,bad,out));
    assert(decodeNativeTexture(b,p,out));
    std::printf("refusals: padded, 8-wide, authored chains, altered plans\n");
}
void registerRules(){
    using namespace gx;
    std::uint8_t r{};
    auto compare=[](unsigned f0,unsigned r0,unsigned op,unsigned f1,unsigned r1){return std::uint32_t(r0|(r1<<8)|(f0<<16)|(f1<<19)|(op<<22));};
    assert(alphaThreshold(compare(6,128,0,7,0),r)&&r==128);   // GEQUAL 128 AND ALWAYS
    assert(alphaThreshold(compare(4,127,1,0,0),r)&&r==128);   // GREATER 127 OR NEVER
    assert(alphaThreshold(compare(7,0,0,7,0),r)&&r==0);       // ALWAYS
    assert(alphaThreshold(compare(6,64,0,6,100),r)&&r==100);  // both lower bounds
    assert(!alphaThreshold(compare(1,128,0,7,0),r));          // LESS: not a lower threshold
    assert(!alphaThreshold(compare(4,0,0,1,200),r));          // a band
    assert(!alphaThreshold(compare(0,0,0,0,0),r));            // NEVER
    CommandRegisters q{};
    q.xf[0x1026]=0;q.xfValid[0x1026]=true;
    q.bp[0x00]=(1u<<10)|1;q.bpValid[0x00]=true; // 2 stages, 1 texgen
    for(unsigned s=0;s<16;++s){q.bp[0x10+s]=0;q.bpValid[0x10+s]=true;}
    q.xf[0x1040]=5u<<7;q.xf[0x1050]=61;q.xfValid[0x1040]=q.xfValid[0x1050]=true; // the game's own: gen=280 post=3d
    assert(worldMipDraw(q,true)&&worldMipCoordinate(q,0));
    assert(!worldMipDraw(q,false));                              // immediate draw (2D, particles)
    auto o=q;o.xf[0x1026]=1;assert(!worldMipDraw(o,true));       // orthographic
    o=q;o.bp[0x00]|=1u<<16;assert(!worldMipDraw(o,true));        // an indirect stage
    o=q;o.bp[0x11]=1u<<9;assert(!worldMipDraw(o,true));          // an indirect command on stage 1
    o=q;o.xf[0x1040]|=2;assert(!worldMipCoordinate(o,0));        // projective 3x4
    o=q;o.xf[0x1040]=1u<<7;assert(!worldMipCoordinate(o,0));     // from the normal: environment
    o=q;o.xf[0x1040]=(2u<<4)|(5u<<7);assert(!worldMipCoordinate(o,0)); // colour-channel ramp
    o=q;o.xf[0x1040]=(1u<<4)|(5u<<7);assert(!worldMipCoordinate(o,0)); // emboss
    o=q;o.xf[0x1050]=30;assert(!worldMipCoordinate(o,0));        // a post matrix
    o=q;o.xf[0x1050]=61|(1u<<8);assert(!worldMipCoordinate(o,0)); // normalized
    assert(!worldMipCoordinate(q,1));                            // a texgen never written
    q.bp[0xf3]=compare(6,128,0,7,0);q.bpValid[0xf3]=true;
    const auto use=worldMipUse(q,true,0);
    assert(use.world&&use.thresholdKnown&&use.reference==128);
    assert(!worldMipUse(q,false,0).world);
    std::printf("register rules: alpha thresholds, draw and coordinate refusals\n");
}
}
int main(){
    colourLevels();fenceCoverage();testedCoverage();refusals();registerRules();
    std::printf("world mips: all checks passed\n");
}
