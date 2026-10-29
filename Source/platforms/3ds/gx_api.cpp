#include "runtime/gx_state.hpp"
#include "runtime/gx_tev_commands.hpp"
#include "runtime/gx_stream.hpp"
#include <bit>
#include <dolphin/os.h>
#include "runtime/gx_lighting_commands.hpp"
#include "runtime/gx_vertex_commands.hpp"
#include "runtime/gx_transform_commands.hpp"
#include <dolphin/gx.h>
#if defined(DUSK_PICA_GAME_DRAW)
#include "native/game_draw_api.hpp"
#endif

// Match the pinned game ABI through its actual declarations. This translation
// unit deliberately includes no libctru/Citro3D headers or hardware FIFO writes.
namespace bridge = dusk::native::gx;
namespace {
void invalidateNativeCamera(){
#if defined(DUSK_PICA_GAME_DRAW)
    dusk::pica::game::invalidateCamera(); // Guard only; never derive native parameters here.
#endif
}
void scalar(float value){GXCmd1u32(std::bit_cast<u32>(value));}
[[noreturn]] void streamFailure(const char* call){OSPanic(__FILE__,__LINE__,"Native typed GX vertex failure: %s",call);}
void packed8(const char* call,u8 x){
    if(!bridge::commandStream())streamFailure(call);
    GXCmd1u8(x);
}
void packed16(const char* call,u16 x){
    if(!bridge::commandStream())streamFailure(call);
    GXCmd1u16(x);
}
}
extern "C" {
void GXSetViewport(f32 x,f32 y,f32 w,f32 h,f32 n,f32 f) { invalidateNativeCamera(); if (bridge::commandViewport(x,y,w,h,n,f)) return; bridge::setViewport(x,y,w,h,n,f); }
void GXSetScissor(u32 x,u32 y,u32 w,u32 h) { invalidateNativeCamera(); if (bridge::commandScissor(x,y,w,h)) return; bridge::setScissor(x,y,w,h); }
void GXSetProjection(const void* matrix,GXProjectionType type) { invalidateNativeCamera(); if (bridge::commandProjection(static_cast<const float*>(matrix),type)) return; bridge::setProjection(static_cast<const float*>(matrix),type); }
void GXLoadPosMtxImm(const void* matrix,u32 id) { if (bridge::commandPositionMatrix(static_cast<const float*>(matrix),id)) return; bridge::loadPositionMatrix(static_cast<const float*>(matrix),id); }
void GXLoadTexMtxImm(const void* matrix,u32 id,GXTexMtxType type) { if (bridge::commandTextureMatrix(static_cast<const float*>(matrix),id,type)) return; bridge::loadTextureMatrix(static_cast<const float*>(matrix),id,type); }
void GXLoadNrmMtxImm(const void* matrix,u32 id) {
    if (bridge::commandNormalMatrix(static_cast<const float*>(matrix),id)) return;
    bridge::unsupportedOperation("GXLoadNrmMtxImm");
}
void GXSetCurrentMtx(u32 id) { if (bridge::commandCurrentMatrix(id)) return; bridge::setCurrentMatrix(id); }
void GXSetBlendMode(GXBlendMode mode,GXBlendFactor src,GXBlendFactor dst,GXLogicOp op) { if (bridge::commandBlend(mode,src,dst,op)) return; bridge::setBlend(mode,src,dst,op); }
void GXSetZMode(GXBool enabled,GXCompare compare,GXBool write) { if (bridge::commandDepth(enabled,compare,write)) return; bridge::setDepth(enabled,compare,write); }
void GXSetAlphaCompare(GXCompare c0,u8 r0,GXAlphaOp op,GXCompare c1,u8 r1) { if (bridge::commandAlpha(c0,r0,op,c1,r1)) return; bridge::setAlpha(c0,r0,op,c1,r1); }
void GXSetCullMode(GXCullMode mode) { if (bridge::commandCull(mode)) return; bridge::setCull(mode); }
void GXSetZCompLoc(GXBool before) { if (bridge::commandZLocation(before)) return; bridge::setZCompareLocation(before); }
void GXSetLineWidth(u8 width,GXTexOffset offsets) { if (bridge::commandLineWidth(width,offsets)) return; bridge::setLineWidth(width,offsets); }
void GXSetNumIndStages(u8 count) { if (bridge::commandTevCount(count,true)) return; bridge::setNumIndirect(count); }
void GXSetTevDirect(GXTevStageID stage) { if (bridge::commandTevDirect(stage)) return; bridge::setTevDirect(stage); }
void GXSetTevOp(GXTevStageID stage,GXTevMode operation) { if (bridge::commandTevOperation(stage,operation)) return; bridge::setTevOperation(stage,operation); }
void GXSetTevOrder(GXTevStageID stage,GXTexCoordID coordinate,GXTexMapID texture,GXChannelID channel) { if (bridge::commandTevOrder(stage,coordinate,texture,channel)) return; bridge::setTevOrder(stage,coordinate,texture,channel); }
void GXSetTevColorIn(GXTevStageID stage,GXTevColorArg a,GXTevColorArg b,GXTevColorArg c,GXTevColorArg d) { if (bridge::commandTevInputs(stage,false,a,b,c,d)) return; bridge::setTevInputs(stage,false,a,b,c,d); }
void GXSetTevAlphaIn(GXTevStageID stage,GXTevAlphaArg a,GXTevAlphaArg b,GXTevAlphaArg c,GXTevAlphaArg d) { if (bridge::commandTevInputs(stage,true,a,b,c,d)) return; bridge::setTevInputs(stage,true,a,b,c,d); }
void GXSetTevColorOp(GXTevStageID stage,GXTevOp op,GXTevBias bias,GXTevScale scale,GXBool clamp,GXTevRegID output) { if (bridge::commandTevOperator(stage,false,op,bias,scale,clamp,output)) return; bridge::setTevOperator(stage,false,op,bias,scale,clamp,output); }
void GXSetTevAlphaOp(GXTevStageID stage,GXTevOp op,GXTevBias bias,GXTevScale scale,GXBool clamp,GXTevRegID output) { if (bridge::commandTevOperator(stage,true,op,bias,scale,clamp,output)) return; bridge::setTevOperator(stage,true,op,bias,scale,clamp,output); }
void GXSetTevColor(GXTevRegID id,GXColor color) { if (bridge::commandTevRegister(id,color.r,color.g,color.b,color.a,false)) return; bridge::setTevRegister(id,{color.r,color.g,color.b,color.a},false); }
void GXSetTevKColor(GXTevKColorID id,GXColor color) { if (bridge::commandTevRegister(id,color.r,color.g,color.b,color.a,true)) return; bridge::setTevRegister(id,{color.r,color.g,color.b,color.a},true); }
void GXSetTevKColorSel(GXTevStageID stage,GXTevKColorSel selection) { if (bridge::commandTevKonstSelection(stage,selection,false)) return; bridge::setTevKonstSelection(stage,selection,false); }
void GXSetTevKAlphaSel(GXTevStageID stage,GXTevKAlphaSel selection) { if (bridge::commandTevKonstSelection(stage,selection,true)) return; bridge::setTevKonstSelection(stage,selection,true); }
void GXSetNumTevStages(u8 count) { if (bridge::commandTevCount(count,false)) return; bridge::setNumTevStages(count); }
void GXSetChanAmbColor(GXChannelID channel,GXColor color) {
    if (bridge::commandChannelColor(channel,(u32(color.r)<<24)|(u32(color.g)<<16)|(u32(color.b)<<8)|color.a,false)) return;
    bridge::unsupportedOperation("GXSetChanAmbColor");
}
void GXSetChanMatColor(GXChannelID channel,GXColor color) {
    if (bridge::commandChannelColor(channel,(u32(color.r)<<24)|(u32(color.g)<<16)|(u32(color.b)<<8)|color.a,true)) return;
    bridge::unsupportedOperation("GXSetChanMatColor");
}
void GXSetNumChans(u8 count) { if (bridge::commandNumChannels(count)) return; bridge::setNumChannels(count); }
void GXSetChanCtrl(GXChannelID channel,GXBool enabled,GXColorSrc ambient,GXColorSrc material,u32 lights,GXDiffuseFn diffuse,GXAttnFn attenuation) { if (bridge::commandChannelControl(channel,enabled,ambient,material,lights,diffuse,attenuation)) return;
    bridge::setChannelControl(channel,enabled,ambient,material,lights,diffuse,attenuation);
}
void GXSetNumTexGens(u8 count) { if (bridge::commandNumTexGens(count)) return; bridge::setNumTexGens(count); }
void GXSetTexCoordGen2(GXTexCoordID destination,GXTexGenType function,GXTexGenSrc source,u32 matrix,GXBool normalize,u32 postMatrix) { if (bridge::commandTexCoordGen(destination,function,source,matrix,normalize,postMatrix)) return;
    bridge::setTexCoordGen(destination,function,source,matrix,normalize,postMatrix);
}
void GXClearVtxDesc() { if (bridge::commandClearDescriptors()) return; bridge::clearVertexDescriptors(); }
void GXSetVtxDesc(GXAttr attribute,GXAttrType type) { if (bridge::commandDescriptor(attribute,type)) return; bridge::setVertexDescriptor(attribute,type); }
void GXSetVtxDescv(GXVtxDescList* list) {
    if(!list)streamFailure("GXSetVtxDescv/null");
    // Original lists are terminated by GX_VA_NULL. Validate the bounded list
    // before changing any descriptor; unrelated descriptors remain intact.
    unsigned count=0;
    for(;count<=GX_VA_MAX_ATTR;++count){
        if(list[count].attr==GX_VA_NULL)break;
        if(unsigned(list[count].attr)>=GX_VA_MAX_ATTR||unsigned(list[count].type)>GX_INDEX16)
            streamFailure("GXSetVtxDescv/entry");
    }
    if(count>GX_VA_MAX_ATTR)streamFailure("GXSetVtxDescv/unterminated");
    for(unsigned i=0;i<count;++i)GXSetVtxDesc(list[i].attr,list[i].type);
}
void GXSetVtxAttrFmt(GXVtxFmt format,GXAttr attribute,GXCompCnt count,GXCompType type,u8 fraction) { if (bridge::commandVertexFormat(format,attribute,count,type,fraction)) return; bridge::setVertexFormat(format,attribute,count,type,fraction); }
void GXSetVtxAttrFmtv(GXVtxFmt format,const GXVtxAttrFmtList* list){
    if(!list)streamFailure("GXSetVtxAttrFmtv/null");
    unsigned count=0;
    for(;count<=GX_VA_MAX_ATTR;++count)if(list[count].attr==GX_VA_NULL)break;
    if(count>GX_VA_MAX_ATTR)streamFailure("GXSetVtxAttrFmtv/unterminated");
    for(unsigned i=0;i<count;++i)GXSetVtxAttrFmt(format,list[i].attr,list[i].cnt,list[i].type,list[i].frac);
}
void GXSetArray(GXAttr attribute,const void* data,u32 size,u8 stride,bool little) {
    if (bridge::commandArray(attribute,data,size,stride,little)) return;
    bridge::unsupportedOperation("GXSetArray");
}
void GXBegin(GXPrimitive primitive,GXVtxFmt format,u16 count) {
    if(auto* stream=bridge::commandStream()){
        const unsigned kind=static_cast<unsigned>(primitive),vat=static_cast<unsigned>(format);
        if(vat>7||(kind!=GX_QUADS&&kind!=GX_TRIANGLES&&kind!=GX_TRIANGLESTRIP&&
           kind!=GX_TRIANGLEFAN&&kind!=GX_LINES&&kind!=GX_LINESTRIP&&kind!=GX_POINTS)||!stream->finish())
            streamFailure("GXBegin/primitive or unfinished draw");
        if(!stream->flushAutomaticSu())streamFailure("GXBegin/automatic SU");
        if(count==GX_AUTO){if(!stream->beginAuto(kind|vat))streamFailure("GXBegin/GX_AUTO");}
        else if(count){GXCmd1u8(kind|vat);GXCmd1u16(count);}
        return;
    }
    bridge::begin(primitive,format,count);
}
void GXBeginIndexed(GXVtxFmt format,u16 count,const u16* indices,u32 indexCount) {
    auto* stream=bridge::commandStream();
    if(!stream||unsigned(format)>7||!count||!indices||!indexCount||indexCount%3||!stream->finish())
        streamFailure("GXBeginIndexed/parameters or mode");
    if(!stream->flushAutomaticSu())streamFailure("GXBeginIndexed/automatic SU");
    GXCmd1u8(GX_AURORA);GXCmd1u16(GX_AURORA_DRAW_INDEXED);
    GXCmd1u8(GX_TRIANGLES|unsigned(format));GXCmd1u16(count);GXCmd1u32(indexCount);
    // Pinned Aurora indexed command carries host-endian indices. Borrow only
    // for the copy; the stream owns the bytes until its synchronous sink call.
    if(size_t(indexCount)>SIZE_MAX/sizeof(u16)||
       !stream->write({reinterpret_cast<const u8*>(indices),size_t(indexCount)*sizeof(u16)}))
        streamFailure("GXBeginIndexed/capacity");
}
void GXPosition3f32(f32 x,f32 y,f32 z) {
    if(bridge::commandStream()){scalar(x);scalar(y);scalar(z);return;}
    bridge::position(x,y,z);
}
void GXPosition2f32(f32 x,f32 y) {
    if(bridge::commandStream()){scalar(x);scalar(y);return;}
    bridge::position(x,y,0);
}
// Preserve the original packed wire representation. The active VAT, including
// signedness/fraction and registered array bounds, owns decoding and scaling.
// These encoders allocate no memory and never dereference a GameCube FIFO.
void GXPosition3u16(u16 x,u16 y,u16 z){packed16("GXPosition3u16",x);GXCmd1u16(y);GXCmd1u16(z);}
void GXPosition3s16(s16 x,s16 y,s16 z){packed16("GXPosition3s16",u16(x));GXCmd1u16(u16(y));GXCmd1u16(u16(z));}
void GXPosition3u8(u8 x,u8 y,u8 z){packed8("GXPosition3u8",x);GXCmd1u8(y);GXCmd1u8(z);}
void GXPosition3s8(s8 x,s8 y,s8 z){packed8("GXPosition3s8",u8(x));GXCmd1u8(u8(y));GXCmd1u8(u8(z));}
void GXPosition2u16(u16 x,u16 y){packed16("GXPosition2u16",x);GXCmd1u16(y);}
void GXPosition2s16(s16 x,s16 y){packed16("GXPosition2s16",u16(x));GXCmd1u16(u16(y));}
void GXPosition2u8(u8 x,u8 y){packed8("GXPosition2u8",x);GXCmd1u8(y);}
void GXPosition2s8(s8 x,s8 y){packed8("GXPosition2s8",u8(x));GXCmd1u8(u8(y));}
void GXPosition1x16(u16 i){packed16("GXPosition1x16",i);}
void GXPosition1x8(u8 i){packed8("GXPosition1x8",i);}
void GXNormal3f32(f32 x,f32 y,f32 z) {
    if(bridge::commandStream()){scalar(x);scalar(y);scalar(z);return;}
    bridge::unsupportedOperation("GXNormal3f32");
}
void GXNormal3s16(s16 x,s16 y,s16 z){packed16("GXNormal3s16",u16(x));GXCmd1u16(u16(y));GXCmd1u16(u16(z));}
void GXNormal3s8(s8 x,s8 y,s8 z){packed8("GXNormal3s8",u8(x));GXCmd1u8(u8(y));GXCmd1u8(u8(z));}
void GXNormal1x16(u16 i){packed16("GXNormal1x16",i);}
void GXNormal1x8(u8 i){packed8("GXNormal1x8",i);}
void GXColor1u32(u32 color) {
    if(bridge::commandStream()){GXCmd1u32(color);return;}
    bridge::color(color);
}
void GXColor4u8(u8 r,u8 g,u8 b,u8 a) {GXColor1u32((u32(r)<<24)|(u32(g)<<16)|(u32(b)<<8)|a);}
void GXColor3u8(u8 r,u8 g,u8 b){packed8("GXColor3u8",r);GXCmd1u8(g);GXCmd1u8(b);}
void GXColor1u16(u16 c){packed16("GXColor1u16",c);}
void GXColor1x16(u16 i){packed16("GXColor1x16",i);}
void GXColor1x8(u8 i){packed8("GXColor1x8",i);}
void GXTexCoord2f32(f32 u,f32 v) {
    if(bridge::commandStream()){scalar(u);scalar(v);return;}
    bridge::textureCoordinate(u,v);
}
void GXTexCoord2s16(s16 u,s16 v) {
    if(bridge::commandStream()){GXCmd1u16(u16(u));GXCmd1u16(u16(v));return;}
    bridge::textureCoordinateShort(u,v);
}
void GXTexCoord2u16(u16 u,u16 v){packed16("GXTexCoord2u16",u);GXCmd1u16(v);}
void GXTexCoord2u8(u8 u,u8 v){packed8("GXTexCoord2u8",u);GXCmd1u8(v);}
void GXTexCoord2s8(s8 u,s8 v){packed8("GXTexCoord2s8",u8(u));GXCmd1u8(u8(v));}
void GXTexCoord1f32(f32 u){if(!bridge::commandStream())streamFailure("GXTexCoord1f32");scalar(u);}
void GXTexCoord1u16(u16 u){packed16("GXTexCoord1u16",u);}
void GXTexCoord1s16(s16 u){packed16("GXTexCoord1s16",u16(u));}
void GXTexCoord1u8(u8 u){packed8("GXTexCoord1u8",u);}
void GXTexCoord1s8(s8 u){packed8("GXTexCoord1s8",u8(u));}
void GXTexCoord1x16(u16 i){packed16("GXTexCoord1x16",i);}
void GXTexCoord1x8(u8 i){packed8("GXTexCoord1x8",i);}
void GXInitTexObj(GXTexObj* object,const void* image,u16 width,u16 height,GXTexFmt format,GXTexWrapMode s,GXTexWrapMode t,GXBool mipmap) { bridge::finishCommandBoundary("GXInitTexObj");
    bridge::initTextureObject(object,image,width,height,format,s,t,mipmap,false);
}
void GXInitTexObjCI(GXTexObj* object,const void* image,u16 width,u16 height,GXCITexFmt format,GXTexWrapMode s,GXTexWrapMode t,GXBool mipmap,u32 tlut) { bridge::finishCommandBoundary("GXInitTexObjCI");
    bridge::initTextureObject(object,image,width,height,format,s,t,mipmap,true,tlut);
}
void GXInitTexObjTlut(GXTexObj* object,u32 tlut) { bridge::finishCommandBoundary("GXInitTexObjTlut");
    bridge::setTextureObjectTlut(object,tlut);
}
void GXInitTexObjLOD(GXTexObj* object,GXTexFilter min,GXTexFilter mag,f32 minLod,f32 maxLod,f32 bias,GXBool clamp,GXBool edge,GXAnisotropy anisotropy) { bridge::finishCommandBoundary("GXInitTexObjLOD");
    bridge::setTextureObjectLod(object,min,mag,minLod,maxLod,bias,clamp,edge,anisotropy);
}
void GXLoadTexObj(GXTexObj* object,GXTexMapID unit) {
    bridge::finishCommandBoundary("GXLoadTexObj");bridge::loadTextureObject(object,unit);
#if defined(__3DS__)
    bridge::Texture texture;
    if(bridge::commandStream()&&!bridge::textureBinding(unit,texture)){
        bridge::queryTextureObject(object,texture);
        OSReport("Native unavailable texture: %ux%u format %u handle %llu unit %u caller %p\n",
            texture.width,texture.height,texture.format,static_cast<unsigned long long>(texture.handle),
            static_cast<unsigned>(unit),__builtin_return_address(0));
    }
#endif
    bridge::commandTextureBinding(unit);
}
void GXDestroyTexObj(GXTexObj* object) { bridge::finishCommandBoundary("GXDestroyTexObj"); bridge::destroyTextureObject(object); }
u16 GXGetTexObjWidth(GXTexObj* object) {
    bridge::Texture metadata;if(!bridge::queryTextureObject(object,metadata))streamFailure("GXGetTexObjWidth/object");
    return metadata.width;
}
u16 GXGetTexObjHeight(GXTexObj* object) {
    bridge::Texture metadata;if(!bridge::queryTextureObject(object,metadata))streamFailure("GXGetTexObjHeight/object");
    return metadata.height;
}
GXTexWrapMode GXGetTexObjWrapS(GXTexObj* object) {
    bridge::Texture metadata;if(!bridge::queryTextureObject(object,metadata))streamFailure("GXGetTexObjWrapS/object");
    return static_cast<GXTexWrapMode>(metadata.wrapS);
}
GXTexWrapMode GXGetTexObjWrapT(GXTexObj* object) {
    bridge::Texture metadata;if(!bridge::queryTextureObject(object,metadata))streamFailure("GXGetTexObjWrapT/object");
    return static_cast<GXTexWrapMode>(metadata.wrapT);
}
u32 GXGetTexBufferSize(u16 width,u16 height,u32 format,GXBool mipmap,u8 maxLod) {
    if(!width||width>1024||!height||height>1024)
        streamFailure("GXGetTexBufferSize/dimensions");
    unsigned shiftX=0,shiftY=0;
    switch(format){
    case GX_TF_I4:case GX_TF_C4:case GX_TF_CMPR:case GX_CTF_R4:case GX_CTF_Z4:
        shiftX=3;shiftY=3;break;
    case GX_TF_I8:case GX_TF_IA4:case GX_TF_C8:case GX_TF_Z8:case GX_CTF_RA4:
    case GX_CTF_A8:case GX_CTF_R8:case GX_CTF_G8:case GX_CTF_B8:case GX_CTF_Z8M:case GX_CTF_Z8L:
        shiftX=3;shiftY=2;break;
    case GX_TF_IA8:case GX_TF_RGB565:case GX_TF_RGB5A3:case GX_TF_RGBA8:case GX_TF_C14X2:
    case GX_TF_Z16:case GX_TF_Z24X8:case GX_CTF_RA8:case GX_CTF_RG8:case GX_CTF_GB8:case GX_CTF_Z16L:
        shiftX=2;shiftY=2;break;
    default:streamFailure("GXGetTexBufferSize/format");
    }
    if(mipmap&&((width&(width-1))||(height&(height-1))))streamFailure("GXGetTexBufferSize/nonpower mipmap");
    const u32 tileBytes=(format==GX_TF_RGBA8||format==GX_TF_Z24X8)?64:32;
    u32 size=0;unsigned levels=mipmap?maxLod:1;
    // Pinned GX uses maxLod as a level count here. Every level, including 1x1,
    // occupies a complete tiled block; a rectangular chain clamps each axis.
    while(levels--){
        size+=tileBytes*((width+(1u<<shiftX)-1)>>shiftX)*((height+(1u<<shiftY)-1)>>shiftY);
        if(width==1&&height==1)break;
        width=width>1?width/2:1;height=height>1?height/2:1;
    }
    return size;
}
void GXInitTlutObj(GXTlutObj* object,const void* palette,GXTlutFmt format,u16 entries) { bridge::finishCommandBoundary("GXInitTlutObj"); bridge::initTlutObject(object,palette,format,entries); }
void GXInitTlutObjData(GXTlutObj* object,const void* palette) { bridge::finishCommandBoundary("GXInitTlutObjData"); bridge::setTlutObjectData(object,palette); }
void GXLoadTlut(const GXTlutObj* object,u32 tlut) { bridge::finishCommandBoundary("GXLoadTlut"); bridge::loadTlutObject(object,tlut); }
void GXDestroyTlutObj(GXTlutObj* object) { bridge::finishCommandBoundary("GXDestroyTlutObj");
    bridge::destroyTlutObject(object);
}
void GXEnd() {
    if(auto* stream=bridge::commandStream()){
        if(!stream->endAuto())streamFailure("GXEnd/incomplete draw");
        return;
    }
    bridge::end();
}
}
