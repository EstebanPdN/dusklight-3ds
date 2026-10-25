// Candidate30 per-pixel reference, with the color LCD contract corrected.
// Depth follows the independently derived native raw contract; see depth-copy-x replay.
// Independent color evidence: test_color_copy_orientation.py.
#pragma once
#include "../runtime/gx_copy_pixels.hpp"

#include <algorithm>
#include <cstring>
namespace dusk::native {
inline bool referencePlanColorCopy(const gx::TargetExtent& extent,unsigned left,unsigned top,unsigned sw,unsigned sh,
                   unsigned w,unsigned h,unsigned format,ColorCopyPlan& out){
    out={};
    const bool redChannel=format==0x28;if(redChannel)format=1;
    if(!extent.valid()||!sw||!sh||left>=extent.logicalWidth||top>=extent.logicalHeight||
       sw>extent.logicalWidth-left||sh>extent.logicalHeight-top||!w||!h||w>1024||h>1024||format>6)return false;
    const unsigned bw=format<3?8:4,bh=format==0?8:4,tile=format==6?64:32;
    out={extent,left,top,sw,sh,w,h,format,std::size_t((w+bw-1)/bw)*((h+bh-1)/bh)*tile,redChannel};
    return true;
}
inline bool referenceEncodeColorCopy(const ColorCopyPlan& p,std::span<const std::byte> source,std::span<std::uint8_t> out){
    ColorCopyPlan check;
    if(!referencePlanColorCopy(p.extent,p.left,p.top,p.sourceWidth,p.sourceHeight,p.width,p.height,p.redChannel?0x28:p.format,check)||
       check.bytes!=p.bytes||source.size()!=std::size_t(p.extent.pixelWidth)*p.extent.pixelHeight*4||out.size()!=p.bytes)return false;
    const auto a=reinterpret_cast<std::uintptr_t>(source.data()),b=reinterpret_cast<std::uintptr_t>(out.data());
    if((a<=b&&b-a<source.size())||(b<a&&a-b<out.size()))return false;
    std::fill(out.begin(),out.end(),0);
    const unsigned bw=p.format<3?8:4,bh=p.format==0?8:4,tile=p.format==6?64:32;
    for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x){
        const unsigned lx=p.left+(std::uint64_t(2*x+1)*p.sourceWidth)/(2*p.width);
        const unsigned ly=p.top+(std::uint64_t(2*y+1)*p.sourceHeight)/(2*p.height);
        const unsigned sx=std::uint64_t(lx)*p.extent.pixelWidth/p.extent.logicalWidth;
        const unsigned sy=std::uint64_t(ly)*p.extent.pixelHeight/p.extent.logicalHeight;
        const auto at=(std::size_t(sx)*p.extent.pixelHeight+p.extent.pixelHeight-1-sy)*4;
        const unsigned alpha=std::to_integer<unsigned>(source[at]),blue=std::to_integer<unsigned>(source[at+1]),
                       green=std::to_integer<unsigned>(source[at+2]),red=std::to_integer<unsigned>(source[at+3]);
        const unsigned intensity=p.redChannel?red:(77*red+150*green+29*blue+128)>>8;
        const auto block=(std::size_t(y/bh)*((p.width+bw-1)/bw)+x/bw)*tile;
        const auto pixel=(y%bh)*bw+x%bw;
        if(p.format==0)out[block+pixel/2]|=std::uint8_t((intensity>>4)<<((pixel&1)?0:4));
        else if(p.format==1)out[block+pixel]=intensity;
        else if(p.format==2)out[block+pixel]=(alpha&240)|(intensity>>4);
        else if(p.format==3){out[block+pixel*2]=alpha;out[block+pixel*2+1]=intensity;}
        else if(p.format==4||p.format==5){
            const unsigned value=p.format==4?((red>>3)<<11)|((green>>2)<<5)|(blue>>3):
                alpha>=224?0x8000|((red>>3)<<10)|((green>>3)<<5)|(blue>>3):
                ((alpha>>5)<<12)|((red>>4)<<8)|((green>>4)<<4)|(blue>>4);
            out[block+pixel*2]=value>>8;out[block+pixel*2+1]=value;
        }else{out[block+pixel*2]=alpha;out[block+pixel*2+1]=red;out[block+32+pixel*2]=green;out[block+33+pixel*2]=blue;}
    }
    return true;
}
inline bool referencePlanDepthCopy(const gx::TargetExtent& extent,unsigned left,unsigned top,unsigned sw,unsigned sh,
                   unsigned w,unsigned h,unsigned format,ColorCopyPlan& out){
    out={};
    // Only the original game's observed Z16->IA8 path is admitted here.
    return format==0x13&&!(extent.pixelWidth&7)&&!(extent.pixelHeight&7)&&
        referencePlanColorCopy(extent,left,top,sw,sh,w,h,3,out);
}
inline bool referenceEncodeDepthCopy(const ColorCopyPlan& p,std::span<const std::byte> source,std::span<std::uint8_t> out){
    ColorCopyPlan check;
    if(p.format!=3||!referencePlanDepthCopy(p.extent,p.left,p.top,p.sourceWidth,p.sourceHeight,p.width,p.height,0x13,check)||
       check.bytes!=p.bytes||source.size()!=std::size_t(p.extent.pixelWidth)*p.extent.pixelHeight*4||out.size()!=p.bytes)return false;
    const auto a=reinterpret_cast<std::uintptr_t>(source.data()),b=reinterpret_cast<std::uintptr_t>(out.data());
    if((a<=b&&b-a<source.size())||(b<a&&a-b<out.size()))return false;
    std::fill(out.begin(),out.end(),0);
    for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x){
        const unsigned lx=p.left+(std::uint64_t(2*x+1)*p.sourceWidth)/(2*p.width);
        const unsigned ly=p.top+(std::uint64_t(2*y+1)*p.sourceHeight)/(2*p.height);
        const unsigned px=p.extent.pixelHeight-1-std::uint64_t(ly)*p.extent.pixelHeight/p.extent.logicalHeight;
        const unsigned py=std::uint64_t(lx)*p.extent.pixelWidth/p.extent.logicalWidth;
        unsigned morton{};
        for(unsigned bit=0;bit<3;++bit){morton|=((px>>bit)&1)<<(2*bit);morton|=((py>>bit)&1)<<(2*bit+1);}
        const auto at=(std::size_t(py/8)*(p.extent.pixelHeight/8)+px/8)*64+morton;
        std::uint32_t pica;std::memcpy(&pica,source.data()+at*4,4);
        const auto depth=0xffffff-(pica&0xffffff);
        const auto block=(std::size_t(y/4)*((p.width+3)/4)+x/4)*32+((y%4)*4+x%4)*2;
        out[block]=depth>>16;out[block+1]=depth>>8;
    }
    return true;
}
}
