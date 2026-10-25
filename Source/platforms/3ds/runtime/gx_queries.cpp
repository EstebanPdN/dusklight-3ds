#include "gx_stream.hpp"
#include "gx_state.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <bit>
#include <cmath>
#include <algorithm>

namespace {
using namespace dusk::native::gx;
[[noreturn]] void fail(const char* name){OSPanic(__FILE__,__LINE__,"Native GX state query failure: %s",name);}
const CommandRegisters* registers(const char* name){
    auto* s=commandStream();if(!s)return nullptr;
    if(!s->finish())fail(name);
    return &s->registers();
}
float xf(const CommandRegisters& r,unsigned index,const char* name){
    if(!r.xfValid[index])fail(name);
    const float value=std::bit_cast<float>(r.xf[index]);
    if(!std::isfinite(value))fail(name);
    return value;
}
}
extern "C" {
void GXGetVtxDesc(GXAttr attribute,GXAttrType* output){
    auto* s=dusk::native::gx::commandStream();unsigned type;
    if(!output||!s||!s->finish()||!s->queryDescriptor(unsigned(attribute),type))fail("GXGetVtxDesc/state or output");
    *output=static_cast<GXAttrType>(type);
}
void GXGetVtxDescv(GXVtxDescList* output){
    if(!output)fail("GXGetVtxDescv/null");
    for(unsigned i=0;i<=GX_VA_TEX7;++i){output[i].attr=static_cast<GXAttr>(i);GXGetVtxDesc(output[i].attr,&output[i].type);}
    output[GX_VA_TEX7+1].attr=GX_VA_NBT;GXGetVtxDesc(GX_VA_NBT,&output[GX_VA_TEX7+1].type);
    output[GX_VA_TEX7+2]={GX_VA_NULL,GX_NONE};
}
void GXGetVtxAttrFmt(GXVtxFmt format,GXAttr attr,GXCompCnt* count,GXCompType* type,u8* fraction){
    const auto* r=registers("GXGetVtxAttrFmt");
    if(!r||!count||!type||!fraction||unsigned(format)>7)fail("GXGetVtxAttrFmt/parameters");
    for(unsigned base:{0x70u,0x80u,0x90u})if(!r->cpValid[base+unsigned(format)])fail("GXGetVtxAttrFmt/missing VAT");
    const u32 a=r->cp[0x70+unsigned(format)],b=r->cp[0x80+unsigned(format)],c=r->cp[0x90+unsigned(format)];
    unsigned cnt=1,kind=0,frac=0;u32 word=0;unsigned shift=0;
    if(attr==GX_VA_POS){word=a;shift=0;}
    else if(attr==GX_VA_NRM||attr==GX_VA_NBT){
        cnt=(a>>9)&1;if(cnt&&(a>>31))cnt=2;kind=(a>>10)&7;
        frac=kind==GX_S8?6:kind==GX_S16?14:0;
    }else if(attr==GX_VA_CLR0||attr==GX_VA_CLR1){shift=attr==GX_VA_CLR0?13:17;cnt=(a>>shift)&1;kind=(a>>(shift+1))&7;}
    else if(attr==GX_VA_TEX0){word=a;shift=21;}
    else if(attr>=GX_VA_TEX1&&attr<=GX_VA_TEX3){word=b;shift=(attr-GX_VA_TEX1)*9;}
    else if(attr==GX_VA_TEX4){cnt=(b>>27)&1;kind=(b>>28)&7;frac=c&31;}
    else if(attr>=GX_VA_TEX5&&attr<=GX_VA_TEX7){word=c;shift=5+(attr-GX_VA_TEX5)*9;}
    else fail("GXGetVtxAttrFmt/attribute");
    if(attr==GX_VA_POS||attr==GX_VA_TEX0||(attr>=GX_VA_TEX1&&attr<=GX_VA_TEX3)||(attr>=GX_VA_TEX5&&attr<=GX_VA_TEX7)){
        cnt=(word>>shift)&1;kind=(word>>(shift+1))&7;frac=(word>>(shift+4))&31;
    }
    *count=static_cast<GXCompCnt>(cnt);*type=static_cast<GXCompType>(kind);*fraction=frac;
}
void GXGetVtxAttrFmtv(GXVtxFmt format,GXVtxAttrFmtList* output){
    if(!output)fail("GXGetVtxAttrFmtv/null");
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr){auto& value=output[attr-GX_VA_POS];value.attr=static_cast<GXAttr>(attr);GXGetVtxAttrFmt(format,value.attr,&value.cnt,&value.type,&value.frac);}
    output[GX_VA_TEX7-GX_VA_POS+1]={GX_VA_NULL,GX_TEX_S,GX_U8,0};
}
void GXGetScissor(u32* x,u32* y,u32* width,u32* height){
    using namespace dusk::native::gx;
    if(!x||!y||!width||!height)fail("GXGetScissor/null");
    Scissor out;
    if(const auto* r=registers("GXGetScissor")){
        if(!r->bpValid[0x20]||!r->bpValid[0x21])fail("GXGetScissor/missing");
        const u32 left=(r->bp[0x20]>>12)&2047,top=r->bp[0x20]&2047;
        const u32 right=(r->bp[0x21]>>12)&2047,bottom=r->bp[0x21]&2047;
        // GX represents an empty scissor as right=left-1 / bottom=top-1.
        if(left<342||top<342||right+1<left||bottom+1<top)fail("GXGetScissor/invalid");
        out={left-342,top-342,right+1-left,bottom+1-top};
    }else if(!queryScissor(out))fail("GXGetScissor/missing immediate state");
    *x=out.x;*y=out.y;*width=out.width;*height=out.height;
}
void GXGetProjectionv(f32* output){
    if(!output)fail("GXGetProjectionv/null");
    float values[7];
    if(const auto* r=registers("GXGetProjectionv")){
        if(!r->xfValid[0x1026]||r->xf[0x1026]>1)fail("GXGetProjectionv/missing type");
        values[0]=float(r->xf[0x1026]);
        for(unsigned i=0;i<6;++i)values[i+1]=xf(*r,0x1020+i,"GXGetProjectionv/missing or invalid coefficients");
    }else if(!dusk::native::gx::queryProjection(values))fail("GXGetProjectionv/missing immediate state");
    std::copy_n(values,7,output);
}
void GXSetProjectionv(const f32* input){
    if(!input||(input[0]!=0&&input[0]!=1))fail("GXSetProjectionv/type");
    for(unsigned i=1;i<7;++i)if(!std::isfinite(input[i]))fail("GXSetProjectionv/nonfinite");
    // Original ABI: type followed by the six compact projection coefficients.
    const bool ortho=input[0]==1;float matrix[16]{};
    matrix[0]=input[1];matrix[ortho?3:2]=input[2];matrix[5]=input[3];matrix[ortho?7:6]=input[4];
    matrix[10]=input[5];matrix[11]=input[6];matrix[ortho?15:14]=ortho?1:-1;
    GXSetProjection(matrix,ortho?GX_ORTHOGRAPHIC:GX_PERSPECTIVE);
}
void GXGetViewportv(f32* output){
    using namespace dusk::native::gx;
    if(!output)fail("GXGetViewportv/null");
    Viewport out;
    if(const auto* r=registers("GXGetViewportv")){
        const float sx=xf(*r,0x101a,"GXGetViewportv/scale"),sy=xf(*r,0x101b,"GXGetViewportv/scale");
        const float sz=xf(*r,0x101c,"GXGetViewportv/depth"),tx=xf(*r,0x101d,"GXGetViewportv/translation");
        const float ty=xf(*r,0x101e,"GXGetViewportv/translation"),tz=xf(*r,0x101f,"GXGetViewportv/depth");
        out={tx-340-sx,ty-340+sy,2*sx,-2*sy,(tz-sz)/16777215.f,tz/16777215.f};
        if(out.width<=0||out.height<=0||out.nearZ<0||out.nearZ>1||out.farZ<0||out.farZ>1)fail("GXGetViewportv/invalid");
    }else if(!queryViewport(out))fail("GXGetViewportv/missing immediate state");
    const float values[]{out.x,out.y,out.width,out.height,out.nearZ,out.farZ};std::copy_n(values,6,output);
}
}
