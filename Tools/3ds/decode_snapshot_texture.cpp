// Private offline diagnostic decoder using the same bounded GX conversion as the port.
#include "../../Source/platforms/3ds/runtime/texture_gx.hpp"
#include <cstdio>
#include <vector>
int main(int argc,char** argv){
    if(argc!=3)return 2;
    FILE* input=std::fopen(argv[1],"rb");if(!input)return 3;
    std::fseek(input,0,SEEK_END);const auto size=std::ftell(input);std::rewind(input);
    if(size<32||size>16*1024*1024){std::fclose(input);return 4;}
    std::vector<unsigned char> bti(size);
    const bool read=std::fread(bti.data(),1,bti.size(),input)==bti.size();std::fclose(input);if(!read)return 5;
    dusk::native::NativeTexturePlan plan;
    if(!dusk::native::inspectNativeTexture(bti,plan)||plan.outputBytes>16*1024*1024)return 6;
    std::vector<unsigned char> tiled(plan.outputBytes),rgba(plan.width*plan.height*4);
    if(!dusk::native::decodeNativeTexture(bti,plan,tiled))return 7;
    const auto bpp=dusk::native::nativeTextureBytesPerPixel(plan.format);
    if(!bpp)return 7;
    for(unsigned y=0;y<plan.height;++y)for(unsigned x=0;x<plan.width;++x){
        unsigned morton=0;for(unsigned bit=0;bit<3;++bit)morton|=((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1);
        const auto at=(((y/8)*(plan.paddedWidth/8)+x/8)*64+morton)*bpp;
        auto* out=&rgba[(y*plan.width+x)*4];
        if(bpp==4){out[0]=tiled[at+3];out[1]=tiled[at+2];out[2]=tiled[at+1];out[3]=tiled[at];}
        else if(plan.format==dusk::native::NativeTextureFormat::rgb565){
            // Diagnostic RGBA remains the original bit-replicated GX decode,
            // not a claim about normalized/filtered PICA sampling precision.
            const unsigned value=unsigned(tiled[at])|(unsigned(tiled[at+1])<<8);
            const unsigned r=value>>11,g=(value>>5)&63,b=value&31;
            out[0]=(r<<3)|(r>>2);out[1]=(g<<2)|(g>>4);out[2]=(b<<3)|(b>>2);out[3]=255;
        }else{out[0]=out[1]=out[2]=bpp==2?tiled[at+1]:(tiled[at]>>4)*17;out[3]=bpp==2?tiled[at]:(tiled[at]&15)*17;}
    }
    FILE* output=std::fopen(argv[2],"wb");if(!output)return 8;
    const unsigned dimensions[]={plan.width,plan.height};
    const bool written=std::fwrite(dimensions,1,sizeof(dimensions),output)==sizeof(dimensions)&&
        std::fwrite(rgba.data(),1,rgba.size(),output)==rgba.size();
    return std::fclose(output)||!written?9:0;
}
