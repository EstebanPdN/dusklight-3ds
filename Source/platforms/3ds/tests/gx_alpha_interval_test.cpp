// Execute the real raster, PICA and arena backend against mock SDK entry points.
// Samples/coverage are scripted; this is not a PICA rasterizer or native proof.
#include "../gx_pica.cpp"
#include "../runtime/gx_commands.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
using namespace dusk::native;
using namespace dusk::native::gx;
namespace {
struct Sample { unsigned alpha=150;float z=.6f;float red=.8f;bool covers=true; };
struct Pixel {float red=.1f,alpha=.4f,z=.3f;unsigned stencil=0xaa;};
struct Effect { bool alpha{},depth{},stencil{};GPU_TESTFUNC af=GPU_ALWAYS,df=GPU_ALWAYS,sf=GPU_ALWAYS;
 unsigned ar{},sr{},in{},out{},writes{};GPU_STENCILOP ops[3]{};
 GPU_BLENDFACTOR src{},dst{},srcA{},dstA{};GPU_BLENDEQUATION eq{},eqA{};
} effect;
std::vector<Sample> samples;
Pixel pixel;
std::vector<unsigned> emitted;
std::vector<std::byte> immutable;
const GpuVertex* boundVertices;
const PicaMaterialPlan* expectedMaterial{};
std::array<std::byte,sizeof(PicaMaterialPlan)> expectedMaterialBytes{};
C3D_FrameBuf frame{};
C3D_AttrInfo attrs;C3D_BufInfo bufs;
unsigned prepareCalls{},failPrepare{},ownerCalls{},failOwner{},materialCalls{};
bool owner=true,failAddress=false,failFlush=false;
unsigned targetChange{},flushes{};
int targetColor,targetDepth,otherDepth;
const GpuGeometryBatch* callbackBatch{};
std::size_t bytesBeforeCallback{};
bool test(unsigned op,float a,float b){
 switch(op){case GPU_NEVER:return false;case GPU_LESS:return a<b;case GPU_EQUAL:return a==b;
 case GPU_LEQUAL:return a<=b;case GPU_GREATER:return a>b;case GPU_NOTEQUAL:return a!=b;
 case GPU_GEQUAL:return a>=b;default:return true;}
}
bool gxTest(unsigned op,unsigned a,unsigned b){
 const std::array<bool,8> truth{false,a<b,a==b,a<=b,a>b,a!=b,a>=b,true};return truth.at(op);
}
bool oracle(const AlphaCompare& c,unsigned a){
 const bool x=gxTest(c.compare0,a,c.reference0),y=gxTest(c.compare1,a,c.reference1);
 const std::array<bool,4> result{x&&y,x||y,x!=y,x==y};return result.at(c.op);
}
bool ownership(const AlphaIntervalTarget& t,void*){
 ++ownerCalls;
 assert(t.color==frame.colorBuf&&t.depth==frame.depthBuf&&t.width==240&&t.height==400);
 return owner&&ownerCalls!=failOwner;
}
float factor(GPU_BLENDFACTOR f,float src,float dst,float sa,float da){
 switch(f){case GPU_ZERO:return 0;case GPU_ONE:return 1;case GPU_SRC_ALPHA:return sa;
 case GPU_ONE_MINUS_SRC_ALPHA:return 1-sa;case GPU_DST_ALPHA:return da;case GPU_ONE_MINUS_DST_ALPHA:return 1-da;
 case GPU_SRC_COLOR:return src;case GPU_ONE_MINUS_SRC_COLOR:return 1-src;case GPU_DST_COLOR:return dst;
 case GPU_ONE_MINUS_DST_COLOR:return 1-dst;default:assert(false);return 0;}
}
float blend(float src,float dst,float sa,float da,GPU_BLENDFACTOR sf,GPU_BLENDFACTOR df,GPU_BLENDEQUATION eq){
 const auto a=src*factor(sf,src,dst,sa,da),b=dst*factor(df,src,dst,sa,da);
 return std::clamp(eq==GPU_BLEND_REVERSE_SUBTRACT?b-a:eq==GPU_BLEND_SUBTRACT?a-b:a+b,0.f,1.f);
}
void stencilOp(GPU_STENCILOP op){
 const unsigned v=op==GPU_STENCIL_ZERO?0:op==GPU_STENCIL_REPLACE?effect.sr:pixel.stencil;
 pixel.stencil=(pixel.stencil&~effect.out)|(v&effect.out);
}
RasterPlan raster(){RasterPlan r;r.alphaInterval={true,101,184};r.alpha={6,101};r.alphaSecond={3,184};
 r.alphaIntervalPolicy={true,ownership,nullptr};r.depth={true,3,true};r.writeRgb=r.writeAlpha=r.storesAlpha=true;
 r.blend={1,4,5,0};r.scissor={0,0,240,400,false};return r;}
void reset(){
 // Test-only teardown boundary: the mock has no in-flight GPU. Recreate arena.
 destroyGxGeometry();assert(initializeGxGeometry()&&beginGxGeometryFrame());
 dusk::native::ready=true;dusk::native::intervalPoisoned=dusk::native::intervalActive=false;
 prepareCalls=failPrepare=ownerCalls=failOwner=materialCalls=targetChange=flushes=0;
 owner=true;failAddress=failFlush=false;callbackBatch=nullptr;emitted.clear();immutable.clear();effect={};pixel={};
 frame={};frame.colorBuf=&targetColor;frame.depthBuf=&targetDepth;frame.width=240;frame.height=400;
 frame.colorFmt=GPU_RB_RGBA8;frame.depthFmt=GPU_RB_DEPTH24_STENCIL8;frame.depthMask=3;frame.colorMask=15;
}
GpuGeometryBatch batch(unsigned triangles){
 GpuGeometryBatch b;assert(reserveGxGeometry(triangles*3,triangles*3,b));
 for(unsigned n=0;n<b.vertices.size();++n){auto& v=b.vertices[n];v={};v.position={float(n),0,-.5f,1};v.color={.8f,.2f,.3f,.7f};b.indices[n]=n;}
 immutable.assign(std::as_bytes(b.vertices).begin(),std::as_bytes(b.vertices).end());return b;
}
void restored(const RasterPlan& r){
 assert(!effect.stencil&&effect.out==0&&effect.ops[0]==GPU_STENCIL_KEEP&&effect.ops[1]==GPU_STENCIL_KEEP&&effect.ops[2]==GPU_STENCIL_KEEP);
 assert(effect.alpha&&effect.af==GPU_GEQUAL&&effect.ar==r.alphaInterval.lower);
 assert(effect.depth==r.depth.enabled);
}
bool submitCommand(const RasterPlan& r,const PicaMaterialPlan& material,const GpuGeometryBatch& b){
 expectedMaterial=&material;std::memcpy(expectedMaterialBytes.data(),&material,sizeof(material));
 const bool result=submitGxCommandGeometry(r,material,b);
 assert(std::memcmp(expectedMaterialBytes.data(),&material,sizeof(material))==0);return result;
}
void truthTests(){
 unsigned checked=0,accepted=0;
 auto check=[&](AlphaCompare c){
  AlphaInterval p{true,1,2};const bool planned=planAlphaInterval(c,p);unsigned first=256,last=0,count=0;
  for(unsigned a=0;a<256;++a)if(oracle(c,a)){first=std::min(first,a);last=a;++count;}
  const bool interior=first>0&&first<256&&last<255&&count==last-first+1;
  assert(planned==interior);if(planned){assert(p.enabled&&p.lower==first&&p.upper==last);++accepted;}
  else assert(!p.enabled&&!p.lower&&!p.upper);
  ++checked;
 };
 for(unsigned lo=0;lo<256;++lo)for(unsigned hi=0;hi<256;++hi){
  check({6,std::uint8_t(lo),0,3,std::uint8_t(hi)});
  check({6,std::uint8_t(lo),3,3,std::uint8_t(hi)});
 }
 unsigned seed=0xabcde;
 for(unsigned i=0;i<32768;++i){seed=seed*1664525+1013904223;check({std::uint8_t(seed&7),std::uint8_t(seed>>8),std::uint8_t((seed>>3)&3),std::uint8_t((seed>>5)&7),std::uint8_t(seed>>16)});}
 AlphaInterval p;assert(planAlphaInterval({6,185,3,3,100},p)&&p.lower==101&&p.upper==184);
 for(auto bad:{AlphaCompare{8,0,0,0,0},AlphaCompare{0,0,4,0,0},AlphaCompare{0,0,0,8,0}})assert(!planAlphaInterval(bad,p));
 std::printf("PASS %u independent 256-byte interval truth sets (%u interior intervals)\n",checked,accepted);
}
void plannerTests(){
 CommandRegisters c;for(unsigned reg:{0u,0x20u,0x21u,0x40u,0x41u,0x42u,0x43u,0xf1u,0xf3u,0xf5u})c.bpValid.set(reg);
 c.bp[0x20]=342|(342u<<12);c.bp[0x21]=789|(949u<<12);c.bp[0xf3]=0xde64b9;c.bp[0x41]=0xf42d;c.bp[0x40]=7;c.bp[0x43]=0x41;
 c.xfValid.set(0x1026);c.xf[0x1026]=0;
 RasterPlan p;RasterError e;const auto policy=raster().alphaIntervalPolicy;
 auto plan=[&](AlphaIntervalPolicy v){return planRaster(c,p,&e,FogPolicy::vertexApproximation,ColorStoragePolicy::rgba8ForRgba6,ClipPolicy::validateGeometry,true,true,v);};
 assert(!plan({})&&e==RasterError::alpha);
 assert(!plan({true,nullptr,nullptr})&&e==RasterError::alphaIntervalOwnership);
 assert(plan(policy)&&p.alphaInterval.enabled&&p.alphaInterval.lower==101&&p.alphaInterval.upper==184&&!p.alphaUnion);
 c.xf[0x1026]=1;assert(!plan(policy)&&e==RasterError::alphaIntervalUi);c.xf[0x1026]=0;
 c.xfValid.reset(0x1026);assert(!plan(policy)&&e==RasterError::alphaIntervalUi);c.xfValid.set(0x1026);
 c.bp[0xf1]=4u<<20;assert(plan(policy)&&p.alphaInterval.enabled);
 assert(!planRaster(c,p,&e,FogPolicy::diagnosticDisabled,ColorStoragePolicy::rgba8ForRgba6,ClipPolicy::validateGeometry,true,true,policy)&&e==RasterError::fog);
 assert(!planRaster(c,p,&e,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6,ClipPolicy::validateGeometry,true,true,policy)&&e==RasterError::fog);c.bp[0xf1]=0;
 c.bp[0x40]|=16;assert(!plan(policy)&&e==RasterError::earlyDepth);c.bp[0x40]&=~16u;
 assert(plan(policy));c.bp[0x43]&=~64u;c.bp[0x40]|=16;assert(plan(policy));
 std::puts("PASS default-off, ownership/UI/fog gates, and early-depth side-effect rejection");
}
void sequenceTests(){
 PicaMaterialPlan material{};
 for(unsigned alpha=0;alpha<256;++alpha){reset();auto r=raster();auto b=batch(1);samples={{alpha,.6f,.8f,true}};
  assert(submitCommand(r,material,b));restored(r);
  assert(emitted==std::vector<unsigned>({0,0,0,0})&&pixel.stencil==0xaa&&alphaIntervalCopySafe());
  const bool accept=alpha>=101&&alpha<=184;const float a=alpha/255.f;
  assert(std::abs(pixel.red-(accept?.8f*a+.1f*(1-a):.1f))<1e-6f);
  assert(std::abs(pixel.alpha-(accept?a*a+.4f*(1-a):.4f))<1e-6f);
  assert(pixel.z==(accept?.6f:.3f));assert(materialCalls==1&&flushes==1);
 }
 unsigned sequences=0;
 for(unsigned cmp=0;cmp<8;++cmp)for(bool write:{false,true})for(bool enabled:{false,true})for(bool rgb:{false,true})
 for(bool alphaWrite:{false,true})for(bool subtract:{false,true}){
  reset();auto r=raster();r.depth={enabled,std::uint8_t(cmp),write};r.writeRgb=rgb;r.writeAlpha=alphaWrite;r.subtractBlend=subtract;
  samples={{150,.6f,.8f,true},{220,.9f,.1f,true},{130,.6f,.4f,true},{99,.8f,.9f,true},{184,.2f,.7f,true},{101,.9f,.2f,false}};
  auto expected=pixel;
  for(const auto& s:samples){
   const bool depth=!enabled||gxTest(cmp,unsigned((1-s.z)*1000),unsigned((1-expected.z)*1000));
   if(!s.covers||!oracle({6,185,3,3,100},s.alpha)||!depth)continue;
   const float a=s.alpha/255.f;
   if(rgb)expected.red=subtract?std::clamp(expected.red-s.red,0.f,1.f):s.red*a+expected.red*(1-a);
   if(alphaWrite)expected.alpha=subtract?std::clamp(expected.alpha-a,0.f,1.f):a*a+expected.alpha*(1-a);
   if(enabled&&write)expected.z=s.z;
  }
  auto b=batch(samples.size());assert(submitCommand(r,material,b));restored(r);
  assert(std::abs(pixel.red-expected.red)<1e-6f&&std::abs(pixel.alpha-expected.alpha)<1e-6f&&pixel.z==expected.z&&pixel.stencil==expected.stencil);
  for(unsigned i=0;i<emitted.size();++i)assert(emitted[i]==i/4);
  ++sequences;
 }
 reset();auto r=raster();auto b=batch(1);samples={{150,.6f,.8f,true}};
 assert(submitCommand(r,material,b)&&pixel.stencil==0xaa);
 r.alphaInterval={};r.alpha={7,0};samples={{20,.8f,.9f,true}};b=batch(1);
 const auto red=pixel.red;assert(submitCommand(r,material,b));
 assert(!effect.stencil&&!effect.alpha&&pixel.red!=red&&pixel.z==.8f&&pixel.stencil==0xaa);
 std::printf("PASS all 256 captured-alpha emissions and %u overlapping ordered blend/depth/write sequences\n",sequences);
}
void failures(){
 PicaMaterialPlan material{};
 reset();auto pending=batch(1);auto unowned=pending;++unowned.ticket;
 auto callback=[](AlphaIntervalPass,void*){assert(false);return false;};
 assert(!submitGxTriangleAlphaInterval(unowned,callback,nullptr));
 assert(!submitGxTriangleAlphaInterval(pending,nullptr,nullptr));assert(cancelGxGeometry(pending));
 for(unsigned fail=1;fail<=8;++fail){reset();auto r=raster();auto b=batch(2);samples={{150,.6f,.8f,true},{160,.7f,.4f,true}};
  callbackBatch=&b;bytesBeforeCallback=gxGeometryBytes();failPrepare=fail;
  assert(!submitCommand(r,material,b));assert(emitted.size()==fail-1);restored(r);
  assert(!cancelGxGeometry(b)); // Every callback follows immutable commit.
  const bool dirty=fail>=3;assert(alphaIntervalCopySafe()!=dirty);
  if(dirty){const auto count=emitted.size();auto next=batch(1);assert(!submitCommand(r,material,next));
   auto ordinary=r;ordinary.alphaInterval={};RigidVertexPlan rigid;
   assert(!submitStaticGxMesh(ordinary,material,1,{}));
   assert(!submitGxRigidGeometry(ordinary,material,next,rigid));
   assert(!submitGxTriangleFogGeometry(ordinary,material,ordinary,material,next));
   assert(emitted.size()==count);assert(cancelGxGeometry(next));}
 }
 for(unsigned fail=1;fail<=9;++fail){reset();auto r=raster();auto b=batch(2);samples={{150,.6f,.8f,true},{160,.7f,.4f,true}};failOwner=fail;
  assert(!submitCommand(r,material,b));assert(emitted.size()==(fail==1?0:fail-2));
  if(fail==1)assert(cancelGxGeometry(b));else {assert(!cancelGxGeometry(b));restored(r);}
 }
 for(unsigned kind=0;kind<12;++kind){reset();auto r=raster();auto b=batch(1);samples={{150,.6f,.8f,true}};
  switch(kind){case 0:owner=false;break;case 1:frame.depthFmt=GPU_RB_DEPTH24;break;case 2:frame.depthMask=2;break;
  case 3:frame.depthBuf=nullptr;break;case 4:frame.block32=true;break;case 5:frame.width=200;break;
  case 6:r.alphaIntervalPolicy.ownsTarget=nullptr;break;case 7:failAddress=true;break;case 8:b.indices[2]=9;break;
  case 9:b.vertices[0].textureQ=0;break;case 10:frame.colorMask=7;break;case 11:frame.colorFmt=GPU_RB_RGB565;break;}
  assert(!submitCommand(r,material,b)&&emitted.empty()&&alphaIntervalCopySafe());assert(cancelGxGeometry(b));
 }
 for(unsigned kind=0;kind<6;++kind){reset();auto r=raster();auto b=batch(1);
  switch(kind){case 0:r.alphaUnion=true;break;case 1:r.earlyDepthAlpha=true;break;
  case 2:r.alphaInterval.lower=0;break;case 3:r.alphaInterval.upper=255;break;
  case 4:r.alphaInterval.lower=200;break;case 5:r.alphaSecond.reference=180;break;}
  assert(!submitCommand(r,material,b)&&emitted.empty()&&alphaIntervalCopySafe());assert(cancelGxGeometry(b));
 }
 reset();auto r=raster();auto b=batch(1);material.quantizedConstants=true;
 assert(!submitCommand(r,material,b)&&emitted.empty()&&alphaIntervalCopySafe());assert(cancelGxGeometry(b));material.quantizedConstants=false;
 reset();r=raster();b=batch(1);samples={{150,.6f,.8f,true}};targetChange=3;
 assert(!submitCommand(r,material,b)&&emitted.size()==2&&!alphaIntervalCopySafe());restored(r);
 reset();r=raster();b=batch(1);samples={{150,.6f,.8f,true}};failFlush=true;
 assert(!submitCommand(r,material,b)&&emitted.empty()&&alphaIntervalCopySafe());assert(!cancelGxGeometry(b));restored(r);
 // Real teardown cannot certify cleared framebuffer pixels: poison survives.
 reset();r=raster();b=batch(1);samples={{150,.6f,.8f,true}};failPrepare=3;
 assert(!submitCommand(r,material,b)&&!alphaIntervalCopySafe());
 destroyGxPica();assert(!alphaIntervalCopySafe()&&!dusk::native::ready&&gxGeometryBytes()==0);
 assert(!submitCommand(r,material,b));
 std::puts("PASS preflight/geometry/address failures, every committed prepare/ownership failure, target loss, cache-flush failure, restoration and poison containment");
}
}
// Mock SDK: official declarations, enum values, and inline helpers are used.
extern "C" {
void* linearMemAlign(size_t n,size_t align){void* p=nullptr;return posix_memalign(&p,align,n)?nullptr:p;}
void linearFree(void* p){free(p);}
Result GSPGPU_FlushDataCache(const void*,u32){++flushes;return failFlush?-1:0;}
void OSReport(const char*,...){}
Result shaderProgramFree(shaderProgram_s*){return 0;}void DVLB_Free(DVLB_s*){}
C3D_AttrInfo* C3D_GetAttrInfo(){return &attrs;}void AttrInfo_Init(C3D_AttrInfo* p){*p={};}
int AttrInfo_AddLoader(C3D_AttrInfo*,int,GPU_FORMATS,int){return 0;}
C3D_BufInfo* C3D_GetBufInfo(){return &bufs;}void BufInfo_Init(C3D_BufInfo* p){*p={};}
int BufInfo_Add(C3D_BufInfo*,const void* p,ptrdiff_t stride,int count,u64 permutation){
 assert(stride==sizeof(GpuVertex)&&count==6&&permutation==0x543210);boundVertices=static_cast<const GpuVertex*>(p);return failAddress?-1:0;}
C3D_FrameBuf* C3D_GetFrameBuf(){return &frame;}
void C3D_BindProgram(shaderProgram_s*){}
void C3D_FogGasMode(GPU_FOGMODE,GPU_GASMODE,bool){}
void C3D_SetViewport(u32,u32,u32,u32){}void C3D_SetScissor(GPU_SCISSORMODE,u32,u32,u32,u32){}
void C3D_CullFace(GPU_CULLMODE){}void C3D_DepthMap(bool,float,float){}
void C3D_EarlyDepthTest(bool enabled,GPU_EARLYDEPTHFUNC,u32){assert(!enabled);}
void C3D_DepthTest(bool enabled,GPU_TESTFUNC f,GPU_WRITEMASK w){effect.depth=enabled;effect.df=f;effect.writes=w;}
void C3D_AlphaTest(bool enabled,GPU_TESTFUNC f,int ref){effect.alpha=enabled;effect.af=f;effect.ar=ref;}
void C3D_StencilTest(bool enabled,GPU_TESTFUNC f,int ref,int in,int out){
 assert(!(out&~1));effect.stencil=enabled;effect.sf=f;effect.sr=ref;effect.in=in;effect.out=out;}
void C3D_StencilOp(GPU_STENCILOP a,GPU_STENCILOP b,GPU_STENCILOP c){effect.ops[0]=a;effect.ops[1]=b;effect.ops[2]=c;}
void C3D_AlphaBlend(GPU_BLENDEQUATION e,GPU_BLENDEQUATION a,GPU_BLENDFACTOR s,GPU_BLENDFACTOR d,GPU_BLENDFACTOR sa,GPU_BLENDFACTOR da){effect.eq=e;effect.eqA=a;effect.src=s;effect.dst=d;effect.srcA=sa;effect.dstA=da;}
C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
void C3D_DrawElements(GPU_Primitive_t prim,int count,int type,const void* indices){
 assert(prim==GPU_TRIANGLES&&count==3&&type==C3D_UNSIGNED_SHORT);
 assert(immutable.size()&&std::memcmp(immutable.data(),boundVertices,immutable.size())==0);
 const auto* ix=static_cast<const u16*>(indices);assert(ix[1]==ix[0]+1&&ix[2]==ix[0]+2);
 const unsigned tri=ix[0]/3;assert(tri<samples.size());emitted.push_back(tri);
 const auto& s=samples[tri];if(!s.covers)return;
 if(effect.alpha&&!test(effect.af,s.alpha,effect.ar))return;
 if(effect.stencil&&!test(effect.sf,effect.sr&effect.in,pixel.stencil&effect.in)){stencilOp(effect.ops[0]);return;}
 if(effect.depth&&!test(effect.df,s.z,pixel.z)){if(effect.stencil)stencilOp(effect.ops[1]);return;}
 if(effect.stencil)stencilOp(effect.ops[2]);
 const float a=s.alpha/255.f,da=pixel.alpha;
 if(effect.writes&GPU_WRITE_RED)pixel.red=blend(s.red,pixel.red,a,da,effect.src,effect.dst,effect.eq);
 if(effect.writes&GPU_WRITE_ALPHA)pixel.alpha=blend(a,da,a,da,effect.srcA,effect.dstA,effect.eqA);
 if(effect.writes&GPU_WRITE_DEPTH)pixel.z=s.z;
}
}
namespace dusk::native {
void destroyGxTextures(){}
namespace gx {void setDrawSink(DrawSink,void*){}}
bool platformBeginGpuCommands(){return true;}
GpuCompletion platformReserveGpuEntries(unsigned n,std::uint64_t){assert(n==2);return GpuCompletion::complete;}
GpuCompletion platformCompleteGpu(std::uint64_t){assert(flushGxGeometry());return GpuCompletion::complete;}
bool platformPrepareGpuDraw(){
 ++prepareCalls;if(callbackBatch){assert(gxGeometryBytes()>bytesBeforeCallback);assert(!cancelGxGeometry(*callbackBatch));}
 if(prepareCalls==targetChange)frame.depthBuf=&otherDepth;
 if(prepareCalls==failPrepare)return false;
 return flushGxGeometry();
}
void applyPicaMaterial(const PicaMaterialPlan& material){
 assert(&material==expectedMaterial&&std::memcmp(expectedMaterialBytes.data(),&material,sizeof(material))==0);
 ++materialCalls;
}
PreparedPicaMaterial preparePicaMaterial(const PicaMaterialPlan&){return {};}
void applyPreparedPicaMaterial(const PreparedPicaMaterial&){}
}

