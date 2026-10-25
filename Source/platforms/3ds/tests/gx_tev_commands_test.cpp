#include "../runtime/gx_tev_commands.hpp"
#include "../runtime/gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
template<class F> void rejected(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}assert(caught);}
int main(){
    CommandMemory memory;CommandProcessor processor(memory);std::array<u8,256> scratch{};CommandStream s(processor,scratch);
    assert(setCommandStream(&s));unsigned cases=0;
    auto compare=[&](auto native,auto gd){
        assert(s.reset());CommandProcessor original(memory);
        // Seed unrelated fields and both latched color banks. GD partial writes
        // must match native typed writes without destroying siblings or aliases.
        alignas(32) std::array<u8,256> bytes{};GDLObj object;
        GDInitGDLObj(&object,bytes.data(),bytes.size());GDSetCurrent(&object);
        for(unsigned reg:{0x28u,0x29u,0xc0u,0xc1u,0xc2u,0xc3u,0xf6u,0xf7u,0xf8u,0xf9u}){
            const u32 seed=0xa5b6c7u&((reg==0x28||reg==0x29)?0x3ff3ffu:0xffffffu);
            assert(s.writeBp(reg,seed));GDWriteBPCmd((reg<<24)|seed);
        }
        assert(original.execute({bytes.data(),GDGetCurrOffset()}));
        GDInitGDLObj(&object,bytes.data(),bytes.size());native();gd();auto size=GDGetCurrOffset();GDSetCurrent(nullptr);
        assert(original.execute({bytes.data(),size}));assert(s.idle());
        for(unsigned i=0;i<256;++i)if(i!=0xfe){if(processor.registers().bp[i]!=original.registers().bp[i]){fprintf(stderr,"case %u reg %x native %x original %x\n",cases,i,processor.registers().bp[i],original.registers().bp[i]);assert(false);}}
        assert(processor.registers().tevColor==original.registers().tevColor);++cases;
    };
    for(unsigned stage=0;stage<16;++stage){auto id=GXTevStageID(stage);
        for(unsigned op=0;op<5;++op)compare([&]{GXSetTevSwapMode(id,GX_TEV_SWAP0,GX_TEV_SWAP0);GXSetTevOp(id,GXTevMode(op));},[&]{GDSetTevOp(id,GXTevMode(op));});
        for(unsigned op:{0u,1u,8u,9u,10u,11u,12u,13u,14u,15u})for(unsigned out=0;out<4;++out){
            compare([&]{GXSetTevColorIn(id,GX_CC_C0,GX_CC_TEXC,GX_CC_HALF,GX_CC_CPREV);GXSetTevColorOp(id,GXTevOp(op),GX_TB_ADDHALF,GX_CS_DIVIDE_2,GX_FALSE,GXTevRegID(out));},
                [&]{GDSetTevColorCalc(id,GX_CC_C0,GX_CC_TEXC,GX_CC_HALF,GX_CC_CPREV,GXTevOp(op),GX_TB_ADDHALF,GX_CS_DIVIDE_2,0,GXTevRegID(out));});
            for(unsigned swap=0;swap<4;++swap)compare([&]{GXSetTevAlphaIn(id,GX_CA_A0,GX_CA_TEXA,GX_CA_KONST,GX_CA_APREV);GXSetTevAlphaOp(id,GXTevOp(op),GX_TB_SUBHALF,GX_CS_SCALE_4,GX_TRUE,GXTevRegID(out));GXSetTevSwapMode(id,GXTevSwapSel(swap),GXTevSwapSel(3-swap));},
                [&]{GDSetTevAlphaCalcAndSwap(id,GX_CA_A0,GX_CA_TEXA,GX_CA_KONST,GX_CA_APREV,GXTevOp(op),GX_TB_SUBHALF,GX_CS_SCALE_4,1,GXTevRegID(out),GXTevSwapSel(swap),GXTevSwapSel(3-swap));});
        }
        compare([&]{GXSetTevDirect(id);},[&]{GDSetTevDirect(id);});
        for(unsigned matrix:{0u,1u,2u,3u,5u,6u,7u,9u,10u,11u})compare([&]{GXSetTevIndirect(id,GX_INDTEXSTAGE3,GX_ITF_3,GX_ITB_STU,GXIndTexMtxID(matrix),GX_ITW_128,GX_ITW_0,GX_TRUE,GX_FALSE,GX_ITBA_U);},
            [&]{GDSetTevIndirect(id,GX_INDTEXSTAGE3,GX_ITF_3,GX_ITB_STU,GXIndTexMtxID(matrix),GX_ITW_128,GX_ITW_0,1,0,GX_ITBA_U);});
    }
    for(unsigned table=0;table<4;++table)for(unsigned pattern=0;pattern<256;++pattern){
        auto r=GXTevColorChan(pattern&3),g=GXTevColorChan((pattern>>2)&3),b=GXTevColorChan((pattern>>4)&3),a=GXTevColorChan(pattern>>6);
        compare([&]{GXSetTevSwapModeTable(GXTevSwapSel(table),r,g,b,a);},[&]{GDSetTevSwapModeTable(GXTevSwapSel(table),r,g,b,a);});
    }
    for(unsigned stage=0;stage<16;stage+=2)for(unsigned select:{0u,7u,12u,15u,16u,23u,31u}){
        auto id=GXTevStageID(stage),next=GXTevStageID(stage+1);
        compare([&]{GXSetTevKColorSel(id,GXTevKColorSel(select));GXSetTevKAlphaSel(id,GX_TEV_KASEL_K0_A);GXSetTevKColorSel(next,GX_TEV_KCSEL_K3_B);GXSetTevKAlphaSel(next,GX_TEV_KASEL_1_8);},
            [&]{GDSetTevKonstantSel(id,GXTevKColorSel(select),GX_TEV_KASEL_K0_A,GX_TEV_KCSEL_K3_B,GX_TEV_KASEL_1_8);});
    }
    for(unsigned stage=0;stage<16;stage+=2){
        auto id=GXTevStageID(stage),next=GXTevStageID(stage+1);
        for(unsigned map:{0u,3u,7u,0x100u,0x107u,0xffu})for(unsigned channel:{0u,4u,6u,7u,8u,255u})
            compare([&]{GXSetTevOrder(id,GX_TEXCOORD3,GXTexMapID(map),GXChannelID(channel));GXSetTevOrder(next,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);},
                [&]{GDSetTevOrder(id,GX_TEXCOORD3,GXTexMapID(map),GXChannelID(channel),GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);});
    }
    for(unsigned reg=0;reg<4;++reg){
        for(int v:{-1024,-513,-1,0,255,1023}){
            GXColorS10 color{s16(v),s16(-v-1),s16(v/2),s16(-v/2)};
            compare([&]{GXSetTevColorS10(GXTevRegID(reg),color);},[&]{GDSetTevColorS10(GXTevRegID(reg),color);});
        }
        compare([&]{GXSetTevColor(GXTevRegID(reg),{12,34,56,78});GXSetTevKColor(GXTevKColorID(reg),{91,82,73,64});},
            [&]{GDSetTevColor(GXTevRegID(reg),{12,34,56,78});GDSetTevKColor(GXTevKColorID(reg),{91,82,73,64});});
    }
    for(unsigned matrix=1;matrix<4;++matrix)for(int scale:{-17,-1,0,10,46}){
        float data[2][3]{{-.75f,.25f,1.f},{.5f,-.125f,-1.f}};
        compare([&]{GXSetIndTexMtx(GXIndTexMtxID(matrix),data,s8(scale));},[&]{GDSetIndTexMtx(GXIndTexMtxID(matrix),data,s8(scale));});
    }
    for(unsigned stage=0;stage<4;stage+=2)for(unsigned scale=0;scale<=8;++scale)
        compare([&]{GXSetIndTexCoordScale(GXIndTexStageID(stage),GXIndTexScale(scale),GX_ITS_256);GXSetIndTexCoordScale(GXIndTexStageID(stage+1),GX_ITS_32,GX_ITS_1);},
            [&]{GDSetIndTexCoordScale(GXIndTexStageID(stage),GXIndTexScale(scale),GX_ITS_256,GX_ITS_32,GX_ITS_1);});
    for(unsigned op=0;op<3;++op)for(GXTexFmt format:{GX_TF_Z8,GX_TF_Z16,GX_TF_Z24X8})
        compare([&]{GXSetZTexture(GXZTexOp(op),format,0x123456);},[&]{GDSetZTexture(GXZTexOp(op),format,0x123456);});
    assert(s.reset());GXSetNumIndStages(4);
    GXSetIndTexOrder(GX_INDTEXSTAGE0,GX_TEXCOORD0,GX_TEXMAP7);GXSetIndTexOrder(GX_INDTEXSTAGE1,GX_TEXCOORD1,GX_TEXMAP3);
    GXSetIndTexOrder(GX_INDTEXSTAGE2,GX_TEXCOORD2,GX_TEXMAP3);GXSetIndTexOrder(GX_INDTEXSTAGE3,GX_TEXCOORD3,GX_TEXMAP1);
    assert(processor.registers().bp[0x0f]==((1u<<7)|(1u<<3)|(1u<<1)));
    GXSetNumIndStages(1);assert(processor.registers().bp[0x0f]==(1u<<7));GXSetNumIndStages(0);assert(processor.registers().bp[0x0f]==0);
    for(unsigned n=1;n<=16;++n){GXSetNumTevStages(n);assert(((processor.registers().bp[0]>>10)&15)==n-1);}
    // Typed color setters and partial swaps preserve earlier raw GD banks/fields.
    assert(s.writeBp(0xfe,1));GXSetTevColor(GX_TEVPREV,{12,34,56,78});assert(processor.registers().tevColor[0][0]==(12u|(78u<<12)));
    rejected([&]{GXSetNumTevStages(0);});rejected([&]{GXSetNumIndStages(5);});rejected([&]{GXSetTevColorIn(GXTevStageID(16),GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO);});
    rejected([&]{GXSetTevColorS10(GX_TEVPREV,{-1025,0,0,0});});rejected([&]{GXSetTevKColorSel(GX_TEVSTAGE0,GXTevKColorSel(8));});
    rejected([&]{GXSetTevSwapModeTable(GXTevSwapSel(4),GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);});
    float bad[6]{};bad[3]=std::numeric_limits<float>::quiet_NaN();rejected([&]{GXSetIndTexMtx(GX_ITM_0,bad,0);});
    bad[3]=std::numeric_limits<float>::max();rejected([&]{GXSetIndTexMtx(GX_ITM_0,bad,0);});
    rejected([&]{GXSetIndTexMtx(GX_ITM_0,nullptr,0);});
    assert(setCommandStream(nullptr));assert(!commandTevDirect(0));rejected([&]{GXSetTevSwapMode(GX_TEVSTAGE0,GX_TEV_SWAP0,GX_TEV_SWAP0);});
    printf("PASS: %u native TEV/indirect/Z-texture cases match original GD, both color banks and sibling fields; counts/texture masks/invalid state tested\n",cases);
}
