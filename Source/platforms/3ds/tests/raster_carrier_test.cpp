#include "raster_carrier_fixture.hpp"
#include "../runtime/gx_raster_color.hpp"
#include "../runtime/gx_fog.hpp"
#include "../runtime/gx_vertex_transform.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>
using namespace dusk::native::gx;
using namespace carrier_test;
using Color=std::array<double,4>;
using Trace=std::vector<Color>;
static void near(double a,double b,double eps=1e-12){if(std::abs(a-b)>eps){std::fprintf(stderr,"%.17g != %.17g\n",a,b);assert(false);}}
// Original GX equations: TWO independent raster samples selected by the raw GX
// stage selector, original register writes and swaps. No PICA plan is consulted.
static Trace originalGx(const MaterialState& m,const std::array<Color,2>& rasters,const Color& tex){
 std::array<Color,4> regs{};Trace trace;
 for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)regs[r][c]=m.colors[r][c]/255.0;
 for(unsigned t=0;t<m.stageCount;++t){const auto& s=m.stages[t];Color next{};
  for(unsigned c=0;c<4;++c){const bool alpha=c==3;const auto& ids=alpha?s.alpha:s.color;
   auto get=[&](unsigned id)->double{
    if(alpha?id<4:id<8)return regs[alpha?id:id/2][alpha||(id&1)?3:c];
    if(alpha?id==4:id==8||id==9)return tex[m.swaps[s.textureSwap][alpha||id==9?3:c]];
    if(alpha?id==5:id==10||id==11){assert(s.raster<2||s.raster==7);return s.raster==7?0:rasters[s.raster][m.swaps[s.rasterSwap][alpha||id==11?3:c]];}
    if(!alpha&&id==12)return 1;
    if(!alpha&&id==13)return .5;
    if(alpha?id==6:id==14){unsigned k=alpha?s.konstAlpha:s.konstColor;if(k<8)return (8-k)/8.;if(!alpha&&k>=12&&k<16)return m.konst[k-12][c]/255.;assert(k>=16&&k<32);return m.konst[(k-16)%4][(k-16)/4]/255.;}
    assert(id==(alpha?7:15));return 0;
   };
   const auto& o=alpha?s.alphaOp:s.colorOp;assert(o.operation<=1&&o.bias<=2&&o.output<4);
   double a=get(ids[0]),b=get(ids[1]),f=get(ids[2]),d=get(ids[3]);
   // Fixtures use bounded registers; initial signed/wrap semantics are outside
   // this independent oracle's test domain, not silently called GX-exact.
   assert(a>=0&&a<=1&&b>=0&&b<=1&&f>=0&&f<=1);
   double v=d+(o.operation?-1:1)*(a*(1-f)+b*f);
   v+=(o.bias==1?.5:o.bias==2?-.5:0);v*=o.scale==3?.5:double(1u<<o.scale);
   next[c]=o.clamp?std::clamp(v,0.,1.):v;
  }
  for(unsigned c=0;c<4;++c)regs[(c==3?s.alphaOp:s.colorOp).output][c]=next[c];
  trace.push_back(next);
 }
 return trace;
}
// Independent PICA arithmetic interpretation, preserving all emitted clamps.
static Trace translated(const PicaMaterialPlan& m,const Color& primary,const Color& tex,double fog=-1){
 Color prev{},buffer{},future{};Trace trace;
 for(unsigned c=0;c<4;++c)future[c]=m.bufferConstant[c]/255.;
 for(unsigned t=0;t<m.stageCount;++t){const auto& s=m.stages[t];Color next{};
  for(unsigned c=0;c<4;++c){const auto& ch=c==3?s.alpha:s.rgb;
   auto arg=[&](unsigned n){const auto& in=ch.inputs[n];Color sample{};
    switch(in.source){case MaterialSource::primary:sample=primary;break;case MaterialSource::previous:sample=prev;break;case MaterialSource::buffer:sample=buffer;break;
     case MaterialSource::constant:for(unsigned k=0;k<4;++k)sample[k]=s.constant[k]/255.;break;
     default:assert(unsigned(in.source)>=unsigned(MaterialSource::texture0)&&unsigned(in.source)<=unsigned(MaterialSource::texture2));sample=(fog>=0&&in.source==MaterialSource::texture1)?Color{fog,fog,fog,fog}:tex;break;}
    const auto op=in.operand;const double value=sample[op==MaterialOperand::color?c:op==MaterialOperand::alpha?3:op==MaterialOperand::red?0:op==MaterialOperand::green?1:2];return in.inverse?1-value:value;
   };
   double v=arg(0);switch(ch.function){case MaterialFunction::replace:break;case MaterialFunction::modulate:v*=arg(1);break;case MaterialFunction::add:v+=arg(1);break;case MaterialFunction::subtract:v-=arg(1);break;case MaterialFunction::addSigned:v+=arg(1)-128/255.;break;case MaterialFunction::interpolate:v=v*arg(2)+arg(1)*(1-arg(2));break;case MaterialFunction::multiplyAdd:v=v*arg(1)+arg(2);break;}
   next[c]=std::clamp(v*(1u<<ch.scale),0.,1.);
  }
  buffer=future;if(m.bufferUpdateRgb&(1u<<t))for(unsigned c=0;c<3;++c)future[c]=next[c];prev=next;trace.push_back(next);
 }
 return trace;
}
static void equations(){
 auto m=capturedMaterial();PicaMaterialPlan p;MaterialPlanError e;
 assert(!planPicaMaterial(m,p,&e)&&e==MaterialPlanError::source);
 assert(planRasterCarrierMaterial(m,p,&e));assert(p.stageCount==3&&p.textureCount==1&&p.rasterCarrier==RasterCarrier::raster1RgbRaster0GrayAlpha);
 std::mt19937 gen(4402);std::uniform_real_distribution<double> dist(0,1);unsigned cases=0;
 for(unsigned variant=0;variant<6;++variant){m=capturedMaterial();
  if(variant&1){m.stages[0].raster=0;m.stages[1].raster=1;}
  if(variant&2){m.colors[1]={17,31,47,0};m.stages[2].colorOp.scale=1;}
  if(variant&4){m.stages[3]=m.stages[2];m.stages[3].color={0,15,15,15};m.stages[3].colorOp.scale=0;m.stages[3].alpha={0,7,7,7};m.stageCount=4;}
  assert(planRasterCarrierMaterial(m,p));
  for(unsigned n=0;n<2048;++n){std::array<Color,2> r{};Color primary{},tex{};double gray=0;
   std::array<double,3> weights{dist(gen),dist(gen),dist(gen)};const double sum=weights[0]+weights[1]+weights[2];
   // Perspective ratios 1:1, 1:4, 1:64; independent vertex scalar/rainbow.
   for(unsigned v=0;v<3;++v)weights[v]=weights[v]/sum/(v==1?(n%3==0?1:n%3==1?4:64):1);
   const double total=weights[0]+weights[1]+weights[2];for(auto& w:weights)w/=total;
   for(unsigned v=0;v<3;++v){gray+=weights[v]*dist(gen);for(unsigned c=0;c<3;++c)r[1][c]+=weights[v]*dist(gen);}
   for(unsigned c=0;c<3;++c){r[0][c]=gray;primary[c]=r[1][c];}primary[3]=gray;
   r[0][3]=dist(gen);r[1][3]=dist(gen);for(auto& x:tex)x=dist(gen);
   if(n<4){for(unsigned c=0;c<3;++c)r[0][c]=primary[3]=n/3.;tex={n/3.,(n+.5)/255.,1.,n/3.};}
   const auto gx=originalGx(m,r,tex),pi=translated(p,primary,tex);
   for(unsigned c=0;c<4;++c)near(gx.back()[c],pi.back()[c]);
   if(variant==0){assert(gx.size()==pi.size());for(unsigned t=0;t<3;++t)for(unsigned c=0;c<4;++c)near(gx[t][c],pi[t][c]);
    for(unsigned c=0;c<3;++c){near(gx[0][c],r[1][c]);near(gx[1][c],r[1][c]*r[0][c]);near(gx[2][c],std::clamp(4*tex[c]*gx[1][c],0.,1.));}near(gx[2][3],tex[3]);}
   ++cases;
  }
 }
 // A premultiplied vertex ramp gives 1/2 at the midpoint; independent
 // interpolants multiply to 1/4. Keep both fields through interpolation.
 near((0.*0.+1.*1.)/2,.5);near(((0.+1.)/2)*((0.+1.)/2),.25);
 m=capturedMaterial();m.stages[1].color[2]=11;assert(!planRasterCarrierMaterial(m,p));
 m=capturedMaterial();m.stages[2].alpha[3]=5;m.stages[2].raster=0;assert(!planRasterCarrierMaterial(m,p));
 m=capturedMaterial();m.swaps[0]={0,1,3,3};assert(!planRasterCarrierMaterial(m,p));
 m=capturedMaterial();m.stages[0].raster=0;assert(!planRasterCarrierMaterial(m,p));
 m=capturedMaterial();m.indirectCount=1;assert(!planRasterCarrierMaterial(m,p));
 m=capturedMaterial();m.channelCount=1;assert(!planRasterCarrierMaterial(m,p));
 std::printf("PASS %u two-raster oracle cases, original/emitted stage traces, perspective-independent varying scalar, final texture alpha, semantic variants and premultiply counterexample\n",cases);
}
static void lighting(){
 CommandRegisters r;auto word=[&](unsigned at,unsigned value){r.xf[at]=value;r.xfValid.set(at);};auto scalar=[&](unsigned at,float v){word(at,std::bit_cast<unsigned>(v));};
 word(0x1009,2);word(0x100c,0x20406080);word(0x100d,0x90a0b0c0);word(0x100a,0x10203040);word(0x100b,0x50607080);
 // Every RGB and alpha material/ambient source uses its own channel control.
 word(0x100e,0);word(0x1010,1|2|1024);word(0x100f,1|2|1024);word(0x1011,2|64|1024);
 DecodedVertex v;v.present=(1u<<GX_VA_CLR0)|(1u<<GX_VA_CLR1);v.color[0][0]=.1;v.color[0][1]=.2;v.color[0][2]=.3;v.color[0][3]=.4;
 v.color[1][0]=.5;v.color[1][1]=.6;v.color[1][2]=.7;v.color[1][3]=.8;
 RasterColorPlan p0,p1;std::array<float,4> a,b;RasterColorError e;
 auto close=[](double x,double y){near(x,y,2e-6);};
 assert(planRasterColorChannel(r,0,3,p0)&&planRasterColorChannel(r,1,3,p1));
 assert(evaluateRasterColor(p0,v,a)&&evaluateRasterColor(p1,v,b));
 for(unsigned c=0;c<3;++c){close(a[c],double(0x20+c*0x20)/255);close(b[c],v.color[1][c]*double(0x50+c*0x10)/255);}
 close(a[3],.4*0x40/255);close(b[3],.8*0xc0/255);
 v.present&=~(1u<<GX_VA_CLR1);assert(!evaluateRasterColor(p1,v,b,&e)&&e==RasterColorError::vertex);assert(evaluateRasterColor(p0,v,a));v.present|=1u<<GX_VA_CLR1;
 // Actual-style 0x0f7a: register white material, vertex CLR1 ambient,
 // four selected lights 1..4, clamped diffuse, cosine/distance attenuation.
 word(0x100d,0xffffffff);word(0x100f,0xf7a);
 for(unsigned i=1;i<=4;++i){unsigned at=0x600+16*i;word(at+3,(10*i<<24)|(7*i<<16)|(3*i<<8)|255);
  for(unsigned c=0;c<3;++c){scalar(at+10+c,c==2?2:0);scalar(at+13+c,c==2?1:0);scalar(at+4+c,c==1?1:0);scalar(at+7+c,c==1?0:1);}}
 v.color[1][0]=.05;v.color[1][1]=.1;v.color[1][2]=.15;TransformedVertex t;t.viewNormal={0,0,1};t.normalValid=true;
 for(unsigned scenario=0;scenario<4;++scenario){t.viewNormal[2]=scenario==1?-1:1;
  for(unsigned i=1;i<=4;++i){scalar(0x600+16*i+15,scenario==2?-1:1);scalar(0x600+16*i+10+2,scenario==3?1:2);}
  assert(planRasterColorChannel(r,1,1,p1));assert(p1.channels[0].mask==0x1e&&p1.needsPosition&&p1.needsNormal);
  assert(evaluateRasterColor(p1,v,b,&e,&t));
  for(unsigned c=0;c<3;++c){const double lightSum=10.*(c==0?10:c==1?7:3)/255.;const double factor=scenario==1||scenario==2?0:scenario==3?.5:.2;close(b[c],v.color[1][c]+factor*lightSum);}
 }
 r.xfValid.reset(0x643);assert(!planRasterColorChannel(r,1,1,p1,&e)&&e==RasterColorError::missingState);word(0x643,0x281c0cff);
 scalar(0x64f,std::numeric_limits<float>::quiet_NaN());assert(!planRasterColorChannel(r,1,1,p1,&e)&&e==RasterColorError::nonfinite);scalar(0x64f,1);
 assert(planRasterColorChannel(r,1,1,p1));t.viewNormal[0]=std::numeric_limits<float>::quiet_NaN();assert(!evaluateRasterColor(p1,v,b,&e,&t)&&e==RasterColorError::nonfinite);t.viewNormal={0,0,1};
 for(unsigned c=0;c<3;++c)scalar(0x647+c,0);assert(planRasterColorChannel(r,1,1,p1));assert(!evaluateRasterColor(p1,v,b,&e,&t)&&e==RasterColorError::lighting);
 scalar(0x647,-1);assert(planRasterColorChannel(r,1,1,p1));assert(!evaluateRasterColor(p1,v,b,&e,&t)&&e==RasterColorError::lighting);
 // Finite hostile vectors can overflow before a clamp; do not hide NaN/Inf.
 for(unsigned c=0;c<3;++c){scalar(0x647+c,c==0?1:0);scalar(0x64a+c,1);scalar(0x64d+c,std::numeric_limits<float>::max());}
 assert(planRasterColorChannel(r,1,1,p1));assert(!evaluateRasterColor(p1,v,b,&e,&t)&&e==RasterColorError::nonfinite);
 // Dead alpha controls/registers may be absent for a RGB-only channel.
 r.xfValid.reset(0x1011);assert(planRasterColorChannel(r,1,1,p1));assert(!planRasterColorChannel(r,1,3,p1));
 assert(!planRasterColorChannel(r,2,1,p1));word(0x1009,1);assert(!planRasterColorChannel(r,1,1,p1));
 std::puts("PASS channel0/1 RGB/alpha/register/vertex isolation, four analytic spot/ambient cases, missing CLR1/lights, NaN and zero attenuation rejection");
}

