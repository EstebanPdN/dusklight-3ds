#include "../runtime/texture_gx.hpp"
#include "../runtime/disc.hpp"
#include "../runtime/resource_archive.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
using namespace dusk::native;
using u8=std::uint8_t;using Span=std::span<const u8>;using Bytes=std::vector<u8>;
std::vector<u8> originalTexture(unsigned,unsigned,unsigned,Span);
std::vector<u8> originalPaletteTexture(unsigned,unsigned,unsigned,Span,unsigned,unsigned,Span);
static unsigned half(Span b,size_t at){assert(at+2<=b.size());return (unsigned(b[at])<<8)|b[at+1];}
static unsigned word(Span b,size_t at){assert(at+4<=b.size());return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];}
static size_t pixel(unsigned x,unsigned y,unsigned width){unsigned m=0;for(unsigned bit=0;bit<3;++bit)m|=((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1);return ((y/8)*(width/8)+x/8)*64+m;}
static size_t checkedTexels=0;
static size_t verify(Span b,bool refusal){
    NativeTexturePlan p;assert(inspectNativeTexture(b,p));
    Bytes guarded(p.outputBytes+16,0xd3);auto output=std::span(guarded).subspan(8,p.outputBytes);
    assert(decodeNativeTexture(b,p,output));
    size_t source=p.dataOffset,destination=0;
    for(unsigned level=0;level<p.mipCount;++level){
        const unsigned w=std::max(1u,p.width>>level),h=std::max(1u,p.height>>level);
        const unsigned pw=std::max(8u,p.paddedWidth>>level),ph=std::max(8u,p.paddedHeight>>level);
        const unsigned bw=p.gxFormat>=3&&p.gxFormat<=6?4:8,bh=p.gxFormat==0||p.gxFormat==14?8:4;
        const size_t size=((w+bw-1)/bw)*((h+bh-1)/bh)*(p.gxFormat==6?64:32);
        auto expected=originalTexture(p.gxFormat,w,h,b.subspan(source,size));
        const unsigned bpp=nativeTextureBytesPerPixel(p.format);assert(bpp);
        for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){
            const auto at=destination+pixel(x,y,pw)*bpp;std::array<u8,4> actual;
            if(p.format==NativeTextureFormat::la4){const u8 v=output[at];const u8 i=(v>>4)*17;actual={i,i,i,u8((v&15)*17)};}
            else if(p.format==NativeTextureFormat::la8){const u8 i=output[at+1];actual={i,i,i,output[at]};}
            else if(p.format==NativeTextureFormat::rgb565){
                const unsigned value=unsigned(output[at])|(unsigned(output[at+1])<<8);
                const unsigned r=value>>11,g=(value>>5)&63,b=value&31;
                actual={u8((r<<3)|(r>>2)),u8((g<<2)|(g>>4)),u8((b<<3)|(b>>2)),255};
            }
            else actual={output[at+3],output[at+2],output[at+1],output[at]};
            const auto target=(std::min(y,h-1)*w+std::min(x,w-1))*4;
            for(unsigned c=0;c<4;++c)assert(actual[c]==expected[target+c]);++checkedTexels;
        }
        source+=size;destination+=size_t(pw)*ph*bpp;
    }
    for(unsigned i=0;i<8;++i)assert(guarded[i]==0xd3&&guarded[guarded.size()-1-i]==0xd3);
    if(refusal){
        const auto before=guarded;NativeTexturePlan bad;
        for(size_t n=0;n<p.dataOffset+p.sourceBytes;++n){assert(!inspectNativeTexture(b.first(n),bad));assert(!decodeNativeTexture(b.first(n),p,output));assert(guarded==before);}
        assert(!decodeNativeTexture(b,p,output.first(output.size()-1))&&guarded==before);
        auto changed=p;++changed.outputBytes;assert(!decodeNativeTexture(b,changed,output)&&guarded==before);
        for(unsigned shift:{0u,8u,16u}){
            Bytes overlap(std::max(b.size()+shift,p.outputBytes+8),0);
            std::copy(b.begin(),b.end(),overlap.begin()+shift);const auto saved=overlap;
            assert(!decodeNativeTexture(std::span(overlap).subspan(shift,b.size()),p,std::span(overlap).subspan(8,p.outputBytes))&&overlap==saved);
        }
    }
    return p.outputBytes;
}
static void synthetic(){
    unsigned cases=0;
    for(u8 format:{0,1,2,3,4,5,6,14})for(unsigned w:{1u,7u,8u,9u,17u})for(unsigned h:{1u,5u,8u,13u}){
        const unsigned bw=format>=3&&format<=6?4:8,bh=format==0||format==14?8:4;
        const size_t size=((w+bw-1)/bw)*((h+bh-1)/bh)*(format==6?64:32);
        Bytes input(33+size);input[0]=format;input[3]=w;input[5]=h;input[24]=1;input[31]=33;
        for(size_t i=33;i<input.size();++i)input[i]=(i*73+cases*91)^(i>>2);
        verify(input,true);++cases;
    }
    Bytes input(96,0);input[3]=8;input[5]=8;input[24]=1;input[31]=32;
    NativeTexturePlan p;
    for(auto [offset,value]:{std::pair{0,8},std::pair{8,1},std::pair{11,1},std::pair{24,2},std::pair{31,31},std::pair{2,5}}){auto bad=input;bad[offset]=value;assert(!inspectNativeTexture(bad,p));}
    // CMPR transparent palette entry keeps the endpoint midpoint RGB.
    input[0]=14;input[32]=0;input[33]=0;input[34]=0xff;input[35]=0xff;
    for(unsigned i=36;i<40;++i)input[i]=255;
    verify(input,true);assert(inspectNativeTexture(input,p));Bytes out(p.outputBytes);assert(decodeNativeTexture(input,p,out));
    assert(out[0]==0&&out[1]==127&&out[2]==127&&out[3]==127);
    printf("{\"synthetic_cases\":%u,\"exact_original_rgba_match\":true,\"bounded_refusals\":true}\n",cases+1);
}
static void indexed(){
    unsigned cases{};
    for(unsigned f:{8u,9u,10u})for(unsigned pf=0;pf<3;++pf)
    for(unsigned w:{1u,7u,8u,13u,17u,16u,32u})for(unsigned h:{1u,5u,8u,13u,16u,32u}){
        const unsigned mips=((w==16&&h==32)||(w==32&&h==16))?2:1;
        const unsigned bw=f==10?4:8,bh=f==8?8:4,colors=f==8?13:f==9?251:1001;
        size_t bytes{};for(unsigned n=0;n<mips;++n)bytes+=((std::max(1u,w>>n)+bw-1)/bw)*((std::max(1u,h>>n)+bh-1)/bh)*32;
        Bytes input(35+bytes+colors*2);input[0]=f;input[3]=w;input[5]=h;input[8]=1;input[9]=pf;
        input[10]=colors>>8;input[11]=colors;input[24]=mips;input[31]=35;
        const unsigned pal=35+bytes;for(unsigned n=0;n<4;++n)input[12+n]=pal>>(24-8*n);
        for(size_t n=35;n<input.size();++n)input[n]=u8((n*53+cases*91)^(n>>3));
        NativeTexturePlan p;assert(inspectNativeTexture(input,p));
        Bytes guarded(p.outputBytes+16,0xd3);auto output=std::span(guarded).subspan(8,p.outputBytes);
        assert(decodeNativeTexture(input,p,output));size_t source=35,destination{};
        for(unsigned n=0;n<mips;++n){
            const unsigned lw=std::max(1u,w>>n),lh=std::max(1u,h>>n);
            const unsigned pw=std::max(8u,p.paddedWidth>>n),ph=std::max(8u,p.paddedHeight>>n);
            const size_t size=((lw+bw-1)/bw)*((lh+bh-1)/bh)*32;
            const auto expected=originalPaletteTexture(f,lw,lh,Span(input).subspan(source,size),pf,colors,Span(input).subspan(pal,colors*2));
            for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){
                const auto out=destination+pixel(x,y,pw)*4;
                const auto at=(std::min(y,lh-1)*lw+std::min(x,lw-1))*4;
                for(unsigned c=0;c<4;++c)assert(output[out+3-c]==expected[at+c]);++checkedTexels;
            }
            source+=size;destination+=size_t(pw)*ph*4;
        }
        const auto before=guarded;
        for(unsigned n=1;n<=std::min(8u,colors*2);++n){const auto shortInput=Span(input).first(input.size()-n);
            NativeTexturePlan rejected;assert(!inspectNativeTexture(shortInput,rejected));assert(!decodeNativeTexture(shortInput,p,output)&&guarded==before);}
        auto stale=p;++stale.paletteOffset;assert(!decodeNativeTexture(input,stale,output)&&guarded==before);
        auto bad=input;bad[9]=3;NativeTexturePlan rejected;assert(!inspectNativeTexture(bad,rejected));
        bad=input;for(unsigned n=0;n<4;++n)bad[12+n]=35>>(24-8*n);assert(!inspectNativeTexture(bad,rejected));
        bad=input;bad[8]=0;assert(!inspectNativeTexture(bad,rejected));
        assert(!decodeNativeTexture(input,p,output.first(output.size()-1))&&guarded==before);
        for(unsigned n=0;n<8;++n)assert(guarded[n]==0xd3&&guarded[guarded.size()-1-n]==0xd3);
        ++cases;
    }
    printf("{\"ci_palette_cases\":%u,\"all_three_index_and_palette_formats\":true,\"morton_padding_and_mips\":true,\"exact_pinned_aurora_rgba_match\":true,\"bounded_refusals\":true}\n",cases);
}
static void modelTextures(const char* path,const char* archivePath,const char* modelName){
    Disc disc;assert(disc.open(path)==DiscStatus::ready);DiscFile file;assert(disc.findFile(archivePath,file)==DiscFileStatus::ready);
    constexpr size_t budget=32*1024*1024;assert(file.size()<=budget);Bytes encoded(file.size());assert(disc.readFile(file,0,encoded.data(),encoded.size()));
    std::uint32_t expanded;assert(yaz0ExpandedSize(encoded.data(),encoded.size(),budget,expanded)==ResourceStatus::ready);
    Bytes storage(expanded);size_t written;assert(decodeYaz0(encoded.data(),encoded.size(),storage.data(),storage.size(),budget,written)==ResourceStatus::ready);
    RarcArchive archive;assert(archive.open(storage.data(),written,{budget,4096,65536,255})==ResourceStatus::ready);
    RarcEntry model;assert(archive.findFile(modelName,model)==ResourceStatus::ready);Span bmd(model.contents.data,model.contents.size);
    assert(bmd.size()>=32&&!std::memcmp(bmd.data(),"J3D2bmd3",8));const unsigned blocks=word(bmd,12);assert(blocks<=64);size_t at=32;Span textures;
    for(unsigned i=0;i<blocks;++i){const auto n=word(bmd,at+4);assert(n>=8&&n<=bmd.size()-at);if(!std::memcmp(bmd.data()+at,"TEX1",4))textures=bmd.subspan(at,n);at+=n;}
    assert(!textures.empty());const auto count=half(textures,8),headers=word(textures,12);size_t compact=0,rgba=0;
    for(unsigned t=0;t<count;++t){const auto header=headers+32*t;assert(header<textures.size());auto b=textures.subspan(header);if(!word(b,28))continue;NativeTexturePlan p;if(!inspectNativeTexture(b,p)){fprintf(stderr,"texture %u format %u dimensions %u %u mips %u indexed %u palette %u\n",t,b[0],half(b,2),half(b,4),b[24],b[8],half(b,10));std::abort();}compact+=verify(b,false);for(unsigned m=0;m<p.mipCount;++m)rgba+=size_t(std::max(8u,p.paddedWidth>>m))*std::max(8u,p.paddedHeight>>m)*4;}
    printf("{\"title_textures\":%u,\"compact_bytes\":%zu,\"rgba8_bytes\":%zu,\"saved_bytes\":%zu,\"exact_original_rgba_match\":true,\"payload_saved\":false}\n",count,compact,rgba,rgba-compact);
}
int main(int argc,char** argv){assert(argc==1||argc==2);synthetic();indexed();if(argc==2){modelTextures(argv[1],"/res/Object/Title.arc","titlelogo_r.bmd");modelTextures(argv[1],"/res/Stage/F_SP103/R00_00.arc","model.bmd");}printf("{\"checked_padded_texels\":%zu}\n",checkedTexels);}
