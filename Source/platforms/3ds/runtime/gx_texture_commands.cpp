#include "gx_compact_cache.hpp"
#include <dolphin/os.h>
#include "gx_texture_commands.hpp"
#include "gx_commands.hpp"
namespace dusk::native::gx {
namespace {
bool cachedRegion(u32 value,bool odd) {
    // IMAGE1/2 cache width/height exponents describe original TMEM policy.
    // PICA samples the validated converted image, without raw TMEM addressing.
    // Explicit preload mode, reserved bits and asymmetric exponents reject.
    if(value&~0x1fffffu)return false;
    const unsigned width=(value>>15)&7,height=(value>>18)&7;
    return width==height&&((width>=3&&width<=5)||(odd&&width==0));
}
}
bool resolveCommandTexture(const CommandRegisters& r,unsigned map,Texture& out,const char*& failure){
    out={};failure="none";
    auto fail=[&](const char* why){failure=why;return false;};
    if(map>=8)return fail("GX command texture map invalid");
    const unsigned offset=(map/4)*32+map%4;
    bool any=false;
    for(unsigned base:{0x80u,0x84u,0x88u,0x8cu,0x90u,0x94u,0x98u})any|=r.bpValid[base+offset];
    if(!any){if(textureBinding(map,out))return true;return fail("GX command texture map missing/stale");}
    if((r.bpValid[0x8c+offset]&&!cachedRegion(r.bp[0x8c+offset],false))||
       (r.bpValid[0x90+offset]&&(!r.bpValid[0x8c+offset]||!cachedRegion(r.bp[0x90+offset],true))))
        return fail("GX texture cache region unsupported");
    for(unsigned base:{0x80u,0x84u,0x88u,0x94u})
        if(!r.bpValid[base+offset])return fail("GX command texture register set incomplete");
    Texture texture;
    if(watchedCaptureToken&&r.bp[0x94+offset]==watchedCaptureToken){
        watchedCaptureLastUse=textureFrameCounter;
        {static unsigned reports;if(reports<6){++reports;OSReport("Native capture consumer #%u: map=%u frame=%llu tevStages=%u list=%p gen=%u proj=%u\n",reports,map,
            static_cast<unsigned long long>(textureFrameCounter),unsigned((r.bp[0]>>10)&15)+1,gx::staticGeometryHint.list,unsigned(gx::staticGeometryHint.generation),
            unsigned(r.xfValid[0x1026]?r.xf[0x1026]:9));}}
        if(watchedCaptureMaterialize)watchedCaptureMaterialize();
    }
    if(!textureFromCommandToken(r.bp[0x94+offset],texture))return fail("GX command texture token missing/stale");
    const auto attr=r.bp[0x88+offset],mode=r.bp[0x80+offset],lod=r.bp[0x84+offset];
    if((attr&1023)+1!=texture.width||((attr>>10)&1023)+1!=texture.height||(attr>>20)!=texture.format)
        return fail("GX command texture dimensions/format mismatch");
    if(texture.bti[8]){
        if(r.tlutOverflow||!r.bpValid[0x98+offset])return fail("GX command TLUT load missing");
        const auto select=r.bp[0x98+offset];
        if(select>>12||((select>>10)&3)>2||((select>>10)&3)!=texture.bti[9])return fail("GX command TLUT format mismatch");
        const unsigned colors=(unsigned(texture.bti[10])<<8)|texture.bti[11];
        bool matched=false;
        for(const auto& load:r.tluts)if(load.entries&&load.offset==(select&1023)&&load.entries>=colors){
            Texture palette;
            matched=textureFromCommandToken(load.token,palette)&&texturePaletteMatches(texture,palette);break;
        }
        if(!matched)return fail("GX command TLUT missing/stale/palette mismatch");
    }
    const unsigned min=(mode>>5)&7,wrapS=mode&3,wrapT=(mode>>2)&3;
    constexpr unsigned allowed=3u|(3u<<2)|(1u<<4)|(7u<<5)|(1u<<8)|(255u<<9)|(1u<<21);
    constexpr unsigned filters[]{0,2,4,255,1,3,5,255};
    const unsigned low=lod&255,high=(lod>>8)&255;
    const float bias=static_cast<std::int8_t>((mode>>9)&255)/32.f;
    if((lod>>16)||(low%16)||(high%16)||low>high||high/16>=texture.mipCount||
       (mode&~allowed)||filters[min]==255||wrapS>2||wrapT>2||
       ((mode&(1u<<21))&&(texture.mipCount>1||bias!=0)))
        return fail("GX command texture sampler unsupported");
    texture.wrapS=wrapS;texture.wrapT=wrapT;texture.minFilter=filters[min];texture.magFilter=(mode>>4)&1;
    texture.minLod=low/16;texture.maxLod=high/16;texture.lodBias=bias;
    out=texture;return true;
}
}
