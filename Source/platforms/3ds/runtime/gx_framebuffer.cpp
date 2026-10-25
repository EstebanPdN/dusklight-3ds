#include "gx_framebuffer.hpp"
#include "alpha_interval_target.hpp"
#include "../gx_pica.hpp"
#include <array>
#include <cstring>
namespace dusk::native {
namespace {
struct Surface {FramebufferHandle handle{};gx::TargetExtent extent{};};
Surface mainSurface{},*active{};
std::array<Surface,8> surfaces{};
std::size_t usedBytes{};
bool inFrame{};
bool pendingClear{};
std::uint32_t clearColor{},clearDepth{};
constexpr std::size_t budget=4*1024*1024;
bool bind(Surface& s){
    if(!alphaIntervalCopySafe()||!s.handle||!s.extent.valid()||!platformBindFramebuffer(s.handle))return false;
    active=&s;gx::currentTargetExtent=s.extent;return true;
}
bool beginOffscreen(Surface& s){
    // Clear takes an explicit handle and flushes prior geometry before its
    // fill. Keep the previous active target/extent on either failure, so a
    // failed create cannot leave it paired with the previous raster state.
    return platformClearFramebuffer(s.handle,0,0)&&bind(s);
}
}
bool initializeFramebuffers(FramebufferHandle main,unsigned width,unsigned height){
    if(!alphaIntervalCopySafe()||mainSurface.handle||!main)return false;
    gx::TargetExtent extent{width,height,width,height};
    if(!extent.valid())return false;
    mainSurface={main,extent};return true;
}
bool configureMainFramebuffer(unsigned width,unsigned height){
    auto extent=mainSurface.extent;extent.logicalWidth=width;extent.logicalHeight=height;
    if(!alphaIntervalCopySafe()||!mainSurface.handle||!extent.valid()||(active&&active!=&mainSurface))return false;
    mainSurface.extent=extent;if(active==&mainSurface)gx::currentTargetExtent=extent;
    return true;
}
bool beginFramebufferFrame(){
    if(inFrame||!bind(mainSurface))return false;
    inFrame=true;return true;
}
bool beginFramebufferRender(){
    if(!alphaIntervalCopySafe()||!inFrame||active!=&mainSurface)return false;
    if(pendingClear){
        if(!platformClearFramebuffer(mainSurface.handle,clearColor,clearDepth))return false;
        pendingClear=false;
    }
    return true;
}
bool clearFramebufferForRender(std::uint32_t color,std::uint32_t depth){
    if(!alphaIntervalCopySafe()||!inFrame||active!=&mainSurface)return false;
    // JFW explicitly clears the entire main EFB before new drawing. Its latest
    // clear replaces an earlier GXCopyDisp clear; submitting both is redundant.
    pendingClear=true;clearColor=color;clearDepth=depth;
    return beginFramebufferRender();
}
bool endFramebufferFrame(){
    if(!alphaIntervalCopySafe()||!inFrame||active!=&mainSurface)return false;
    inFrame=false;active=nullptr;return true;
}
bool beginOffscreenFramebuffer(unsigned width,unsigned height){
    if(!alphaIntervalCopySafe()||!inFrame||!active||!width||!height||width>1024||height>1024)return false;
    for(auto& s:surfaces)if(s.handle&&s.extent.logicalWidth==width&&s.extent.logicalHeight==height)
        return beginOffscreen(s);
    auto extent=gx::TargetExtent{width,height,(width+7)&~7u,(height+7)&~7u};
    const std::size_t bytes=std::size_t(extent.pixelWidth)*extent.pixelHeight*8;
    if(bytes>budget-usedBytes)return false;
    for(auto& s:surfaces)if(!s.handle){
        auto handle=platformCreateFramebuffer(extent.pixelWidth,extent.pixelHeight);
        if(!handle)return false;
        s={handle,extent};usedBytes+=bytes;
        return beginOffscreen(s);
    }
    return false;
}
bool restoreMainFramebuffer(){return alphaIntervalCopySafe()&&inFrame&&active&&active!=&mainSurface&&bind(mainSurface);}
FramebufferHandle activeFramebuffer(){return inFrame&&active?active->handle:nullptr;}
FramebufferHandle mainFramebuffer(){return mainSurface.handle;}
bool deferMainFramebufferClear(std::uint32_t color,std::uint32_t depth){
    if(!alphaIntervalCopySafe()||!inFrame||active!=&mainSurface)return false;
    pendingClear=true;clearColor=color;clearDepth=depth;return true;
}
std::size_t offscreenFramebufferBytes(){return usedBytes;}
void releaseFramebufferRecords(){alphaIntervalTargets.release();mainSurface={};surfaces={};active=nullptr;inFrame=false;pendingClear=false;usedBytes=0;gx::currentTargetExtent={};}
bool sampleFramebufferDepthImage(const gx::TargetExtent& extent,std::span<const std::byte> snapshot,
                                 unsigned x,unsigned y,std::uint32_t& out){
    if(!extent.valid()||(extent.pixelWidth&7)||(extent.pixelHeight&7)||
       x>=extent.logicalWidth||y>=extent.logicalHeight||
       snapshot.size()<std::size_t(extent.pixelWidth)*extent.pixelHeight*4)return false;
    const unsigned px=extent.pixelHeight-1-std::uint64_t(y)*extent.pixelHeight/extent.logicalHeight;
    const unsigned py=std::uint64_t(x)*extent.pixelWidth/extent.logicalWidth;
    unsigned morton{};for(unsigned bit=0;bit<3;++bit){morton|=((px>>bit)&1)<<(2*bit);morton|=((py>>bit)&1)<<(2*bit+1);}
    const auto at=(std::size_t(py/8)*(extent.pixelHeight/8)+px/8)*64+morton;
    std::uint32_t value;std::memcpy(&value,snapshot.data()+at*4,4);
    out=0xffffff-(value&0xffffff);return true;
}
}
