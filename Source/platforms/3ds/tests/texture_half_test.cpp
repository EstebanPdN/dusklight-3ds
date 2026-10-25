// Host independent oracle driver plus atomic-refusal checks. Python constructs
// GX words, SHA-256 and PICA addresses independently; this driver uses runtime.
#include "../runtime/texture_gx.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace dusk::native;
using Bytes=std::vector<std::uint8_t>;
static Bytes fixture(){
    Bytes b(32+(32*32+16*16+8*8)*2);b[0]=4;b[3]=b[5]=32;b[24]=3;b[31]=32;
    for(unsigned n=32;n<b.size();++n)b[n]=n%251;
    return b;
}
static void refusals(){
    auto b=fixture();NativeTexturePlan p;assert(inspectNativeTexture(b,p,false,true));
    assert(p.width==16&&p.height==16&&p.originalWidth==32&&p.originalHeight==32);
    assert(p.mipCount==2&&p.originalMipCount==3&&p.sourceMip==1&&p.dataOffset==2080&&p.outputBytes==640);
    Bytes guarded(p.outputBytes+18,0x69);auto out=std::span(guarded).subspan(9,p.outputBytes);
    const auto saved=guarded;
    for(std::size_t n=0;n<b.size();++n){
        NativeTexturePlan bad;
        assert(!inspectNativeTexture(std::span(b).first(n),bad,false,true));
        assert(!decodeNativeTexture(std::span(b).first(n),p,out)&&guarded==saved);
    }
    for(unsigned field=0;field<8;++field){auto bad=p;
        switch(field){case 0:bad.sourceMip=0;break;case 1:bad.sourceMip=2;break;
        case 2:++bad.originalWidth;break;case 3:--bad.originalMipCount;break;
        case 4:++bad.dataOffset;break;case 5:--bad.sourceBytes;break;
        case 6:--bad.outputBytes;break;case 7:bad.format=NativeTextureFormat::la8;break;}
        assert(!decodeNativeTexture(b,bad,out)&&guarded==saved);
    }
    assert(!decodeNativeTexture(b,p,out.first(p.outputBytes-1))&&guarded==saved);
    for(unsigned shift:{0u,7u,32u,2100u}){
        Bytes overlap(b.size()+shift+p.outputBytes);std::copy(b.begin(),b.end(),overlap.begin()+shift);
        const auto before=overlap;
        assert(!decodeNativeTexture(std::span(overlap).subspan(shift,b.size()),p,
                                   std::span(overlap).subspan(shift+32,p.outputBytes))&&overlap==before);
    }
    assert(decodeNativeTexture(b,p,out));
    for(unsigned n=0;n<9;++n)assert(guarded[n]==0x69&&guarded[guarded.size()-1-n]==0x69);
    // Other storage classes, including alpha, palettes and single-level
    // render-copy-like BTIs, fails half inspection before output mutation.
    for(unsigned format:{0u,1u,2u,3u,5u,6u,8u,9u,10u}){
        auto bad=b;bad[0]=format;NativeTexturePlan q;assert(!inspectNativeTexture(bad,q,false,true));
    }
    for(unsigned field:{1u,3u,5u,24u}){auto bad=b;bad[field]=field==1?1:field==24?1:8;
        NativeTexturePlan q;assert(!inspectNativeTexture(bad,q,false,true));}
    auto bad=b;bad[24]=4;NativeTexturePlan q;assert(!inspectNativeTexture(bad,q,false,true));
    NativeTexturePlan full;assert(inspectNativeTexture(b,full)&&full.sourceMip==0&&full.width==32&&full.mipCount==3);
    NativeTextureFingerprint digest,other;assert(nativeTextureFingerprint(b,digest));
    // Sampler settings, header alpha flag and rebased data offset are excluded
    // from source identity. Alpha flag still separately disqualifies admission.
    auto alias=b;alias[6]=2;alias[20]=5;alias[1]=1;
    alias.insert(alias.begin()+32,7,0);alias[31]=39;
    assert(nativeTextureFingerprint(alias,other)&&digest==other);
    alias[39]^=1;assert(nativeTextureFingerprint(alias,other)&&digest!=other);
    assert(!nativeTextureFingerprint(std::span(b).first(31),other));
    std::puts("PASS half mip validation, stale plans, every input prefix, bounds/alias atomicity, protected format controls and canonical source identity");
}
int main(int argc,char** argv){
    if(argc==1){refusals();return 0;}
    assert(argc==4);auto* f=std::fopen(argv[1],"rb");assert(f);
    assert(!std::fseek(f,0,SEEK_END));const auto size=std::ftell(f);assert(size>=32&&size<16*1024*1024);std::rewind(f);
    Bytes b(size);assert(std::fread(b.data(),1,b.size(),f)==b.size());assert(!std::fclose(f));
    NativeTexturePlan p;NativeTextureFingerprint digest;
    assert(inspectNativeTexture(b,p,argv[3][0]=='1',true)&&nativeTextureFingerprint(b,digest));
    const auto original=b;Bytes out(p.outputBytes);assert(decodeNativeTexture(b,p,out)&&b==original);
    f=std::fopen(argv[2],"wb");assert(f);assert(std::fwrite(out.data(),1,out.size(),f)==out.size());assert(!std::fclose(f));
    std::printf("{\"width\":%u,\"height\":%u,\"mips\":%u,\"original_width\":%u,\"original_height\":%u,\"original_mips\":%u,\"source_mip\":%u,\"bytes\":%zu,\"fingerprint\":\"",
                p.width,p.height,p.mipCount,p.originalWidth,p.originalHeight,p.originalMipCount,p.sourceMip,p.outputBytes);
    for(auto v:digest)std::printf("%02x",unsigned(v));std::puts("\"}");return 0;
}
