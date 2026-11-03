#pragma once
// Include after native_engine_test.cpp's readFile()/uniforms() helpers and call
// nativeModelOverrides(argv[1],argv[3]) from the parent harness.
namespace {
namespace modelOverrideCases {
void put(std::vector<std::byte>& bytes,unsigned at,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes[at+n]=std::byte(value>>(8*n));}
std::uint32_t imageId(unsigned index){return index<3?index+1:1001+(index-3)*17;}
std::vector<std::byte> bank(unsigned count=5){
    std::vector<std::byte> bytes(32+count*32);std::memcpy(bytes.data(),"DUSKTX01",8);
    put(bytes,8,3);put(bytes,16,count);put(bytes,20,32);put(bytes,24,count*32);
    for(unsigned n=0;n<count;++n){
        const bool fog=n==4;
        const unsigned at=32+n*32,offset=unsigned(bytes.size()),width=n?8:16,height=fog?256:width,levels=n?1:2,size=fog?8192:n?256:1280;
        bytes.resize(offset+size,fog?std::byte{}:std::byte(n+1));
        if(fog)for(unsigned y=0;y<256;++y)for(unsigned x=0;x<8;++x){
            unsigned texel=(y/8)*64;for(unsigned bit=0;bit<3;++bit)texel|=((x>>bit)&1)<<(2*bit)|(((y%8)>>bit)&1)<<(2*bit+1);
            bytes[offset+4*texel]=std::byte(255-y);
        }
        put(bytes,at,imageId(n));put(bytes,at+4,width);put(bytes,at+8,height);put(bytes,at+12,unsigned(TextureFormat::rgba8));
        put(bytes,at+16,levels);put(bytes,at+20,offset);put(bytes,at+24,size);put(bytes,at+28,crc32(std::span(bytes).subspan(offset,size)));
    }
    put(bytes,12,unsigned(bytes.size()));put(bytes,28,crc32(std::span(bytes).first(28),std::span(bytes).subspan(32,count*32)));return bytes;
}
C3D_Tex selected(const TexturePack& bank,std::uint32_t id,const Sampler& sampler,unsigned unit=0){
    TextureView expected;assert(bank.image(id,expected)==PackError::none);
    const auto actual=*nativeMock::textures[unit];
    assert(actual.width==expected.image.width&&actual.height==expected.image.height);
    assert(std::memcmp(actual.data,expected.payload.data(),expected.payload.size())==0);
    const auto wrap=[](Wrap w){return w==Wrap::clamp?GPU_CLAMP_TO_EDGE:w==Wrap::repeat?GPU_REPEAT:GPU_MIRRORED_REPEAT;};
    const unsigned registers=GPU_TEXTURE_MAG_FILTER(unsigned(sampler.magnify))|GPU_TEXTURE_MIN_FILTER(unsigned(sampler.minify))|GPU_TEXTURE_MIP_FILTER(unsigned(sampler.mipmap))|
        GPU_TEXTURE_WRAP_S(wrap(sampler.wrapS))|GPU_TEXTURE_WRAP_T(wrap(sampler.wrapT));
    const unsigned mask=GPU_TEXTURE_MAG_FILTER(1)|GPU_TEXTURE_MIN_FILTER(1)|GPU_TEXTURE_WRAP_S(3)|GPU_TEXTURE_WRAP_T(3)|
        (expected.image.mipLevels>1?GPU_TEXTURE_MIP_FILTER(1):0);
    assert((actual.param&mask)==(registers&mask)&&actual.minLevel==sampler.firstLevel&&actual.maxLevel==sampler.lastLevel&&actual.lodBias==(unsigned(sampler.lodBias)&0x1fff));
    return actual;
}
void same(const C3D_Tex& a,const C3D_Tex& b){assert(a.data==b.data&&a.param==b.param&&a.minLevel==b.minLevel&&a.maxLevel==b.maxLevel&&a.lodBias==b.lodBias);}
C3D_Tex* framebuffer{};unsigned framebufferCalls{};
bool provideFramebuffer(void*& image,unsigned& width,unsigned& height){
    ++framebufferCalls;if(!framebuffer)return false;
    image=framebuffer;width=320;height=240;
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0); // Simulate capture's temporary target.
    return true;
}
}
void nativeModelOverrides(const char* meshPath,const char* materialPath){
    using namespace modelOverrideCases;
    auto meshBytes=readFile(meshPath),materialBytes=readFile(materialPath);auto imageBytes=bank();
    MeshPack geometry;MaterialPack fragments;TexturePack pixels;
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none);
    assert(MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    assert(TexturePack::open(imageBytes,pixels)==PackError::none);
    for(unsigned n=0;n<5;++n){TextureView byIndex,byId;assert(pixels.imageAt(n,byIndex)==PackError::none&&byIndex.id==imageId(n));assert(pixels.image(byIndex.id,byId)==PackError::none&&byIndex.payload.data()==byId.payload.data());}
    TextureView out;assert(pixels.imageAt(5,out)==PackError::missing&&!out.id&&out.payload.empty());
    TexturePack empty;assert(empty.imageAt(0,out)==PackError::header&&!out.id);
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto model=std::make_unique<Model>();
    std::array<PartBinding,2> parts{};parts[0].mesh=3;parts[0].material=23;parts[0].samplerMask=1;
    parts[0].samplers[0]={Wrap::repeat,Wrap::mirror,Filter::nearest,Filter::nearest,Filter::nearest,0,1,0};
    parts[1]=parts[0];parts[1].mesh=17;parts[1].uniformSlot=1;
    const auto beforeLoad=nativeMock::allocationCalls;
    {auto invalid=parts;invalid[1].material=43;assert(!model->load(*r,geometry,fragments,pixels,invalid)&&model->error()==Error::invalidAsset&&nativeMock::allocationCalls==beforeLoad);}
    {auto corrupt=imageBytes;corrupt.back()^=std::byte{1};TexturePack bad;assert(TexturePack::open(corrupt,bad)==PackError::none);
        assert(bad.imageAt(4,out)==PackError::checksum&&!out.id&&out.payload.empty());
        assert(!model->load(*r,geometry,fragments,bad,parts)&&model->error()==Error::invalidAsset&&nativeMock::allocationCalls==beforeLoad);}
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    // Fail during each image upload, including the unreferenced gapped helper.
    for(unsigned offset=2;offset<=6;++offset){nativeMock::failAllocationAt=nativeMock::allocationCalls+offset;
        assert(!model->load(*r,geometry,fragments,pixels,parts)&&model->error()==Error::allocation&&!model->parts());nativeMock::failAllocationAt=0;
        assert(r->begin(&target)&&r->statistics().meshBytes==0&&r->statistics().textureBytes==256);assert(r->end());nativeMock::complete();}
    assert(model->load(*r,geometry,fragments,pixels,parts)&&model->resources().images==5);
    auto lookup=std::make_unique<Model>();PartBinding lut=parts[0];lut.material=53;lut.samplerMask=7;
    assert(lookup->load(*r,geometry,fragments,pixels,std::span(&lut,1)));
    auto fogged=std::make_unique<Model>();PartBinding fogPart=parts[0];fogPart.material=61;fogPart.samplerMask=3;
    assert(fogged->load(*r,geometry,fragments,pixels,std::span(&fogPart,1)));
    std::array<DrawUniforms,2> parameters{uniforms(),uniforms()};for(auto& u:parameters){u.palette.count=3;u.material.suppliedMask=1;}
    const auto nextFrame=[&]{assert(r->end());nativeMock::complete();assert(r->begin(&target));};
    assert(r->begin(&target));auto result=model->draw(parameters);assert(result&&result.submitted==2);
    const auto original=selected(pixels,1,parts[1].samplers[0]);
    const auto* queuedGeometry=nativeMock::buffer.data;
    std::array<std::byte,3*sizeof(Vertex)> queuedVertices{};std::memcpy(queuedVertices.data(),queuedGeometry,queuedVertices.size());
    std::array<std::byte,1280> queuedImage{};std::memcpy(queuedImage.data(),original.data,queuedImage.size());
    const auto freesBeforeAdmission=nativeMock::frees;
    auto admitted=std::make_unique<Model>();assert(admitted->load(*r,geometry,fragments,pixels,parts));
    assert(nativeMock::pendingDraws==2&&nativeMock::frees==freesBeforeAdmission);
    assert(std::memcmp(queuedGeometry,queuedVertices.data(),queuedVertices.size())==0&&std::memcmp(original.data,queuedImage.data(),queuedImage.size())==0);
    assert(admitted->draw(parameters));assert(nativeMock::textures[0]->data!=original.data);
    assert(model->draw(parameters));same(selected(pixels,1,parts[1].samplers[0]),original);
    const auto allocated=nativeMock::allocationCalls;const auto textureBytes=r->statistics().textureBytes;
    nextFrame();auto& animated=parameters[1];animated.imageOverrideMask=animated.samplerMask=1;animated.imageOverrides[0]=2;
    animated.samplers[0]={Wrap::mirror,Wrap::repeat,Filter::linear,Filter::linear,Filter::linear,0,0,-125};
    result=model->draw(parameters);assert(result&&result.submitted==2);const auto second=selected(pixels,2,animated.samplers[0]);assert(second.data!=original.data&&second.param!=original.param&&second.maxLevel!=original.maxLevel);
    nextFrame();parameters[1]=parameters[0];assert(model->draw(parameters));same(selected(pixels,1,parts[1].samplers[0]),original);
    // Independent parameter objects are two instances of one immutable model.
    auto a=parameters[0],b=a;a.imageOverrideMask=a.samplerMask=1;a.imageOverrides[0]=2;a.samplers[0]={Wrap::mirror,Wrap::clamp,Filter::linear,Filter::nearest,Filter::linear,0,0,17};
    b.imageOverrideMask=b.samplerMask=1;b.imageOverrides[0]=1001;b.samplers[0]={Wrap::clamp,Wrap::repeat,Filter::nearest,Filter::linear,Filter::nearest,0,0,-256};
    std::array<SceneItem,2> instances{{{model.get(),&a,0,0,0},{model.get(),&b,1,1,0}}};
    nativeMock::traceSize=0;nativeMock::traceDraws=true;result=Scene::draw(*r,instances);assert(result&&result.submitted==2&&nativeMock::traceSize==2);
    const auto helper=selected(pixels,1001,b.samplers[0]);assert(nativeMock::drawTrace[0].images[0]==second.data&&nativeMock::drawTrace[1].images[0]==helper.data&&helper.data!=second.data);
    assert(nativeMock::drawTrace[0].sampling[0]!=nativeMock::drawTrace[1].sampling[0]);nativeMock::traceDraws=false;
    assert(model->drawPart(0,a));assert(selected(pixels,2,a.samplers[0]).data==second.data);
    assert(model->drawPart(1,b));same(selected(pixels,1001,b.samplers[0]),helper);
    assert(model->drawPart(0,parameters[0]));same(selected(pixels,1,parts[0].samplers[0]),original);
    // Image-only uses the binding sampler; sampler-only keeps the static image.
    auto imageOnly=parameters[0];imageOnly.imageOverrideMask=1;imageOnly.imageOverrides[0]=1;assert(model->drawPart(0,imageOnly));same(selected(pixels,1,parts[0].samplers[0]),original);
    auto samplerOnly=parameters[0];samplerOnly.samplerMask=1;samplerOnly.samplers[0]=a.samplers[0];assert(model->drawPart(0,samplerOnly));selected(pixels,1,samplerOnly.samplers[0]);
    for(unsigned bad=0;bad<12;++bad){auto invalid=parameters;
        auto& u=invalid[1];
        if(bad==0){u.imageOverrideMask=1;u.imageOverrides[0]=0;}
        if(bad==1){u.imageOverrideMask=1;u.imageOverrides[0]=99;}
        if(bad==2){u.imageOverrideMask=2;u.imageOverrides[1]=2;}
        if(bad==3)u.samplerMask=2;
        if(bad==4){u.imageOverrideMask=1;u.imageOverrides[0]=2;} // Static lastLevel1 cannot sample this image.
        if(bad==5){u=a;u.samplers[0].lastLevel=1;} // Live sampler checked against the selected image, not image1.
        if(bad==6){u.samplerMask=1;u.samplers[0].lastLevel=2;}
        if(bad==7){u.projective.enabled=true;u.uvTransforms[0].source=1;}
        if(bad==8)u.material.suppliedMask=0;
        if(bad==9){u.imageOverrideMask=1;u.imageOverrides[0]=0xffffffffu;}
        if(bad==10){u.fog.provided=u.fog.enabled=true;}
        if(bad==11){u.imageOverrides[0]=2;} // Nonzero ID without its provided bit.
        const auto draws=nativeMock::draws,buffers=nativeMock::bufferCalls;
        result=model->draw(invalid);assert(!result&&result.error==Error::invalidAsset&&!result.submitted);
        result=model->drawParts(std::array<unsigned,2>{0,1},invalid);assert(!result&&result.error==Error::invalidAsset&&!result.submitted);
        result=model->drawPart(1,u);assert(!result&&result.error==Error::invalidAsset&&!result.submitted);
        assert(nativeMock::draws==draws&&nativeMock::bufferCalls==buffers);
    }
    auto selectedParameters=parameters;selectedParameters[0]=a;selectedParameters[1]=b;
    nativeMock::traceSize=0;nativeMock::traceDraws=true;result=model->drawParts(std::array<unsigned,3>{1,0,1},selectedParameters);assert(result&&result.submitted==3&&nativeMock::traceSize==3);
    assert(nativeMock::drawTrace[0].count==3&&nativeMock::drawTrace[1].count==6&&nativeMock::drawTrace[2].count==3);
    assert(nativeMock::drawTrace[0].images[0]==helper.data&&nativeMock::drawTrace[1].images[0]==second.data&&nativeMock::drawTrace[2].images[0]==helper.data);nativeMock::traceDraws=false;
    selectedParameters[1].imageOverrides[0]=99;result=model->drawParts(std::array<unsigned,1>{0},selectedParameters);assert(result&&result.submitted==1); // Invisible part is not validated or drawn.
    assert(lookup->drawPart(0,parameters[0]));const Sampler lutSampler{};selected(pixels,2,lutSampler,1);selected(pixels,3,lutSampler,2);
    assert(lookup->drawPart(0,a));selected(pixels,2,a.samplers[0]);selected(pixels,2,lutSampler,1);selected(pixels,3,lutSampler,2);
    auto projected=a;projected.projective.enabled=true;assert(model->drawPart(0,projected));const auto projection=selected(pixels,2,a.samplers[0]);
    assert(((projection.param>>28)&7)==GPU_TEX_PROJECTION);assert(model->drawPart(0,parameters[0]));same(selected(pixels,1,parts[0].samplers[0]),original);
    for(unsigned mask:{2u,4u}){auto u=parameters[0];u.samplerMask=std::uint8_t(mask);const auto draws=nativeMock::draws,buffers=nativeMock::bufferCalls;
        assert(!lookup->drawPart(0,u));u.samplerMask=0;u.imageOverrideMask=std::uint8_t(mask);u.imageOverrides[mask==2?1:2]=2;assert(!lookup->drawPart(0,u));assert(nativeMock::draws==draws&&nativeMock::bufferCalls==buffers);}
    auto fogParameters=parameters[0];fogParameters.fog.provided=fogParameters.fog.enabled=true;
    fogParameters.fog.a=.25f;fogParameters.fog.b=1;fogParameters.fog.c=.125f;fogParameters.fog.color=0x44332211;
    fogParameters.material.suppliedMask|=128;fogParameters.material.colors[7]=fogParameters.fog.color;
    nextFrame();const auto fogDraws=nativeMock::draws;
    result=fogged->drawPart(0,fogParameters);assert(result&&result.submitted==1&&nativeMock::draws==fogDraws+1);
    selected(pixels,1,fogPart.samplers[0]);selected(pixels,imageId(4),Sampler{},1);
    assert(nativeMock::uniforms[83][1]==float(FogCarrier::texture1)&&nativeMock::env[1].color==fogParameters.fog.color);
    assert(nativeMock::uniforms[84][0]==.25f&&nativeMock::uniforms[85][1]==1);
    assert(model->drawPart(0,parameters[0]));same(selected(pixels,1,parts[0].samplers[0]),original);
    assert(nativeMock::uniforms[83][1]==0&&nativeMock::uniforms[85][1]==0);
    for(unsigned bad=0;bad<5;++bad){auto invalid=fogParameters;
        if(bad==0){invalid.imageOverrideMask=2;invalid.imageOverrides[1]=2;}
        if(bad==1)invalid.samplerMask=2;
        if(bad==2)invalid.uvTransforms[1].source=0;
        if(bad==3)invalid.material.colors[7]^=1;
        if(bad==4)invalid.fog.provided=false;
        std::array<DrawUniforms,1> batch{invalid};const auto draws=nativeMock::draws,buffers=nativeMock::bufferCalls;
        for(auto rejected:{fogged->draw(batch),fogged->drawParts(std::array<unsigned,1>{0},batch),fogged->drawPart(0,invalid)})
            assert(!rejected&&rejected.error==Error::invalidAsset&&!rejected.submitted);
        assert(nativeMock::draws==draws&&nativeMock::bufferCalls==buffers);
    }
    assert(fogged->drawPart(0,fogParameters));selected(pixels,imageId(4),Sampler{},1);
    assert(nativeMock::allocationCalls==allocated&&r->statistics().textureBytes==textureBytes);
    nextFrame();assert(model->draw(parameters));same(selected(pixels,1,parts[1].samplers[0]),original);
    assert(r->end());nativeMock::complete();C3D_RenderTarget noDepth{{reinterpret_cast<void*>(1),240,400,nullptr}};assert(r->begin(&noDepth));
    const auto beforeDepth=nativeMock::draws;result=model->draw(parameters);assert(!result&&result.error==Error::invalidAsset&&!result.submitted);
    result=model->drawParts(std::array<unsigned,2>{0,1},parameters);assert(!result&&result.error==Error::invalidAsset&&!result.submitted&&nativeMock::draws==beforeDepth);
    assert(model->unload()&&lookup->unload()&&fogged->unload()&&admitted->unload()&&r->end());nativeMock::complete();assert(r->begin(&target)&&r->statistics().textureBytes==256&&r->statistics().meshBytes==0);assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    // Bank budgeting includes every unselected image, before any model allocation.
    Limits small;small.textureBytes=256+1280;assert(r->initialize(small));const auto budgetAllocations=nativeMock::allocationCalls;
    assert(!model->load(*r,geometry,fragments,pixels,parts)&&model->error()==Error::budget&&nativeMock::allocationCalls==budgetAllocations);assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    // Keep the existing 64-image limit, retaining the last gapped ID as resident.
    auto maximum=bank(64);TexturePack full;assert(TexturePack::open(maximum,full)==PackError::none&&full.size()==64);
    auto excessive=bank(65);TexturePack tooMany;assert(TexturePack::open(excessive,tooMany)==PackError::bounds);
    assert(r->initialize({})&&model->load(*r,geometry,fragments,full,parts)&&model->resources().images==64);
    assert(r->begin(&target));auto last=a;last.imageOverrides[0]=imageId(63);assert(model->drawPart(0,last));selected(full,imageId(63),last.samplers[0]);
    assert(model->unload()&&r->end());nativeMock::complete();assert(r->begin(&target)&&r->statistics().textureBytes==256);assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS actual Model/Renderer image-bank residency, animated GPU image/filter/wrap/mip changes and return, independent shared-model instances, per-part/batch refusals, immutable LUT units, full-bank CRC/budget/64-image bounds and upload rollback (SDK substitutes)");
}
void nativeFramebufferBorrow(const char* meshPath,const char* materialPath){
    using namespace modelOverrideCases;
    auto meshBytes=readFile(meshPath),materialBytes=readFile(materialPath);
    MeshPack geometry;MaterialPack fragments;TexturePack pixels;
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none);
    assert(MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    std::vector<std::byte> bytes(64);std::memcpy(bytes.data(),"DUSKTX01",8);
    put(bytes,8,3);put(bytes,12,64);put(bytes,16,1);put(bytes,20,32);put(bytes,24,32);
    put(bytes,32,1);put(bytes,44,4);put(bytes,48,1);put(bytes,52,64);
    put(bytes,28,crc32(std::span(bytes).first(28),std::span(bytes).subspan(32,32)));
    assert(TexturePack::open(bytes,pixels)==PackError::none);
    TextureView role;assert(pixels.image(1,role)==PackError::none&&role.image.framebufferColor&&role.payload.empty()&&role.image.mipLevels==1);
    for(unsigned at:{36u,40u,48u,56u,60u}){
        auto bad=bytes;put(bad,at,2);put(bad,28,crc32(std::span(bad).first(28),std::span(bad).subspan(32,32)));
        TexturePack rejected;assert(TexturePack::open(bad,rejected)!=PackError::none);
    }
    auto r=std::make_unique<Renderer>();auto model=std::make_unique<Model>();Limits limits;limits.textureBytes=256;
    assert(r->initialize(limits)&&r->setFramebufferProvider(provideFramebuffer));
    PartBinding part{};part.mesh=3;part.material=23;part.samplerMask=1;
    const auto before=nativeMock::allocationCalls;
    assert(model->load(*r,geometry,fragments,pixels,std::span(&part,1)));
    assert(nativeMock::allocationCalls==before+1&&r->statistics().textureBytes==256);
    C3D_Tex first{},second{};
    assert(C3D_TexInit(&first,512,256,GPU_RGBA8)&&C3D_TexInit(&second,512,256,GPU_RGBA8));
    assert(!R_FAILED(GSPGPU_FlushDataCache(first.data,first.size))&&!R_FAILED(GSPGPU_FlushDataCache(second.data,second.size)));
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(r->begin(&target)&&r->scissor(3,4,230,390));
    auto u=uniforms();u.palette.count=3;u.material.suppliedMask=1;
    framebuffer=nullptr;assert(!model->drawPart(0,u));
    framebuffer=&first;u.uvTransforms[0].rows={{{2,3,4},{5,6,7}}};u.uvTransforms[0].translation={8,9};
    assert(model->drawPart(0,u)&&nativeMock::textures[0]->data==first.data);
    assert((nativeMock::uniforms[73]==Row{1.25f,1.875f,2.5f,5.f}));
    assert((nativeMock::uniforms[74]==Row{-4.6875f,-5.625f,-6.5625f,-7.4375f}));
    assert((nativeMock::scissor==std::array<unsigned,5>{unsigned(GPU_SCISSOR_NORMAL),3,4,230,390}));
    const auto acquired=framebufferCalls;auto invalid=u;invalid.samplerMask=1;invalid.samplers[0].wrapS=Wrap::repeat;
    assert(!model->drawPart(0,invalid)&&framebufferCalls==acquired);
    framebuffer=&second;auto projective=uniforms();projective.palette.count=3;projective.material.suppliedMask=1;
    projective.projective.enabled=true;projective.projective.rows={Row{2,0,0,4},Row{0,2,0,6},Row{0,0,1,2}};
    const auto allocations=nativeMock::allocationCalls;
    assert(model->drawPart(0,projective)&&nativeMock::textures[0]->data==second.data&&nativeMock::allocationCalls==allocations);
    assert((nativeMock::uniforms[80]==Row{1.25f,0,0,2.5f})&&(nativeMock::uniforms[81]==Row{0,-1.875f,1,-3.625f}));
    assert(model->unload()&&r->end());nativeMock::complete();
    assert(r->begin(&target)&&r->statistics().textureBytes==256);
    assert(nativeMock::allocations.count(first.data)&&nativeMock::allocations.count(second.data));
    assert(r->end());nativeMock::complete();
    assert(model->load(*r,geometry,fragments,pixels,std::span(&part,1)));
    assert(r->begin(&target)&&model->drawPart(0,projective)&&model->unload()&&r->end());nativeMock::complete();
    assert(r->releaseAfterContextDestroyed());
    assert(nativeMock::allocations.count(first.data)&&nativeMock::allocations.count(second.data));
    C3D_TexDelete(&first);C3D_TexDelete(&second);framebuffer=nullptr;assert(nativeMock::allocations.empty());
    std::puts("PASS borrowed live framebuffer capture, clamp/refusal, affine/projective padded UVs and ownership through collect (host SDK substitutes)");
}
}
