// Private, read-only compatibility probe. Saves technical metadata only, never model or texture bytes.
#include "../runtime/disc.hpp"
#include "../runtime/resource_archive.hpp"
#include <aurora/dl.hpp>
#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_material.hpp"
#include "../runtime/gx_raster_color.hpp"
#include "material_equivalence.hpp"
#include "../runtime/gx_commands.hpp"
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <stdexcept>
#include <bit>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>
extern "C" void OSPanic(const char*,int,const char* msg,...){throw std::runtime_error(msg);}
extern "C" void DCFlushRange(void*,u32){}
using namespace dusk::native;
using namespace aurora::gx::dl;
using Bytes = std::vector<u8>;
using Span = std::span<const u8>;
static Span range(Span b, size_t at, size_t n) {
  assert(at <= b.size() && n <= b.size() - at); return b.subspan(at,n);
}
static u32 word(Span b, size_t at) {
  auto s=range(b,at,4); return (u32(s[0])<<24)|(u32(s[1])<<16)|(u32(s[2])<<8)|s[3];
}
static u16 half(Span b, size_t at) { auto s=range(b,at,2); return (u16(s[0])<<8)|s[1]; }
int main(int argc,char** argv) {
  assert(argc==2); Disc disc; assert(disc.open(argv[1])==DiscStatus::ready);
  DiscFile file; assert(disc.findFile("/res/Object/Title.arc",file)==DiscFileStatus::ready);
  constexpr size_t budget=32*1024*1024;
  assert(file.size()<=budget); Bytes encoded(file.size());
  assert(disc.readFile(file,0,encoded.data(),encoded.size()));
  u32 expanded=0; assert(yaz0ExpandedSize(encoded.data(),encoded.size(),budget,expanded)==ResourceStatus::ready);
  Bytes archiveBytes(expanded); size_t written=0;
  assert(decodeYaz0(encoded.data(),encoded.size(),archiveBytes.data(),archiveBytes.size(),budget,written)==ResourceStatus::ready);
  RarcArchive archive; assert(archive.open(archiveBytes.data(),written,{budget,4096,65536,255})==ResourceStatus::ready);
  RarcEntry model; assert(archive.findFile("titlelogo_r.bmd",model)==ResourceStatus::ready);
  Span bmd(model.contents.data,model.contents.size); assert(bmd.size()>=32 && !memcmp(bmd.data(),"J3D2bmd3",8));
  assert(word(bmd,8)==bmd.size()); const u32 blocks=word(bmd,12); assert(blocks<=64);
  size_t at=32; Span materials, textures;
  for(u32 i=0;i<blocks;++i) {
    const u32 size=word(bmd,at+4); assert(size>=8); const auto block=range(bmd,at,size);
    if(!memcmp(block.data(),"MAT3",4)) { assert(materials.empty()); materials=block; }
    if(!memcmp(block.data(),"TEX1",4)) { assert(textures.empty()); textures=block; }
    at+=size;
  }
  range(materials,0,0x84);range(textures,0,0x14);
  const unsigned count=half(materials,8),textureCount=half(textures,8);
  const unsigned init=word(materials,12),remap=word(materials,16);
  printf("{\"materials\":%u,\"textures\":%u,\"payload_saved\":false}\n",count,textureCount);
  using namespace dusk::native::gx;
  CommandMemory memory;CommandProcessor processor(memory);MaterialState decoded;
  alignas(32) u8 commands[8192];GDLObj gd;
  unsigned verified=0;
  for(unsigned m=0;m<count;++m){
    const auto data=range(materials,init+0x14c*half(materials,remap+2*m),0x14c);
    const unsigned stages=range(materials,word(materials,0x58)+data[4],1)[0];
    const unsigned texgens=range(materials,word(materials,0x34)+data[3],1)[0];
    const unsigned channels=range(materials,word(materials,0x24)+data[2],1)[0];
    assert(stages<=16 && texgens<=8 && channels<=2);
    const auto indirect=range(materials,word(materials,0x18)+m*0x138,0x138);
    const unsigned indCount=indirect[0]?indirect[1]:0;assert(indCount<=4);
    printf("{\"material_draw_state\":%u,\"channel_controls\":[",m);
    for(unsigned c=0;c<4;++c){
      const auto index=half(data,0x0c+2*c);
      printf("%s",c?",":"");
      if(index==0xffff)printf("null");
      else{auto info=range(materials,word(materials,0x28)+8*index,8);
        printf("[%u,%u,%u,%u,%u,%u]",info[0],info[1],info[2],info[3],info[4],info[5]);}
    }
    auto z=range(materials,word(materials,0x74)+4*data[6],4);
    auto blend=range(materials,word(materials,0x70)+4*half(data,0x148),4);
    auto alpha=range(materials,word(materials,0x6c)+8*half(data,0x146),8);
    printf("],\"zmode\":[%u,%u,%u],\"blend\":[%u,%u,%u,%u],\"alpha\":[%u,%u,%u,%u,%u],\"texture_indices\":[",z[0],z[1],z[2],blend[0],blend[1],blend[2],blend[3],alpha[0],alpha[1],alpha[2],alpha[3],alpha[4]);
    for(unsigned t=0;t<8;++t){auto index=half(data,0x84+2*t);
      if(t)printf(",");if(index==0xffff)printf("null");else printf("%u",half(materials,word(materials,0x48)+2*index));}
    printf("]}\n");
    processor.reset();GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
    GDSetGenMode2(texgens,channels,stages,indCount,GX_CULL_NONE);
    // Original J3D channel storage is RGB0,A0,RGB1,A1; XF register order is
    // RGB0,RGB1,A0,A1 (J3DMatBlock.cpp writes [0],[2],[1],[3]).
    constexpr GXChannelID ids[]{GX_COLOR0,GX_ALPHA0,GX_COLOR1,GX_ALPHA1};
    for(unsigned c=0;c<4;++c){const auto index=half(data,0x0c+2*c);assert(index!=0xffff);
      auto info=range(materials,word(materials,0x28)+8*index,8);
      GDSetChanCtrl(ids[c],info[0],static_cast<GXColorSrc>(info[5]),static_cast<GXColorSrc>(info[1]),info[2],static_cast<GXDiffuseFn>(info[3]),static_cast<GXAttnFn>(info[4]));}
    for(unsigned c=0;c<2;++c){const auto index=half(data,8+2*c);GXColor color{255,255,255,255};
      if(index!=0xffff){auto info=range(materials,word(materials,0x20)+4*index,4);color={info[0],info[1],info[2],info[3]};}
      GDSetChanMatColor(c?GX_COLOR1A1:GX_COLOR0A0,color);}

    std::array<std::array<s16,4>,3> expectedColors{};
    std::array<std::array<u8,4>,4> expectedKonst{};
    for(unsigned r=0;r<3;++r){
      const auto index=half(data,0xdc+2*r);auto& c=expectedColors[r];
      if(index!=0xffff){auto bytes=range(materials,word(materials,0x50)+8*index,8);
        for(unsigned n=0;n<4;++n)c[n]=std::bit_cast<s16>(half(bytes,2*n));}
      GDSetTevColorS10(static_cast<GXTevRegID>(r+1),{c[0],c[1],c[2],c[3]});
    }
    for(unsigned r=0;r<4;++r){
      const auto index=half(data,0x94+2*r);auto& c=expectedKonst[r];c={255,255,255,255};
      if(index!=0xffff){auto bytes=range(materials,word(materials,0x54)+4*index,4);
        for(unsigned n=0;n<4;++n)c[n]=bytes[n];}
      GDSetTevKColor(static_cast<GXTevKColorID>(r),{c[0],c[1],c[2],c[3]});
      const auto swapIndex=half(data,0x124+2*r);std::array<u8,4> swap{0,1,2,3};
      if(swapIndex!=0xffff){auto bytes=range(materials,word(materials,0x64)+4*swapIndex,4);
        for(unsigned n=0;n<4;++n)swap[n]=bytes[n];}
      GDSetTevSwapModeTable(static_cast<GXTevSwapSel>(r),static_cast<GXTevColorChan>(swap[0]),static_cast<GXTevColorChan>(swap[1]),static_cast<GXTevColorChan>(swap[2]),static_cast<GXTevColorChan>(swap[3]));
    }
    for(unsigned t=0;t<stages;t+=2){
      std::array<u8,3> orders[2]{{255,255,255},{255,255,255}};
      for(unsigned side=0;side<2&&t+side<stages;++side){
        auto record=range(materials,word(materials,0x4c)+4*half(data,0xbc+2*(t+side)),4);
        for(unsigned n=0;n<3;++n)orders[side][n]=record[n];
      }
      GDSetTevOrder(static_cast<GXTevStageID>(t),static_cast<GXTexCoordID>(orders[0][0]),static_cast<GXTexMapID>(orders[0][1]),static_cast<GXChannelID>(orders[0][2]),static_cast<GXTexCoordID>(orders[1][0]),static_cast<GXTexMapID>(orders[1][1]),static_cast<GXChannelID>(orders[1][2]));
      GDSetTevKonstantSel(static_cast<GXTevStageID>(t),static_cast<GXTevKColorSel>(data[0x9c+t]),static_cast<GXTevKAlphaSel>(data[0xac+t]),static_cast<GXTevKColorSel>(data[0x9d+t]),static_cast<GXTevKAlphaSel>(data[0xad+t]));
    }
    printf("{\"material\":%u,\"stages\":%u,\"texgens\":%u,\"channels\":%u,\"stage_config\":[",m,stages,texgens,channels);
    for(unsigned t=0;t<stages;++t){
      auto stage=range(materials,word(materials,0x5c)+20*half(data,0xe4+2*t),20);
      auto order=range(materials,word(materials,0x4c)+4*half(data,0xbc+2*t),4);
      const auto id=static_cast<GXTevStageID>(t);
      const auto swapIndex=half(data,0x104+2*t);u8 rasterSwap=0,textureSwap=0;
      if(swapIndex!=0xffff){auto swap=range(materials,word(materials,0x60)+4*swapIndex,4);rasterSwap=swap[0];textureSwap=swap[1];}
      GDSetTevColorCalc(id,static_cast<GXTevColorArg>(stage[1]),static_cast<GXTevColorArg>(stage[2]),static_cast<GXTevColorArg>(stage[3]),static_cast<GXTevColorArg>(stage[4]),static_cast<GXTevOp>(stage[5]),static_cast<GXTevBias>(stage[6]),static_cast<GXTevScale>(stage[7]),stage[8],static_cast<GXTevRegID>(stage[9]));
      GDSetTevAlphaCalcAndSwap(id,static_cast<GXTevAlphaArg>(stage[10]),static_cast<GXTevAlphaArg>(stage[11]),static_cast<GXTevAlphaArg>(stage[12]),static_cast<GXTevAlphaArg>(stage[13]),static_cast<GXTevOp>(stage[14]),static_cast<GXTevBias>(stage[15]),static_cast<GXTevScale>(stage[16]),stage[17],static_cast<GXTevRegID>(stage[18]),static_cast<GXTevSwapSel>(rasterSwap),static_cast<GXTevSwapSel>(textureSwap));
      if(indirect[0]){
        const auto ind=range(indirect,0x78+12*t,12);
        GDSetTevIndirect(id,static_cast<GXIndTexStageID>(ind[0]),static_cast<GXIndTexFormat>(ind[1]),static_cast<GXIndTexBiasSel>(ind[2]),static_cast<GXIndTexMtxID>(ind[3]),static_cast<GXIndTexWrap>(ind[4]),static_cast<GXIndTexWrap>(ind[5]),ind[6],ind[7],static_cast<GXIndTexAlphaSel>(ind[8]));
      }else GDSetTevDirect(id);
      printf("%s{\"order\":[%u,%u,%u],\"color_inputs\":[%u,%u,%u,%u],\"color_op\":[%u,%u,%u,%u,%u],\"alpha_inputs\":[%u,%u,%u,%u],\"alpha_op\":[%u,%u,%u,%u,%u]}",t?",":"",order[0],order[1],order[2],stage[1],stage[2],stage[3],stage[4],stage[5],stage[6],stage[7],stage[8],stage[9],stage[10],stage[11],stage[12],stage[13],stage[14],stage[15],stage[16],stage[17],stage[18]);
    }
    const auto length=GDGetCurrOffset();GDSetCurrent(nullptr);
    assert(processor.execute({commands,length})&&decodeMaterial(processor.registers(),decoded));
    assert(decoded.stageCount==stages&&decoded.channelCount==channels&&decoded.texgenCount==texgens);
    assert(decoded.indirectCount==indCount);
    assert(decoded.colorValid==14&&decoded.konstValid==15&&decoded.swapValid==15);
    for(unsigned r=0;r<3;++r)assert(decoded.colors[r+1]==expectedColors[r]);
    assert(decoded.konst==expectedKonst);
    for(unsigned t=0;t<stages;++t){
      auto stage=range(materials,word(materials,0x5c)+20*half(data,0xe4+2*t),20);
      for(unsigned n=0;n<4;++n){assert(decoded.stages[t].color[n]==stage[1+n]);assert(decoded.stages[t].alpha[n]==stage[10+n]);}
      assert(decoded.stages[t].colorOp.operation==stage[5]&&decoded.stages[t].colorOp.scale==stage[7]);
      assert(decoded.stages[t].alphaOp.operation==stage[14]&&decoded.stages[t].alphaOp.scale==stage[16]);
    }
    ++verified;
    printf("],\"indirect_stages\":%u,\"indirect_words\":[",indCount);
    for(unsigned t=0;t<stages;++t)printf("%s%u",t?",":"",decoded.stages[t].indirect);
    PicaMaterialPlan plan;MaterialPlanError error;
    const bool admitted=planPicaMaterial(decoded,plan,&error);
    RasterColorPlan rasterColor;
    if(admitted){material_test::compare(decoded,plan,256);RasterColorError colorError;
      assert(planRasterColor(processor.registers(),plan,rasterColor,&colorError));
      DecodedVertex vertex;std::array<float,4> value;assert(evaluateRasterColor(rasterColor,vertex,value));
      assert(!rasterColor.vertex); // Title has no vertex color arrays.
      assert(rasterColor.required==((m==0||m==1)?2:0));
    }

    printf("],\"pica_admitted\":%s,\"pica_stages\":%u,\"pica_textures\":%u,\"pica_error\":\"%s\"}\n",admitted?"true":"false",plan.stageCount,plan.textureCount,materialPlanErrorText(error));
    if(admitted)printf("{\"material_raster_dependency\":%u,\"required_mask\":%u,\"vertex_mask\":%u}\n",m,rasterColor.required,rasterColor.vertex);
  }
  printf("{\"decoded_materials\":%u,\"color_banks_retained\":true}\n",verified);
  for(unsigned t=0;t<textureCount;++t){
    const auto info=range(textures,word(textures,12)+32*t,32);
    printf("{\"texture\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"wrap\":[%u,%u],\"mips\":%u}\n",t,info[0],half(info,2),half(info,4),info[6],info[7],info[24]);
  }
}