void signedQBackendTests(){
    auto r=raster();r.alphaInterval={};r.alphaUnion=false;r.alpha={7,0};
    PicaMaterialPlan material;material.textureCount=1;material.projectiveTexture0=true;
    auto setup=[&](){reset();samples={{}};auto b=batch(1);b.qPolicy=GeometryQPolicy::directTexture0Diagnostic;
        b.vertices[0].textureQ=-2;b.vertices[1].textureQ=4;b.vertices[2].textureQ=-8;
        immutable.assign(std::as_bytes(b.vertices).begin(),std::as_bytes(b.vertices).end());return b;};
    auto b=setup();assert(submitCommand(r,material,b)&&emitted==std::vector<unsigned>{0});
    // Material/policy mismatches and invalid values must fail before even the
    // platform prepare callback, hence before any native command emission.
    for(unsigned mode=0;mode<8;++mode){b=setup();auto m=material;auto p=r;
        if(mode==0)m.projectiveTexture0=false;
        if(mode==1)m.textureCount=0;
        if(mode==2)p.alphaUnion=true;
        if(mode==3)p.alphaInterval={true,101,184};
        if(mode==4)b.vertices[2].textureQ=0;
        if(mode==5)b.vertices[2].textureQ=std::numeric_limits<float>::quiet_NaN();
        if(mode==6)b.vertices[2].texcoord[0][0]=std::numeric_limits<float>::infinity();
        if(mode==7)b.qPolicy=static_cast<GeometryQPolicy>(77);
        assert(!submitCommand(p,m,b)&&emitted.empty()&&prepareCalls==0&&cancelGxGeometry(b));
    }
    b=setup();assert(!submitGxTriangleFogGeometry(r,material,r,material,b)&&emitted.empty()&&prepareCalls==0&&cancelGxGeometry(b));
    b=setup();RigidVertexPlan rigid;assert(!submitGxRigidGeometry(r,material,b,rigid)&&emitted.empty()&&prepareCalls==0&&cancelGxGeometry(b));
    std::puts("PASS signed-q real material wrapper: direct token preserved, full bytes unchanged; unavailable/invalid/multipass/rigid fail before prepare/emission");
}

int main(){signedQBackendTests();truthTests();plannerTests();sequenceTests();failures();destroyGxGeometry();std::puts("PASS alpha interval real backend mock emission (native untested)");}
