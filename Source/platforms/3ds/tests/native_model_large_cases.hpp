#pragma once
// Include after native_engine_test.cpp's vertices()/uniforms()/readFile() helpers.
// Parent integration: nativeModelLargeCases(argv[3],argv[7]);
#include <filesystem>
namespace {
namespace modelLargeCases {
void put(std::vector<std::byte>& bytes,unsigned at,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes[at+n]=std::byte(value>>(8*n));}
std::vector<std::byte> geometry(unsigned count=384){
    std::vector<std::byte> bytes(32+count*32);std::memcpy(bytes.data(),"DUSKPN01",8);
    put(bytes,8,2);put(bytes,16,count);put(bytes,20,32);put(bytes,24,count*32);
    for(unsigned n=0;n<count;++n){
        auto v=vertices();for(auto& vertex:v)vertex.position[0]=float(n);
        const unsigned at=32+n*32,vo=unsigned(bytes.size()),vb=sizeof(v),ib=n%3==1?6:12;
        bytes.resize(align16(vo+vb));std::memcpy(bytes.data()+vo,v.data(),vb);
        const unsigned io=unsigned(bytes.size());bytes.resize(align16(io+ib));std::memcpy(bytes.data()+io,indices.data(),ib);
        put(bytes,at,n+1);put(bytes,at+4,unsigned(v.size()));put(bytes,at+8,ib/2);put(bytes,at+12,vo);put(bytes,at+16,io);put(bytes,at+20,vb);put(bytes,at+24,ib);
        put(bytes,at+28,crc32(std::span(bytes).subspan(vo,vb),std::span(bytes).subspan(io,ib)));
    }
    put(bytes,12,unsigned(bytes.size()));put(bytes,28,crc32(std::span(bytes).first(28),std::span(bytes).subspan(32,count*32)));return bytes;
}
std::vector<std::byte> plan(unsigned count=maxModelParts){
    const unsigned offset=48+count*64;std::vector<std::byte> bytes(offset+count*80);std::memcpy(bytes.data(),"DUSKML01",8);
    put(bytes,8,4);put(bytes,12,unsigned(bytes.size()));put(bytes,16,count);put(bytes,20,count);put(bytes,24,poseInput|colorInput);put(bytes,28,48);put(bytes,32,offset);put(bytes,36,count*80);
    for(unsigned n=0;n<count;++n){
        const unsigned at=48+n*64,u=offset+n*80;
        put(bytes,at,n%384+1);put(bytes,at+4,29);put(bytes,at+8,n);put(bytes,at+16,n&1);put(bytes,at+20,n);
        put(bytes,u,29);put(bytes,u+4,1);put(bytes,u+8,1);put(bytes,u+12,1);put(bytes,u+64,n/3);
        for(unsigned b=28;b<34;++b)bytes[u+b]=std::byte{255};
    }
    put(bytes,44,crc32(std::span(bytes).first(44),std::span(bytes).subspan(48)));return bytes;
}
std::vector<std::byte> envelope(const SourceIdentity& source,std::span<const std::byte> mesh,std::span<const std::byte> material,std::span<const std::byte> metadata){
    std::vector<std::byte> bytes(160);std::memcpy(bytes.data(),"DUSKNA02",8);put(bytes,8,2);put(bytes,16,source.bytes);put(bytes,20,maxModelParts);
    std::memcpy(bytes.data()+24,source.sha256.data(),32);std::memcpy(bytes.data()+100,source.imageContext.data(),32);
    const std::array<std::span<const std::byte>,4> sections{mesh,{},material,metadata};
    for(unsigned n=0;n<4;++n){const unsigned at=unsigned(bytes.size());put(bytes,56+n*8,at);put(bytes,60+n*8,unsigned(sections[n].size()));bytes.resize(align16(at+sections[n].size()));if(!sections[n].empty())std::memcpy(bytes.data()+at,sections[n].data(),sections[n].size());}
    const unsigned at=unsigned(bytes.size());put(bytes,88,at);put(bytes,92,maxModelParts*4);bytes.resize(align16(at+maxModelParts*4));
    for(unsigned n=0;n<maxModelParts;++n)put(bytes,at+n*4,n/3);
    put(bytes,12,unsigned(bytes.size()));put(bytes,156,crc32(std::span(bytes).first(156),std::span(bytes).subspan(160)));return bytes;
}
std::string key(const SourceIdentity& source){
    constexpr char hex[]="0123456789abcdef";std::string result;
    for(auto b:source.sha256){result+=hex[b>>4];result+=hex[b&15];}result+='-';
    for(auto b:source.imageContext){result+=hex[b>>4];result+=hex[b&15];}return result+".dna";
}
struct Context {GameDraw* draw{};unsigned calls{},badSlot=maxModelParts,failSlot=maxModelParts;std::array<unsigned,2> slots{};bool reenter{};};
bool produce(const void* instance,unsigned slot,DrawUniforms& out,void* user) noexcept {
    auto& c=*static_cast<Context*>(user);if(c.calls<c.slots.size())c.slots[c.calls]=slot;++c.calls;
    if(c.reenter)assert(!c.draw->forget(instance));
    if(slot==c.failSlot)return false;
    out=uniforms();out.viewProjection[0][0]=float(slot+1);if(slot==c.badSlot)out.palette.count=0;return true;
}
}
void nativeModelLargeCases(const char* materialPath,const char* root){
    using namespace modelLargeCases;
    auto meshBytes=geometry(),planBytes=plan();auto materialBytes=readFile(materialPath);
    MeshPack meshes;MaterialPack materials;TexturePack images;
    assert(MeshPack::open(meshBytes,meshes)==PackError::none&&MaterialPack::open(std::as_bytes(std::span(materialBytes)),materials)==PackError::none);
    ModelPlan parsed;assert(ModelPlan::open(planBytes,parsed)==PackError::none);
    PartBinding last;unsigned pass;assert(parsed.entry(maxModelParts-1,last,pass)&&last.uniformSlot==maxModelParts-1&&last.mesh==256&&pass==1);
    ModelPlan moved=std::move(parsed);assert(!parsed.size()&&!parsed.uniformCount()&&parsed.bindings(63).empty());
    std::fill(planBytes.begin(),planBytes.end(),std::byte{});
    assert(moved.entry(maxModelParts-1,last,pass)&&moved.uniform(last.uniformSlot)->palette[0]==1);
    auto invalidPlan=plan(maxModelParts+1);assert(ModelPlan::open(invalidPlan,parsed)!=PackError::none&&!parsed.size());
    auto r=std::make_unique<Renderer>();Limits limits;limits.drawsPerFrame=maxModelParts;limits.commandBytes=32768+maxModelParts*4096;limits.uniformBytes=2*sizeof(DrawUniforms);
    assert(r->initialize(limits));auto model=std::make_unique<Model>();auto draw=std::make_unique<GameDraw>();assert(draw->initialize(*r));
    {
        auto over=allocateStorage<PartBinding>(maxModelParts+1);assert(over);
        for(unsigned n=0;n<=maxModelParts;++n){over[n].mesh=1;over[n].material=29;}
        const auto beforeAdmission=nativeMock::allocationCalls;
        assert(!model->load(*r,meshes,materials,images,{over.get(),maxModelParts+1})&&model->error()==Error::invalidAsset&&!model->parts()&&nativeMock::allocationCalls==beforeAdmission);
    }
    auto parts=allocateStorage<PartBinding>(384);auto shapes=allocateStorage<unsigned>(384);
    for(unsigned n=0;n<384;++n){parts[n].mesh=n+1;parts[n].material=29;parts[n].uniformSlot=(n/3)*8+(n%3==1?0:7);shapes[n]=n/3;}
    assert(model->load(*r,meshes,materials,images,{parts.get(),384}));
    int instance{};Context context{draw.get()};assert(draw->bind(&instance,*model,{shapes.get(),384},produce,&context));
    assert(draw->uniformBytes()==2*sizeof(DrawUniforms)&&draw->metadataBytes()==384*sizeof(std::uint16_t));
    // Both binding and model metadata must survive releasing their source storage.
    shapes.reset();parts.reset();std::vector<std::byte>().swap(meshBytes);std::vector<char>().swap(materialBytes);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    const auto begin=[&]{assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&draw->beginExternalFrame(&target));};
    const auto end=[&]{assert(draw->endExternalFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);nativeMock::complete();};
    begin();const auto allocations=nativeMock::allocationCalls;
    // Every admitted range is exercised, with independent local indices, source
    // order, shared high uniform IDs, and exactly two producer calls per shape.
    for(unsigned shape=0;shape<128;++shape){
        context.calls=0;nativeMock::traceDraws=true;nativeMock::traceSize=0;
        const auto result=draw->draw(&instance,shape);assert(result&&result.submitted==3&&context.calls==2);
        assert(context.slots[0]==shape*8&&context.slots[1]==shape*8+7);
        const auto& trace=nativeMock::drawTrace;
        assert(trace[0].count==6&&trace[1].count==3&&trace[2].count==6);
        assert(trace[0].projectionX==float(shape*8+8)&&trace[1].projectionX==float(shape*8+1)&&trace[2].projectionX==float(shape*8+8));
        assert(static_cast<const Vertex*>(nativeMock::buffer.data)[0].position[0]==float(shape*3+2));
        nativeMock::traceDraws=false;
    }
    assert(nativeMock::allocationCalls==allocations);end();begin();
    const auto before=nativeMock::draws,buffers=nativeMock::bufferCalls;
    context.badSlot=1016;context.calls=0;auto result=draw->draw(&instance,127);
    assert(!result&&result.error==Error::invalidAsset&&!result.submitted&&context.calls==2&&nativeMock::draws==before&&nativeMock::bufferCalls==buffers);context.badSlot=maxModelParts;
    context.failSlot=1023;context.calls=0;result=draw->draw(&instance,127);assert(!result&&!result.submitted&&nativeMock::draws==before);context.failSlot=maxModelParts;
    context.reenter=true;result=draw->draw(&instance,127);assert(!result&&result.error==Error::protocol&&!result.submitted&&nativeMock::draws==before);context.reenter=false;
    nativeMock::failBufferAt=nativeMock::bufferCalls+2;result=draw->draw(&instance,127);nativeMock::failBufferAt=0;
    assert(!result&&result.error==Error::backend&&result.submitted==1&&nativeMock::draws==before+1);
    const auto retired=r->statistics().meshBytes;assert(model->unload()&&r->statistics().meshBytes==retired);
    result=draw->draw(&instance,127);assert(!result&&result.error==Error::invalidHandle&&!result.submitted);
    assert(draw->forget(&instance)&&draw->empty()&&!draw->metadataBytes());end();begin();assert(!r->statistics().meshBytes);end();
    assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    // Bind-time workspace rejection publishes no partial binding; a later
    // smaller source-shape mapping must still bind and draw using native slot1023.
    meshBytes=geometry();materialBytes=readFile(materialPath);
    assert(MeshPack::open(meshBytes,meshes)==PackError::none&&MaterialPack::open(std::as_bytes(std::span(materialBytes)),materials)==PackError::none);
    limits.uniformBytes=sizeof(DrawUniforms);assert(r->initialize(limits));
    parts=allocateStorage<PartBinding>(384);shapes=allocateStorage<unsigned>(384);
    for(unsigned n=0;n<384;++n){parts[n].mesh=n+1;parts[n].material=29;parts[n].uniformSlot=n&1?1023:0;shapes[n]=5;}
    assert(model->load(*r,meshes,materials,images,{parts.get(),384}));
    assert(!draw->bind(&instance,*model,{shapes.get(),384},produce,&context)&&draw->error()==Error::budget&&draw->empty()&&!draw->metadataBytes());
    for(unsigned n=1;n<384;n+=2)shapes[n]=8;
    assert(draw->bind(&instance,*model,{shapes.get(),384},produce,&context));parts.reset();shapes.reset();
    begin();context.calls=0;const auto triangles=r->statistics().triangles;result=draw->draw(&instance,8);
    assert(result&&result.submitted==192&&context.calls==1&&context.slots[0]==1023&&nativeMock::uniforms[24][0]==1024);
    assert(r->statistics().triangles==triangles+320&&static_cast<const Vertex*>(nativeMock::buffer.data)[0].position[0]==383);
    assert(draw->forget(&instance)&&model->unload());end();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    // The unchanged aggregate asset-metadata budget accounts for all owned
    // plan/model/shape storage BEFORE another bank can be published.
    limits.uniformBytes=256u<<10;assert(r->initialize(limits));
    auto factoryDraw=std::make_unique<GameDraw>();auto factory=std::make_unique<GameAssets>();assert(factoryDraw->initialize(*r)&&factory->initialize(*r,*factoryDraw,root));
    std::array<std::byte,32> original{};SourceIdentity first,second;std::memcpy(original.data(),"J3D2bmd3",8);original[11]=std::byte{32};
    original[12]=std::byte{183};assert(identifySource(original,first));original[12]=std::byte{184};assert(identifySource(original,second));
    for(auto* source:{&first,&second}){assert(identifyImageContext({},source->imageContext));source->hasImageContext=true;}
    planBytes=plan();const auto materialSpan=std::as_bytes(std::span(materialBytes));
    const auto writeAsset=[&](const SourceIdentity& source){const auto data=envelope(source,meshBytes,materialSpan,planBytes);const auto path=std::filesystem::path(root)/key(source);std::ofstream file(path,std::ios::binary);assert(file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size())));return path;};
    const auto pathA=writeAsset(first),pathB=writeAsset(second);int other{};
    const GameSourceProducer sourceProducer=+[](const void*,const UniformBinding&,DrawUniforms& u,void*) noexcept {u=uniforms();u.material.suppliedMask=1;return true;};
    assert(factory->bind(&instance,first,sourceProducer,nullptr,63));const auto metadata=factory->metadataBytes();const auto published=nativeMock::allocationCalls;
    assert(metadata>128u<<10&&metadata<=256u<<10);
    assert(!factory->bind(&other,second,sourceProducer,nullptr,63)&&factory->error()==GameAssetError::budget&&nativeMock::allocationCalls==published&&factory->residentAssets()==1&&factory->metadataBytes()==metadata);
    assert(factory->forget(&instance)&&factory->empty()&&!factory->metadataBytes()&&factoryDraw->empty());
    assert(std::filesystem::remove(pathA)&&std::filesystem::remove(pathB));assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS 384 source-ordered resident ranges, slot1023 dense per-shape uniforms, copied metadata lifetime, full preflight, exact backend prefix, reentry/stale guards, workspace rollback and pre-publication variable asset budget (SDK substitutes)");
}
}
