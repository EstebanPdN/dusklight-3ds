// Independent packed-word oracle: source coordinates, GX blocks and PICA Morton
// addresses are computed separately. No native GPU sampling claim.
#include "../runtime/texture_gx.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace dusk::native;
using Bytes=std::vector<std::uint8_t>;
static unsigned padded(unsigned n){unsigned p=8;while(p<n)p*=2;return p;}
static void half(Bytes& b,std::size_t at,unsigned v){b[at]=v>>8;b[at+1]=v;}
static std::size_t pixel(unsigned x,unsigned y,unsigned w){
    unsigned m=0;for(unsigned bit=0;bit<3;++bit)m|=((x>>bit)&1)<<(bit*2)|((y>>bit)&1)<<(bit*2+1);
    return ((y/8)*(w/8)+x/8)*64+m;
}
static unsigned sample(unsigned x,unsigned y,unsigned w,unsigned level){return (y*w+x+level*7919)&65535;}
static Bytes image(unsigned w,unsigned h,unsigned mips){
    std::size_t size=35;for(unsigned n=0;n<mips;++n)size+=((w>>n)+3)/4*(((h>>n)+3)/4)*32;
    Bytes b(size,0xa7);std::fill_n(b.begin(),35,0);b[0]=4;half(b,2,w);half(b,4,h);b[24]=mips;b[31]=35;
    std::size_t cursor=35;
    for(unsigned n=0;n<mips;++n){const unsigned lw=w>>n,lh=h>>n;
        for(unsigned y=0;y<lh;++y)for(unsigned x=0;x<lw;++x){
            const auto at=cursor+((y/4)*((lw+3)/4)+x/4)*32+((y%4)*4+x%4)*2;
            half(b,at,sample(x,y,lw,n));
        }
        cursor+=((lw+3)/4)*((lh+3)/4)*32;
    }
    return b;
}
static unsigned checked{};
static void verify(unsigned w,unsigned h,unsigned mips,bool refusals=false){
    auto b=image(w,h,mips);NativeTexturePlan p;assert(inspectNativeTexture(b,p));
    assert(p.format==NativeTextureFormat::rgb565&&p.gxFormat==4&&nativeTextureBytesPerPixel(p.format)==2);
    std::size_t bytes=0;for(unsigned n=0;n<mips;++n)bytes+=padded(w>>n)*padded(h>>n)*2;
    assert(p.outputBytes==bytes&&p.sourceBytes==b.size()-35);
    Bytes guarded(bytes+18,0x6d);auto out=std::span(guarded).subspan(9,bytes);
    assert(decodeNativeTexture(b,p,out));
    std::size_t cursor=0;
    for(unsigned n=0;n<mips;++n){const unsigned lw=w>>n,lh=h>>n,pw=padded(lw),ph=padded(lh);
        for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){
            const auto value=sample(std::min(x,lw-1),std::min(y,lh-1),lw,n);
            const auto at=cursor+pixel(x,y,pw)*2;
            assert(out[at]==(value&255)&&out[at+1]==value>>8);++checked;
        }
        cursor+=pw*ph*2;
    }
    assert(cursor==bytes);
    NativeTexturePlan reference;assert(inspectNativeTexture(b,reference,true));
    assert(reference.format==NativeTextureFormat::rgba8&&reference.outputBytes==2*bytes);
    Bytes rgba(reference.outputBytes+18,0x6d);auto expanded=std::span(rgba).subspan(9,reference.outputBytes);
    assert(decodeNativeTexture(b,reference,expanded));
    for(std::size_t pixel=0;pixel<bytes/2;++pixel){
        const unsigned value=unsigned(out[2*pixel])|(unsigned(out[2*pixel+1])<<8);
        const unsigned r=value>>11,g=(value>>5)&63,blue=value&31;
        assert(expanded[4*pixel]==255&&expanded[4*pixel+1]==((blue<<3)|(blue>>2))&&
               expanded[4*pixel+2]==((g<<2)|(g>>4))&&expanded[4*pixel+3]==((r<<3)|(r>>2)));
    }
    for(unsigned n=0;n<9;++n)assert(rgba[n]==0x6d&&rgba[rgba.size()-1-n]==0x6d);
    auto wrongReference=reference;wrongReference.outputBytes=bytes;
    assert(!decodeNativeTexture(b,wrongReference,expanded));
    assert(!decodeNativeTexture(b,reference,expanded.first(reference.outputBytes-1)));
    for(unsigned n=0;n<9;++n)assert(guarded[n]==0x6d&&guarded[guarded.size()-1-n]==0x6d);
    if(refusals){
        const auto saved=guarded;
        // Incrementally available input must not mutate output until the full
        // declared chain is present. Test every prefix, including each mip.
        for(std::size_t size=0;size<b.size();++size){
            NativeTexturePlan bad;assert(!inspectNativeTexture(std::span(b).first(size),bad));
            assert(!decodeNativeTexture(std::span(b).first(size),p,out)&&guarded==saved);
        }
        // The API accepts complete bounded spans, not a streaming state. Model
        // a caller accumulating chunks, retrying each prefix, then decoding all
        // mips exactly once when the final chunk completes the declared input.
        for(unsigned chunk:{1u,7u,31u,32u,33u,127u,511u}){
            Bytes assembled,decoded(bytes+19,0x93);
            for(std::size_t at=0;at<b.size();at+=chunk){
                const auto end=std::min(at+chunk,b.size());
                assembled.insert(assembled.end(),b.begin()+at,b.begin()+end);
                const bool complete=end==b.size();
                assert(decodeNativeTexture(assembled,p,decoded)==complete);
                if(!complete)assert(std::all_of(decoded.begin(),decoded.end(),[](auto v){return v==0x93;}));
            }
            assert(std::equal(out.begin(),out.end(),decoded.begin()));
            for(std::size_t at=bytes;at<decoded.size();++at)assert(decoded[at]==0x93);
        }
        for(std::size_t size:{std::size_t(0),bytes/2,bytes-1})assert(!decodeNativeTexture(b,p,out.first(size))&&guarded==saved);
        auto stale=p;stale.format=NativeTextureFormat::la8;assert(!decodeNativeTexture(b,stale,out)&&guarded==saved);
        stale=p;stale.format=static_cast<NativeTextureFormat>(255);assert(!decodeNativeTexture(b,stale,out)&&guarded==saved);
        stale=p;--stale.mipCount;assert(!decodeNativeTexture(b,stale,out)&&guarded==saved);
        for(unsigned shift:{0u,7u,35u}){
            Bytes overlap(std::max(b.size(),bytes)+shift+64,0);
            std::copy(b.begin(),b.end(),overlap.begin()+shift);const auto before=overlap;
            assert(!decodeNativeTexture(std::span(overlap).subspan(shift,b.size()),p,std::span(overlap).subspan(9,bytes))&&overlap==before);
        }
    }
}
int main(int argc,char** argv){
    // Optional private host comparison with an independently emitted lab payload.
    if(argc==3){
        auto* in=std::fopen(argv[1],"rb");assert(in);assert(!std::fseek(in,0,SEEK_END));
        const auto size=std::ftell(in);assert(size>=32&&size<=16*1024*1024);std::rewind(in);
        Bytes b(size);assert(std::fread(b.data(),1,b.size(),in)==b.size());assert(!std::fclose(in));
        NativeTexturePlan p;assert(inspectNativeTexture(b,p)&&p.format==NativeTextureFormat::rgb565);
        Bytes out(p.outputBytes);assert(decodeNativeTexture(b,p,out));
        auto* file=std::fopen(argv[2],"wb");assert(file);assert(std::fwrite(out.data(),1,out.size(),file)==out.size());assert(!std::fclose(file));
        return 0;
    }
    assert(argc==1);
    static_assert(nativeTextureBytesPerPixel(NativeTextureFormat::rgba8)==4);
    static_assert(nativeTextureBytesPerPixel(NativeTextureFormat::la8)==2);
    static_assert(nativeTextureBytesPerPixel(NativeTextureFormat::la4)==1);
    static_assert(nativeTextureBytesPerPixel(static_cast<NativeTextureFormat>(255))==0);
    // All 65536 source words occur once at different coordinates/tile positions.
    verify(256,256,1);
    for(unsigned w:{1u,3u,7u,8u,9u,13u,17u,31u,33u,63u,65u,129u})
        for(unsigned h:{1u,5u,7u,8u,9u,15u,17u,33u})verify(w,h,1);
    verify(13,17,1,true);verify(32,16,2,true);verify(128,64,4,true);
    verify(1024,1024,8);verify(8,1024,1);verify(1024,8,1);
    for(auto dimensions:{std::pair{13u,16u},std::pair{16u,8u},std::pair{4u,4u}}){
        auto b=image(dimensions.first,dimensions.second,2);NativeTexturePlan p;
        assert(!inspectNativeTexture(b,p));
    }
    std::printf("PASS RGB565: all 65536 words, 96 odd/min8 dimension pairs, rectangular/full mip chains, exact LE Morton words, clamped padding, chunked input refusal, output guards/overlap/stale plans (%u padded texels)\n",checked);
}
