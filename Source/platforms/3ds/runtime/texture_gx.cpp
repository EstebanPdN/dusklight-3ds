#include "texture_gx.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <cstring>
namespace dusk::native {
namespace {
using u8=std::uint8_t;
unsigned half(const u8* p){return (unsigned(p[0])<<8)|p[1];}
std::uint32_t word(const u8* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
unsigned padded(unsigned n){unsigned p=8;while(p<n)p*=2;return p;}
struct Color {u8 r,g,b,a;};
Color rgb565(unsigned value){
    const unsigned r=value>>11,g=(value>>5)&63,b=value&31;
    return {u8((r<<3)|(r>>2)),u8((g<<2)|(g>>4)),u8((b<<3)|(b>>2)),255};
}
Color rgb5a3(unsigned value){
    if(value&0x8000){const unsigned r=(value>>10)&31,g=(value>>5)&31,b=value&31;
        return {u8((r<<3)|(r>>2)),u8((g<<3)|(g>>2)),u8((b<<3)|(b>>2)),255};}
    const unsigned a=value>>12,r=(value>>8)&15,g=(value>>4)&15,b=value&15;
    return {u8(r*17),u8(g*17),u8(b*17),u8((a<<5)|(a<<2)|(a>>1))};
}
using Palette=std::array<Color,4>;
Palette cmpr(const u8* block){
    const unsigned a=half(block),b=half(block+2);Palette p{rgb565(a),rgb565(b),{}, {}};
    auto blend=[&](unsigned x,unsigned y){return u8(a>b?(5*x+3*y)/8:(x+y)/2);};
    p[2]={blend(p[0].r,p[1].r),blend(p[0].g,p[1].g),blend(p[0].b,p[1].b),255};
    p[3]={blend(p[1].r,p[0].r),blend(p[1].g,p[0].g),blend(p[1].b,p[0].b),u8(a>b?255:0)};
    return p;
}
// Input extents/payload are validated before this scan. Inspect selectors only
// for logical texels: endpoint order alone does not imply transparent pixels.
// No decoded image is allocated. Used only during half planning/upload, never
// a clean immutable cache hit.
bool opaqueCmpr(std::span<const u8> b,std::size_t offset,unsigned w,unsigned h,unsigned mips){
    for(unsigned level=0;level<mips;++level){
        const unsigned blocksWide=(w+7)/8;
        for(unsigned by=0;by<h;by+=8)for(unsigned bx=0;bx<w;bx+=8){
            const auto* tile=b.data()+offset+((by/8)*blocksWide+bx/8)*32;
            for(unsigned sub=0;sub<4;++sub){
                const auto* block=tile+sub*8;
                if(half(block)>half(block+2))continue;
                for(unsigned y=0;y<4&&by+(sub/2)*4+y<h;++y)
                    for(unsigned x=0;x<4&&bx+(sub%2)*4+x<w;++x)
                        if(((block[4+y]>>(6-2*x))&3)==3)return false;
            }
        }
        offset+=std::size_t(blocksWide)*((h+7)/8)*32;
        w=std::max(1u,w/2);h=std::max(1u,h/2);
    }
    return true;
}
}
bool inspectNativeTexture(std::span<const u8> b,NativeTexturePlan& out,bool rgb565Rgba8Reference,bool halfResolution){
    out={};if(b.size()<32||!b[24]||b[24]>8)return false;
    const bool indexed=b[0]==8||b[0]==9||b[0]==10;
    const unsigned colors=half(b.data()+10);
    const std::size_t palette=word(b.data()+12);
    if(indexed){
        const unsigned limit=b[0]==8?16:b[0]==9?256:16384;
        if(b[8]!=1||b[9]>2||!colors||colors>limit||palette<32||
           palette>b.size()||std::size_t(colors)*2>b.size()-palette)return false;
    }else if(b[8]||colors)return false;
    const unsigned w=half(b.data()+2),h=half(b.data()+4);const std::size_t offset=word(b.data()+28);
    if(!w||!h||w>1024||h>1024||offset<32||offset>b.size())return false;
    unsigned bw=8,bh=8,blockBytes=32,bpp=4;NativeTextureFormat format=NativeTextureFormat::rgba8;
    switch(b[0]){
    case 0:format=NativeTextureFormat::la4;bpp=1;break;
    case 1:format=NativeTextureFormat::la8;bpp=2;bh=4;break;
    case 2:format=NativeTextureFormat::la4;bpp=1;bh=4;break;
    case 3:format=NativeTextureFormat::la8;bpp=2;bw=bh=4;break;
    case 4:format=rgb565Rgba8Reference?NativeTextureFormat::rgba8:NativeTextureFormat::rgb565;bpp=rgb565Rgba8Reference?4:2;bw=bh=4;break;
    case 5:bw=bh=4;break;
    case 6:bw=bh=4;blockBytes=64;break;
    case 14:break;
    case 8:break;
    case 9:bh=4;break;
    case 10:bw=bh=4;break;
    default:return false;
    }
    std::size_t bytes=0,output=0;
    if(b[24]>1&&(w!=padded(w)||h!=padded(h)||(w>>(b[24]-1))<8||(h>>(b[24]-1))<8))return false;
    for(unsigned level=0;level<b[24];++level){
        const auto lw=std::max(1u,w>>level),lh=std::max(1u,h>>level);
        bytes+=((lw+bw-1)/bw)*((lh+bh-1)/bh)*blockBytes;
        output+=std::size_t(padded(lw))*padded(lh)*bpp;
    }
    if(bytes>b.size()-offset)return false;
    if(indexed&&offset<palette+std::size_t(colors)*2&&palette<offset+bytes)return false;
    out={w,h,padded(w),padded(h),offset,bytes,output,b[0],format,b[24],
        indexed?palette:0,static_cast<std::uint16_t>(indexed?colors:0),static_cast<u8>(indexed?b[9]:0),w,h,b[24],0};
    if(halfResolution){
        if((b[0]!=4&&b[0]!=14)||(b[0]==4&&b[1]!=0)||w<16||h<16||b[24]<2||
           (b[0]==14&&!opaqueCmpr(b,offset,w,h,b[24]))){out={};return false;}
        const std::size_t skipped=((w+bw-1)/bw)*((h+bh-1)/bh)*blockBytes;
        out.width=w/2;out.height=h/2;out.paddedWidth=padded(out.width);out.paddedHeight=padded(out.height);
        out.dataOffset+=skipped;out.sourceBytes-=skipped;out.outputBytes-=std::size_t(w)*h*bpp;
        --out.mipCount;out.sourceMip=1;
    }
    return true;
}
bool nativeTextureFingerprint(std::span<const u8> b,NativeTextureFingerprint& out){
    out={};NativeTexturePlan p;
    if(!inspectNativeTexture(b,p)||(p.gxFormat!=4&&p.gxFormat!=14))return false;
    // Small streaming SHA-256: no retained source pointer or image-sized copy.
    constexpr std::array<std::uint32_t,64> k{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t,8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<u8,64> block{};unsigned used{};
    auto compress=[&]{
        std::array<std::uint32_t,64> w{};for(unsigned n=0;n<16;++n)w[n]=word(block.data()+4*n);
        for(unsigned n=16;n<64;++n){const auto x=w[n-15],y=w[n-2];
            w[n]=w[n-16]+(std::rotr(x,7)^std::rotr(x,18)^(x>>3))+w[n-7]+(std::rotr(y,17)^std::rotr(y,19)^(y>>10));}
        auto a=state[0],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7],v=state[1];
        for(unsigned n=0;n<64;++n){
            const auto t=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[n]+w[n];
            const auto s=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&v)^(a&c)^(v&c));
            h=g;g=f;f=e;e=d+t;d=c;c=v;v=a;a=t+s;
        }
        state[0]+=a;state[1]+=v;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    };
    auto put=[&](u8 value){block[used++]=value;if(used==64){compress();used=0;}};
    for(u8 value:{b[0],b[2],b[3],b[4],b[5],b[24]})put(value);
    for(const auto value:b.subspan(p.dataOffset,p.sourceBytes))put(value);
    const auto bits=std::uint64_t(p.sourceBytes+6)*8;
    put(0x80);while(used!=56)put(0);
    for(unsigned n=0;n<8;++n)put(bits>>(56-8*n));
    for(unsigned n=0;n<32;++n)out[n]=state[n/4]>>(24-8*(n%4));
    return true;
}
// Whole interior tiles have a fixed GX-to-PICA address map for the level.
// Specialization keeps format branches and source block arithmetic out of the
// hot texel loop; edge tiles retain the exact clamped reference path below.
template<unsigned Format,bool Rgba565=false>
static void decodeInteriorTile(const u8* data,u8* output,const std::array<unsigned,64>& offsets,
                               const std::array<std::array<u8,4>,256>* palette=nullptr){
    for(unsigned m=0;m<64;++m){
        const unsigned at=offsets[m];
        if constexpr(Format==0){const auto shift=(m&1)?0:4;const u8 i=(data[at]>>shift)&15;output[m]=(i<<4)|i;}
        else if constexpr(Format==1){output[m*2]=data[at];output[m*2+1]=data[at];}
        else if constexpr(Format==2){const u8 v=data[at];output[m]=(v<<4)|(v>>4);}
        else if constexpr(Format==3){output[m*2]=data[at];output[m*2+1]=data[at+1];}
        else if constexpr(Format==6){output[m*4]=data[at];output[m*4+1]=data[at+33];output[m*4+2]=data[at+32];output[m*4+3]=data[at+1];}
        else if constexpr(Format==4){
            // GX is big-endian; PICA keeps the same R5/G6/B5 bits, little-endian.
            if constexpr(Rgba565){
                const Color c=rgb565(half(data+at));
                output[m*4]=c.a;output[m*4+1]=c.b;output[m*4+2]=c.g;output[m*4+3]=c.r;
            }else{output[m*2]=data[at+1];output[m*2+1]=data[at];}
        }else if constexpr(Format==5){
            const Color c=rgb5a3(half(data+at));
            output[m*4]=c.a;output[m*4+1]=c.b;output[m*4+2]=c.g;output[m*4+3]=c.r;
        }else if constexpr(Format==8||Format==9){
            const unsigned index=Format==8?(data[at]>>((m&1)?0:4))&15:data[at];
            std::memcpy(output+m*4,(*palette)[index].data(),4);
        }
    }
}
template<unsigned Bpp>
static void copyPaddingTile(const u8* source,u8* target,unsigned mask,unsigned edge){
    for(unsigned m=0;m<64;++m)std::memcpy(target+m*Bpp,source+((m&mask)|edge)*Bpp,Bpp);
}
static bool decodeLevel(std::span<const u8> b,const NativeTexturePlan& p,std::span<u8> output,bool lean){
    const auto* data=b.data()+p.dataOffset;std::size_t destination=0;
    const bool indexedTile=p.gxFormat==8||p.gxFormat==9;
    const bool mapped=p.gxFormat<=6||indexedTile;
    const unsigned bw=p.gxFormat>=3&&p.gxFormat<=6?4:8;
    const unsigned bh=p.gxFormat==0||p.gxFormat==8?8:4;
    const unsigned blockBytes=p.gxFormat==6?64:32;
    const unsigned blocksWide=(p.width+bw-1)/bw;
    const unsigned bpp=nativeTextureBytesPerPixel(p.format);
    // CI4/CI8 need at most 256 palette entries. Resolve the current live TLUT
    // once per level; invalid indices retain the reference transparent value.
    // No image/palette data is cached across GX invalidation or resource reuse.
    std::array<std::array<u8,4>,256> palette{};
    if(indexedTile)for(unsigned n=0;n<p.paletteEntries;++n){
        const auto* entry=b.data()+p.paletteOffset+2*n;Color c;
        if(p.paletteFormat==0)c={entry[1],entry[1],entry[1],entry[0]};
        else c=p.paletteFormat==1?rgb565(half(entry)):rgb5a3(half(entry));
        palette[n]={c.a,c.b,c.g,c.r};
    }
    std::array<unsigned,64> offsets{};
    if(mapped)for(unsigned m=0;m<64;++m){
        const unsigned x=(m&1)+((m>>1)&2)+((m>>2)&4),y=((m>>1)&1)+((m>>2)&2)+((m>>3)&4);
        const unsigned base=(y/bh*blocksWide+x/bw)*blockBytes;
        offsets[m]=base+((p.gxFormat==0||p.gxFormat==8)?(y%8)*4+x/2:
            (p.gxFormat==1||p.gxFormat==2||p.gxFormat==9)?(y%4)*8+x:
            ((y%4)*4+x%4)*2);
    }
    for(unsigned ty=0;ty<p.paddedHeight;ty+=8)for(unsigned tx=0;tx<p.paddedWidth;tx+=8){
        // Padding clamps to already decoded native texels. Reuse their exact
        // bytes instead of decoding identical GX pixels once per padded texel.
        // Tile traversal guarantees the source tile is fully initialized.
        if(ty>=p.height||tx>=p.width){
            if(lean&&(tx/8>(p.width+7)/8||ty/8>(p.height+7)/8)){destination+=64*bpp;continue;}
            auto even=[](unsigned x){return (x&1)|((x&2)<<1)|((x&4)<<2);};
            const bool bottom=ty>=p.height;
            const unsigned sx=bottom?tx:(p.width-1),sy=bottom?(p.height-1):ty;
            const auto* source=output.data()+((sy/8)*(p.paddedWidth/8)+sx/8)*64*bpp;
            auto* target=output.data()+destination;
            const unsigned mask=bottom?0x15:0x2a,edge=bottom?even(sy)<<1:even(sx);
            if(bpp==4)copyPaddingTile<4>(source,target,mask,edge);
            else if(bpp==2)copyPaddingTile<2>(source,target,mask,edge);
            else copyPaddingTile<1>(source,target,mask,edge);
            destination+=64*bpp;continue;
        }
        if(mapped&&tx+8<=p.width&&ty+8<=p.height){
            const auto* tile=data+(ty/bh*blocksWide+tx/bw)*blockBytes;
            auto* target=output.data()+destination;
            switch(p.gxFormat){
            case 0:decodeInteriorTile<0>(tile,target,offsets);break;
            case 1:decodeInteriorTile<1>(tile,target,offsets);break;
            case 2:decodeInteriorTile<2>(tile,target,offsets);break;
            case 3:decodeInteriorTile<3>(tile,target,offsets);break;
            case 4:
                if(p.format==NativeTextureFormat::rgba8)decodeInteriorTile<4,true>(tile,target,offsets);
                else decodeInteriorTile<4>(tile,target,offsets);
                break;
            case 5:decodeInteriorTile<5>(tile,target,offsets);break;
            case 6:decodeInteriorTile<6>(tile,target,offsets);break;
            case 8:decodeInteriorTile<8>(tile,target,offsets,&palette);break;
            case 9:decodeInteriorTile<9>(tile,target,offsets,&palette);break;
            }
            destination+=64*bpp;continue;
        }
        std::array<Palette,4> palettes{};const u8* compressed=nullptr;
        if(p.gxFormat==14){
            const unsigned bx=std::min(tx,p.width-1)/8,by=std::min(ty,p.height-1)/8;
            compressed=data+(by*((p.width+7)/8)+bx)*32;
            for(unsigned i=0;i<4;++i)palettes[i]=cmpr(compressed+8*i);
        }
        for(unsigned m=0;m<64;++m){
            const unsigned x=std::min(tx+(m&1)+((m>>1)&2)+((m>>2)&4),p.width-1);
            const unsigned y=std::min(ty+((m>>1)&1)+((m>>2)&2)+((m>>3)&4),p.height-1);
            Color c{};
            if(p.gxFormat==0){
                const unsigned at=((y/8)*((p.width+7)/8)+x/8)*32+(y%8)*4+(x%8)/2;
                const u8 i=((data[at]>>((x&1)?0:4))&15)*17;c={i,i,i,i};
            }else if(p.gxFormat==1||p.gxFormat==2){
                const unsigned at=((y/4)*((p.width+7)/8)+x/8)*32+(y%4)*8+x%8;
                const u8 value=data[at],i=p.gxFormat==1?value:(value&15)*17;
                c={i,i,i,p.gxFormat==1?value:u8((value>>4)*17)};
            }else if(p.gxFormat==3){
                const unsigned at=((y/4)*((p.width+3)/4)+x/4)*32+((y%4)*4+x%4)*2;
                const u8 i=data[at+1];c={i,i,i,data[at]};
            }else if(p.gxFormat==4||p.gxFormat==5){
                const unsigned at=((y/4)*((p.width+3)/4)+x/4)*32+((y%4)*4+x%4)*2;
                if(p.gxFormat==4&&p.format==NativeTextureFormat::rgb565){
                    output[destination++]=data[at+1];output[destination++]=data[at];
                    continue;
                }
                c=p.gxFormat==4?rgb565(half(data+at)):rgb5a3(half(data+at));
            }else if(p.gxFormat==6){
                const unsigned at=((y/4)*((p.width+3)/4)+x/4)*64+((y%4)*4+x%4)*2;
                c={data[at+1],data[at+32],data[at+33],data[at]};
            }else if(p.paletteEntries){
                unsigned index{};
                if(p.gxFormat==8){
                    const unsigned at=((y/8)*((p.width+7)/8)+x/8)*32+(y%8)*4+(x%8)/2;
                    index=(data[at]>>((x&1)?0:4))&15;
                }else if(p.gxFormat==9){
                    const unsigned at=((y/4)*((p.width+7)/8)+x/8)*32+(y%4)*8+x%8;
                    index=data[at];
                }else{
                    const unsigned at=((y/4)*((p.width+3)/4)+x/4)*32+((y%4)*4+x%4)*2;
                    index=half(data+at)&0x3fff;
                }
                // Preserve the pinned Aurora decoder's explicit out-of-table
                // transparent result without reading beyond the actual TLUT.
                if(index<p.paletteEntries){
                    const auto* entry=b.data()+p.paletteOffset+2*index;
                    const unsigned value=half(entry);
                    if(p.paletteFormat==0)c={entry[1],entry[1],entry[1],entry[0]};
                    else c=p.paletteFormat==1?rgb565(value):rgb5a3(value);
                }
            }else{
                const unsigned block=(y%8)/4*2+(x%8)/4;
                const unsigned index=(compressed[block*8+4+y%4]>>(6-2*(x%4)))&3;
                c=palettes[block][index];
            }
            if(p.format==NativeTextureFormat::la4)output[destination++]=(c.r&0xf0)|(c.a>>4);
            else if(p.format==NativeTextureFormat::la8){output[destination++]=c.a;output[destination++]=c.r;}
            else{output[destination++]=c.a;output[destination++]=c.b;output[destination++]=c.g;output[destination++]=c.r;}
        }
    }
    return true;
}
bool inspectNativeTextureBoxHalf(std::span<const u8> b,NativeTexturePlan& out,bool rgb565Rgba8Reference){
    NativeTexturePlan full;
    if(!inspectNativeTexture(b,full,rgb565Rgba8Reference)||full.mipCount!=1||full.width<16||full.height<16){out={};return false;}
    out=full;out.width=full.width/2;out.height=full.height/2;
    out.paddedWidth=padded(out.width);out.paddedHeight=padded(out.height);
    out.outputBytes=std::size_t(out.paddedWidth)*out.paddedHeight*nativeTextureBytesPerPixel(full.format);
    out.sourceMip=2;return true;
}
namespace {
unsigned picaTexel(unsigned x,unsigned y,unsigned paddedWidth){
    return ((y/8)*(paddedWidth/8)+x/8)*64+((x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2)|((x&4)<<2)|((y&4)<<3));
}
bool decodeBoxHalf(std::span<const u8> b,const NativeTexturePlan& p,std::span<u8> output){
    NativeTexturePlan full,check;
    if(!inspectNativeTexture(b,full,p.format==NativeTextureFormat::rgba8&&p.gxFormat==4)||
       !inspectNativeTextureBoxHalf(b,check,p.format==NativeTextureFormat::rgba8&&p.gxFormat==4)||check!=p||output.size()<p.outputBytes)return false;
    auto* scratch=static_cast<u8*>(std::malloc(full.outputBytes));if(!scratch)return false;
    const bool ok=decodeNativeTexture(b,full,{scratch,full.outputBytes},true);
    if(ok){
        const unsigned bpp=nativeTextureBytesPerPixel(p.format);
        for(unsigned y=0;y<p.paddedHeight;++y)for(unsigned x=0;x<p.paddedWidth;++x){
            // Clamp to the image so padding replicates the edge (clamp sampling).
            const unsigned sx=std::min(x,p.width-1)*2,sy=std::min(y,p.height-1)*2;
            const u8* t[4]={scratch+picaTexel(sx,sy,full.paddedWidth)*bpp,scratch+picaTexel(sx+1,sy,full.paddedWidth)*bpp,
                            scratch+picaTexel(sx,sy+1,full.paddedWidth)*bpp,scratch+picaTexel(sx+1,sy+1,full.paddedWidth)*bpp};
            u8* o=output.data()+picaTexel(x,y,p.paddedWidth)*bpp;
            if(p.format==NativeTextureFormat::rgb565){
                unsigned r=0,g=0,bl=0;
                for(auto* q:t){const unsigned v=q[0]|(q[1]<<8);r+=v>>11;g+=(v>>5)&63;bl+=v&31;}
                const unsigned v=((r+2)/4<<11)|((g+2)/4<<5)|((bl+2)/4);o[0]=u8(v);o[1]=u8(v>>8);
            }else if(p.format==NativeTextureFormat::la4){
                unsigned l=0,a=0;for(auto* q:t){l+=q[0]>>4;a+=q[0]&15;}
                o[0]=u8((((l+2)/4)<<4)|((a+2)/4));
            }else for(unsigned c=0;c<bpp;++c)o[c]=u8((t[0][c]+t[1][c]+t[2][c]+t[3][c]+2)/4);
        }
    }
    std::free(scratch);return ok;
}
}
bool decodeNativeTexture(std::span<const u8> b,const NativeTexturePlan& p,std::span<u8> output,bool leanPadding){
    if(p.sourceMip==2)return decodeBoxHalf(b,p,output);
    NativeTexturePlan checked;
    if(!inspectNativeTexture(b,checked,p.gxFormat==4&&p.format==NativeTextureFormat::rgba8,p.sourceMip==1)||p!=checked||output.size()<p.outputBytes)return false;
    const auto in=reinterpret_cast<std::uintptr_t>(b.data()),out=reinterpret_cast<std::uintptr_t>(output.data());
    if((out>=in&&out-in<b.size())||(in>=out&&in-out<p.outputBytes))return false;
    unsigned bw=8,bh=8,blockBytes=32,bpp=nativeTextureBytesPerPixel(p.format);
    if(p.gxFormat==1||p.gxFormat==2||p.gxFormat==9)bh=4;
    else if((p.gxFormat>=3&&p.gxFormat<=6)||p.gxFormat==10){bw=bh=4;if(p.gxFormat==6)blockBytes=64;}
    auto level=p;std::size_t cursor=0;
    for(unsigned n=0;n<p.mipCount;++n){
        level.sourceBytes=((level.width+bw-1)/bw)*((level.height+bh-1)/bh)*blockBytes;
        level.outputBytes=std::size_t(level.paddedWidth)*level.paddedHeight*bpp;
        if(!decodeLevel(b,level,output.subspan(cursor,level.outputBytes),leanPadding))return false;
        cursor+=level.outputBytes;level.dataOffset+=level.sourceBytes;
        level.width=std::max(1u,level.width/2);level.height=std::max(1u,level.height/2);
        level.paddedWidth=padded(level.width);level.paddedHeight=padded(level.height);
    }
    return cursor==p.outputBytes;
}
}
