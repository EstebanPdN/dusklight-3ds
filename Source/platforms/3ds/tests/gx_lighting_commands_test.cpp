#include "../runtime/gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <bit>
#include <cmath>
#include <cstring>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
int main(){
    CommandMemory memory;CommandProcessor native(memory),reference(memory);u8 scratch[256];CommandStream stream(native,scratch);
    assert(setCommandStream(&stream));alignas(32) u8 bytes[128];GDLObj gd;
    auto start=[&]{GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);};
    auto finish=[&]{const auto count=GDGetCurrOffset();GDSetCurrent(nullptr);assert(reference.execute({bytes,count}));};
    unsigned lightCases=0;
    for(unsigned index=0;index<8;++index) {
        const auto id=static_cast<GXLightID>(1u<<index);const unsigned base=0x600+index*16;
        GXLightObj light;std::memset(&light,0xa5,sizeof(light));
        const GXColor color{static_cast<u8>(index+20),34,56,78};
        GXInitLightColor(&light,color);GXInitLightAttn(&light,1,.25f,.5f,1,.125f,.0625f);
        GXInitLightPos(&light,2,-3,4);GXInitLightDir(&light,.25f,-.5f,1);
        GXLoadLightObjImm(&light,id);
        start();GDSetLightColor(id,color);GDSetLightAttn(id,1,.25f,.5f,1,.125f,.0625f);
        GDSetLightPos(id,2,-3,4);GDSetLightDir(id,-.25f,.5f,-1);finish();
        for(unsigned w=3;w<16;++w)assert(native.registers().xfValid[base+w]&&native.registers().xf[base+w]==reference.registers().xf[base+w]);
        const auto commands=native.statistics().commands;GXLoadLightObjImm(&light,id);
        assert(native.statistics().commands==commands);
        // Native object stays byte-for-byte unchanged during XF upload.
        GXLightObj copy=light;GXLoadLightObjImm(&light,id);assert(!std::memcmp(&copy,&light,sizeof(light)));
        for(unsigned function=0;function<=6;++function)for(float cutoff:{-1.f,0.f,30.f,60.f,90.f,91.f}) {
            GXInitLightSpot(&light,cutoff,static_cast<GXSpotFn>(function));GXLoadLightObjImm(&light,id);
            start();GDSetLightSpot(id,cutoff,static_cast<GXSpotFn>(function));finish();
            for(unsigned w=4;w<7;++w)assert(native.registers().xf[base+w]==reference.registers().xf[base+w]);
            ++lightCases;
        }
        for(unsigned function=0;function<=3;++function)for(float distance:{-1.f,2.f,32.f})for(float brightness:{0.f,.25f,.5f,1.f}) {
            GXInitLightDistAttn(&light,distance,brightness,static_cast<GXDistAttnFn>(function));GXLoadLightObjImm(&light,id);
            start();GDSetLightDistAttn(id,distance,brightness,static_cast<GXDistAttnFn>(function));finish();
            for(unsigned w=7;w<10;++w)assert(native.registers().xf[base+w]==reference.registers().xf[base+w]);
            ++lightCases;
        }
        GXInitSpecularDir(&light,0,0,-1);GXLoadLightObjImm(&light,id);
        assert(std::bit_cast<float>(native.registers().xf[base+12])==1048576.f);
        assert(std::bit_cast<float>(native.registers().xf[base+15])==1.f);
        GXInitSpecularDir(&light,0,0,1);GXLoadLightObjImm(&light,id);
        for(unsigned w=13;w<16;++w)assert(std::bit_cast<float>(native.registers().xf[base+w])==0);
        for(unsigned bad:{0u,3u,256u}) {
            bool rejected=false;const auto before=native.statistics().commands;
            try{GXLoadLightObjImm(&light,static_cast<GXLightID>(bad));}catch(const std::runtime_error&){rejected=true;}
            assert(rejected&&before==native.statistics().commands);
        }
    }
    GXSetCurrentMtx(GX_PNMTX2);unsigned texCases=0,channelCases=0;
    for(unsigned destination=0;destination<8;++destination)for(unsigned function=0;function<=10;++function)
      for(unsigned source=0;source<GX_MAX_TEXGENSRC;++source){
        const bool bump=function>=2&&function<=9;
        const bool color=source==GX_TG_COLOR0||source==GX_TG_COLOR1;
        const bool texcoord=source>=GX_TG_TEXCOORD0&&source<=GX_TG_TEXCOORD6;
        if((function<=1&&texcoord)||(bump&&!texcoord)||(function==10&&!color))continue;
        const u32 matrix=(destination+1)*3,post=64+destination*3;const u8 normalize=destination%2;
        GXSetTexCoordGen2(static_cast<GXTexCoordID>(destination),static_cast<GXTexGenType>(function),
                         static_cast<GXTexGenSrc>(source),matrix,normalize,post);
        start();GDSetTexCoordGen(static_cast<GXTexCoordID>(destination),static_cast<GXTexGenType>(function),
                                static_cast<GXTexGenSrc>(source),normalize,post);finish();
        // Pinned GD defaults unused emboss-row bits to 5; pinned GX leaves them zero.
        const u32 mask=bump?0xffffffffu:~(7u<<12);
        assert((native.registers().xf[0x1040+destination]&mask)==(reference.registers().xf[0x1040+destination]&mask));
        assert(native.registers().xf[0x1050+destination]==reference.registers().xf[0x1050+destination]);
        const unsigned slot=destination/4,shift=slot?6*(destination-4):6*(destination+1);
        assert(((native.registers().matrixIndex[slot]>>shift)&63)==matrix);
        assert((native.registers().matrixIndex[0]&63)==GX_PNMTX2);++texCases;
      }
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_TRUE,GX_PTIDENTITY);
    assert(native.registers().xf[0x1050]==317);
    const auto beforeRepeat=native.statistics().commands;
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_TRUE,GX_PTIDENTITY);
    assert(native.statistics().commands==beforeRepeat);
    // A new selector preserves fields from the most recent raw CP write.
    GXCmd1u8(GX_LOAD_CP_REG);GXCmd1u8(0x30);GXCmd1u32(0x15555546);
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX3x4,GX_TG_POS,GX_TEXMTX0,GX_FALSE,GX_PTIDENTITY);
    assert(native.registers().matrixIndex[0]==((0x15555546u&~(63u<<6))|(30u<<6)));
    const u32 masks[]{0,1,0x55,0xaa,0xff};
    for(unsigned channel=0;channel<6;++channel)for(unsigned enabled=0;enabled<2;++enabled)
      for(unsigned ambient=0;ambient<2;++ambient)for(unsigned material=0;material<2;++material)
       for(unsigned diffuse=0;diffuse<3;++diffuse)for(unsigned attenuation=0;attenuation<3;++attenuation)
        for(u32 lights:masks){
          GXSetChanCtrl(static_cast<GXChannelID>(channel),enabled,static_cast<GXColorSrc>(ambient),static_cast<GXColorSrc>(material),lights,
                        static_cast<GXDiffuseFn>(diffuse),static_cast<GXAttnFn>(attenuation));
          start();GDSetChanCtrl(static_cast<GXChannelID>(channel),enabled,static_cast<GXColorSrc>(ambient),static_cast<GXColorSrc>(material),lights,
                               static_cast<GXDiffuseFn>(diffuse),static_cast<GXAttnFn>(attenuation));finish();
          const unsigned first=channel<4?channel:channel-4;
          assert(native.registers().xf[0x100e + first]==reference.registers().xf[0x100e + first]);
          if(channel>=4)assert(native.registers().xf[0x1010+first]==reference.registers().xf[0x1010+first]);++channelCases;
        }
    for(unsigned channel=0;channel<6;++channel){
        const GXColor color{0x12,0x34,0x56,static_cast<u8>(0x70+channel)};
        GXSetChanAmbColor(static_cast<GXChannelID>(channel),color);GXSetChanMatColor(static_cast<GXChannelID>(channel),color);
        start();GDSetChanAmbColor(static_cast<GXChannelID>(channel),color);GDSetChanMatColor(static_cast<GXChannelID>(channel),color);finish();
        assert(native.registers().xf[0x100a+(channel&1)]==reference.registers().xf[0x100a+(channel&1)]);
        assert(native.registers().xf[0x100c+(channel&1)]==reference.registers().xf[0x100c+(channel&1)]);
    }
    GXSetCullMode(GX_CULL_FRONT);
    for(u8 textures=0;textures<=8;++textures)for(u8 channels=0;channels<=2;++channels){
        GXSetNumTexGens(textures);GXSetNumChans(channels);
        start();GDSetGenMode(textures,channels,1);finish();
        assert((native.registers().bp[0]&0x3f)==(reference.registers().bp[0]&0x3f));
        assert((native.registers().bp[0]&0xc000)==0x8000);
        assert(native.registers().xf[0x1009]==reference.registers().xf[0x1009]);
        assert(native.registers().xf[0x103f]==reference.registers().xf[0x103f]);
    }
    // Unchanged BP fields still consume a pending one-shot mask.
    GXCmd1u8(GX_LOAD_BP_REG);GXCmd1u32(0xfe000000);GXSetNumChans(2);assert(native.registers().bpMask==0xffffff);
    bool rejected=false;const auto before=native.statistics().commands;
    try{GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_BUMP0,GX_TG_POS,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);}
    catch(const std::runtime_error&){rejected=true;}assert(rejected&&native.statistics().commands==before);
    GXClearVtxDesc();GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    native.setSink([](const CommandDraw&,void*){try{GXSetNumChans(2);}catch(const std::runtime_error&){return false;}return true;});
    const u8 point[]{GX_POINTS,0,1,0,0,0,0,0,0,0,0,0,0,0,0};
    assert(!native.execute(point)&&stream.error()==StreamError::reentrant);assert(stream.reset());
    assert(setCommandStream(nullptr));
    printf("PASS: %u light, %u texgen and %u channel configurations match original GD semantics; native object ABI, specular direction, matrix selectors, aliases, counts and cache boundaries\n",lightCases,texCases,channelCases);
}
