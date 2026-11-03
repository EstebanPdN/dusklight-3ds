#pragma once
#include "native_game_source_fixture.hpp"
#include <filesystem>
namespace {
void saveBytes(const std::filesystem::path& p,std::span<const char> bytes){std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(bytes.data(),bytes.size());f.close();assert(f);}
std::string sourceKey(const SourceIdentity& source){std::string s;for(const auto* digest:{&source.sha256,&source.imageContext}){if(!s.empty())s.push_back('-');for(auto b:*digest){s.push_back("0123456789abcdef"[b>>4]);s.push_back("0123456789abcdef"[b&15]);}}return s;}
void nativeGameSources(const char* root){
    const std::filesystem::path directory(root);unsigned hashes=0;
    for(unsigned size:{32u,55u,56u,63u,64u,65u,97u,128u,1024u,4096u}){
        auto bytes=readFile((directory/(std::to_string(size)+".source")).c_str());auto expected=readFile((directory/(std::to_string(size)+".sha256")).c_str());SourceIdentity id;
        assert(identifySource(std::as_bytes(std::span(bytes)),id)&&id.bytes==size&&expected.size()==32&&std::memcmp(id.sha256.data(),expected.data(),32)==0);++hashes;
        const auto preserved=id;bytes.back()^=1;SourceIdentity changed;
        if(size>32)assert(identifySource(std::as_bytes(std::span(bytes)),changed)&&changed!=preserved);
        bytes[0]=0;assert(!identifySource(std::as_bytes(std::span(bytes)),id)&&id.bytes==0);
    }
    std::array<std::array<std::uint8_t,32>,5> imageDigests{};
    for(unsigned n=0;n<imageDigests.size();++n){
        auto bti=readFile((directory/("image-"+std::to_string(n)+".bti")).c_str());
        auto expected=readFile((directory/("image-"+std::to_string(n)+".sha256")).c_str());
        const auto* p=reinterpret_cast<const std::uint8_t*>(bti.data());
        const auto be32=[&](unsigned at){unsigned v=0;for(unsigned j=0;j<4;++j)v=(v<<8)|p[at+j];return v;};
        const auto imageAt=be32(28);const auto paletteAt=n<3?imageAt:be32(12);
        assert(identifyImage({p,32},{p+imageAt,bti.size()-imageAt},{p+paletteAt,imageAt-paletteAt},imageDigests[n]));
        assert(expected.size()==32&&!std::memcmp(expected.data(),imageDigests[n].data(),32));
        auto broken=imageDigests[n];assert(!identifyImage({p,31},{p+imageAt,bti.size()-imageAt},{},broken)&&(broken==std::array<std::uint8_t,32>{}));
    }
    std::array<std::uint8_t,32> contextDigest{};
    auto expectedContext=readFile((directory/"images.context").c_str());
    assert(identifyImageContext(imageDigests,contextDigest)&&expectedContext.size()==32&&!std::memcmp(contextDigest.data(),expectedContext.data(),32));
    auto bytes=readFile((directory/"97.source").c_str());SourceIdentity source;assert(identifySource(std::as_bytes(std::span(bytes)),source));
    {auto bad=bytes;bad[8]^=1;SourceIdentity id;assert(!identifySource(std::as_bytes(std::span(bad)),id)&&id.bytes==0);}
    namespace f=nativeSourceFixture;f::Archive archive{unsigned(bytes.size())};f::Resource resource;resource.mArchive=&archive;
    assert(resource.run(0x424d4452,std::as_writable_bytes(std::span(bytes)))==0&&resource.model.source==source&&resource.mRes[0]==&resource.model); // Actual BMDR capture/attach blocks, source mutated between them.
    archive.size=31;assert(resource.run(0x424d4452,std::as_writable_bytes(std::span(bytes)))==-1);
    float positions[3][3][4]{},normals[3][3][3]{};
    for(unsigned m=0;m<3;++m)for(unsigned r=0;r<3;++r){positions[m][r][r]=1;positions[m][r][3]=float(10*m+r);normals[m][r][r]=1;}
    UniformBinding binding{23,1,3,{3,1,2}};Palette palette;
    assert(copyGamePalette(binding,3,positions,normals,palette)&&palette.count==3&&palette.position[0][3]==20&&palette.position[3][3]==0&&palette.position[6][3]==10);
    std::vector<std::byte> unalignedP(sizeof(positions)+1),unalignedN(sizeof(normals)+1);std::memcpy(unalignedP.data()+1,positions,sizeof(positions));std::memcpy(unalignedN.data()+1,normals,sizeof(normals));
    assert(copyGamePalette(binding,3,reinterpret_cast<const float(*)[3][4]>(unalignedP.data()+1),reinterpret_cast<const float(*)[3][3]>(unalignedN.data()+1),palette));
    f::Data data;f::Buffer buffer;buffer.p[0]=positions;buffer.n[0]=normals;f::J3DModel model{&data,&buffer};assert(model.copyNativePalette(binding,palette));
    for(unsigned flags:{0x100u,0x110u}){data.flags=flags;buffer.mMtxNum=2;buffer.mCurrentViewNo=1;model.accesses=0;assert(!model.copyNativePalette(binding,palette)&&palette.count==0&&model.accesses==0);}
    data.flags=0;buffer.mMtxNum=1;buffer.mCurrentViewNo=0;
    data.shapes[0].nbt=1;assert(!model.copyNativePalette(binding,palette));data.shapes[0].nbt=0;
    data.shapes[0].groupCount=1;data.shapes[0].groups[0].type=0x534d4242;
    assert(!model.copyNativePalette(binding,palette));data.shapes[0].groupCount=0;
    for(auto field:{&model.mDeformData,&model.mSkinDeform,&model.mVtxColorCalc,&model.mUnkCalc1,&model.mUnkCalc2}){*field=&data;assert(!model.copyNativePalette(binding,palette));*field=nullptr;}
    for(unsigned flags:{4u,8u,12u}){model.flags=flags;assert(!model.copyNativePalette(binding,palette));}model.flags=0;
    for(unsigned flags:{1u,3u}){model.flags=flags;model.accesses=0;assert(!model.copyNativePalette(binding,palette)&&model.accesses==0);}
    model.flags=2;assert(model.copyNativePalette(binding,palette));model.flags=0;
    auto invalid=binding;invalid.palette[1]=4;assert(!copyGamePalette(invalid,3,positions,normals,palette)&&palette.count==0);
    invalid=binding;invalid.paletteCount=9;assert(!copyGamePalette(invalid,3,positions,normals,palette));
    assert(!copyGamePalette(binding,0,positions,normals,palette)&&!copyGamePalette(binding,3,nullptr,normals,palette));
    for(unsigned bits:{1u,0x7f800001u,0x7fc00000u,0x7f800000u}){positions[0][0][0]=std::bit_cast<float>(bits);assert(!model.copyNativePalette(binding,palette)&&palette.count==0);}positions[0][0][0]=1;
    normals[0][0][0]=257;assert(!model.copyNativePalette(binding,palette));normals[0][0][0]=1;
    assert(model.copyNativePalette(binding,palette));
    // SHP concat-view billboards consume the remapped animation bank, not the
    // already-calculated draw bank; preserve translation and inverse scale.
    data.flags=0x10;
    f::Mtx animation[3]{};for(unsigned i=0;i<3;++i)f::MTXIdentity(animation[i]);
    animation[2][0][0]=0;animation[2][0][1]=-3;animation[2][1][0]=2;animation[2][1][1]=0;animation[2][2][2]=4;animation[2][0][3]=37;
    f::MTXIdentity(f::j3dSys.view);buffer.mpUserAnmMtx=animation;
    auto& shape=data.shapes[0];shape.groupCount=3;
    for(unsigned i=0;i<3;++i){shape.groups[i].type=0x534d4242;shape.groups[i].useMtxIndex=i;}
    auto billboard=binding;billboard.billboardMode=1;
    assert(model.copyNativePalette(billboard,palette)&&palette.position[0][0]==2&&palette.position[1][1]==3&&palette.position[2][2]==4&&palette.position[0][3]==37);
    assert(palette.normal[0][0]==.5f&&palette.normal[1][1]==1.f/3&&palette.normal[2][2]==.25f);
    buffer.mpWeightEvlpMtx=animation;data.drawFlags[2]=1;
    assert(model.copyNativePalette(billboard,palette)&&palette.position[0][3]==37);
    buffer.mpWeightEvlpMtx=nullptr;assert(!model.copyNativePalette(billboard,palette));data.drawFlags[2]=0;
    data.flags=0;shape.groupCount=1;shape.groups[0].type=0x534d5458;
    assert(model.copyNativePalette(billboard,palette)&&palette.position[0][3]==20);
    shape.groupCount=0;buffer.mpUserAnmMtx=nullptr;
    {
        // PAL R_SP01 vrbox_sora: BMDP flags 0x59020030, shape mode 0,
        // draw 0 -> joint 0, unit joint scale, no envelope or normal attribute.
        f::Data sky;sky.flags=0x59020030;sky.count=1;sky.jointCount=1;sky.envelopeCount=0;
        f::Buffer skyBuffer;
        f::Mtx joints[3]{},envelope[1]{},base{};for(auto& joint:joints)f::MTXIdentity(joint);
        f::MTXIdentity(envelope[0]);f::MTXIdentity(base);
        base[0][0]=0;base[0][1]=-1;base[1][0]=1;base[1][1]=0;base[0][3]=10;base[1][3]=20;base[2][3]=30;
        joints[0][0][3]=3;joints[0][1][3]=4;joints[0][2][3]=5;
        std::uint8_t scales[3]{1,1,1};skyBuffer.mpUserAnmMtx=joints;skyBuffer.scaleFlags=scales;
        f::J3DShapePacket skyPackets[1]{};skyPackets[0].base=&base;
        f::J3DModel skyModel{&sky,&skyBuffer};skyModel.mShapePacket=skyPackets;
        auto& skyShape=sky.shapes[0];skyShape.groupCount=1;skyShape.groups[0].type=0x534d4356;
        UniformBinding skyBinding{1,1,1,{1}};Palette actual;
        assert(skyModel.copyNativePalette(skyBinding,actual)&&skyModel.accesses==0&&actual.count==1);
        skyBuffer.mMtxNum=2;skyBuffer.mCurrentViewNo=1;
        assert(skyModel.copyNativePalette(skyBinding,actual)&&skyModel.accesses==0);
        assert((actual.position[0]==Row{0,-1,0,6})&&(actual.position[1]==Row{1,0,0,23})&&(actual.position[2]==Row{0,0,1,35}));
        assert((actual.normal[0]==Row{0,-1,0,0})&&(actual.normal[1]==Row{1,0,0,0})&&(actual.normal[2]==Row{0,0,1,0}));
        auto bad=skyBinding;bad.palette[0]=2;assert(!skyModel.copyNativePalette(bad,actual)&&actual.count==0);
        skyModel.flags=f::J3DMdlFlag_EnableLOD;assert(!skyModel.copyNativePalette(skyBinding,actual)&&actual.count==0);skyModel.flags=0;
        skyModel.flags=f::J3DMdlFlag_UseDefaultJ3D;assert(!skyModel.copyNativePalette(skyBinding,actual)&&actual.count==0);skyModel.flags=0;
        skyPackets[0].base=nullptr;assert(!skyModel.copyNativePalette(skyBinding,actual));skyPackets[0].base=&base;
        skyShape.groups[0].type=0x534d5458;assert(!skyModel.copyNativePalette(skyBinding,actual));skyShape.groups[0].type=0x534d4356;
        scales[0]=0;joints[0][0][0]=2;joints[0][1][1]=3;joints[0][2][2]=4;
        assert(skyModel.copyNativePalette(skyBinding,actual));
        assert((actual.position[0]==Row{0,-3,0,6})&&(actual.position[1]==Row{2,0,0,23}));
        assert(std::abs(actual.normal[0][1]+1.f/3)<1e-6f&&actual.normal[1][0]==.5f&&actual.normal[2][2]==.25f);
        joints[0][2][2]=0;assert(!skyModel.copyNativePalette(skyBinding,actual)&&actual.count==0);joints[0][2][2]=4;
        skyShape.groups[0].type=0x534d4242;auto skyBillboard=skyBinding;skyBillboard.billboardMode=1;
        f::MTXIdentity(f::j3dSys.view);f::j3dSys.view[0][3]=100;
        assert(skyModel.copyNativePalette(skyBillboard,actual)&&actual.position[0][3]==103);
        sky.count=3;sky.jointCount=3;sky.envelopeCount=1;sky.drawIndices={2,0,1};sky.drawFlags={0,1,0};
        skyBuffer.mpWeightEvlpMtx=envelope;envelope[0][0][0]=2;envelope[0][1][1]=3;envelope[0][2][2]=4;envelope[0][0][3]=7;
        skyShape.groups[0].type=0x534d4d43;skyShape.groups[0].useMtxNum=3;skyShape.groups[0].indices={0,1,2};
        UniformBinding multi{1,1,3,{2,1,3}};assert(skyModel.copyNativePalette(multi,actual)&&actual.count==3);
        assert(actual.position[0][3]==10&&actual.position[1][3]==27&&actual.position[2][3]==30);
        assert(std::abs(actual.normal[0][1]+1.f/3)<1e-6f&&actual.normal[1][0]==.5f&&actual.normal[2][2]==.25f);
        {   // Link al_face: envelope index past the joint scale table (5 joints, 35 envelopes) uses inverse transpose.
            f::Mtx envelopes[4]{};for(auto& e:envelopes)f::MTXIdentity(e);envelopes[3][0][0]=2;envelopes[3][1][1]=4;envelopes[3][2][2]=8;
            sky.envelopeCount=4;sky.drawIndices[1]=3;skyBuffer.mpWeightEvlpMtx=envelopes;
            assert(skyModel.copyNativePalette(multi,actual)&&actual.count==3&&(actual.position[0]==Row{0,-4,0,10}));
            assert((actual.normal[0]==Row{0,-.25f,0,0})&&(actual.normal[1]==Row{.5f,0,0,0})&&(actual.normal[2]==Row{0,0,.125f,0}));
            sky.envelopeCount=1;sky.drawIndices[1]=0;skyBuffer.mpWeightEvlpMtx=envelope;
        }
        skyShape.groups[0].indices[1]=65535;assert(!skyModel.copyNativePalette(multi,actual)&&actual.count==0);
    }
    // Real deferred matrices expose their storage and count. Production
    // nativeTexGen chooses posed linear normal and retains authored translation.
    std::array<f::J3DShapePacket,8> packets{};f::Mtx texMatrices[1]{{{2,0,0,.25f},{0,3,0,-.5f},{0,0,1,0}}};
    f::J3DTexMtxObj deferred{texMatrices,nullptr,1};packets[0].matrices=&deferred;model.mShapePacket=packets.data();
    auto textured=binding;textured.channelReadMask=0;textured.sourceStageCount=1;textured.sourceTexGenCount=1;textured.sourceAttributes=1u<<f::GX_VA_NRM;textured.textureSlots={0,255,255};textured.textureGenerators={0,255,255};
    data.material.coordinates[0].source=f::GX_TG_NRM;data.material.coordinates[0].matrix=f::GX_TEXMTX0;data.material.matrices[0].info.mInfo=1;
    DrawUniforms converted;
    assert(model.copyNativeParameters(textured,converted)&&converted.uvTransforms[0].source==7);
    assert((converted.uvTransforms[0].rows[0]==std::array<float,3>{2,0,0})&&(converted.uvTransforms[0].translation==std::array<float,2>{.25f,-.5f}));
    deferred.mpTexMtx=nullptr;assert(!model.copyNativeParameters(textured,converted));model.mShapePacket=nullptr;
    // Exercise the original count toggles against the actual parameter producer.
    // The appended image remains in slot 3, so toggling cannot change the key.
    f::Data warpData;warpData.materialCount=1;warpData.source=source;
    f::J3DModel warpModel{&warpData};auto& warp=warpData.material;
    warp.texture.count=2;warp.textureNumbers[3]=1;
    warp.tev.stageCount=3;warp.tev.orders[1].texMap=0;warp.tev.orders[2].texMap=3;warp.texGen.count=3;
    f::dRes_info_c::offWarpMaterial(&warpData);
    UniformBinding prepared;prepared.material=1;prepared.sourceStageCount=2;prepared.sourceTexGenCount=2;
    prepared.channelReadMask=0;prepared.sourceAttributes=1u<<f::GX_VA_TEX0;
    prepared.textureSlots={0,255,255};prepared.textureGenerators={0,255,255};prepared.parameterReadMasks[0]=15;
    assert(warpModel.copyNativeParameters(prepared,converted)&&converted.imageOverrides[0]==1);
    warp.textureNumbers[0]=1;warp.texture.images[1].wrapS=2;warp.registers[0]={1,2,3,4};
    warp.coordinates[0].matrix=f::GX_TEXMTX0;warp.matrices[0].value[0][3]=.25f;
    assert(warpModel.copyNativeParameters(prepared,converted)&&converted.imageOverrides[0]==2&&
           converted.samplers[0].wrapS==Wrap::mirror&&converted.material.colors[0]==0x04030201&&
           converted.uvTransforms[0].rows[0][2]==.25f);
    f::dRes_info_c::onWarpMaterial(&warpData);
    assert(warpData.source==source&&warp.texture.count==2&&warp.textureNumbers[3]==1);
    assert(!warpModel.copyNativeParameters(prepared,converted));
    warp.texGen.count=2;assert(!warpModel.copyNativeParameters(prepared,converted));
    warp.tev.stageCount=2;warp.texGen.count=3;assert(!warpModel.copyNativeParameters(prepared,converted));
    warp.tev.stageCount=3;f::dRes_info_c::offWarpMaterial(&warpData);
    assert(warpModel.copyNativeParameters(prepared,converted)&&converted.imageOverrides[0]==2);
    prepared.sourceStageCount=0;prepared.sourceTexGenCount=0;
    assert(!warpModel.copyNativeParameters(prepared,converted));
    assert(identifyImageContext({},source.imageContext));source.hasImageContext=true;
    // Palette guard follows the four resident matrices, not the original eight.
    invalid=binding;invalid.paletteCount=paletteSize+1;assert(!copyGamePalette(invalid,3,positions,normals,palette));
    std::printf("PASS %u independent SHA256 source identities; actual archive capture, typed J3D palette/weighted billboard and deferred normal texgen conversion, unaligned input, numeric and unsupported-mode guards (host)\n",hashes);
}
void nativeGameAssets(const char* root){
    const std::filesystem::path directory(root);auto bytes=readFile((directory/"97.source").c_str());SourceIdentity source;assert(identifySource(std::as_bytes(std::span(bytes)),source));
    assert(identifyImageContext({},source.imageContext));source.hasImageContext=true;
    auto packed=readFile((directory/(sourceKey(source)+".dna")).c_str());AssetPack pack;assert(AssetPack::open(std::as_bytes(std::span(packed)),source,pack)==PackError::none&&pack.plan.size()==3&&pack.shapes[0]==5&&pack.shapes[1]==5&&pack.shapes[2]==8);
    unsigned rejects=0;
    for(unsigned size=0;size<160;++size){assert(AssetPack::open(std::as_bytes(std::span(packed)).first(size),source,pack)!=PackError::none&&pack.plan.size()==0);++rejects;}
    const auto put=[](std::vector<char>& b,unsigned at,unsigned value){for(unsigned n=0;n<4;++n)b[at+n]=char(value>>(8*n));};
    const auto repair=[&](std::vector<char>& b){put(b,156,crc32(std::as_bytes(std::span(b)).first(156),std::as_bytes(std::span(b)).subspan(160)));};
    const auto reject=[&](std::vector<char> b,bool recalc=true){if(recalc)repair(b);assert(AssetPack::open(std::as_bytes(std::span(b)),source,pack)!=PackError::none&&pack.plan.size()==0);++rejects;};
    for(unsigned at:{0u,8u,12u,16u,20u,24u,55u,56u,60u,64u,68u,72u,76u,80u,84u,88u,92u,100u,123u,131u,132u,155u}){auto b=packed;b[at]^=1;reject(b);}
    {auto b=packed;put(b,96,2);reject(b);}
    {auto b=packed;b.back()^=1;reject(b,false);}
    {auto b=packed;put(b,56,0xfffffff0u);reject(b);}
    {auto b=packed;put(b,60,0xffffffffu);reject(b);}
    const auto get=[](const std::vector<char>& b,unsigned at){unsigned v=0;for(unsigned n=0;n<4;++n)v|=unsigned(static_cast<unsigned char>(b[at+n]))<<(8*n);return v;};
    {auto b=packed;put(b,get(b,88),4096);reject(b);}
    for(unsigned table:{56u,64u,72u,80u}){auto b=packed;const unsigned at=get(b,table);b[at]^=1;reject(b);}
    SourceIdentity wrong=source;wrong.sha256[0]^=1;assert(AssetPack::open(std::as_bytes(std::span(packed)),wrong,pack)!=PackError::none);
    wrong=source;wrong.imageContext[0]^=1;assert(AssetPack::open(std::as_bytes(std::span(packed)),wrong,pack)!=PackError::none);
    wrong=source;wrong.hasImageContext=false;assert(AssetPack::open(std::as_bytes(std::span(packed)),wrong,pack)!=PackError::none);
    wrong=source;wrong.sha256[0]^=1;
    auto r=std::make_unique<Renderer>();auto d=std::make_unique<GameDraw>();auto store=std::make_unique<GameAssets>();assert(r->initialize({})&&d->initialize(*r)&&store->initialize(*r,*d,root));
    struct Context{GameAssets* assets{};bool reenter{},fail{};unsigned calls{};};Context context{store.get()};
    GameSourceProducer producer=+[](const void* instance,const UniformBinding& binding,DrawUniforms& u,void* user) noexcept {
        auto& c=*static_cast<Context*>(user);++c.calls;if(c.reenter)assert(!c.assets->forget(instance));
        if(c.fail&&binding.material==29)return false;
        u=uniforms();u.palette.count=binding.paletteCount;u.material.suppliedMask=1;u.material.colors[0]=0x12345678;return true;
    };
    int a{},b{},c{};const auto allocated=nativeMock::allocationCalls;
    constexpr unsigned inputs=poseInput|colorInput;
    assert(!store->bind(&a,source,producer,&context,orderingInput)&&store->error()==GameAssetError::inputs&&nativeMock::allocationCalls==allocated&&store->empty());
    const auto tls=nativeMock::tls;int other{};nativeMock::tls=&other;assert(!store->bind(&a,source,producer,&context,inputs)&&store->error()==GameAssetError::owner);nativeMock::tls=tls;
    assert(!store->bind(&a,wrong,producer,&context,inputs)&&store->error()==GameAssetError::missing);
    const auto path=directory/(sourceKey(source)+".dna");
    {auto bad=packed;bad.back()^=1;saveBytes(path,bad);assert(!store->bind(&a,source,producer,&context,inputs)&&store->error()==GameAssetError::pack&&nativeMock::allocationCalls==allocated);saveBytes(path,packed);}
    {auto missing=source;missing.hasImageContext=false;assert(!store->bind(&a,missing,producer,&context,inputs)&&store->error()==GameAssetError::textureContext&&nativeMock::allocationCalls==allocated&&store->empty());}
    nativeMock::failAllocationAt=nativeMock::allocationCalls+1;assert(!store->bind(&a,source,producer,&context,inputs)&&store->error()==GameAssetError::backend&&store->empty());nativeMock::failAllocationAt=0;
    assert(store->bind(&a,source,producer,&context,inputs)&&store->residentAssets()==1&&store->metadataBytes()>0&&store->metadataBytes()<=256u<<10);
    // Same original model with another verified external-image context must
    // use a separate filename and resident resource, never alias the first.
    auto variant=source;variant.imageContext[0]^=1;
    auto variantPack=packed;std::memcpy(variantPack.data()+100,variant.imageContext.data(),32);
    put(variantPack,96,1);repair(variantPack);
    const auto variantPath=directory/(sourceKey(variant)+".dna");saveBytes(variantPath,variantPack);
    assert(store->bind(&c,variant,producer,&context,inputs)&&store->residentAssets()==2);
    const auto variantAllocations=nativeMock::allocationCalls;
    assert(store->bind(&c,source,producer,&context,inputs)&&store->residentAssets()==1&&nativeMock::allocationCalls==variantAllocations);
    assert(store->forget(&c)&&store->residentAssets()==1&&std::filesystem::remove(variantPath));
    const auto resident=nativeMock::allocationCalls;assert(std::filesystem::remove(path));
    assert(store->bind(&b,source,producer,&context,inputs)&&nativeMock::allocationCalls==resident&&store->residentAssets()==1);saveBytes(path,packed);
    assert(!store->bind(&a,wrong,producer,&context,inputs));FogParameters fog;assert(!store->bind(&c,source,producer,&context,inputs,&fog)&&store->error()==GameAssetError::inputs);
    C3D_RenderTarget surface{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&d->beginExternalFrame(&surface));
    context.reenter=true;auto result=d->draw(&a,5);assert(!result&&!result.submitted&&store->residentAssets()==1);context.reenter=false;
    result=d->draw(&a,5);assert(result&&result.submitted==2);result=d->draw(&b,8);assert(result&&result.submitted==1&&nativeMock::allocationCalls==resident);
    assert(store->bind(&c,source,producer,&context,inputs)&&nativeMock::allocationCalls==resident);assert(store->forget(&c));
    assert(!d->draw(&a,999));assert(store->forget(&a)&&!store->empty()&&store->residentAssets()==1);assert(store->forget(&b)&&store->empty()&&d->empty());
    const auto retired=r->statistics().meshBytes;assert(retired);assert(d->endExternalFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);nativeMock::complete();
    assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&d->beginExternalFrame(&surface)&&r->statistics().meshBytes==0);assert(d->endExternalFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);nativeMock::complete();
    std::array<int,65> instances{};for(unsigned n=0;n<64;++n)assert(store->bind(&instances[n],source,producer,&context,inputs));
    assert(!store->bind(&instances[64],source,producer,&context,inputs)&&store->error()==GameAssetError::budget);
    for(auto& i:instances)assert(store->forget(&i));assert(store->empty()&&d->empty());
    std::array<SourceIdentity,17> sources{};
    for(unsigned n=0;n<17;++n){auto changed=bytes;changed.back()=char(n+1);assert(identifySource(std::as_bytes(std::span(changed)),sources[n]));auto copy=packed;
        sources[n].imageContext=source.imageContext;sources[n].hasImageContext=true;
        std::memcpy(copy.data()+24,sources[n].sha256.data(),32);repair(copy);saveBytes(directory/(sourceKey(sources[n])+".dna"),copy);
        const bool loaded=store->bind(&instances[n],sources[n],producer,&context,inputs);
        if(n<16)assert(loaded&&store->residentAssets()==n+1&&store->metadataBytes()<=256u<<10);else assert(!loaded&&store->error()==GameAssetError::budget);
    }
    for(unsigned n=0;n<17;++n)assert(store->forget(&instances[n]));assert(store->empty()&&d->empty());
    assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::printf("PASS native asset envelope: %u malformed/truncated cases, source match, pre-upload input rejection, main-thread ownership, allocation rollback, shared resident instances, copied metadata, reentry refusal, 64-instance/16-asset budgets and completion-dependent retirement (SDK substitutes)\n",rejects);
}
}
