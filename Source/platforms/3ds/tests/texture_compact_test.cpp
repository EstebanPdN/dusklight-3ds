// Alpha 12 compact texture storage: every 16-bit plan must hold the picture
// the RGBA8 plan holds. RGB5A3 (all opaque) and palettes exactly; CMPR within
// one 5/6-bit step per channel with exact alpha. Mixed-alpha RGB5A3 and short
// palettes must stay RGBA8.
#include "../runtime/texture_gx.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dusk::native;
using Bytes=std::vector<std::uint8_t>;
static void put16(Bytes& b,size_t at,unsigned v){b[at]=std::uint8_t(v>>8);b[at+1]=std::uint8_t(v);}
static void put32(Bytes& b,size_t at,unsigned v){put16(b,at,v>>16);put16(b,at+2,v&0xffff);}
static Bytes header(unsigned format,unsigned w,unsigned h,size_t payload,unsigned mips=1){
    Bytes b(32+payload);b[0]=std::uint8_t(format);put16(b,2,w);put16(b,4,h);b[24]=std::uint8_t(mips);put32(b,28,32);return b;
}
// One texel of a decoded plan as RGBA8 (bit replication, as the PICA samples it).
static void texel(const NativeTexturePlan& p,const Bytes& out,unsigned x,unsigned y,unsigned rgba[4]){
    unsigned m=0;for(unsigned bit=0;bit<3;++bit)m|=((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1);
    const size_t at=((y/8)*(p.paddedWidth/8)+x/8)*64+m;
    auto e5=[](unsigned v){return (v<<3)|(v>>2);};auto e6=[](unsigned v){return (v<<2)|(v>>4);};
    switch(p.format){
    case NativeTextureFormat::rgba8:rgba[3]=out[at*4];rgba[2]=out[at*4+1];rgba[1]=out[at*4+2];rgba[0]=out[at*4+3];break;
    case NativeTextureFormat::la8:rgba[3]=out[at*2];rgba[0]=rgba[1]=rgba[2]=out[at*2+1];break;
    case NativeTextureFormat::rgb565:{const unsigned v=out[at*2]|(out[at*2+1]<<8);rgba[0]=e5(v>>11);rgba[1]=e6((v>>5)&63);rgba[2]=e5(v&31);rgba[3]=255;break;}
    case NativeTextureFormat::rgba5551:{const unsigned v=out[at*2]|(out[at*2+1]<<8);rgba[0]=e5(v>>11);rgba[1]=e5((v>>6)&31);rgba[2]=e5((v>>1)&31);rgba[3]=(v&1)?255:0;break;}
    default:assert(false);
    }
}
static int failures=0;
static void compare(const char* name,const Bytes& bti,NativeTextureFormat expected,unsigned tolerance){
    NativeTexturePlan full,compact;
    assert(inspectNativeTexture(bti,full,false,false,false));
    assert(inspectNativeTexture(bti,compact,false,false,true));
    if(compact.format!=expected){std::printf("FAIL %s: format %u expected %u\n",name,unsigned(compact.format),unsigned(expected));++failures;return;}
    Bytes a(full.outputBytes),c(compact.outputBytes);
    assert(decodeNativeTexture(bti,full,a));assert(decodeNativeTexture(bti,compact,c));
    unsigned worst=0,alphaErrors=0;
    for(unsigned y=0;y<full.height;++y)for(unsigned x=0;x<full.width;++x){
        unsigned r1[4],r2[4];texel(full,a,x,y,r1);texel(compact,c,x,y,r2);
        for(unsigned k=0;k<3;++k){const unsigned d=r1[k]>r2[k]?r1[k]-r2[k]:r2[k]-r1[k];if(d>worst)worst=d;}
        alphaErrors+=r1[3]!=r2[3];
    }
    const bool ok=worst<=tolerance&&!alphaErrors;
    std::printf("%s %s: %ux%u %zu -> %zu bytes, largest colour difference %u, alpha differences %u\n",ok?"ok  ":"FAIL",name,
        full.width,full.height,full.outputBytes,compact.outputBytes,worst,alphaErrors);
    failures+=!ok;
}
int main(){
    std::mt19937 rng(12);
    {   // RGB5A3, every texel opaque: RGBA5551, exact.
        auto b=header(5,64,32,64*32*2);
        for(size_t n=0;n<64*32;++n)put16(b,32+2*n,0x8000|(rng()&0x7fff));
        compare("rgb5a3 opaque",b,NativeTextureFormat::rgba5551,0);
        b[32]&=0x7f; // one translucent texel: must stay RGBA8
        compare("rgb5a3 mixed",b,NativeTextureFormat::rgba8,0);
    }
    for(int transparent=0;transparent<2;++transparent){   // CMPR
        auto b=header(14,64,64,64*64/2);
        for(size_t block=0;block<64*64/16;++block){
            unsigned c0=rng()&0xffff,c1=rng()&0xffff;
            if(!transparent&&c0<=c1)std::swap(c0,c1);
            if(!transparent&&c0==c1)c0|=1,c1&=~1u;
            if(transparent&&c0>c1)std::swap(c0,c1);
            put16(b,32+block*8,c0);put16(b,32+block*8+2,c1);
            for(int k=0;k<4;++k)b[32+block*8+4+k]=std::uint8_t(rng());
        }
        compare(transparent?"cmpr punch-through":"cmpr opaque",b,
                transparent?NativeTextureFormat::rgba5551:NativeTextureFormat::rgb565,8);
    }
    for(unsigned paletteFormat=0;paletteFormat<3;++paletteFormat){   // C8 with a full table
        const unsigned w=32,h=32;auto b=header(9,w,h,w*h+512);
        b[8]=1;b[9]=std::uint8_t(paletteFormat);put16(b,10,256);put32(b,12,32+w*h);
        for(size_t n=0;n<w*h;++n)b[32+n]=std::uint8_t(rng());
        for(unsigned n=0;n<256;++n){unsigned v=rng()&0xffff;if(paletteFormat==2)v|=0x8000;put16(b,32+w*h+2*n,v);}
        const NativeTextureFormat expected=paletteFormat==0?NativeTextureFormat::la8:paletteFormat==1?NativeTextureFormat::rgb565:NativeTextureFormat::rgba5551;
        compare(paletteFormat==0?"c8 ia8":paletteFormat==1?"c8 rgb565":"c8 rgb5a3 opaque",b,expected,0);
    }
    {   // C4 with a short table: out-of-table texels are transparent black, stay RGBA8.
        const unsigned w=16,h=16;auto b=header(8,w,h,w*h/2+16);
        b[8]=1;b[9]=1;put16(b,10,8);put32(b,12,32+w*h/2);
        for(size_t n=0;n<w*h/2;++n)b[32+n]=std::uint8_t(rng());
        compare("c4 short table",b,NativeTextureFormat::rgba8,0);
    }
    std::printf("status: %s\n",failures?"fail":"pass");
    return failures?1:0;
}
