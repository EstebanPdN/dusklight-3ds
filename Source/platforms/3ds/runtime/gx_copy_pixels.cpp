#include "gx_copy_pixels.hpp"
#include "gx_profile.hpp"
#include <algorithm>
#include <array>
#include <cstring>
namespace dusk::native {
bool planColorCopy(const gx::TargetExtent& extent,unsigned left,unsigned top,unsigned sw,unsigned sh,
                   unsigned w,unsigned h,unsigned format,ColorCopyPlan& out){
    out={};
    const bool redChannel=format==0x28;if(redChannel)format=1;
    if(!extent.valid()||!sw||!sh||left>=extent.logicalWidth||top>=extent.logicalHeight||
       sw>extent.logicalWidth-left||sh>extent.logicalHeight-top||!w||!h||w>1024||h>1024||format>6)return false;
    const unsigned bw=format<3?8:4,bh=format==0?8:4,tile=format==6?64:32;
    out={extent,left,top,sw,sh,w,h,format,std::size_t((w+bw-1)/bw)*((h+bh-1)/bh)*tile,redChannel};
    return true;
}
namespace {
template<unsigned Format,bool Red=false>
void encodeColorTiles(const ColorCopyPlan& p,std::span<const std::byte> source,std::span<std::uint8_t> out,
                      const std::array<std::uint32_t,1024>& columns,const std::array<std::uint32_t,1024>& rows){
    constexpr unsigned bw=Format<3?8:4,bh=Format==0?8:4,tile=Format==6?64:32;
    // Complete tiles overwrite every byte. Partial edge tiles retain the
    // original zero padding, including the low nibble of odd-width I4 rows.
    if((p.width%bw)||(p.height%bh))std::fill(out.begin(),out.end(),0);
    std::size_t block=0;
    for(unsigned by=0;by<p.height;by+=bh)for(unsigned bx=0;bx<p.width;bx+=bw,block+=tile)
    for(unsigned y=0;y<bh&&by+y<p.height;++y)for(unsigned x=0;x<bw&&bx+x<p.width;++x){
        const auto at=columns[bx+x]+rows[by+y];
        const unsigned alpha=std::to_integer<unsigned>(source[at]),blue=std::to_integer<unsigned>(source[at+1]),
                       green=std::to_integer<unsigned>(source[at+2]),red=std::to_integer<unsigned>(source[at+3]);
        const unsigned intensity=Red?red:(77*red+150*green+29*blue+128)>>8;
        const auto pixel=y*bw+x;
        if constexpr(Format==0){
            if(pixel&1)out[block+pixel/2]|=std::uint8_t(intensity>>4);
            else out[block+pixel/2]=std::uint8_t(intensity&240);
        }
        else if constexpr(Format==1)out[block+pixel]=intensity;
        else if constexpr(Format==2)out[block+pixel]=(alpha&240)|(intensity>>4);
        else if constexpr(Format==3){out[block+pixel*2]=alpha;out[block+pixel*2+1]=intensity;}
        else if constexpr(Format==4||Format==5){
            const unsigned value=Format==4?((red>>3)<<11)|((green>>2)<<5)|(blue>>3):
                alpha>=224?0x8000|((red>>3)<<10)|((green>>3)<<5)|(blue>>3):
                ((alpha>>5)<<12)|((red>>4)<<8)|((green>>4)<<4)|(blue>>4);
            out[block+pixel*2]=value>>8;out[block+pixel*2+1]=value;
        }else{out[block+pixel*2]=alpha;out[block+pixel*2+1]=red;out[block+32+pixel*2]=green;out[block+33+pixel*2]=blue;}
    }
 }
}
bool encodeColorCopy(const ColorCopyPlan& p,std::span<const std::byte> source,std::span<std::uint8_t> out){
    GxProfileScope profile(GxProfilePart::encode);
    ColorCopyPlan check;
    if(!planColorCopy(p.extent,p.left,p.top,p.sourceWidth,p.sourceHeight,p.width,p.height,p.redChannel?0x28:p.format,check)||
       check.bytes!=p.bytes||source.size()!=std::size_t(p.extent.pixelWidth)*p.extent.pixelHeight*4||out.size()!=p.bytes)return false;
    const auto a=reinterpret_cast<std::uintptr_t>(source.data()),b=reinterpret_cast<std::uintptr_t>(out.data());
    if((a<=b&&b-a<source.size())||(b<a&&a-b<out.size()))return false;
    // Preserve the exact integer sample locations, but divide once per axis
    // coordinate instead of four times per copied pixel on ARM11.
    std::array<std::uint32_t,1024> columns,rows;
    for(unsigned x=0;x<p.width;++x){
        const unsigned lx=p.left+(std::uint64_t(2*x+1)*p.sourceWidth)/(2*p.width);
        const unsigned sx=std::uint64_t(lx)*p.extent.pixelWidth/p.extent.logicalWidth;
        columns[x]=sx*p.extent.pixelHeight*4;
    }
    for(unsigned y=0;y<p.height;++y){
        const unsigned ly=p.top+(std::uint64_t(2*y+1)*p.sourceHeight)/(2*p.height);
        const unsigned sy=std::uint64_t(ly)*p.extent.pixelHeight/p.extent.logicalHeight;
        rows[y]=(p.extent.pixelHeight-1-sy)*4;
    }
    switch(p.format){
    case 0:encodeColorTiles<0>(p,source,out,columns,rows);break;
    case 1:if(p.redChannel)encodeColorTiles<1,true>(p,source,out,columns,rows);
           else encodeColorTiles<1>(p,source,out,columns,rows);break;
    case 2:encodeColorTiles<2>(p,source,out,columns,rows);break;
    case 3:encodeColorTiles<3>(p,source,out,columns,rows);break;
    case 4:encodeColorTiles<4>(p,source,out,columns,rows);break;
    case 5:encodeColorTiles<5>(p,source,out,columns,rows);break;
    case 6:encodeColorTiles<6>(p,source,out,columns,rows);break;
    default:return false;
    }
    return true;
}
bool planDepthCopy(const gx::TargetExtent& extent,unsigned left,unsigned top,unsigned sw,unsigned sh,
                   unsigned w,unsigned h,unsigned format,ColorCopyPlan& out){
    out={};
    // Only the original game's observed Z16->IA8 path is admitted here.
    return format==0x13&&!(extent.pixelWidth&7)&&!(extent.pixelHeight&7)&&
        planColorCopy(extent,left,top,sw,sh,w,h,3,out);
}
bool encodeDepthCopy(const ColorCopyPlan& p,std::span<const std::byte> source,std::span<std::uint8_t> out){
    GxProfileScope profile(GxProfilePart::encode);
    ColorCopyPlan check;
    if(p.format!=3||!planDepthCopy(p.extent,p.left,p.top,p.sourceWidth,p.sourceHeight,p.width,p.height,0x13,check)||
       check.bytes!=p.bytes||source.size()!=std::size_t(p.extent.pixelWidth)*p.extent.pixelHeight*4||out.size()!=p.bytes)return false;
    const auto a=reinterpret_cast<std::uintptr_t>(source.data()),b=reinterpret_cast<std::uintptr_t>(out.data());
    if((a<=b&&b-a<source.size())||(b<a&&a-b<out.size()))return false;
    std::fill(out.begin(),out.end(),0);
    std::array<std::uint32_t,1024> columns,rows;
    auto even=[](unsigned x){return (x&1)|((x&2)<<1)|((x&4)<<2);};
    for(unsigned x=0;x<p.width;++x){
        const unsigned lx=p.left+(std::uint64_t(2*x+1)*p.sourceWidth)/(2*p.width);
        const unsigned py=std::uint64_t(lx)*p.extent.pixelWidth/p.extent.logicalWidth;
        columns[x]=(py/8)*(p.extent.pixelHeight/8)*64+(even(py)<<1);
    }
    for(unsigned y=0;y<p.height;++y){
        const unsigned ly=p.top+(std::uint64_t(2*y+1)*p.sourceHeight)/(2*p.height);
        const unsigned px=p.extent.pixelHeight-1-std::uint64_t(ly)*p.extent.pixelHeight/p.extent.logicalHeight;
        rows[y]=(px/8)*64+even(px);
    }
    std::size_t tile=0;
    for(unsigned by=0;by<p.height;by+=4)for(unsigned bx=0;bx<p.width;bx+=4,tile+=32)
    for(unsigned y=0;y<4&&by+y<p.height;++y)for(unsigned x=0;x<4&&bx+x<p.width;++x){
        const auto at=columns[bx+x]+rows[by+y];
        std::uint32_t pica;std::memcpy(&pica,source.data()+at*4,4);
        const auto depth=0xffffff-(pica&0xffffff);
        const auto block=tile+(y*4+x)*2;
        out[block]=depth>>16;out[block+1]=depth>>8;
    }
    return true;
}
}
