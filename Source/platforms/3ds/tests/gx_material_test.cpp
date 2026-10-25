#include "../runtime/gx_material.hpp"
#include "../runtime/gx_commands.hpp"
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
int main(){
    CommandMemory memory;CommandProcessor processor(memory);MaterialState state;MaterialError error;
    assert(!decodeMaterial(processor.registers(),state,&error)&&error==MaterialError::missingState);
    alignas(32) u8 bytes[8192];GDLObj gd;
    auto start=[&]{GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);};
    auto finish=[&]{const auto n=GDGetCurrOffset();GDSetCurrent(nullptr);assert(processor.execute({bytes,n}));};
    start();GDSetGenMode2(8,2,16,4,GX_CULL_BACK);
    for(unsigned i=0;i<16;++i){
        const auto stage=static_cast<GXTevStageID>(i);
        const auto op=static_cast<GXTevOp>(i<8?i%2:i);
        GDSetTevColorCalc(stage,GX_CC_C0,GX_CC_C1,GX_CC_TEXC,GX_CC_CPREV,op,GX_TB_ADDHALF,GX_CS_DIVIDE_2,1,GX_TEVREG2);
        GDSetTevAlphaCalcAndSwap(stage,GX_CA_A0,GX_CA_A1,GX_CA_TEXA,GX_CA_APREV,op,GX_TB_SUBHALF,GX_CS_SCALE_4,0,GX_TEVREG1,GX_TEV_SWAP2,GX_TEV_SWAP3);
        GDWriteBPCmd(((0x10+i)<<24)|0x12345);
        if(!(i&1)){
            GDSetTevOrder(stage,GX_TEXCOORD2,GX_TEXMAP1,GX_COLOR0A0,GX_TEXCOORD4,GX_TEXMAP3,GX_ALPHA_BUMPN);
            GDSetTevKonstantSel(stage,GX_TEV_KCSEL_K0,GX_TEV_KASEL_K1_A,GX_TEV_KCSEL_K3_B,GX_TEV_KASEL_1_2);
        }
    }
    for(unsigned i=0;i<4;++i){
        GDSetTevSwapModeTable(static_cast<GXTevSwapSel>(i),GX_CH_BLUE,GX_CH_GREEN,GX_CH_RED,GX_CH_ALPHA);
        GDSetTevColorS10(static_cast<GXTevRegID>(i),{-1024,1023,-1,static_cast<s16>(256+i)});
        GDSetTevKColor(static_cast<GXTevKColorID>(i),{static_cast<u8>(10+i),20,30,40});
    }
    finish();assert(decodeMaterial(processor.registers(),state,&error));
    assert(state.stageCount==16&&state.texgenCount==8&&state.channelCount==2&&state.indirectCount==4);
    assert(state.colorValid==15&&state.konstValid==15&&state.swapValid==15);
    for(unsigned i=0;i<16;++i){
        const auto& s=state.stages[i];
        assert((s.color==std::array<u8,4>{2,4,8,0}));assert((s.alpha==std::array<u8,4>{1,2,4,0}));
        const unsigned op=i<8?i%2:i;
        assert(s.colorOp.operation==op&&s.alphaOp.operation==op);
        assert(s.colorOp.bias==(i<8?1:0)&&s.colorOp.scale==(i<8?3:0)&&s.colorOp.clamp==1&&s.colorOp.output==3);
        assert(s.alphaOp.bias==(i<8?2:0)&&s.alphaOp.scale==(i<8?2:0)&&s.alphaOp.clamp==0&&s.alphaOp.output==2);
        assert(s.textureEnabled&&s.texture==(i%2?3:1)&&s.coordinate==(i%2?4:2)&&s.raster==(i%2?6:0));
        assert(s.rasterSwap==2&&s.textureSwap==3&&s.indirect==0x12345);
        assert(s.konstColor==(i%2?GX_TEV_KCSEL_K3_B:GX_TEV_KCSEL_K0));
        assert(s.konstAlpha==(i%2?GX_TEV_KASEL_1_2:GX_TEV_KASEL_K1_A));
    }
    for(unsigned i=0;i<4;++i){
        assert((state.colors[i]==std::array<s16,4>{-1024,1023,-1,static_cast<s16>(256+i)}));
        assert((state.konst[i]==std::array<u8,4>{static_cast<u8>(10+i),20,30,40}));
        assert((state.swaps[i]==std::array<u8,4>{2,1,0,3}));
    }
    // Overwrite only signed bank RA; retain its BG and the complete konst color.
    assert(processor.writeBp(0xe0,0x001230));
    assert(decodeMaterial(processor.registers(),state));
    assert((state.colors[0]==std::array<s16,4>{560,1023,-1,1}));assert(state.konst[0][0]==10);
    // The selector is evaluated after masking against the last raw BP shadow.
    assert(processor.writeBp(0xfe,0x800000)&&processor.writeBp(0xe0,0x800000));
    assert(decodeMaterial(processor.registers(),state));
    assert(state.konst[0][0]==0x30&&state.konst[0][3]==1&&state.colors[0][0]==560);
    assert(processor.writeBp(0xfe,0xff)&&processor.writeBp(0xe0,0x55));
    assert(decodeMaterial(processor.registers(),state)&&state.konst[0][0]==0x55&&state.colors[0][0]==560);
    assert(processor.registers().bpMask==0xffffff);
    // A zero mask preserves the selector and value but still consumes the mask.
    assert(processor.writeBp(0xfe,0)&&processor.writeBp(0xe0,0));
    assert(processor.registers().bpMask==0xffffff&&processor.registers().tevColor[1][0]==0x801255);
    // Partial banks are never advertised as fully initialized.
    processor.reset();assert(!processor.registers().tevColorValid.any());
    start();GDSetGenMode(0,0,1);GDSetTevOp(GX_TEVSTAGE0,GX_PASSCLR);
    GDSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR0A0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);
    GDSetTevKonstantSel(GX_TEVSTAGE0,GX_TEV_KCSEL_1,GX_TEV_KASEL_1,GX_TEV_KCSEL_1,GX_TEV_KASEL_1);
    GDWriteBPCmd(0x10000000);GDWriteBPCmd(0xe0000012);finish();
    assert(decodeMaterial(processor.registers(),state)&&!state.colorValid&&!state.konstValid&&!state.swapValid);
    assert(!state.stages[0].textureEnabled);
    assert(processor.writeBp(0,9));assert(!decodeMaterial(processor.registers(),state,&error)&&error==MaterialError::invalidCount);
    assert(!state.stageCount&&!state.colorValid);
    puts("PASS: 16 original-GD TEV stages, operations/swaps/orders, signed and konst bank retention, masked selector dispatch, partial validity and reset");
}
