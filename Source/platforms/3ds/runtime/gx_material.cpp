#include "gx_material.hpp"
#include "gx_commands.hpp"

namespace dusk::native::gx {
namespace {
unsigned field(u32 value,unsigned shift,unsigned bits){return (value>>shift)&((1u<<bits)-1);}
MaterialOperation operation(u32 word){
    const unsigned bias=field(word,16,2),scale=field(word,20,2),op=field(word,18,1);
    if(bias==3)return {static_cast<u8>(8+2*scale+op),0,0,static_cast<u8>(field(word,19,1)),static_cast<u8>(field(word,22,2))};
    return {static_cast<u8>(op),static_cast<u8>(bias),static_cast<u8>(scale),static_cast<u8>(field(word,19,1)),static_cast<u8>(field(word,22,2))};
}
std::int16_t signed11(u32 word){return static_cast<std::int16_t>(static_cast<int>((word&2047)^1024)-1024);}
}
bool decodeMaterial(const CommandRegisters& regs,MaterialState& out,MaterialError* error){
    out={};if(error)*error=MaterialError::none;
    auto fail=[&](MaterialError value){out={};if(error)*error=value;return false;};
    if(!regs.bpValid[0])return fail(MaterialError::missingState);
    const u32 gen=regs.bp[0];
    out.stageCount=field(gen,10,4)+1;out.texgenCount=field(gen,0,4);
    out.channelCount=field(gen,4,3);out.indirectCount=field(gen,16,3);
    if(out.texgenCount>8||out.channelCount>2||out.indirectCount>4)return fail(MaterialError::invalidCount);
    for(unsigned i=0;i<out.stageCount;++i){
        const unsigned colorReg=0xc0+2*i,alphaReg=colorReg+1,orderReg=0x28+i/2,kselReg=0xf6+i/2;
        if(!regs.bpValid[colorReg]||!regs.bpValid[alphaReg]||!regs.bpValid[orderReg]||
           !regs.bpValid[kselReg]||!regs.bpValid[0x10+i])return fail(MaterialError::missingState);
        auto& stage=out.stages[i];const u32 color=regs.bp[colorReg],alpha=regs.bp[alphaReg];
        for(unsigned c=0;c<4;++c){stage.color[c]=field(color,12-4*c,4);stage.alpha[c]=field(alpha,13-3*c,3);}
        stage.colorOp=operation(color);stage.alphaOp=operation(alpha);
        stage.rasterSwap=field(alpha,0,2);stage.textureSwap=field(alpha,2,2);
        const u32 order=regs.bp[orderReg]>>(12*(i&1));
        stage.texture=field(order,0,3);stage.coordinate=field(order,3,3);
        stage.textureEnabled=field(order,6,1);stage.raster=field(order,7,3);
        const u32 ksel=regs.bp[kselReg]>>(10*(i&1));
        stage.konstColor=field(ksel,4,5);stage.konstAlpha=field(ksel,9,5);
        stage.indirect=regs.bp[0x10+i];
    }
    for(unsigned i=0;i<4;++i){
        const unsigned slot=2*i;
        if(regs.tevColorValid[slot]&&regs.tevColorValid[slot+1]){
            const u32 ra=regs.tevColor[0][slot],bg=regs.tevColor[0][slot+1];
            out.colors[i]={signed11(ra),signed11(bg>>12),signed11(bg),signed11(ra>>12)};
            out.colorValid|=1u<<i;
        }
        if(regs.tevColorValid[8+slot]&&regs.tevColorValid[9+slot]){
            const u32 ra=regs.tevColor[1][slot],bg=regs.tevColor[1][slot+1];
            out.konst[i]={static_cast<u8>(ra),static_cast<u8>(bg>>12),static_cast<u8>(bg),static_cast<u8>(ra>>12)};
            out.konstValid|=1u<<i;
        }
        if(regs.bpValid[0xf6+slot]&&regs.bpValid[0xf7+slot]){
            const u32 rg=regs.bp[0xf6+slot],ba=regs.bp[0xf7+slot];
            out.swaps[i]={static_cast<u8>(rg&3),static_cast<u8>((rg>>2)&3),static_cast<u8>(ba&3),static_cast<u8>((ba>>2)&3)};
            out.swapValid|=1u<<i;
        }
    }
    return true;
}
}
