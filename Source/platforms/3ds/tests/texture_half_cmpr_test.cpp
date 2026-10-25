// Host driver for an independent Python CMPR/opacity/Morton/hash oracle.
#include "../runtime/texture_gx.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace dusk::native;
using Bytes=std::vector<std::uint8_t>;
static Bytes fixture(){
    Bytes b(32+(32*32+16*16+8*8)/2);b[0]=14;b[3]=b[5]=32;b[24]=3;b[31]=32;
    for(unsigned at=32;at<b.size();at+=8){b[at]=0xf8;b[at+3]=31;std::fill_n(b.begin()+at+4,4,0xff);}
    return b;
}
static void refusals(){
    auto b=fixture();NativeTexturePlan p;assert(inspectNativeTexture(b,p,false,true));
    assert(p.dataOffset==544&&p.sourceBytes==160&&p.outputBytes==1280&&p.format==NativeTextureFormat::rgba8);
    Bytes guarded(p.outputBytes+18,0x69);auto out=std::span(guarded).subspan(9,p.outputBytes);const auto saved=guarded;
    for(std::size_t n=0;n<b.size();++n){
        NativeTexturePlan bad;NativeTextureFingerprint digest;digest.fill(9);
        assert(!inspectNativeTexture(std::span(b).first(n),bad,false,true)&&bad==NativeTexturePlan{});
        assert(!nativeTextureFingerprint(std::span(b).first(n),digest)&&digest==NativeTextureFingerprint{});
        assert(!decodeNativeTexture(std::span(b).first(n),p,out)&&guarded==saved);
    }
    for(unsigned field=0;field<9;++field){auto bad=p;
        switch(field){case 0:bad.sourceMip=0;break;case 1:bad.sourceMip=2;break;case 2:++bad.originalWidth;break;
        case 3:--bad.originalMipCount;break;case 4:++bad.dataOffset;break;case 5:--bad.sourceBytes;break;
        case 6:--bad.outputBytes;break;case 7:bad.format=NativeTextureFormat::rgb565;break;case 8:bad.gxFormat=4;break;}
        assert(!decodeNativeTexture(b,bad,out)&&guarded==saved);
    }
    assert(!decodeNativeTexture(b,p,out.first(p.outputBytes-1))&&guarded==saved);
    Bytes overlap(6000);std::copy(b.begin(),b.end(),overlap.begin()+1500);auto before=overlap;
    for(unsigned at:{1499u,1500u,1532u,2044u})
        assert(!decodeNativeTexture(std::span(overlap).subspan(1500,b.size()),p,std::span(overlap).subspan(at,p.outputBytes))&&overlap==before);
    // Every sub-block, selector position and mip: alpha in discarded L0 or
    // either retained mip must reject before a single destination write.
    for(unsigned level=0,offset=32;level<3;offset+=(32u>>level)*(32u>>level)/2,++level)
        for(unsigned sub=0;sub<4;++sub)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){
            auto bad=b;const auto at=offset+sub*8;bad[at]=bad[at+1]=bad[at+2]=bad[at+3]=0;
            std::fill_n(bad.begin()+at+4,4,0);bad[at+4+y]=3u<<(6-2*x);
            NativeTexturePlan q;assert(inspectNativeTexture(bad,q));
            assert(!inspectNativeTexture(bad,q,false,true)&&q==NativeTexturePlan{});
            assert(!decodeNativeTexture(bad,p,out)&&guarded==saved);
        }
    for(unsigned field:{0u,3u,5u,8u,24u,31u}){
        auto bad=b;bad[field]=field==0?6:field==24?1:field==31?31:field==8?1:8;
        NativeTexturePlan q;assert(!inspectNativeTexture(bad,q,false,true)&&q==NativeTexturePlan{});
        assert(!decodeNativeTexture(bad,p,out)&&guarded==saved);
    }
    assert(decodeNativeTexture(b,p,out));
    for(unsigned n=0;n<9;++n)assert(guarded[n]==0x69&&guarded[guarded.size()-1-n]==0x69);
    std::puts("PASS CMPR every prefix, mip/subblock/selector alpha refusal, stale plans, overlap, guards and atomic output");
}
int main(int argc,char** argv){
    if(argc==1){refusals();return 0;}
    if(argc==3){
        // Private actual-source oracle mode: pipe bytes in/out in memory only.
        // No extracted asset/decoded image is written to disk by this mode.
        Bytes b;std::uint8_t block[4096];std::size_t n;
        while((n=std::fread(block,1,sizeof(block),stdin))){
            assert(b.size()+n<16*1024*1024);b.insert(b.end(),block,block+n);
        }
        assert(!std::ferror(stdin));const auto original=b;NativeTexturePlan p;
        assert(inspectNativeTexture(b,p,false,argv[2][0]=='1'));
        Bytes out(p.outputBytes);assert(decodeNativeTexture(b,p,out)&&b==original);
        assert(std::fwrite(out.data(),1,out.size(),stdout)==out.size());return 0;
    }
    assert(argc==5);
    auto* f=std::fopen(argv[1],"rb");assert(f);assert(!std::fseek(f,0,SEEK_END));auto size=std::ftell(f);
    assert(size>=0&&size<16*1024*1024);std::rewind(f);Bytes b(size);
    assert(std::fread(b.data(),1,b.size(),f)==b.size());assert(!std::fclose(f));const auto original=b;
    NativeTexturePlan p;NativeTextureFingerprint digest;
    const bool accepted=inspectNativeTexture(b,p,argv[3][0]=='1',argv[4][0]=='1');
    const bool hashed=nativeTextureFingerprint(b,digest);
    Bytes out(accepted?p.outputBytes:17,0x69);auto saved=out;
    if(accepted)assert(decodeNativeTexture(b,p,out));else assert(!decodeNativeTexture(b,p,out)&&out==saved);
    assert(b==original);f=std::fopen(argv[2],"wb");assert(f);
    assert(std::fwrite(out.data(),1,out.size(),f)==out.size());assert(!std::fclose(f));
    std::printf("{\"accepted\":%s,\"hashed\":%s,\"width\":%u,\"height\":%u,\"mips\":%u,\"source_mip\":%u,\"bytes\":%zu,\"fingerprint\":\"",
        accepted?"true":"false",hashed?"true":"false",p.width,p.height,p.mipCount,p.sourceMip,p.outputBytes);
    for(auto v:digest)std::printf("%02x",unsigned(v));std::puts("\"}");
}
