#include "gx_tev_commands.hpp"
#include "gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <cmath>
#include <limits>

namespace dusk::native::gx {
namespace {
[[noreturn]] void fail(const char* call){OSPanic(__FILE__,__LINE__,"Native GX TEV command failure: %s",call);}
CommandStream& stream(const char* call){auto* s=commandStream();if(!s||!s->finish())fail(call);return *s;}
void masked(CommandStream& s,u8 reg,u32 mask,u32 value,const char* call){
    // Reset a pending GD mask explicitly, then update exactly our fields. The
    // command processor retains every sibling field and consumes this mask.
    if(!s.writeBp(0xfe,mask)||!s.writeBp(reg,value))fail(call);
}
void whole(CommandStream& s,u8 reg,u32 value,const char* call){masked(s,reg,0xffffff,value,call);}
void stageValid(unsigned stage,const char* call){if(stage>15)fail(call);}
void indirectMask(CommandStream& s){
    const auto& r=s.registers();const unsigned count=(r.bp[0]>>16)&7;
    if(count>4)fail("indirect stage count");
    u32 mask=0;for(unsigned i=0;i<count;++i)mask|=1u<<((r.bp[0x27]>>(6*i))&7);
    masked(s,0x0f,255,mask,"indirect texture mask");
}
}
bool commandTevCount(unsigned count,bool indirect){
    if(!commandStream()) return false;
    auto& s=stream("GXSetNumTev/IndStages");
    if(indirect){if(count>4)fail("GXSetNumIndStages/count");masked(s,0,7u<<16,count<<16,"GXSetNumIndStages");indirectMask(s);}
    else{if(count<1||count>16)fail("GXSetNumTevStages/count");masked(s,0,15u<<10,(count-1)<<10,"GXSetNumTevStages");}
    if(!s.automaticSuCount(count,indirect))fail("GXSetNumStages/automatic SU");
    return true;
}
bool commandTevDirect(unsigned stage){
    if(!commandStream()) return false;
    auto& s=stream("GXSetTevDirect");stageValid(stage,"GXSetTevDirect");
    whole(s,0x10+stage,0,"GXSetTevDirect");return true;
}
bool commandTevInputs(unsigned stage,bool alpha,unsigned a,unsigned b,unsigned c,unsigned d){
    if(!commandStream()) return false;
    auto& s=stream("GXSetTevInputs");stageValid(stage,"GXSetTevInputs");
    const unsigned max=alpha?7:15;if(a>max||b>max||c>max||d>max)fail("GXSetTevInputs/value");
    const u32 value=alpha?(a<<13)|(b<<10)|(c<<7)|(d<<4):(a<<12)|(b<<8)|(c<<4)|d;
    masked(s,0xc0+2*stage+unsigned(alpha),alpha?0xfff0:0xffff,value,"GXSetTevInputs");return true;
}
bool commandTevOperator(unsigned stage,bool alpha,unsigned op,unsigned bias,unsigned scale,unsigned clamp,unsigned output){
    if(!commandStream()) return false;
    auto& s=stream("GXSetTevOperator");stageValid(stage,"GXSetTevOperator");
    if((op>1&&(op<8||op>15))||bias>2||scale>3||clamp>1||output>3)fail("GXSetTevOperator/value");
    const u32 value=((op<=1?bias:3)<<16)|((op&1)<<18)|(clamp<<19)|((op<=1?scale:(op>>1)&3)<<20)|(output<<22);
    masked(s,0xc0+2*stage+unsigned(alpha),0xff0000,value,"GXSetTevOperator");return true;
}
bool commandTevOperation(unsigned stage,unsigned op){
    if(!commandStream()) return false;
    stageValid(stage,"GXSetTevOp");if(op>4)fail("GXSetTevOp/value");
    const unsigned color=stage?GX_CC_CPREV:GX_CC_RASC,alpha=stage?GX_CA_APREV:GX_CA_RASA;
    switch(op){
    case GX_MODULATE:commandTevInputs(stage,false,GX_CC_ZERO,GX_CC_TEXC,color,GX_CC_ZERO);break;
    case GX_DECAL:commandTevInputs(stage,false,color,GX_CC_TEXC,GX_CC_TEXA,GX_CC_ZERO);break;
    case GX_BLEND:commandTevInputs(stage,false,color,GX_CC_ONE,GX_CC_TEXC,GX_CC_ZERO);break;
    case GX_REPLACE:commandTevInputs(stage,false,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_TEXC);break;
    case GX_PASSCLR:commandTevInputs(stage,false,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,color);break;
    }
    if(op==GX_MODULATE||op==GX_BLEND)commandTevInputs(stage,true,GX_CA_ZERO,GX_CA_TEXA,alpha,GX_CA_ZERO);
    else commandTevInputs(stage,true,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,op==GX_REPLACE?GX_CA_TEXA:alpha);
    commandTevOperator(stage,false,0,0,0,1,0);commandTevOperator(stage,true,0,0,0,1,0);return true;
}
bool commandTevOrder(unsigned stage,unsigned coord,unsigned map,unsigned channel){
    if(!commandStream()) return false;
    auto& s=stream("GXSetTevOrder");stageValid(stage,"GXSetTevOrder");
    const unsigned unit=map&~unsigned(GX_TEX_DISABLE);
    if((coord>7&&coord!=GX_TEXCOORD_NULL)||(unit>7&&map!=GX_TEXMAP_NULL)||
       (channel>GX_COLOR_ZERO&&channel!=GX_ALPHA_BUMP&&channel!=GX_ALPHA_BUMPN&&channel!=GX_COLOR_NULL))fail("GXSetTevOrder/value");
    constexpr u8 c2r[]{0,1,0,1,0,1,7,5,6,0,0,0,0,0,0,7};
    const unsigned shift=(stage&1)*12;
    const u32 value=(map&7)|((coord&7)<<3)|(u32(map!=GX_TEXMAP_NULL&&!(map&GX_TEX_DISABLE))<<6)|(u32(c2r[channel&15])<<7);
    masked(s,0x28+stage/2,0x3ffu<<shift,value<<shift,"GXSetTevOrder");
    if(!s.automaticSuOrder(stage,coord,map,false))fail("GXSetTevOrder/automatic SU");
    return true;
}
bool commandTevRegister(unsigned id,int r,int g,int b,int a,bool konst){
    if(!commandStream()) return false;
    auto& s=stream("GXSetTevColor/KColor/S10");
    const int low=konst?0:-1024,high=konst?255:1023;
    if(id>3||r<low||r>high||g<low||g>high||b<low||b>high||a<low||a>high)fail("GXSetTevRegister/value");
    const u32 bank=u32(konst)<<23;
    whole(s,0xe0+id*2,(u32(r)&2047)|((u32(a)&2047)<<12)|bank,"GXSetTevRegister/RA");
    whole(s,0xe1+id*2,(u32(b)&2047)|((u32(g)&2047)<<12)|bank,"GXSetTevRegister/BG");return true;
}
bool commandTevKonstSelection(unsigned stage,unsigned selection,bool alpha){
    if(!commandStream()) return false;
    auto& s=stream("GXSetTevKColor/AlphaSel");stageValid(stage,"GXSetTevKonstSelection");
    if(selection>31||(selection>7&&selection<(alpha?16u:12u)))fail("GXSetTevKonstSelection/value");
    const unsigned shift=4+(stage&1)*10+unsigned(alpha)*5;
    masked(s,0xf6+stage/2,31u<<shift,selection<<shift,"GXSetTevKonstSelection");return true;
}
}
extern "C" {
void GXSetTevColorS10(GXTevRegID id,GXColorS10 c){
    if(!dusk::native::gx::commandTevRegister(id,c.r,c.g,c.b,c.a,false))dusk::native::gx::unsupportedOperation("GXSetTevColorS10/immediate");
}
void GXSetTevSwapMode(GXTevStageID stage,GXTevSwapSel raster,GXTevSwapSel texture){
    using namespace dusk::native::gx;auto& s=stream("GXSetTevSwapMode");stageValid(stage,"GXSetTevSwapMode");
    if(unsigned(raster)>3||unsigned(texture)>3)fail("GXSetTevSwapMode/value");
    masked(s,0xc1+2*unsigned(stage),15,unsigned(raster)|(unsigned(texture)<<2),"GXSetTevSwapMode");
}
void GXSetTevSwapModeTable(GXTevSwapSel table,GXTevColorChan r,GXTevColorChan g,GXTevColorChan b,GXTevColorChan a){
    using namespace dusk::native::gx;auto& s=stream("GXSetTevSwapModeTable");
    if(unsigned(table)>3||unsigned(r)>3||unsigned(g)>3||unsigned(b)>3||unsigned(a)>3)fail("GXSetTevSwapModeTable/value");
    masked(s,0xf6+2*unsigned(table),15,unsigned(r)|(unsigned(g)<<2),"GXSetTevSwapModeTable/RG");
    masked(s,0xf7+2*unsigned(table),15,unsigned(b)|(unsigned(a)<<2),"GXSetTevSwapModeTable/BA");
}
void GXSetTevIndirect(GXTevStageID stage,GXIndTexStageID indirect,GXIndTexFormat format,GXIndTexBiasSel bias,GXIndTexMtxID matrix,GXIndTexWrap wrapS,GXIndTexWrap wrapT,GXBool add,GXBool lod,GXIndTexAlphaSel alpha){
    using namespace dusk::native::gx;auto& s=stream("GXSetTevIndirect");stageValid(stage,"GXSetTevIndirect");
    const unsigned m=unsigned(matrix);const bool matrixValid=m<=3||(m>=5&&m<=7)||(m>=9&&m<=11);
    if(unsigned(indirect)>3||unsigned(format)>3||unsigned(bias)>7||!matrixValid||unsigned(wrapS)>6||unsigned(wrapT)>6||unsigned(alpha)>3)fail("GXSetTevIndirect/value");
    const u32 value=unsigned(indirect)|(unsigned(format)<<2)|(unsigned(bias)<<4)|(unsigned(alpha)<<7)|(m<<9)|(unsigned(wrapS)<<13)|(unsigned(wrapT)<<16)|(u32(lod)<<19)|(u32(add)<<20);
    whole(s,0x10+unsigned(stage),value,"GXSetTevIndirect");
}
void GXSetIndTexOrder(GXIndTexStageID stage,GXTexCoordID coord,GXTexMapID map){
    using namespace dusk::native::gx;auto& s=stream("GXSetIndTexOrder");
    if(unsigned(stage)>3||unsigned(coord)>7||unsigned(map)>7)fail("GXSetIndTexOrder/value");
    const unsigned shift=6*unsigned(stage);masked(s,0x27,63u<<shift,(unsigned(map)|(unsigned(coord)<<3))<<shift,"GXSetIndTexOrder");indirectMask(s);
    if(!s.automaticSuOrder(unsigned(stage),unsigned(coord),unsigned(map),true))fail("GXSetIndTexOrder/automatic SU");
}
void GXSetIndTexCoordScale(GXIndTexStageID stage,GXIndTexScale sScale,GXIndTexScale tScale){
    using namespace dusk::native::gx;auto& s=stream("GXSetIndTexCoordScale");
    if(unsigned(stage)>3||unsigned(sScale)>8||unsigned(tScale)>8)fail("GXSetIndTexCoordScale/value");
    const unsigned shift=(unsigned(stage)&1)*8;masked(s,0x25+unsigned(stage)/2,255u<<shift,(unsigned(sScale)|(unsigned(tScale)<<4))<<shift,"GXSetIndTexCoordScale");
}
void GXSetIndTexMtx(GXIndTexMtxID matrix,const void* data,s8 exponent){
    using namespace dusk::native::gx;auto& s=stream("GXSetIndTexMtx");const auto* m=static_cast<const float*>(data);
    if(unsigned(matrix)<1||unsigned(matrix)>3||!m||exponent<-17||exponent>46)fail("GXSetIndTexMtx/value");
    s32 fixed[6];for(unsigned i=0;i<6;++i){
        const double v=double(m[i])*1024;
        if(!std::isfinite(v)||v<std::numeric_limits<s32>::min()||v>std::numeric_limits<s32>::max())fail("GXSetIndTexMtx/nonfinite or overflow");
        fixed[i]=s32(v);
    }
    const unsigned scale=int(exponent)+17;
    for(unsigned i=0;i<3;++i)whole(s,6+(unsigned(matrix)-1)*3+i,(u32(fixed[i])&2047)|((u32(fixed[3+i])&2047)<<11)|(((scale>>(2*i))&3)<<22),"GXSetIndTexMtx/column");
}
void GXSetZTexture(GXZTexOp op,GXTexFmt format,u32 bias){
    using namespace dusk::native::gx;auto& s=stream("GXSetZTexture");u32 encoded;
    if(op>GX_ZT_REPLACE||bias>0xffffff)fail("GXSetZTexture/value");
    if(format==GX_TF_Z8)encoded=0;else if(format==GX_TF_Z16)encoded=1;else if(format==GX_TF_Z24X8)encoded=2;else fail("GXSetZTexture/format");
    whole(s,0xf4,bias,"GXSetZTexture/bias");masked(s,0xf5,15,encoded|(unsigned(op)<<2),"GXSetZTexture/mode");
}
}
