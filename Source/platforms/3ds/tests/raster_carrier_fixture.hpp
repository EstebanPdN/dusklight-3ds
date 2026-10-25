#pragma once
#include "../runtime/gx_commands.hpp"
#include "../runtime/gx_pica_material.hpp"
namespace carrier_test {
using namespace dusk::native::gx;
inline MaterialState capturedMaterial(){MaterialState m{};
m.stageCount=3;
m.texgenCount=1;
m.channelCount=2;
m.indirectCount=0;
m.colorValid=15;
m.konstValid=15;
m.swapValid=15;
m.colors[0]={255,255,255,255};
m.konst[0]={255,255,255,255};
m.swaps[0]={0,1,2,3};
m.colors[1]={0,0,0,0};
m.konst[1]={255,255,255,255};
m.swaps[1]={0,0,0,3};
m.colors[2]={255,255,255,255};
m.konst[2]={255,255,255,255};
m.swaps[2]={1,1,1,3};
m.colors[3]={255,255,255,255};
m.konst[3]={255,255,255,255};
m.swaps[3]={2,2,2,3};
m.stages[0].color={15,15,15,10};
m.stages[0].alpha={7,7,7,7};
m.stages[0].colorOp={0,0,0,1,0};
m.stages[0].alphaOp={0,0,0,1,0};
m.stages[0].texture=7;
m.stages[0].coordinate=7;
m.stages[0].raster=1;
m.stages[0].konstColor=6;
m.stages[0].konstAlpha=0;
m.stages[0].rasterSwap=0;
m.stages[0].textureSwap=0;
m.stages[0].indirect=0;
m.stages[0].textureEnabled=false;
m.stages[1].color={15,0,10,15};
m.stages[1].alpha={7,7,7,7};
m.stages[1].colorOp={0,0,0,1,0};
m.stages[1].alphaOp={0,0,0,1,0};
m.stages[1].texture=7;
m.stages[1].coordinate=7;
m.stages[1].raster=0;
m.stages[1].konstColor=6;
m.stages[1].konstAlpha=0;
m.stages[1].rasterSwap=0;
m.stages[1].textureSwap=0;
m.stages[1].indirect=0;
m.stages[1].textureEnabled=false;
m.stages[2].color={15,8,0,2};
m.stages[2].alpha={7,7,7,4};
m.stages[2].colorOp={0,0,2,1,0};
m.stages[2].alphaOp={0,0,0,1,0};
m.stages[2].texture=0;
m.stages[2].coordinate=0;
m.stages[2].raster=7;
m.stages[2].konstColor=6;
m.stages[2].konstAlpha=0;
m.stages[2].rasterSwap=0;
m.stages[2].textureSwap=0;
m.stages[2].indirect=0;
m.stages[2].textureEnabled=true;
return m;}
// Encode the fixture through the actual command material decoder in adapter tests.
inline void installMaterial(const MaterialState& m,CommandRegisters& r){
 auto bp=[&](unsigned at,unsigned value){r.bp[at]=value;r.bpValid.set(at);};
 bp(0,(m.stageCount-1u)<<10|unsigned(m.channelCount)<<4|m.texgenCount|unsigned(m.indirectCount)<<16);
 auto op=[](const MaterialOperation& v){return unsigned(v.bias)<<16|unsigned(v.operation)<<18|unsigned(v.clamp)<<19|unsigned(v.scale)<<20|unsigned(v.output)<<22;};
 for(unsigned i=0;i<8;++i)bp(0xf6+i,0);
 for(unsigned i=0;i<4;++i){const auto& s=m.swaps[i];bp(0xf6+2*i,s[0]|unsigned(s[1])<<2);bp(0xf7+2*i,s[2]|unsigned(s[3])<<2);}
 for(unsigned i=0;i<m.stageCount;++i){const auto& t=m.stages[i];unsigned rgb=op(t.colorOp),a=op(t.alphaOp)|t.rasterSwap|unsigned(t.textureSwap)<<2;
  for(unsigned n=0;n<4;++n){rgb|=unsigned(t.color[n])<<(12-4*n);a|=unsigned(t.alpha[n])<<(13-3*n);}
  bp(0xc0+2*i,rgb);bp(0xc1+2*i,a);bp(0x10+i,t.indirect);
  const auto shift=12*(i&1);const unsigned order=t.texture|unsigned(t.coordinate)<<3|unsigned(t.textureEnabled)<<6|unsigned(t.raster)<<7;
  const unsigned at=0x28+i/2;bp(at,(i&1?r.bp[at]&0xfffu:0u)|(order<<shift));
  const auto ka=0xf6+i/2;const unsigned ks=10*(i&1);
  bp(ka,r.bp[ka]|(unsigned(t.konstColor)<<(4+ks))|(unsigned(t.konstAlpha)<<(9+ks)));
 }
 for(unsigned i=0;i<4;++i){const auto& c=m.colors[i];const auto& k=m.konst[i];
  r.tevColor[0][2*i]=(unsigned(c[0])&2047)|((unsigned(c[3])&2047)<<12);
  r.tevColor[0][2*i+1]=(unsigned(c[2])&2047)|((unsigned(c[1])&2047)<<12);
  r.tevColor[1][2*i]=k[0]|unsigned(k[3])<<12;r.tevColor[1][2*i+1]=k[2]|unsigned(k[1])<<12;
  for(unsigned n=0;n<2;++n){r.tevColorValid[2*i+n]=bool(m.colorValid&(1u<<i));r.tevColorValid[8+2*i+n]=bool(m.konstValid&(1u<<i));}
 }
}
}
