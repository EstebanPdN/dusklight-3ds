#pragma once
#include "raster_carrier_fixture.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include "../runtime/gx_profile.hpp"
#include "../native/color_lookup.hpp"
#include "../runtime/gx_material_liveness.hpp"
#include "material_equivalence.hpp"
// Included after the existing adapter test's bounded backend definitions.
static void carrierAdapterCases(const CommandDraw& original){
 using namespace carrier_test;
 const auto oldArena=arena;const auto oldSubmissions=submissions,oldBinds=binds;
 const auto oldRigid=rigidSubmissions,oldCompact=compactSubmissions;
 alignas(16) static std::array<std::byte,1024*1024> carrierStorage;
 arena={};assert(arena.attach(carrierStorage)&&arena.begin());carrierMode=true;
 auto reset=[&]{assert(arena.end()&&arena.begin());};
 auto regs=original.registers;
 auto xf=[&](unsigned at,unsigned val){regs.xf[at]=val;regs.xfValid.set(at);};
 auto scalar=[&](unsigned at,float f){xf(at,std::bit_cast<u32>(f));};
 xf(0x1026,0);const float projection[]{1,0,1,0,0,-.5f};
 for(unsigned i=0;i<6;++i)scalar(0x1020+i,projection[i]);
 xf(0x1009,2);xf(0x100e,1|1024);xf(0x100f,1|1024);
 // Neither alpha control is initialized: both are dead to the carrier.
 regs.xfValid.reset(0x1010);regs.xfValid.reset(0x1011);
 regs.bp[0xf1]=0;regs.bp[0xe8]=0;regs.bpValid.set(0xe8);regs.bp[0x43]&=~64u;
 auto m=capturedMaterial();installMaterial(m,regs);
 MaterialState decodedMaterial;assert(decodeMaterial(regs,decodedMaterial));
 assert(decodedMaterial.stageCount==3&&decodedMaterial.colors==m.colors&&decodedMaterial.swaps==m.swaps);
 for(unsigned i=0;i<3;++i){assert(decodedMaterial.stages[i].color==m.stages[i].color&&decodedMaterial.stages[i].alpha==m.stages[i].alpha&&decodedMaterial.stages[i].raster==m.stages[i].raster);}
 VertexDecodePlan plan;VertexArrays arrays{};std::vector<u8> bytes;
 auto make=[&](unsigned n,bool clr1=true,bool normal=true){
  aurora::gx::dl::VtxLayout layout{};std::vector<GXVtxAttrFmtList> formats;unsigned cursor=0;
  auto attr=[&](GXAttr a,unsigned size,GXCompCnt cnt,GXCompType type){layout.attrs[a]={u8(cursor),u8(size),GX_DIRECT};cursor+=size;formats.push_back({a,cnt,type,0});};
  attr(GX_VA_POS,12,GX_POS_XYZ,GX_F32);if(normal)attr(GX_VA_NRM,12,GX_NRM_XYZ,GX_F32);
  attr(GX_VA_CLR0,4,GX_CLR_RGBA,GX_RGBA8);if(clr1)attr(GX_VA_CLR1,4,GX_CLR_RGBA,GX_RGBA8);attr(GX_VA_TEX0,8,GX_TEX_ST,GX_F32);
  layout.stride=cursor;assert(planVertexDecode(layout,formats,plan));bytes.clear();
  auto value=[&](float f){auto bits=std::bit_cast<u32>(f);for(int s=24;s>=0;s-=8)bytes.push_back(u8(bits>>s));};
  for(unsigned i=0;i<n;++i){value(i%3==1?.6f:-.6f);value(i%3==2?.6f:-.6f);value(-1);
   if(normal){value(0);value(0);value(1);}
   u8 gray=u8(i);bytes.insert(bytes.end(),{gray,gray,gray,17});
   if(clr1)bytes.insert(bytes.end(),{u8(i*3),u8(i*7+41),u8(255-i),211});value(.2f);value(.7f);
  }
 };
 auto count=[&]{return unsigned(bytes.size()/plan.packedStride());};
 auto draw=[&](CommandDrawContext& c,unsigned primitive=GX_TRIANGLES,std::span<const u8> indices={}){
  CommandDraw d{static_cast<GXPrimitive>(primitive),u16(count()),u32(indices.size()/2),bytes,indices,plan,arrays,regs};return drawGxCommand(d,&c);
 };
 auto fresh=[](){CommandDrawContext c;c.diagnosticRasterCarrier=true;c.allowRigidGpu=true;return c;};
 unsigned rejectCase=0;
 auto reject=[&](CommandDrawContext& c,const char* text=nullptr,unsigned primitive=GX_TRIANGLES){
  const auto submits=submissions;const auto used=arena.used();++rejectCase;const bool result=draw(c,primitive);if(result)std::fprintf(stderr,"unexpected success rejectCase=%u expected=%s vertices=%u primitive=%u\n",rejectCase,text?text:"unspecified",count(),primitive);assert(!result);
  if(text){if(std::strcmp(c.failure,text))std::fprintf(stderr,"expected '%s', got '%s'\n",text,c.failure);assert(!std::strcmp(c.failure,text));}
  assert(c.rejected==1&&!c.submitted&&!c.rasterCarrierDraws&&!c.skippedDraws&&!c.simplifiedDraws&&submissions==submits&&arena.used()==used);
 };
 make(258);auto context=fresh();CompactVertexCache cache;std::array<u8,4096> cacheBytes{};assert(cache.attach(cacheBytes));context.compactCache=&cache;
 assert(draw(context)&&context.rasterCarrierDraws==1&&context.rasterCarrierAttempts==1&&!context.rejected);
 assert(!cache.hits&&!cache.misses&&!cache.stores&&rigidSubmissions==oldRigid&&compactSubmissions==oldCompact);
 assert(carrierVertices.size()==258&&carrierMaterial.stageCount==3&&carrierMaterial.textureCount==1);
 for(unsigned i=0;i<258;++i){const auto& c=carrierVertices[i].color;
  assert(c[0]==float(u8(i*3))/255&&c[1]==float(u8(i*7+41))/255&&c[2]==float(u8(255-i))/255&&c[3]==float(u8(i))/255);}
 RasterPlan wanted;assert(planRaster(regs,wanted,nullptr,FogPolicy::requireSupported,ColorStoragePolicy::rgb8Only,ClipPolicy::validateGeometry));
 assert(carrierRaster.depth.enabled==wanted.depth.enabled&&carrierRaster.depth.compare==wanted.depth.compare&&carrierRaster.depth.write==wanted.depth.write&&carrierRaster.alpha.comparison==wanted.alpha.comparison&&carrierRaster.writeAlpha==wanted.writeAlpha&&carrierRaster.blend.src==wanted.blend.src);
 reset();
 // Last of 3447 vertices is non-gray. No backend submit or committed arena
 // bytes are permitted; the first failure index is retained without log spam.
 make(3447);bytes[(count()-1)*plan.packedStride()+24+1]^=1;context=fresh();reject(context,"GX diagnostic raster0 not grayscale");
 assert(context.rasterCarrierRejects==1&&context.rasterCarrierFirstVertex==3446&&context.rasterCarrierFirstFailure);
 const auto first=context.rasterCarrierFirstFailure;assert(!draw(context)&&context.rasterCarrierRejects==2&&context.rasterCarrierFirstVertex==3446&&context.rasterCarrierFirstFailure==first);
 make(6);bytes[5*plan.packedStride()+25]^=1;context=fresh();
 const std::array<u16,6> reuse{0,1,2,3,4,5};const auto submits=submissions;
 assert(!draw(context,GX_TRIANGLES,{reinterpret_cast<const u8*>(reuse.data()),sizeof(reuse)})&&context.rasterCarrierFirstVertex==5&&submissions==submits);
 make(3,false);context=fresh();reject(context,"GX raster vertex color missing");
 make(3);auto validRegs=regs;
 regs.xfValid.reset(0x100f);context=fresh();reject(context,"GX raster color state missing");regs=validRegs;
 // Four selected lights force a normal matrix dependency on the actual adapter.
 xf(0x100f,0xf7a);xf(0x100d,0xffffffff);
 for(unsigned i=1;i<=4;++i){const unsigned at=0x600+16*i;xf(at+3,0x101010ff);
  for(unsigned c=0;c<3;++c){scalar(at+10+c,c==2?1:0);scalar(at+13+c,c==2?1:0);scalar(at+4+c,c==0?1:0);scalar(at+7+c,c==0?1:0);}}
 for(unsigned row=0;row<3;++row)for(unsigned c=0;c<3;++c)scalar(0x400+row*3+c,row==c?1:0);
 const auto beforeLit=gxProfileTotals.litVertices;context=fresh();assert(draw(context)&&context.rasterCarrierDraws==1&&gxProfileTotals.litVertices==beforeLit+count());reset();
 auto litRegs=regs;
 make(3,false);context=fresh();reject(context,"GX raster vertex color missing");make(3);
 regs.xfValid.reset(0x400);context=fresh();reject(context,"XF transform state missing");regs=litRegs;
 regs.xfValid.reset(0x643);context=fresh();reject(context,"GX raster color state missing");regs=litRegs;
 scalar(0x61f,std::numeric_limits<float>::quiet_NaN());context=fresh();reject(context,"GX raster vertex color invalid");regs=litRegs;
 scalar(0x617,0);context=fresh();reject(context,"GX lighting configuration or attenuation invalid");regs=litRegs;
 make(3,true,false);context=fresh();reject(context,"GX transform source attribute missing");
 make(3);regs=validRegs;
 // Ambient-only lighting has no normal dependency but still executes the
 // second raster lighting evaluator and must contribute to litVertices.
 xf(0x100f,2|1024);xf(0x100d,0xffffffff);xf(0x100b,0x808080ff);
 const auto ambientLitBefore=gxProfileTotals.litVertices;context=fresh();assert(draw(context)&&gxProfileTotals.litVertices==ambientLitBefore+count());reset();regs=validRegs;
 // Equality is proved on evaluated raster0, never on raw CLR0 alone.
 xf(0x100e,0);xf(0x100c,0x808080ff);bytes[24]=3;context=fresh();assert(draw(context));
 for(const auto& v:carrierVertices)assert(v.color[3]==128.f/255);reset();
 make(3);xf(0x100e,2|1024);xf(0x100a,0x102030ff);xf(0x100c,0xffffffff);context=fresh();reject(context,"GX diagnostic raster0 not grayscale");assert(context.rasterCarrierFirstVertex==0);
 regs=validRegs;bytes[20]=0x7f;bytes[21]=0xc0;context=fresh();reject(context);make(3);
 for(unsigned test=0;test<9;++test){regs=validRegs;context=fresh();context.allowVertexFog=context.allowTriangleFog=context.allowDegradedDraws=context.allowConstantEarlyDepth=true;
  context.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
  if(test==0)regs.xf[0x1026]=1;
  if(test==1){regs.xf[0x1005]=1;regs.xfValid.set(0x1005);}
  if(test==2)regs.bp[0xf1]=1u<<21; // Existing evaluator rejects unsupported function.
  if(test==3){regs.bp[0xf1]=2u<<21;regs.bp[0xe8]=1024;regs.bpValid.reset(0xed);}
  if(test==4)regs.bp[0x43]|=64;
  if(test==5)regs.xf[0x1040]|=2;
  if(test==6)regs.bp[0x10]=1;
  if(test==7){auto swap=m;swap.swaps[0]={3,1,2,3};installMaterial(swap,regs);}
  if(test==8){auto alpha=m;alpha.stages[0].alpha[3]=5;installMaterial(alpha,regs);}
  reject(context);
 }
 regs=validRegs;context=fresh();reject(context,nullptr,GX_TRIANGLESTRIP);
 // Existing alpha union/interval ownership remains independent. Neither
 // interaction may enter a carrier even when all relevant policies are ON.
 regs=validRegs;regs.bp[0xf3]=0x73d241;context=fresh();context.allowAlphaUnion=true;reject(context,"GX diagnostic raster carrier interaction unsupported");
 regs=validRegs;regs.bp[0xf3]=0xde64b9;context=fresh();context.alphaIntervalPolicy={true,[](const AlphaIntervalTarget&,void*){return true;},nullptr};reject(context,"GX diagnostic raster carrier interaction unsupported");
 regs=validRegs;
 // Fog uses the existing range/depth evaluator and a real helper ramp. No
 // primary component can be borrowed from either independently varying raster.
 auto fogRegs=validRegs;
 auto bp=[&](unsigned at,unsigned value){regs.bp[at]=value;regs.bpValid.set(at);};
 regs=validRegs;
 for(auto [at,value]:std::array<std::array<unsigned,2>,6>{{{0xf1,0x400000},{0xe8,0x696},{0xee,0x38aec},{0xef,0x40005c},{0xf0,2},{0xf2,0xabc69a}}})bp(at,value);
 for(unsigned at=0xe9;at<=0xed;++at)bp(at,64|(64u<<12)); // Synthetic complete range table, not native45 evidence.
 fogRegs=regs;
 auto fogContext=[&]{auto c=fresh();c.allowVertexFog=true;c.allowTriangleFog=true;return c;};
 // Exhaust the actual texture registry before its first helper registration.
 // A failed mathematical helper is a resource failure, never a white fallback.
 std::vector<std::array<u8,64>> fillers(textureRegistrationCapacity());std::vector<TextureHandle> handles;
 for(auto& data:fillers){data[3]=data[5]=8;data[24]=1;data[31]=32;TextureHandle h{};
  if(!registerTexture(data,h,true))break;handles.push_back(h);}
 assert(!handles.empty());make(3);context=fogContext();reject(context,"GX vertex fog ramp unavailable");assert(context.failureKind==DrawFailureKind::resource);
 for(auto h:handles)assert(unregisterTexture(h));
 make(3447);context=fogContext();assert(draw(context)&&context.rasterCarrierDraws==1&&context.vertexFogDraws==1&&!context.triangleFogDraws);
 assert(carrierMaterial.textureCount==2&&carrierMaterial.stageCount==4&&carrierIndices.size()==3447);
 for(unsigned i=0;i<count();++i){const auto& v=carrierVertices[i];assert(carrierIndices[i]==i);
  assert(v.color[3]==float(u8(i))/255&&v.color[0]==float(u8(i*3))/255&&v.color[1]==float(u8(i*7+41))/255&&v.color[2]==float(u8(255-i))/255);
  VertexFogPlan fp;assert(planVertexFog(regs,fp)&&fp.rangeEnabled&&fp.enabled);
  const float f=evaluateVertexFog(fp,v.position,currentTargetExtent.logicalWidth);
  assert(v.texcoord[1][0]==(f*255+.5f)/256&&v.texcoord[1][1]==.5f);
 }
 const auto& ramp=carrierTextures[1];assert(ramp.width==256&&ramp.height==8&&ramp.format==1&&ramp.minFilter==1&&ramp.magFilter==1&&!ramp.wrapS&&!ramp.wrapT&&ramp.immutablePixels);
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<256;++x)assert(ramp.image[((y/4)*32+x/8)*32+(y%4)*8+x%8]==x);
 const auto& fogStage=carrierMaterial.stages[3];assert(fogStage.rgb.function==MaterialFunction::interpolate&&fogStage.rgb.inputs[2].source==MaterialSource::texture1&&fogStage.rgb.inputs[2].operand==MaterialOperand::alpha);
 assert(fogStage.alpha.function==MaterialFunction::replace&&fogStage.alpha.inputs[0].source==MaterialSource::previous&&fogStage.alpha.inputs[0].operand==MaterialOperand::alpha);
 reset();bytes[(count()-1)*plan.packedStride()+25]^=1;context=fogContext();reject(context,"GX diagnostic raster0 not grayscale");assert(context.rasterCarrierFirstVertex==3446);
 make(3447);bytes[3446*plan.packedStride()+25]^=1;context=fogContext();const std::array<u16,3> firstOnly{0,1,2};const auto beforeIndexed=submissions;
 assert(!draw(context,GX_TRIANGLES,{reinterpret_cast<const u8*>(firstOnly.data()),sizeof(firstOnly)})&&context.rasterCarrierFirstVertex==3446&&submissions==beforeIndexed); // Even unreferenced last vertex.
 make(3);regs=fogRegs;
 for(unsigned reg:{0xe8u,0xeeu,0xefu,0xf0u,0xf2u,0xe9u,0xeau,0xebu,0xecu,0xedu}){regs=fogRegs;regs.bpValid.reset(reg);context=fogContext();reject(context,"GX vertex fog parameters invalid");}
 regs=fogRegs;regs.xfValid.reset(0x101a);context=fogContext();reject(context,"XF transform state missing");
 for(float width:{0.f,-1.f,std::numeric_limits<float>::infinity()}){regs=fogRegs;scalar(0x101a,width);context=fogContext();reject(context);}
 regs=fogRegs;context=fresh();reject(context,"GX fog effect pending");
 // Original material alpha continues to own alpha testing/writes/blending.
 regs=fogRegs;regs.bp[0x43]=(regs.bp[0x43]&~7u)|1;regs.bp[0x41]|=16;regs.bp[0xf3]=128|(1u<<16)|(7u<<19);
 context=fogContext();context.allowRgba6Storage=true;assert(draw(context));assert(carrierRaster.writeAlpha&&carrierRaster.storesAlpha&&carrierRaster.alpha.comparison==3&&carrierRaster.alpha.reference==127);reset();
 // Capacity is a physical invariant: two original samplers + fog fit; three
 // originals or six material stages reject before reserve/submit, even if
 // triangle-fog and visual fallbacks are also requested.
 auto two=m;two.stageCount=4;two.stages[3]=m.stages[2];two.stages[3].texture=1;two.stages[3].color={15,8,0,15};two.stages[3].colorOp.scale=0;two.stages[3].alpha={7,7,7,0};
 regs=fogRegs;installMaterial(two,regs);context=fogContext();assert(draw(context)&&carrierMaterial.textureCount==3&&carrierMaterial.stageCount==5);reset();
 auto three=two;three.stageCount=5;three.stages[4]=two.stages[3];three.stages[4].texture=2;
 regs=fogRegs;installMaterial(three,regs);context=fogContext();context.allowDegradedDraws=true;reject(context,"GX vertex fog TEV/texture capacity");
 auto six=m;six.stageCount=6;for(unsigned i=3;i<6;++i){six.stages[i]=two.stages[3];six.stages[i].texture=0;}
 regs=fogRegs;installMaterial(six,regs);context=fogContext();reject(context,"GX vertex fog TEV/texture capacity");
 regs=fogRegs;carrierFailBind=1;context=fogContext();reject(context,"test texture failure");assert(context.failureKind==DrawFailureKind::resource);carrierFailBind=UINT32_MAX;
 auto imageToken=regs.bp[0x94];regs.bp[0x94]=0;context=fogContext();reject(context);regs.bp[0x94]=imageToken;
 scopeFailure=true;context=fogContext();reject(context,"test texture failure");scopeFailure=false;
 reservationFailure=true;context=fogContext();reject(context,"test arena failure");reservationFailure=false;
 cancellationFailure=true;make(3);bytes[25]^=1;context=fogContext();reject(context,"GX geometry cancellation failed");cancellationFailure=false;make(3);
 gpuFailure=true;context=fogContext();reject(context,"test GPU failure");gpuFailure=false;
 gpuCommittedFailure=true;context=fogContext();const auto committedBefore=submissions;
 assert(!draw(context)&&context.failureKind==DrawFailureKind::submission&&!std::strcmp(context.failure,"test GPU failure")&&context.rasterCarrierRejects==1&&!context.rasterCarrierDraws&&!context.submitted&&!context.skippedDraws&&!context.simplifiedDraws&&submissions==committedBefore+1);
 gpuCommittedFailure=false;reset();
 carrierInvalidScale=true;context=fogContext();reject(context,"GX diagnostic raster carrier geometry invalid");carrierInvalidScale=false;
 // The current normal-overflow invisible proof stays on ordinary draws. A
 // carrier cannot convert its failed full-draw shading validation into a cull.
 regs=litRegs;scalar(0x400,std::numeric_limits<float>::max());scalar(0x401,0);scalar(0x402,0);
 for(unsigned i=0;i<count();++i){const u32 f=std::bit_cast<u32>(1.f);for(unsigned c=0;c<4;++c)bytes[i*plan.packedStride()+12+c]=u8(f>>(24-8*c));}
 context=fresh();reject(context);assert(!context.normalOverflowCulled&&!context.culled);
 auto ordinaryMaterial=m;ordinaryMaterial.channelCount=1;ordinaryMaterial.stages[0].raster=0;
 installMaterial(ordinaryMaterial,regs);xf(0x1009,1);xf(0x100e,0xf7a);xf(0x100c,0xffffffff);
 for(unsigned i=0;i<count();++i){const u32 f=std::bit_cast<u32>(10.f);for(unsigned c=0;c<4;++c)bytes[i*plan.packedStride()+c]=u8(f>>(24-8*c));}
 context=fresh();const auto invisibleBefore=submissions;assert(draw(context)&&context.normalOverflowCulled==1&&context.culled==1&&!context.submitted&&!context.rasterCarrierAttempts&&submissions==invisibleBefore);
 // Render target and inset viewport affect clip coordinates before range fog.
 // Both logical native width and downscaled target storage are respected.
 make(3);const auto target=currentTargetExtent;
 for(auto extent:std::array<TargetExtent,3>{{{400,240,400,240},{640,480,320,240},{256,256,128,128}}}){
  currentTargetExtent=extent;regs=fogRegs;const float vw=extent.logicalWidth*.5f,vx=extent.logicalWidth*.125f;
  scalar(0x101a,vw*.5f);scalar(0x101b,-float(extent.logicalHeight)*.25f);scalar(0x101d,342+vx+vw*.5f);scalar(0x101e,342+float(extent.logicalHeight)*.5f);
  context=fogContext();assert(draw(context));assert(carrierVertices.size()==3);for(const auto& v:carrierVertices){VertexFogPlan fp;assert(planVertexFog(regs,fp));const auto f=evaluateVertexFog(fp,v.position,extent.logicalWidth);assert(v.texcoord[1][0]==(f*255+.5f)/256);}
  reset();
 }
 currentTargetExtent=target;regs=validRegs;make(3);
 std::puts("PASS carrier fog adapter: complete 3447/indexed-unreferenced last negatives; pinned fog words + synthetic range table; real ramp bytes/filter; range/viewport/target; original alpha/depth; 2+1 sampler and stage capacity; missing state, exhausted registry, helper bind/scope/reserve/cancel/submit failures");
 // Full-color translation must submit the formerly rejected final non-gray
 // vertex without selecting a skipped/simplified visual stand-in.
 make(3447);bytes[(count()-1)*plan.packedStride()+25]^=1;context={};
 assert(draw(context)&&context.rasterCarrierDraws==1&&!context.rejected&&!context.skippedDraws&&!context.simplifiedDraws);
 reset();make(3);
 // Clip-side/camera/depth crossings retain the CPU geometry and interpolants.
 // This is host ownership/state verification, not hardware coverage proof.
 for(float z:{-1.f,-.1f,1.f}){make(3);const auto bits=std::bit_cast<u32>(z);for(unsigned i=0;i<3;++i)for(unsigned c=0;c<4;++c)bytes[i*plan.packedStride()+8+c]=u8(bits>>(24-8*c));
  context=fresh();assert(draw(context));for(const auto& v:carrierVertices)for(float c:v.position)assert(std::isfinite(c));reset();}
 make(3);regs=validRegs;regs.bp[0xf3]=128|(1u<<16)|(7u<<19);context=fresh();assert(draw(context));assert(carrierRaster.alpha.comparison==3&&carrierRaster.alpha.reference==127);reset();
 regs=validRegs;make(3);
 textureFailure=true;context=fresh();reject(context,"test texture failure");textureFailure=false;
 scopeFailure=true;context=fresh();reject(context,"test texture failure");scopeFailure=false;
 reservationFailure=true;context=fresh();reject(context,"test arena failure");reservationFailure=false;
 gpuFailure=true;context=fresh();reject(context,"test GPU failure");gpuFailure=false;
 // Automatic full-RGBA2 + fog: no original samplers, two lookup resources,
 // then the fog ramp in unit 2. Keep the real packed stream layout.
 {
  auto rgba=m;rgba.stageCount=2;rgba.texgenCount=0;
  rgba.stages[0].alpha={7,7,7,5};rgba.stages[1].alpha={7,0,5,7};
  regs=fogRegs;installMaterial(rgba,regs);xf(0x1010,1|1024);xf(0x1011,1|1024);
  MaterialState decoded;assert(decodeMaterial(regs,decoded));
  for(unsigned i=0;i<decoded.stageCount;++i)assert(!decoded.stages[i].textureEnabled);
  PicaMaterialPlan lookup;assert(planRasterLookupMaterial(decoded,lookup));
  assert(!lookup.sourceTextures()&&lookup.textureCount==2&&lookup.rasterReadMask[0]==3&&lookup.rasterReadMask[1]==3);
  RasterColorPlan primaryPlan,secondaryPlan;VertexTransformPlan transform;VertexFogPlan fog;
  assert(planRasterColorChannel(regs,0,3,primaryPlan)&&planRasterColorChannel(regs,1,3,secondaryPlan));
  assert(regs.xf[0x1026]==0&&planVertexTransform(regs,0,transform));
  assert(planVertexFog(regs,fog)&&fog.enabled&&fog.rangeEnabled&&!fog.orthographic);
  const std::array<std::array<u8,4>,3> primary{{{19,83,147,211},{231,37,109,61},{73,199,43,157}}};
  const std::array<std::array<u8,4>,3> secondary{{{173,29,97,53},{47,181,239,127},{223,113,11,193}}};
  auto automaticVertices=[&](bool clr1=true){
   make(3,clr1);
   for(unsigned i=0;i<3;++i){
    const auto at=i*plan.packedStride();const auto z=std::bit_cast<u32>(-float(1u<<i));
    for(unsigned c=0;c<4;++c){bytes[at+8+c]=u8(z>>(24-8*c));bytes[at+24+c]=primary[i][c];if(clr1)bytes[at+28+c]=secondary[i][c];}
   }
  };
  auto automaticContext=[](){CommandDrawContext c;c.diagnosticRasterCarrier=false;c.allowVertexFog=true;return c;};
  automaticVertices();context=automaticContext();carrierTextures={};
  const auto automaticBefore=submissions;
  assert(draw(context)&&context.submitted==1&&submissions==automaticBefore+1&&context.rasterCarrierDraws==1&&context.rasterCarrierAttempts==1&&context.vertexFogDraws==1);
  assert(!context.rejected&&!context.triangleFogDraws&&!context.skippedDraws&&!context.simplifiedDraws);
  assert(carrierMaterial.rasterCarrier==RasterCarrier::raster0RgbaRaster1Lookup&&carrierMaterial.textureCount==3);
  const std::array lookupBytes{dusk::pica::colorLookupBti(0),dusk::pica::colorLookupBti(1)};
  for(unsigned unit=0;unit<3;++unit){
   const auto& texture=carrierTextures[unit];
   assert(texture.handle&&texture.immutablePixels&&texture.height==8&&texture.minFilter==1&&texture.magFilter==1&&!texture.wrapS&&!texture.wrapT);
   for(unsigned other=0;other<unit;++other)assert(texture.handle!=carrierTextures[other].handle&&texture.image.data()!=carrierTextures[other].image.data());
   if(unit<2)assert(texture.width==8&&texture.format==6&&texture.bti.size()==lookupBytes[unit].size()&&!std::memcmp(texture.bti.data(),lookupBytes[unit].data(),lookupBytes[unit].size()));
  }
  const auto& fogTexture=carrierTextures[2];assert(fogTexture.width==256&&fogTexture.format==1);
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<256;++x)assert(fogTexture.image[((y/4)*32+x/8)*32+(y%4)*8+x%8]==x);
  const auto& fogStage=carrierMaterial.stages[carrierMaterial.stageCount-1];
  assert(fogStage.rgb.function==MaterialFunction::interpolate&&fogStage.rgb.inputs[2].source==MaterialSource::texture2&&fogStage.rgb.inputs[2].operand==MaterialOperand::alpha&&fogStage.constant==fog.color);
  assert(fogStage.alpha.function==MaterialFunction::replace&&fogStage.alpha.inputs[0].source==MaterialSource::previous&&fogStage.alpha.inputs[0].operand==MaterialOperand::alpha);
  assert(carrierVertices.size()==3&&carrierIndices.size()==3);
  for(unsigned i=0;i<3;++i){
   DecodedVertex decodedVertex;TransformedVertex transformed;std::array<float,4> p,s;
   assert(decodeVertex(plan,std::span<const u8>(bytes).subspan(i*plan.packedStride(),plan.packedStride()),arrays,decodedVertex));
   assert(transformVertex(transform,decodedVertex,transformed)&&transformed.clip[3]==float(1u<<i));
   assert(evaluateRasterColor(primaryPlan,decodedVertex,p,nullptr,&transformed)&&evaluateRasterColor(secondaryPlan,decodedVertex,s,nullptr,&transformed));
   const auto& vertex=carrierVertices[i];assert(carrierIndices[i]==i&&vertex.position==transformed.clip&&vertex.color==p);
   for(unsigned c=0;c<4;++c)assert(p[c]==float(primary[i][c])/255&&s[c]==float(secondary[i][c])/255);
   for(unsigned unit=0;unit<2;++unit){
    assert(vertex.texcoord[unit][0]==(.5f+s[2*unit])/8.f);
    assert(vertex.texcoord[unit][1]==1.f-(.5f+s[2*unit+1])/8.f);
   }
   const float factor=evaluateVertexFog(fog,transformed.clip,currentTargetExtent.logicalWidth);
   assert(std::isfinite(factor)&&factor>0&&factor<1);
   assert(vertex.texcoord[2][0]==(factor*255+.5f)/256&&vertex.texcoord[2][1]==.5f);
  }
  reset();
  for(unsigned unit=0;unit<3;++unit){carrierFailBind=unit;context=automaticContext();reject(context,"test texture failure");assert(context.failureKind==DrawFailureKind::resource);}
  carrierFailBind=UINT32_MAX;
  scopeFailure=true;context=automaticContext();reject(context,"test texture failure");assert(context.failureKind==DrawFailureKind::resource);scopeFailure=false;
  automaticVertices(false);context=automaticContext();reject(context,"GX raster vertex color missing");
  automaticVertices();context=automaticContext();assert(draw(context)&&context.rasterCarrierDraws==1&&context.vertexFogDraws==1&&!context.rejected&&!context.skippedDraws&&!context.simplifiedDraws);reset();
  std::puts("PASS automatic full RGBA2 fog adapter: independent primary/secondary RGBA, lookup units 0/1 + fog unit/UV2, real helper bytes, perspective transform, bind/scope/missing CLR1 rejection");
 }
 // Only TEV alpha reads Ras1. The same two-texture RGB draw must use ordinary
 // planning before considering lookup, without losing either original UV.
 {
  const auto savedRegs=regs;PicaMaterialPlan ordinaryRgb;RasterPlan ordinaryRgbRaster;
  std::vector<GpuVertex> ordinaryRgbVertices;std::vector<u16> ordinaryRgbIndices;
  std::array<Texture,3> ordinaryRgbTextures;CommandRegisters alphaRegs;
  MaterialState alphaMaterial;
  for(bool rasterAlpha:{false,true}){
   alignas(32) u8 commands[512];GDLObj gd;const auto previousGd=__GDCurrentDL;
   GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
   GDSetGenMode2(2,2,2,0,GX_CULL_NONE);
   GDSetTevSwapModeTable(GX_TEV_SWAP0,GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
   GDSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR1A1,GX_TEXCOORD1,GX_TEXMAP1,GX_COLOR_NULL);
   GDSetTevColorCalc(GX_TEVSTAGE0,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_TEXC,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
   GDSetTevColorCalc(GX_TEVSTAGE1,GX_CC_ZERO,GX_CC_TEXC,GX_CC_CPREV,GX_CC_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
   for(unsigned i=0;i<2;++i){const auto stage=GXTevStageID(i);
    GDSetTevAlphaCalcAndSwap(stage,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,i?GX_CA_APREV:(rasterAlpha?GX_CA_RASA:GX_CA_ZERO),GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV,GX_TEV_SWAP0,GX_TEV_SWAP0);
    GDSetTevDirect(stage);
   }
   const auto size=GDGetCurrOffset();GDSetCurrent(previousGd);
   CommandMemory memory;CommandProcessor capture(memory);assert(capture.execute({commands,size}));
   regs=original.registers;
   for(unsigned at=0;at<256;++at)if(capture.registers().bpValid[at]){regs.bp[at]=capture.registers().bp[at];regs.bpValid.set(at);}
   regs.bp[0x41]=(regs.bp[0x41]&~(1u|2u|16u|2048u))|8u; // No blend or alpha writes.
   regs.bp[0x43]&=~71u;regs.bp[0xf1]=0;regs.bp[0xf3]=(7u<<16)|(7u<<19);
   MaterialState decoded;RasterPlan raster;
   assert(decodeMaterial(regs,decoded)&&planRaster(regs,raster));
   assert(decoded.stageCount==2&&decoded.texgenCount==2&&decoded.channelCount==2);
   assert(!raster.blend.mode&&!raster.writeAlpha&&raster.alpha.comparison==7);
   if(rasterAlpha){
    alphaRegs=regs;alphaMaterial=decoded;PicaMaterialPlan refused;MaterialPlanError error;
    assert(!planPicaMaterial(decoded,refused,&error)&&error==MaterialPlanError::source);
    // Explicit negative control: committing lookup first rejects this formerly
    // supported RGB draw before the adapter can discard its unobserved alpha.
    assert(!planRasterLookupMaterial(decoded,refused,&error)&&error==MaterialPlanError::textureUnits);
   }
   CommandDraw candidate{original.primitive,original.vertexCount,original.indexCount,original.vertices,original.indices,original.plan,original.arrays,regs};
   context={};carrierTextures={};const auto sent=submissions,bound=binds;
   assert(drawGxCommand(candidate,&context)&&submissions==sent+1&&binds==bound+2&&context.submitted==1);
   assert(!context.rejected&&!context.culled&&!context.skippedDraws&&!context.simplifiedDraws&&!context.rasterCarrierAttempts&&!context.rasterCarrierDraws&&!context.rasterCarrierRejects&&!context.rasterCarrierFirstFailure);
   assert(carrierMaterial.rasterCarrier==RasterCarrier::none&&carrierMaterial.sourceTextures()==2&&carrierMaterial.textureCount==2&&!carrierTextures[2].handle);
   assert(carrierRaster.writeRgb&&!carrierRaster.writeAlpha&&!carrierRaster.blend.mode&&carrierRaster.alpha.comparison==7);
   for(unsigned unit=0;unit<2;++unit){
    assert((carrierMaterial.textures[unit]==MaterialTextureBinding{u8(unit),u8(unit)}));Texture expected;const char* why;
    assert(resolveCommandTexture(regs,unit,expected,why)&&carrierTextures[unit].handle==expected.handle&&carrierTextures[unit].image.data()==expected.image.data());
   }
   assert(carrierVertices.size()==4&&(carrierIndices==std::vector<u16>{0,1,2,2,3,0}));
   for(const auto& v:carrierVertices)for(unsigned unit=0;unit<2;++unit){
    assert(std::abs(v.texcoord[unit][0]-(.1f+.2f*unit))<1e-6f);
    assert(std::abs(v.texcoord[unit][1]-(1-(.2f+.2f*unit)*.5f))<1e-6f);
   }
   if(!rasterAlpha){ordinaryRgb=carrierMaterial;ordinaryRgbRaster=carrierRaster;ordinaryRgbVertices=carrierVertices;ordinaryRgbIndices=carrierIndices;ordinaryRgbTextures=carrierTextures;}
   else{
    assert(carrierMaterial.textures==ordinaryRgb.textures&&carrierIndices==ordinaryRgbIndices&&carrierVertices.size()==ordinaryRgbVertices.size());
    assert(!std::memcmp(carrierVertices.data(),ordinaryRgbVertices.data(),carrierVertices.size()*sizeof(GpuVertex)));
    for(unsigned unit=0;unit<2;++unit)assert(carrierTextures[unit].handle==ordinaryRgbTextures[unit].handle);
    // Independent normalized-real TEV/PICA evaluation plus the actual raster
    // write mask: RGB=T0*T1 and framebuffer alpha remains the destination's.
    using PixelRGBA=material_test::Color;
    for(unsigned sample=0;sample<256;++sample){
     material_test::Samples inputs{};PixelRGBA expected{},actual{},control{};
     inputs.secondary[3]=double(sample)/255;
     for(unsigned c=0;c<4;++c){
      inputs.texture[0][0][c]=double((sample+37*c)&255)/255;
      inputs.texture[1][1][c]=double((251-3*sample+19*c)&255)/255;
      expected[c]=actual[c]=control[c]=double((71+11*sample+23*c)&255)/255;
     }
     const auto gx=material_test::gxEvaluate(decoded,inputs),pica=material_test::picaEvaluate(carrierMaterial,inputs),plain=material_test::picaEvaluate(ordinaryRgb,inputs);
     assert(gx[3]==inputs.secondary[3]);
     for(unsigned c=0;c<4;++c){
      if(c<3){expected[c]=inputs.texture[0][0][c]*inputs.texture[1][1][c];assert(std::abs(gx[c]-expected[c])<1e-12);}
      if(c<3?carrierRaster.writeRgb:carrierRaster.writeAlpha)actual[c]=pica[c];
      if(c<3?ordinaryRgbRaster.writeRgb:ordinaryRgbRaster.writeAlpha)control[c]=plain[c];
      assert(std::abs(actual[c]-expected[c])<1e-12&&std::abs(control[c]-expected[c])<1e-12);
     }
    }
   }
   reset();
  }
  // Each real observer independently forbids alpha removal. Even requesting
  // visual fallbacks must not turn the two-original-texture lookup limit into
  // a dropped alpha dependency or a simplified/skipped successful draw.
  for(unsigned observer=0;observer<3;++observer){
   regs=alphaRegs;context={};context.allowRgba6Storage=true;context.allowDegradedDraws=true;
   context.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
   if(observer==0)regs.bp[0xf3]=128|(4u<<16)|(7u<<19);
   if(observer==1)regs.bp[0x41]|=1;
   if(observer==2){regs.bp[0x43]|=1;regs.bp[0x41]|=16;}
   RasterPlan raster;assert(planRaster(regs,raster,nullptr,FogPolicy::requireSupported,ColorStoragePolicy::rgba8ForRgba6));
   auto live=alphaMaterial;assert(!discardUnobservedTevAlpha(live,raster)&&live.stages[0].alpha==alphaMaterial.stages[0].alpha);
   CommandDraw candidate{original.primitive,original.vertexCount,original.indexCount,original.vertices,original.indices,original.plan,original.arrays,regs};
   const auto sent=submissions,bound=binds;const auto used=arena.used();
   assert(!drawGxCommand(candidate,&context)&&submissions==sent&&binds==bound&&arena.used()==used);
   assert(context.rejected==1&&context.failureKind==DrawFailureKind::state&&!std::strcmp(context.failure,"PICA texture unit budget"));
   assert(context.rejectedByKind[unsigned(DrawFailureKind::state)]==1&&context.rasterCarrierAttempts==1&&context.rasterCarrierRejects==1&&context.rasterCarrierFirstFailure==context.failure&&context.rasterCarrierFirstVertex==UINT32_MAX);
   assert(!context.submitted&&!context.skippedDraws&&!context.simplifiedDraws&&!context.rasterCarrierDraws);
   const auto first=context.failure;assert(!drawGxCommand(candidate,&context)&&context.failure==first&&context.rasterCarrierFirstFailure==first&&context.rejected==2&&context.rasterCarrierRejects==2&&submissions==sent&&binds==bound&&arena.used()==used);
  }
  // If RGB still needs Ras1, lookup receives the liveness-reduced material:
  // its second original texture was alpha-only and must not consume a slot.
  auto liveLookup=alphaMaterial;liveLookup.stages[1].color={15,0,10,15};
  liveLookup.stages[1].raster=1;liveLookup.stages[1].alpha={7,7,7,4};
  regs=validRegs;installMaterial(liveLookup,regs);regs.bp[0x41]&=~17u;
  PicaMaterialPlan refused;MaterialPlanError error;
  assert(!planRasterLookupMaterial(liveLookup,refused,&error)&&error==MaterialPlanError::textureUnits);
  make(3);context={};const auto sent=submissions;assert(draw(context)&&submissions==sent+1);
  assert(context.submitted==1&&context.rasterCarrierAttempts==1&&context.rasterCarrierDraws==1&&!context.rejected&&!context.skippedDraws&&!context.simplifiedDraws);
  assert(carrierMaterial.rasterCarrier==RasterCarrier::raster0RgbaRaster1Lookup&&carrierMaterial.sourceTextures()==1&&carrierMaterial.textureCount==3);
  assert((carrierMaterial.textures[0]==MaterialTextureBinding{0,0}));
  material_test::Samples inputs{};inputs.texture[0][0]={.2,.4,.6,.8};inputs.texture[1][1]={.9,.7,.5,.3};inputs.secondary={.8,.6,.4,.2};
  const auto gx=material_test::gxEvaluate(liveLookup,inputs),pica=material_test::picaEvaluate(carrierMaterial,inputs);
  for(unsigned c=0;c<3;++c)assert(std::abs(gx[c]-pica[c])<1e-12&&std::abs(pica[c]-inputs.texture[0][0][c]*inputs.secondary[c])<1e-12);
  reset();
  regs=savedRegs;
  std::puts("PASS automatic Ras1 alpha liveness: GD two-texture RGB/resources/UV0/UV1 and destination RGBA parity without lookup; alpha-test/blend/alpha-write observers retain unsupported alpha and first state failure");
 }
 // Following a carrier draw, the ordinary GX draw has the same semantic plan
 // and exact host vertex bytes with the diagnostic switch on or off.
 carrierMode=false;rigidTestMode=true;
 CommandDrawContext ordinary;assert(drawGxCommand(original,&ordinary));auto ordinaryVertices=rigidExpected;reset();
 ordinary={};ordinary.diagnosticRasterCarrier=true;assert(drawGxCommand(original,&ordinary));assert(rigidExpected.size()==ordinaryVertices.size());
 assert(!std::memcmp(rigidExpected.data(),ordinaryVertices.data(),rigidExpected.size()*sizeof(GpuVertex))&&!ordinary.rasterCarrierAttempts);
 rigidTestMode=false;arena=oldArena;submissions=oldSubmissions;binds=oldBinds;
 std::puts("PASS private carrier adapter: exact RGBA8 component transport, 3447th/indexed last-vertex zero-submit rejection, bounded telemetry, missing CLR1/light/matrix/normal/NaN, unsupported interactions, CPU/cache isolation, alpha/depth/clipping state, resource/submit failures and following ordinary bytes");
}
