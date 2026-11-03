// Mac-only asset compiler adapter. This executable is never linked on 3DS.
// The existing bounded source decoder is reused offline; emitted bytes contain
// only native PICA image storage, with no source header/register state.
#include "../../Source/platforms/3ds/runtime/texture_gx.hpp"
#include <cstdio>
#include <vector>
#include <cstdint>
#include <bit>
static_assert(std::endian::native==std::endian::little);
int main(int argc,char** argv){
    if(argc!=3)return 2;
    FILE* input=std::fopen(argv[1],"rb");if(!input)return 3;
    std::fseek(input,0,SEEK_END);const auto size=std::ftell(input);std::rewind(input);
    if(size<32||size>16*1024*1024){std::fclose(input);return 4;}
    std::vector<std::uint8_t> source(size);
    const bool read=std::fread(source.data(),1,source.size(),input)==source.size();std::fclose(input);if(!read)return 5;
    dusk::native::NativeTexturePlan plan;
    if(!dusk::native::inspectNativeTexture(source,plan)||plan.width!=plan.paddedWidth||plan.height!=plan.paddedHeight)return 6;
    std::vector<std::uint8_t> decoded(plan.outputBytes);
    if(!dusk::native::decodeNativeTexture(source,plan,decoded))return 7;
    const auto bpp=dusk::native::nativeTextureBytesPerPixel(plan.format);
    const bool packed=plan.format==dusk::native::NativeTextureFormat::rgb565;
    std::vector<std::uint8_t> output;
    if(packed||bpp==4)output=std::move(decoded);
    else {
        output.resize(decoded.size()/bpp*4);
        for(std::size_t n=0;n<decoded.size()/bpp;++n){
            const auto intensity=bpp==2?decoded[n*2+1]:std::uint8_t((decoded[n]>>4)*17);
            const auto alpha=bpp==2?decoded[n*2]:std::uint8_t((decoded[n]&15)*17);
            output[n*4]=alpha;output[n*4+1]=output[n*4+2]=output[n*4+3]=intensity;
        }
    }
    std::uint32_t kind=packed?1u:0u;
    if(plan.gxFormat==14){
        bool opaque=true;
        for(std::size_t n=0;n<output.size();n+=4)opaque&=output[n]==255;
        kind=opaque?1u:2u;
        std::vector<std::uint8_t> words(output.size()/2);
        for(std::size_t n=0;n<output.size()/4;++n){
            const auto* p=&output[n*4]; // Native RGBA8 bytes are A, B, G, R.
            const unsigned r=(unsigned(p[3])*31+127)/255;
            const unsigned g=(unsigned(p[2])*(opaque?63:31)+127)/255;
            const unsigned b=(unsigned(p[1])*31+127)/255;
            const unsigned value=opaque?(r<<11)|(g<<5)|b:(r<<11)|(g<<6)|(b<<1)|(p[0]!=0);
            words[n*2]=value&255;words[n*2+1]=value>>8;
        }
        output=std::move(words);
    }
    // Little-endian host is explicit, including this private intermediate.
    const std::uint32_t header[]={0x31474d49,plan.width,plan.height,kind,plan.mipCount,std::uint32_t(output.size())};
    FILE* target=std::fopen(argv[2],"wbx");if(!target)return 8;
    const bool written=std::fwrite(header,1,sizeof(header),target)==sizeof(header)&&std::fwrite(output.data(),1,output.size(),target)==output.size();
    return std::fclose(target)||!written?9:0;
}
