#include "gx_raster_plan.hpp"
#include "gx_commands.hpp"
#include <algorithm>
namespace dusk::native::gx {
bool planRaster(const CommandRegisters& r,RasterPlan& out,RasterError* error,FogPolicy fogPolicy,ColorStoragePolicy colorPolicy,ClipPolicy clipPolicy,bool validateConstantEarlyAlpha,bool allowAlphaUnion,AlphaIntervalPolicy intervalPolicy){
    out={};if(error)*error=RasterError::none;
    auto fail=[&](RasterError e){if(error)*error=e;return false;};
    for(unsigned reg:{0u,0x20u,0x21u,0x40u,0x41u,0x42u,0x43u,0xf1u,0xf3u,0xf5u})
        if(!r.bpValid[reg])return fail(RasterError::missingState);
    const unsigned pixelFormat=r.bp[0x43]&7;
    if((r.bp[0x43]&56)||(pixelFormat!=0&&
       !(pixelFormat==1&&colorPolicy==ColorStoragePolicy::rgba8ForRgba6)))return fail(RasterError::pixelFormat);
    if(r.bp[0x42]&256)return fail(RasterError::destinationAlpha);
    if((r.bp[0xf1]>>20) && fogPolicy==FogPolicy::requireSupported)return fail(RasterError::fog);
    // Typed global-state setters must not silently enable unsupported GX
    // coplanar depth or disabled clipping while PICA still uses defaults.
    if(r.bp[0]&(1u<<19))return fail(RasterError::coPlanar);
    const bool clipDisabled=r.xfValid[0x1005]&&r.xf[0x1005]!=0;
    if(clipDisabled&&clipPolicy==ClipPolicy::requireEnabled)return fail(RasterError::clipMode);
    if((r.bp[0xf5]>>2)&3)return fail(RasterError::zTexture);
    // Original pinned renderer uses the default 342-pixel scissor bias. Other
    // box-offset semantics need explicit support, not accidental clipping.
    if(r.bpValid[0x59]&&(r.bp[0x59]&0x3ffff)!=(171u|(171u<<9)))return fail(RasterError::scissorOffset);
    const auto blend=r.bp[0x41],depth=r.bp[0x40],alpha=r.bp[0xf3];
    if(blend&(1u<<1))return fail(RasterError::blend);
    RasterPlan p;p.clipDisabled=clipDisabled;
    p.blend={std::uint8_t(blend&1),std::uint8_t((blend>>8)&7),std::uint8_t((blend>>5)&7),std::uint8_t((blend>>12)&15)};
    p.subtractBlend=(blend&1)&&(blend&(1u<<11));
    p.depth={bool(depth&1),std::uint8_t((depth>>1)&7),bool(depth&16)};
    p.storesAlpha=pixelFormat==1;
    p.writeRgb=blend&8;p.writeAlpha=p.storesAlpha&&(blend&16);
    const AlphaCompare compare{std::uint8_t((alpha>>16)&7),std::uint8_t(alpha),std::uint8_t((alpha>>22)&3),
                               std::uint8_t((alpha>>19)&7),std::uint8_t(alpha>>8)};
    if(!reduceAlpha(compare,p.alpha)){
        std::array<AlphaPlan,2> pair;
        if(allowAlphaUnion&&splitAlphaUnion(compare,pair)){
            p.alpha=pair[0];p.alphaSecond=pair[1];p.alphaUnion=true;
        }else{
            if(!intervalPolicy.enabled||!planAlphaInterval(compare,p.alphaInterval))return fail(RasterError::alpha);
            if(!intervalPolicy.ownsTarget)return fail(RasterError::alphaIntervalOwnership);
            // Keep UI strict. This candidate has no orthographic exception.
            if(!r.xfValid[0x1026]||r.xf[0x1026]!=0)return fail(RasterError::alphaIntervalUi);
            // Only the explicit vertex path may compose fog with exact final-TEV
            // alpha. Its appended RGB stage preserves previous alpha; the adapter
            // still validates parameters, input carriers and TEV/texture capacity.
            if((r.bp[0xf1]>>20)&&fogPolicy!=FogPolicy::vertexApproximation)return fail(RasterError::fog);
            p.alpha={6,p.alphaInterval.lower};p.alphaSecond={3,p.alphaInterval.upper};
            p.alphaIntervalPolicy=intervalPolicy;
        }
    }
    p.earlyDepthAlpha=(r.bp[0x43]&64)&&p.depth.enabled&&p.depth.write&&p.alpha.comparison!=7;
    if(p.earlyDepthAlpha&&(!validateConstantEarlyAlpha||p.alphaUnion||p.alphaInterval.enabled))return fail(RasterError::earlyDepth);
    const auto hwCull=(r.bp[0]>>14)&3;p.cull=hwCull==1?2:hwCull==2?1:hwCull;
    const int x=int((r.bp[0x20]>>12)&2047)-342,y=int(r.bp[0x20]&2047)-342;
    const int right=int((r.bp[0x21]>>12)&2047)-341,bottom=int(r.bp[0x21]&2047)-341;
    if(!currentTargetExtent.valid())return fail(RasterError::missingState);
    const int w=currentTargetExtent.logicalWidth,h=currentTargetExtent.logicalHeight;
    const int x0=std::clamp(x,0,w),y0=std::clamp(y,0,h),x1=std::clamp(right,0,w),y1=std::clamp(bottom,0,h);
    p.scissor=picaScissor({unsigned(x0),unsigned(y0),unsigned(std::max(0,x1-x0)),unsigned(std::max(0,y1-y0))});
    out=p;return true;
}
const char* rasterErrorText(RasterError e){
    switch(e){case RasterError::none:return "none";
    case RasterError::missingState:return "GX raster state missing";
    case RasterError::blend:return "GX logic blend unsupported";
    case RasterError::earlyDepth:return "GX early depth plus alpha unsupported";
    case RasterError::alphaIntervalOwnership:return "GX alpha interval stencil ownership unproved";
    case RasterError::alphaIntervalUi:return "GX alpha interval orthographic/missing projection unsupported";
    case RasterError::alpha:return "GX dual alpha comparison unsupported";
    case RasterError::pixelFormat:return "GX pixel/depth format unsupported";
    case RasterError::destinationAlpha:return "GX destination alpha override unsupported";
    case RasterError::fog:return "GX fog effect pending";
    case RasterError::coPlanar:return "GX coplanar depth effect pending";
    case RasterError::clipMode:return "GX disabled clipping pending";
    case RasterError::zTexture:return "GX depth texture effect pending";
    case RasterError::scissorOffset:return "GX nondefault scissor box offset pending";}
    return "unknown raster error";
}
}
