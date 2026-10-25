#include "fused_color.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace dusk::native::private_fused {
namespace {
bool overlaps(const void* a, std::size_t n, const void* b, std::size_t m) {
    const auto x=reinterpret_cast<std::uintptr_t>(a), y=reinterpret_cast<std::uintptr_t>(b);
    return n && m && (x<=y ? y-x<n : x-y<m);
}
constexpr unsigned spread(unsigned x) { return (x&1)|((x&2)<<1)|((x&4)<<2); }
std::size_t address(unsigned x,unsigned y,unsigned width,unsigned bpp) {
    return (std::size_t(y/8)*(width/8)*64+(x/8)*64+spread(x)+2*spread(y))*bpp;
}
// Same native-tile clamp principle as texture_gx.cpp::copyPaddingTile.
// Conversion above writes every logical texel first. Partial edge tiles read
// only logical texels in their own tile; later full padding tiles read either
// those completed edge tiles or completed right padding in the last tile row.
// No extra image, allocation, or persistent state is needed.
template<unsigned Bpp>
void padNative(const ColorCopyPlan& p,const NativeTexturePlan& n,std::span<std::uint8_t> native) {
    for(unsigned ty=0;ty<n.paddedHeight;ty+=8)for(unsigned tx=0;tx<n.paddedWidth;tx+=8) {
        if(tx+8<=p.width&&ty+8<=p.height)continue;
        auto* target=native.data()+(std::size_t(ty/8)*(n.paddedWidth/8)+tx/8)*64*Bpp;
        if(tx>=p.width||ty>=p.height) {
            const bool bottom=ty>=p.height;
            const unsigned sx=bottom?tx:p.width-1,sy=bottom?p.height-1:ty;
            const auto* source=native.data()+(std::size_t(sy/8)*(n.paddedWidth/8)+sx/8)*64*Bpp;
            const unsigned mask=bottom?0x15:0x2a,edge=bottom?2*spread(sy):spread(sx);
            for(unsigned m=0;m<64;++m)
                std::memcpy(target+m*Bpp,source+((m&mask)|edge)*Bpp,Bpp);
        } else {
            const unsigned lastX=std::min(7u,p.width-1-tx),lastY=std::min(7u,p.height-1-ty);
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
                if(x>lastX||y>lastY)
                    std::memcpy(target+(spread(x)+2*spread(y))*Bpp,
                        target+(spread(std::min(x,lastX))+2*spread(std::min(y,lastY)))*Bpp,Bpp);
        }
    }
}
template<unsigned F,bool Reference565=false>
void convert(const ColorCopyPlan& p,std::span<const std::byte> in,std::span<std::uint8_t> gx,
             const NativeTexturePlan& n,std::span<std::uint8_t> native,
             const std::array<std::uint32_t,1024>& columns,const std::array<std::uint32_t,1024>& rows) {
    constexpr unsigned bw=F<3?8:4,bh=F==0?8:4,tile=F==6?64:32;
    constexpr unsigned bpp=F==0||F==2?1:F==1||F==3||(F==4&&!Reference565)?2:4;
    if(p.width%bw||p.height%bh)std::fill(gx.begin(),gx.end(),0);
    std::size_t block=0;
    for(unsigned by=0;by<p.height;by+=bh)for(unsigned bx=0;bx<p.width;bx+=bw,block+=tile) {
    // Every GX block starts at a 4/8-texel boundary and stays within one
    // native 8x8 tile. Its local Morton bits are disjoint from the block base.
    auto* base=native.data()+address(bx,by,n.paddedWidth,bpp);
    const unsigned countX=std::min(bw,p.width-bx),countY=std::min(bh,p.height-by);
    for(unsigned y=0;y<countY;++y) {
    auto* row=base+2*spread(y)*bpp;
    for(unsigned x=0;x<countX;++x) {
        const auto at=columns[bx+x]+rows[by+y];
        unsigned a=std::to_integer<unsigned>(in[at]),b=std::to_integer<unsigned>(in[at+1]),
                 g=std::to_integer<unsigned>(in[at+2]),r=std::to_integer<unsigned>(in[at+3]);
        const unsigned i=p.redChannel?r:(77*r+150*g+29*b+128)>>8;
        const unsigned pixel=y*bw+x;
        auto* dst=row+spread(x)*bpp;
        if constexpr(F==0) {
            if(pixel&1)gx[block+pixel/2]|=std::uint8_t(i>>4);
            else gx[block+pixel/2]=std::uint8_t(i&240);
            dst[0]=std::uint8_t((i&240)|(i>>4));
        } else if constexpr(F==1) {
            gx[block+pixel]=i;dst[0]=dst[1]=i;
        } else if constexpr(F==2) {
            gx[block+pixel]=(a&240)|(i>>4);dst[0]=(i&240)|(a>>4);
        } else if constexpr(F==3) {
            gx[block+pixel*2]=a;gx[block+pixel*2+1]=i;dst[0]=a;dst[1]=i;
        } else if constexpr(F==4||F==5) {
            const unsigned value=F==4?((r>>3)<<11)|((g>>2)<<5)|(b>>3):
                a>=224?0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3):
                ((a>>5)<<12)|((r>>4)<<8)|((g>>4)<<4)|(b>>4);
            gx[block+pixel*2]=value>>8;gx[block+pixel*2+1]=value;
            if constexpr(F==4&&!Reference565) { dst[0]=value;dst[1]=value>>8; }
            else {
                // Expand the quantized GX word, never unquantized framebuffer RGB.
                if constexpr(F==4) {
                    r=value>>11;g=(value>>5)&63;b=value&31;
                    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);a=255;
                } else if(value&0x8000) {
                    r=(value>>10)&31;g=(value>>5)&31;b=value&31;
                    r=(r<<3)|(r>>2);g=(g<<3)|(g>>2);b=(b<<3)|(b>>2);a=255;
                } else {
                    a=value>>12;a=(a<<5)|(a<<2)|(a>>1);
                    r=((value>>8)&15)*17;g=((value>>4)&15)*17;b=(value&15)*17;
                }
                dst[0]=a;dst[1]=b;dst[2]=g;dst[3]=r;
            }
        } else {
            gx[block+pixel*2]=a;gx[block+pixel*2+1]=r;
            gx[block+32+pixel*2]=g;gx[block+33+pixel*2]=b;
            dst[0]=a;dst[1]=b;dst[2]=g;dst[3]=r;
        }
    }
    }
    }
    padNative<bpp>(p,n,native);
}
}
bool encode(const ColorCopyPlan& p,std::span<const std::byte> input,std::span<std::uint8_t> bti,
            const NativeTexturePlan& n,std::span<std::uint8_t> native) {
    ColorCopyPlan c;NativeTexturePlan checked;
    // Validate the ENTIRE copy descriptor, unlike accepting a forged red flag
    // with a coincidentally equal tile byte count.
    if((p.redChannel&&p.format!=1)||!planColorCopy(p.extent,p.left,p.top,p.sourceWidth,p.sourceHeight,
        p.width,p.height,p.redChannel?0x28:p.format,c)||c.bytes!=p.bytes||c.format!=p.format||
        !inspectNativeTexture(bti,checked,n.gxFormat==4&&n.format==NativeTextureFormat::rgba8)||
        checked!=n||n.gxFormat!=p.format||n.width!=p.width||n.height!=p.height||
        n.mipCount!=1||n.sourceMip||n.paletteEntries||n.sourceBytes!=p.bytes||
        input.size()!=std::size_t(p.extent.pixelWidth)*p.extent.pixelHeight*4||native.size()!=n.outputBytes||
        overlaps(input.data(),input.size(),bti.data(),bti.size())||
        overlaps(input.data(),input.size(),native.data(),native.size())||
        overlaps(bti.data(),bti.size(),native.data(),native.size()))return false;
    std::array<std::uint32_t,1024> columns,rows;
    for(unsigned x=0;x<p.width;++x) {
        const unsigned lx=p.left+std::uint64_t(2*x+1)*p.sourceWidth/(2*p.width);
        columns[x]=std::uint64_t(lx)*p.extent.pixelWidth/p.extent.logicalWidth*p.extent.pixelHeight*4;
    }
    for(unsigned y=0;y<p.height;++y) {
        const unsigned ly=p.top+std::uint64_t(2*y+1)*p.sourceHeight/(2*p.height);
        rows[y]=(p.extent.pixelHeight-1-std::uint64_t(ly)*p.extent.pixelHeight/p.extent.logicalHeight)*4;
    }
    auto gx=bti.subspan(n.dataOffset,n.sourceBytes);
    switch(p.format) {
    case 0:convert<0>(p,input,gx,n,native,columns,rows);break;
    case 1:convert<1>(p,input,gx,n,native,columns,rows);break;
    case 2:convert<2>(p,input,gx,n,native,columns,rows);break;
    case 3:convert<3>(p,input,gx,n,native,columns,rows);break;
    case 4:if(n.format==NativeTextureFormat::rgba8)convert<4,true>(p,input,gx,n,native,columns,rows);
           else convert<4>(p,input,gx,n,native,columns,rows);break;
    case 5:convert<5>(p,input,gx,n,native,columns,rows);break;
    case 6:convert<6>(p,input,gx,n,native,columns,rows);break;
    default:return false; // Preflight excludes this before writes.
    }
    return true;
}
}