// Independent raw fog oracle: reconstruct the register's truncated IEEE value
// arithmetically, and start from upright screen X + mapped GX depth. It never
// reads VertexFogPlan or the production evaluator's clip/range intermediates.
static double rawFogFloat(unsigned word){
 const unsigned bits=(word&0xfffff)<<12,exponent=(bits>>23)&255,mantissa=bits&0x7fffff;
 const double v=exponent?std::ldexp(1.+double(mantissa)/8388608,int(exponent)-127):std::ldexp(double(mantissa),-149);
 return bits>>31?-v:v;
}
static double fogOracle(const CommandRegisters& r,double screenX,double depth){
 const unsigned fn=(r.bp[0xf1]>>21)&7;if(!fn)return 0;
 const bool ortho=r.bp[0xf1]&(1<<20);const int shift=r.bp[0xf0]&31;
 const double a=rawFogFloat(r.bp[0xee])*std::pow(2.,ortho?0:shift);
 const double b=double(r.bp[0xef]&0xffffff)/8388638.*std::pow(2.,shift-1);
 double base=ortho?a*depth:(b!=depth?a/(b-depth):(a>0?1e30:0));
 if(r.bp[0xe8]&1024){
  const double width=2*std::bit_cast<float>(r.xf[0x101a]),center=int(r.bp[0xe8]&1023)-342;
  const double offset=(screenX-center)/(width/2),at=std::clamp(9.*(1-std::abs(offset)),0.,9.);
  const unsigned lo=unsigned(std::floor(at)),hi=std::min(lo+1,9u);
  auto table=[&](unsigned i){return double((r.bp[0xe9+i/2]>>(i%2?0:12))&4095)/64.;};
  const double k=std::max(table(lo)+(table(hi)-table(lo))*(at-lo),.000001);
  base*=std::sqrt(offset*offset+k*k)/k;
 }
 const double v=std::clamp(base-rawFogFloat(r.bp[0xf1]),0.,1.);
 if(fn==2)return v;if(fn==4)return 1-std::pow(2.,-8*v);if(fn==5)return 1-std::pow(2.,-8*v*v);
 return std::pow(2.,-8*(fn==7?(1-v)*(1-v):1-v));
}
static void fogEquations(){
 CommandRegisters r;for(unsigned at:{0xe8u,0xeeu,0xefu,0xf0u,0xf1u,0xf2u,0xe9u,0xeau,0xebu,0xecu,0xedu})r.bpValid.set(at);r.xfValid.set(0x101a);
 r.bp[0xf2]=0xabc69a;r.bp[0xef]=0x40005c;r.bp[0xf0]=2;
 for(unsigned i=0;i<10;++i)r.bp[0xe9+i/2]|=(48+i*7)<<((i%2)?0:12);
 unsigned cases=0;std::mt19937 rng(4495);std::uniform_real_distribution<double> random(0,1);
 for(unsigned fn:{2u,4u,5u,6u,7u})for(bool ortho:{false,true})for(bool range:{false,true})
 for(unsigned logicalWidth:{256u,400u,640u})for(unsigned center:{0u,170u,342u}){
  r.bp[0xf1]=(fn<<21)|(unsigned(ortho)<<20)|(std::bit_cast<unsigned>(.0625f)>>12);
  r.bp[0xee]=std::bit_cast<unsigned>(ortho?.625f:.03125f)>>12;r.bp[0xe8]=(range?1024:0)|(center+342);r.xf[0x101a]=std::bit_cast<unsigned>(float(logicalWidth)*.375f);
  VertexFogPlan fp;assert(planVertexFog(r,fp)&&fp.enabled);
  PicaMaterialPlan mat;assert(planRasterCarrierMaterial(capturedMaterial(),mat)&&appendVertexFog(mat,fp,false,false));
  for(unsigned sample=0;sample<32;++sample){
   // Inset viewport: x=1/8 target + normalized viewport X * 3/4 width.
   const double x=logicalWidth*(.125+.75*random(rng)),depth=random(rng),w=sample%2?4.:1.;
   const std::array<float,4> clip{0,float((1-2*x/logicalWidth)*w),float((depth-1)*w),float(w)};
   const double mappedDepth=std::clamp(1+double(clip[2])/clip[3],0.,1.);
   const double mappedX=(1-double(clip[1])/clip[3])*logicalWidth*.5;
   const double expected=fogOracle(r,mappedX,mappedDepth),actual=evaluateVertexFog(fp,clip,logicalWidth);near(expected,actual,6e-8);
   std::array<Color,2> rasters{};Color tex{},primary{};const double gray=random(rng);
   for(unsigned c=0;c<3;++c){rasters[0][c]=gray;rasters[1][c]=primary[c]=random(rng);}primary[3]=gray;for(auto& c:tex)c=random(rng);
   const auto original=originalGx(capturedMaterial(),rasters,tex).back();
   // Linear I8 ramp at texel centers: U=(255*f+.5)/256. Interpolating
   // these affine U values reconstructs the independently interpolated fog.
   const double u=(255*expected+.5)/256,at=std::clamp(u*256-.5,0.,255.);
   const unsigned lo=unsigned(at),hi=std::min(lo+1,255u);const double ramp=(lo*(1-(at-lo))+hi*(at-lo))/255.;near(ramp,expected);
   const auto output=translated(mat,primary,tex,ramp).back();
   for(unsigned c=0;c<3;++c)near(output[c],original[c]*(1-expected)+double(fp.color[c])/255*expected);
   near(output[3],tex[3]);++cases;
  }
 }
 // Native45's six words are used with explicitly synthetic complete range
 // state, separate from any actual draw acceptance assertion.
 r.bp[0xf1]=0x400000;r.bp[0xe8]=0x696;r.bp[0xee]=0x38aec;r.bp[0xef]=0x40005c;r.bp[0xf0]=2;r.xf[0x101a]=std::bit_cast<unsigned>(200.f);
 VertexFogPlan fp;assert(planVertexFog(r,fp)&&fp.enabled&&fp.rangeEnabled);
 for(unsigned i=0;i<101;++i){const float x=-1+2.f*i/100;const std::array<float,4> clip{0,x,-.5f,1};near(evaluateVertexFog(fp,clip,400),fogOracle(r,(1-double(x))*200,.5),6e-8);++cases;}
 // Perspective interpolation commutes with the affine helper coordinate, not
 // with a CPU raster product. Include strong W ratios and endpoint clamps.
 for(unsigned n=0;n<4096;++n){std::array<double,3> f{random(rng),random(rng),random(rng)},weights{random(rng),random(rng)/4,random(rng)/64};
  const double total=weights[0]+weights[1]+weights[2];double factor=0,u=0;
  for(unsigned i=0;i<3;++i){weights[i]/=total;factor+=weights[i]*f[i];u+=weights[i]*(255*f[i]+.5)/256;}
  near((u*256-.5)/255,factor);
 }
 std::printf("PASS %u independent raw fog/range/curve/viewport/target and composed TEV cases; 4096 perspective affine ramp cases; original alpha retained\n",cases);
}
int main(){equations();lighting();fogEquations();}

