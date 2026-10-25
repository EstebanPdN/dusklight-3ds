#include "../runtime/gx_commands.hpp"
#include "../runtime/gx_texture_commands.hpp"
#include "../runtime/gx_state.hpp"
#include "../runtime/gx_stream.hpp"
#include "JSystem/J3DGraphBase/J3DGD.h"
#include "JSystem/J3DGraphBase/J3DFifo.h"
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <stdexcept>
using namespace dusk::native::gx;
void OriginalGXInitTexCacheRegion(GXTexRegion*,u8,u32,GXTexCacheSize,u32,GXTexCacheSize);
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
int main(){
    std::array<std::array<u8,96>,2> images{};
    TextureHandle handles[2]{};
    for(unsigned i=0;i<2;++i){auto& t=images[i];t[0]=1;t[3]=t[5]=8;t[24]=1;t[31]=32;
        assert(registerTexture(t,handles[i]));}
    CommandMemory memory;CommandProcessor processor(memory);
    alignas(32) u8 bytes[256];GDLObj gd;
    auto emit=[&](unsigned map,unsigned image,GXTexFilter min){
        GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);
        const auto id=static_cast<GXTexMapID>(map);
        J3DGDSetTexImgPtr(id,images[image].data()+32);
        J3DGDSetTexImgAttr(id,8,8,GX_TF_I8);
        J3DGDSetTexLookupMode(id,GX_CLAMP,GX_CLAMP,min,GX_LINEAR,0,0,0,GX_TRUE,GX_TRUE,GX_ANISO_1);
        const auto count=GDGetCurrOffset();GDSetCurrent(nullptr);
        assert(count==20&&processor.execute({bytes,count}));
    };
    const char* failure;Texture texture;
    for(unsigned map=0;map<8;++map){
        emit(map,map%2,map%2?GX_LINEAR:GX_NEAR);
        assert(resolveCommandTexture(processor.registers(),map,texture,failure));
        assert(texture.handle==handles[map%2]&&texture.minFilter==map%2&&texture.magFilter==1);
        assert(texture.image.data()==images[map%2].data()+32);
    }
    unsigned regionCases=0,originalFifoCases=0;
    constexpr u32 addresses[]{0,32,0x7ffe0,0x80000,0xc0000,0xfffe0};
    for(u32 even:addresses)for(u32 odd:addresses)for(unsigned a=0;a<3;++a)for(unsigned b=0;b<4;++b)for(unsigned mip=0;mip<2;++mip){
        GXTexRegion native,original;std::memset(&native,0xa5,sizeof(native));original=native;
        GXInitTexCacheRegion(&native,mip,even,static_cast<GXTexCacheSize>(a),odd,static_cast<GXTexCacheSize>(b));
        OriginalGXInitTexCacheRegion(&original,mip,even,static_cast<GXTexCacheSize>(a),odd,static_cast<GXTexCacheSize>(b));
        assert(!std::memcmp(&native,&original,sizeof(native)));++regionCases;
    }
    // Execute the exact original draw-initialization cache encoder through the
    // native stream. This formerly rejected otherwise legal texture bindings.
    std::array<u8,32> scratch{};CommandStream stream(processor,scratch);
    assert(setCommandStream(&stream));
    constexpr u32 sizes[]{0x8000,0x20000,0x80000};
    for(unsigned map=0;map<8;++map)for(unsigned a=0;a<3;++a)for(unsigned b=0;b<4;++b)for(unsigned swap=0;swap<2;++swap){
        assert(stream.reset());emit(map,map%2,GX_LINEAR);
        const u32 even=(map*sizes[a])%0x80000+(swap?0x80000:0);
        const u32 odd=even^0x80000;
        J3DFifoLoadTexCached(static_cast<GXTexMapID>(map),even,static_cast<GXTexCacheSize>(a),odd,static_cast<GXTexCacheSize>(b));
        assert(stream.finish()&&resolveCommandTexture(processor.registers(),map,texture,failure));
        assert(texture.handle==handles[map%2]);
        const unsigned offset=(map/4)*32+map%4;
        const auto& r=processor.registers();
        assert(r.bp[0x8c+offset]==((even>>5)|((a+3)<<15)|((a+3)<<18)));
        assert(r.bpValid[0x90+offset]==(b!=GX_TEXCACHE_NONE));
        if(b!=GX_TEXCACHE_NONE)assert(r.bp[0x90+offset]==((odd>>5)|((b+3)<<15)|((b+3)<<18)));
        ++originalFifoCases;
    }
    assert(stream.reset());
    const u8 incomplete=0x61;assert(stream.write({&incomplete,1}));
    GXTexRegion interrupted;std::memset(&interrupted,0x6b,sizeof(interrupted));const auto unchanged=interrupted;
    bool interruptedCaught=false;try{GXInitTexCacheRegion(&interrupted,0,0,GX_TEXCACHE_32K,0,GX_TEXCACHE_32K);}catch(const std::runtime_error&){interruptedCaught=true;}
    assert(interruptedCaught&&!std::memcmp(&interrupted,&unchanged,sizeof(interrupted)));
    assert(stream.reset()&&setCommandStream(nullptr));
    for(unsigned map=0;map<8;++map)emit(map,map%2,map%2?GX_LINEAR:GX_NEAR);
    // Reject invalid opaque descriptors before changing any caller storage.
    auto invalidRegion=[&](u32 even,unsigned a,u32 odd,unsigned b){
        GXTexRegion target;std::memset(&target,0x5a,sizeof(target));const auto prior=target;
        bool caught=false;try{GXInitTexCacheRegion(&target,0,even,static_cast<GXTexCacheSize>(a),odd,static_cast<GXTexCacheSize>(b));}catch(const std::runtime_error&){caught=true;}
        assert(caught&&!std::memcmp(&target,&prior,sizeof(target)));
    };
    invalidRegion(1,0,0,0);invalidRegion(0,0,1,0);invalidRegion(0x100000,0,0,0);invalidRegion(0,0,0x100000,0);
    invalidRegion(0,3,0,0);invalidRegion(0,4,0,0);invalidRegion(0,0,0,4);
    bool nullCaught=false;try{GXInitTexCacheRegion(nullptr,0,0,GX_TEXCACHE_32K,0,GX_TEXCACHE_32K);}catch(const std::runtime_error&){nullCaught=true;}assert(nullCaught);
    // A generated list is independent of the current typed object/map. Its
    // registration identity remains valid until the owner releases the image.
    u32 oldToken;assert(textureCommandToken(images[0].data()+32,oldToken)&&oldToken>=0x800001);
    assert(!textureFromCommandToken(u16(handles[0]),texture)); // Unpatched J3D texture index.
    assert(!textureFromCommandToken(0x1000000,texture));
    auto regs=processor.registers();
    auto reject=[&](const char* reason){assert(!resolveCommandTexture(regs,0,texture,failure));
        assert(!texture.handle&&std::strcmp(reason,failure)==0);regs=processor.registers();};
    regs.bp[0x88]++;reject("GX command texture dimensions/format mismatch");
    regs.bp[0x84]=1;reject("GX command texture sampler unsupported");
    regs.bp[0x80]=(regs.bp[0x80]&~(1u<<21))|(1u<<9);assert(resolveCommandTexture(regs,0,texture,failure)&&texture.lodBias==1.f/32);regs=processor.registers();
    regs.bp[0x80]=(regs.bp[0x80]&~224u)|(3u<<5);reject("GX command texture sampler unsupported");
    regs.bpValid.set(0x98);regs.bp[0x98]=0xffff;assert(resolveCommandTexture(regs,0,texture,failure));regs=processor.registers(); // Unused TLUT state does not affect direct images.
    for(unsigned exponent:{0u,1u,2u,6u,7u}){
        regs.bpValid.set(0x8c);regs.bp[0x8c]=(exponent<<15)|(exponent<<18);reject("GX texture cache region unsupported");
    }
    for(u32 unsupported:{1u<<21,1u<<22,1u<<23,(3u<<15)|(4u<<18)}){
        regs.bpValid.set(0x8c);regs.bp[0x8c]=unsupported;reject("GX texture cache region unsupported");
    }
    regs.bpValid.set(0x90);regs.bp[0x90]=0;reject("GX texture cache region unsupported");
    regs.bpValid.set(0x8c);regs.bp[0x8c]=(3u<<15)|(3u<<18);
    regs.bpValid.set(0x90);regs.bp[0x90]=0x4000;assert(resolveCommandTexture(regs,0,texture,failure));regs=processor.registers();
    regs.bpValid.reset(0x94);reject("GX command texture register set incomplete");
    regs.bp[0x94]=u16(handles[0]);reject("GX command texture token missing/stale");
    assert(unregisterTexture(handles[0]));
    assert(!resolveCommandTexture(processor.registers(),0,texture,failure));
    assert(!textureFromCommandToken(oldToken,texture));
    assert(registerTexture(images[0],handles[0]));
    assert(!textureFromCommandToken(oldToken,texture));
    assert(!resolveCommandTexture(processor.registers(),0,texture,failure));
    emit(0,0,GX_LINEAR);assert(resolveCommandTexture(processor.registers(),0,texture,failure));
    assert(texture.handle==handles[0]);
    GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);
    bool caught=false;try{J3DGDSetTexImgPtr(GX_TEXMAP0,images[0].data()+33);}catch(const std::runtime_error&){caught=true;}
    assert(caught&&GDGetCurrOffset()==0);GDSetCurrent(nullptr);
    // Execute original J3D TLUT commands for two independently retained
    // embedded palettes. Later loads cannot overwrite an earlier map's latch.
    std::array<std::array<u8,96>,2> ci{};TextureHandle ciHandles[2]{};
    for(unsigned n=0;n<2;++n){auto& b=ci[n];b[0]=8;b[3]=b[5]=8;b[8]=1;b[9]=n+1;b[11]=16;b[15]=64;b[24]=1;b[31]=32;
        for(unsigned j=32;j<96;++j)b[j]=j*37+n*71;
        assert(registerTexture(b,ciHandles[n]));
    }
    auto ciEmit=[&](unsigned map,unsigned image,unsigned address){
        auto& b=ci[image];const auto id=static_cast<GXTexMapID>(map);
        GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);
        J3DGDSetTexImgPtr(id,b.data()+32);J3DGDSetTexImgAttr(id,8,8,static_cast<GXTexFmt>(8));
        J3DGDSetTexLookupMode(id,GX_CLAMP,GX_CLAMP,GX_LINEAR,GX_LINEAR,0,0,0,GX_FALSE,GX_TRUE,GX_ANISO_1);
        J3DGDLoadTlut(b.data()+64,address,GX_TLUT_16);J3DGDSetTexTlut(id,address,static_cast<GXTlutFmt>(b[9]));
        const auto count=GDGetCurrOffset();GDSetCurrent(nullptr);assert(count==55&&processor.execute({bytes,count}));
    };
    ciEmit(0,0,0xf0000);ciEmit(1,1,0xf2000);
    for(unsigned n=0;n<2;++n){assert(resolveCommandTexture(processor.registers(),n,texture,failure));assert(texture.handle==ciHandles[n]);}
    auto paletteRegs=processor.registers();paletteRegs.bp[0x98]^=1u<<10;
    assert(!resolveCommandTexture(paletteRegs,0,texture,failure)&&!std::strcmp(failure,"GX command TLUT format mismatch"));
    paletteRegs=processor.registers();paletteRegs.bp[0x98]|=1u<<12;
    assert(!resolveCommandTexture(paletteRegs,0,texture,failure));
    // An actual load into the same TMEM range retires the previous selection.
    GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);J3DGDLoadTlut(ci[1].data()+64,0xf0000,GX_TLUT_16);
    auto count=GDGetCurrOffset();GDSetCurrent(nullptr);assert(processor.execute({bytes,count}));
    assert(!resolveCommandTexture(processor.registers(),0,texture,failure)&&!std::strcmp(failure,"GX command TLUT missing/stale/palette mismatch"));
    assert(resolveCommandTexture(processor.registers(),1,texture,failure));
    ciEmit(0,0,0xf0000);
    u32 paletteToken;assert(paletteCommandToken(ci[0].data()+64,paletteToken));
    assert(unregisterTexture(ciHandles[0])&&!textureFromCommandToken(paletteToken,texture));
    assert(!resolveCommandTexture(processor.registers(),0,texture,failure));
    assert(resolveCommandTexture(processor.registers(),1,texture,failure));
    assert(unregisterTexture(ciHandles[1]));
    for(auto handle:handles)assert(unregisterTexture(handle));
    std::puts("PASS original J3D CI/TLUT encoding: distinct map/color selection, ordered TMEM replacement, format/bounds validation, palette and list lifetime");
    std::printf("PASS original J3D textures: %u original cache-region layouts, %u real FIFO cache encodings, eight maps, sampler/format/token validation, preload rejection and stale-list/address-reuse rejection\n",regionCases,originalFifoCases);
}
