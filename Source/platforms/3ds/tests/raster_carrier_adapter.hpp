#pragma once
#include "raster_carrier_fixture.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include "../runtime/gx_profile.hpp"
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
 // Default-off preserves the existing fatal rejection; diagnostic policy does
 // not grant a degraded raster0 substitution.
 context={};context.allowDegradedDraws=true;context.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};reject(context,"TEV source/order unsupported");
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
 // Following a carrier draw, the ordinary GX draw has the same semantic plan
 // and exact host vertex bytes with the diagnostic switch on or off.
 carrierMode=false;rigidTestMode=true;
 CommandDrawContext ordinary;assert(drawGxCommand(original,&ordinary));auto ordinaryVertices=rigidExpected;reset();
 ordinary={};ordinary.diagnosticRasterCarrier=true;assert(drawGxCommand(original,&ordinary));assert(rigidExpected.size()==ordinaryVertices.size());
 assert(!std::memcmp(rigidExpected.data(),ordinaryVertices.data(),rigidExpected.size()*sizeof(GpuVertex))&&!ordinary.rasterCarrierAttempts);
 rigidTestMode=false;arena=oldArena;submissions=oldSubmissions;binds=oldBinds;
 std::puts("PASS private carrier adapter: exact RGBA8 component transport, 3447th/indexed last-vertex zero-submit rejection, bounded telemetry, missing CLR1/light/matrix/normal/NaN, unsupported interactions, CPU/cache isolation, alpha/depth/clipping state, resource/submit failures and following ordinary bytes");
}
