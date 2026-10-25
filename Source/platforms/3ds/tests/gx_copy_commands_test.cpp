#include "../runtime/gx_stream.hpp"
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){
    char text[256];va_list args;va_start(args,message);std::vsnprintf(text,sizeof(text),message,args);va_end(args);
    throw std::runtime_error(text);
}
void OriginalCopyReset(u32,u32);
u32 OriginalCopyField(unsigned);
const std::vector<u32>& OriginalCopyWrites();
void OriginalGXSetDispCopySrc(u16,u16,u16,u16);
void OriginalGXSetTexCopySrc(u16,u16,u16,u16);
void OriginalGXSetDispCopyDst(u16,u16);
void OriginalGXSetTexCopyDst(u16,u16,GXTexFmt,GXBool);
void OriginalGXSetCopyClamp(GXFBClamp);
void OriginalGXSetDispCopyGamma(GXGamma);
u32 OriginalGXSetDispCopyYScale(f32);
void OriginalGXSetCopyClear(GXColor,u32);
void OriginalGXSetCopyFilter(GXBool,const u8[12][2],GXBool,const u8[7]);
template<class F>void rejected(F action){bool failure=false;try{action();}catch(const std::runtime_error&){failure=true;}assert(failure);}
u32 packedSource(const CopySurface& s){return s.source[0]|u32(s.source[1])<<10;}
u32 packedSize(const CopySurface& s){return (s.source[2]-1)|u32(s.source[3]-1)<<10;}
std::vector<u8> packet(u16 command,std::initializer_list<u32> values){
    std::vector<u8> bytes{GX_AURORA,u8(command>>8),u8(command)};
    for(u32 word:values)for(int shift=24;shift>=0;shift-=8)bytes.push_back(word>>shift);
    return bytes;
}
int main(){
    CommandMemory memory;CommandProcessor p(memory);std::array<u8,64> scratch{};CommandStream stream(p,scratch);
    assert(setCommandStream(&stream));unsigned comparisons=0;
    for(u16 width:{1,2,31,64,608,1024})for(u16 height:{1,3,240,448,1024}){
        assert(stream.reset());OriginalCopyReset(0x82a5e3,0x1748ad);
        const u16 left=1024-width,top=1024-height;
        GXSetDispCopySrc(left,top,width,height);OriginalGXSetDispCopySrc(left,top,width,height);
        GXSetTexCopySrc(0,0,height,width);OriginalGXSetTexCopySrc(0,0,height,width);
        const auto& c=p.registers().copy;
        assert(c.display.sourceReady&&c.texture.sourceReady);
        assert(packedSource(c.display)==(OriginalCopyField(0)&0xffffff)&&packedSize(c.display)==(OriginalCopyField(1)&0xffffff));
        assert(packedSource(c.texture)==(OriginalCopyField(2)&0xffffff)&&packedSize(c.texture)==(OriginalCopyField(3)&0xffffff));
        assert(!p.registers().bpValid[0x49]&&!p.registers().bpValid[0x4a]); // Deferred, independent setup.
        ++comparisons;
    }
    for(u16 width:{16,32,608,640,1024})for(u16 height:{1,240,448,1024}){
        assert(stream.reset());OriginalCopyReset(0,0);
        GXSetDispCopyDst(width,height);OriginalGXSetDispCopyDst(width,height);
        const auto& c=p.registers().copy.display;
        assert(c.destinationReady&&c.width==width&&c.height==height&&c.stride==(OriginalCopyField(4)&0xffffff));++comparisons;
    }
    constexpr u32 formats[]{0,1,2,3,4,5,6,8,9,10,GX_CTF_R4,GX_CTF_RA4,GX_CTF_RA8,
        GX_CTF_A8,GX_CTF_R8,GX_CTF_G8,GX_CTF_B8,GX_CTF_RG8,GX_CTF_GB8,
        GX_TF_Z8,GX_TF_Z16,GX_TF_Z24X8,GX_CTF_Z4,GX_CTF_Z8M,GX_CTF_Z8L,GX_CTF_Z16L};
    for(u32 format:formats)for(u16 width:{1,3,7,64,608,1024})for(u16 height:{1,9,448})for(bool mipmap:{false,true}){
        assert(stream.reset());OriginalCopyReset(0x82a5e3,0x1748ad);
        assert(stream.setCopyControl(true,0xffffff,0x1748ad));
        GXSetTexCopyDst(width,height,static_cast<GXTexFmt>(format),mipmap);
        try { OriginalGXSetTexCopyDst(width,height,static_cast<GXTexFmt>(format),mipmap); }
        catch (...) { std::fprintf(stderr,"Original destination rejected format=%u width=%u height=%u\n",format,width,height);throw; }
        const auto& c=p.registers().copy.texture;
        assert(c.destinationReady&&c.width==width&&c.height==height&&u32(c.format)==format&&c.mipmap==mipmap);
        assert(c.stride==(OriginalCopyField(5)&0xffffff)&&c.control==(OriginalCopyField(7)&0xffffff)&&c.depth==bool(OriginalCopyField(8)));
        ++comparisons;
    }
    for(unsigned clamp=0;clamp<4;++clamp)for(unsigned gamma=0;gamma<3;++gamma){
        assert(stream.reset());OriginalCopyReset(0x82a5e3,0x1748ad);
        assert(stream.setCopyControl(false,0xffffff,0x82a5e3)&&stream.setCopyControl(true,0xffffff,0x1748ad));
        GXSetCopyClamp(static_cast<GXFBClamp>(clamp));OriginalGXSetCopyClamp(static_cast<GXFBClamp>(clamp));
        GXSetDispCopyGamma(static_cast<GXGamma>(gamma));OriginalGXSetDispCopyGamma(static_cast<GXGamma>(gamma));
        assert(p.registers().copy.display.control==(OriginalCopyField(6)&0xffffff)&&p.registers().copy.texture.control==(OriginalCopyField(7)&0xffffff));++comparisons;
    }
    for(u16 height:{1,2,128,240,448,480,528,1024})for(unsigned q=1;q<=256;++q){
        assert(stream.reset());OriginalCopyReset(0x82a5e3,0);
        assert(stream.setCopyControl(false,0xffffff,0x82a5e3));
        GXSetDispCopySrc(0,0,608,height);OriginalGXSetDispCopySrc(0,0,608,height);
        const float scale=256.f/q;
        assert(GXSetDispCopyYScale(scale)==OriginalGXSetDispCopyYScale(scale));
        assert(p.registers().copy.display.control==(OriginalCopyField(6)&0xffffff));
        assert(OriginalCopyWrites().size()==1&&p.registers().bp[0x4e]==(OriginalCopyWrites()[0]&0xffffff));++comparisons;
    }
    for(unsigned seed=0;seed<256;++seed){
        assert(stream.reset());OriginalCopyReset(0,0);
        GXColor color{u8(seed),u8(seed*3),u8(seed*7),u8(seed*11)};u32 depth=seed*0x010101;
        GXSetCopyClear(color,depth);OriginalGXSetCopyClear(color,depth);
        for(u32 word:OriginalCopyWrites())assert(p.registers().bp[word>>24]==(word&0xffffff));++comparisons;
        for(bool aa:{false,true})for(bool vertical:{false,true}){
            assert(stream.reset());OriginalCopyReset(0,0);
            u8 samples[12][2]{};u8 filter[7]{};
            for(unsigned i=0;i<12;++i){samples[i][0]=(seed+i*3)%16;samples[i][1]=(seed+i*7)%16;}
            for(unsigned i=0;i<7;++i)filter[i]=(seed+i*13)%64;
            GXSetCopyFilter(aa,aa?samples:nullptr,vertical,vertical?filter:nullptr);
            OriginalGXSetCopyFilter(aa,samples,vertical,filter);
            assert(OriginalCopyWrites().size()==6);
            for(u32 word:OriginalCopyWrites())assert(p.registers().bp[word>>24]==(word&0xffffff));++comparisons;
        }
    }
    // Original Aurora packets and typed calls converge on the same metadata.
    auto src=packet(GX_AURORA_LOAD_COPY_SRC,{16,24,304,224});
    auto dst=packet(GX_AURORA_LOAD_COPY_DST,{152,112,GX_TF_RGBA8});dst.push_back(1);
    auto dest=packet(GX_AURORA_LOAD_COPY_DEST,{0x12345678,0x9abcdee0});
    for(const auto& bytes:{src,dst,dest}){
        for(size_t split=0;split<=bytes.size();++split){
            assert(stream.reset());assert(stream.write({bytes.data(),split}));
            assert(stream.write({bytes.data()+split,bytes.size()-split})&&stream.idle());
        }
        for(size_t length=0;length<bytes.size();++length){
            assert(stream.reset());assert(stream.write({bytes.data(),length}));
            if(length)assert(!stream.finish()&&stream.error()==StreamError::truncated);
        }
    }
    assert(stream.reset());assert(stream.call(src)&&stream.call(dst)&&stream.call(dest));
    const auto& copy=p.registers().copy;
    assert((copy.texture.source==std::array<u16,4>{16,24,304,224}));
    assert(copy.texture.width==152&&copy.texture.height==112&&copy.texture.mipmap);
    assert(copy.textureDestinationReady&&copy.textureDestination==0x123456789abcdee0ULL);
    // Trigger rejection must leave both the output bytes and trigger register untouched.
    alignas(32)std::array<u8,64> sentinel;sentinel.fill(0xa5);
    MemoryHandle sentinelHandle{};assert(memory.add(sentinel,sentinelHandle));
    const auto address=u64(reinterpret_cast<std::uintptr_t>(sentinel.data()));
    auto validDestination=packet(GX_AURORA_LOAD_COPY_DEST,{u32(address>>32),u32(address)});
    assert(stream.call(validDestination)&&p.registers().copy.textureDestination==address);
    std::array<u8,5> trigger{GX_LOAD_BP_REG,0x52,0,0,0};
    assert(!stream.call(trigger)&&p.error()==CommandError::unsupported&&!p.registers().bpValid[0x52]);
    for(auto byte:sentinel)assert(byte==0xa5);
    assert(memory.remove(sentinelHandle));
    assert(stream.reset());assert(!stream.writeBp(0x52,0)&&p.error()==CommandError::unsupported);
    assert(stream.reset());rejected([&]{GXSetDispCopyDst(15,448);});
    assert(stream.reset());rejected([&]{GXSetTexCopySrc(1023,0,2,1);});
    assert(stream.reset());rejected([&]{GXSetTexCopyDst(1,1,GX_TF_CMPR,GX_FALSE);});
    assert(stream.reset());rejected([&]{GXSetTexCopyDst(1,1,GX_CTF_YUVA8,GX_FALSE);});
    OriginalCopyReset(0,0);rejected([&]{OriginalGXSetTexCopyDst(1,1,GX_CTF_YUVA8,GX_FALSE);});
    assert(stream.reset());rejected([&]{GXSetCopyClear({},0x1000000);});
    assert(stream.reset());rejected([&]{GXSetCopyFilter(GX_TRUE,nullptr,GX_FALSE,nullptr);});
    assert(stream.reset());u8 invalidFilter[7]{0,0,0,0,0,0,64};rejected([&]{GXSetCopyFilter(GX_FALSE,nullptr,GX_TRUE,invalidFilter);});
    assert(stream.reset());rejected([&]{GXSetDispCopyGamma(static_cast<GXGamma>(3));});
    assert(stream.reset());rejected([&]{GXSetCopyClamp(static_cast<GXFBClamp>(4));});
    assert(stream.reset());rejected([&]{GXSetDispCopyYScale(1);});
    assert(stream.reset());GXSetDispCopySrc(0,0,608,448);rejected([&]{GXSetDispCopyYScale(std::numeric_limits<float>::infinity());});
    assert(stream.reset());dst.back()=2;assert(!stream.call(dst)&&p.error()==CommandError::format);
    assert(stream.reset());dest.back()=1;assert(!stream.call(dest)&&p.error()==CommandError::format);
    assert(stream.reset());auto overflow=packet(GX_AURORA_LOAD_COPY_SRC,{0,0,65536,1});assert(!stream.call(overflow)&&p.error()==CommandError::format);
    assert(stream.reset());assert(setCommandStream(nullptr));
    std::printf("PASS: %u comparisons with actual original GX copy setup, independent display/texture state, Aurora packet splits/addresses, invalid input and rejection before an unimplemented EFB transfer\n",comparisons);
}
