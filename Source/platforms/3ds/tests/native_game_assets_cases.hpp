#pragma once
#include "native_game_source_fixture.hpp"
#include <filesystem>
namespace {
void saveBytes(const std::filesystem::path& p,std::span<const char> bytes){std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(bytes.data(),bytes.size());f.close();assert(f);}
std::string sourceKey(const SourceIdentity& source){std::string s;for(auto b:source.sha256){s.push_back("0123456789abcdef"[b>>4]);s.push_back("0123456789abcdef"[b&15]);}return s;}
void nativeGameSources(const char* root){
    const std::filesystem::path directory(root);unsigned hashes=0;
    for(unsigned size:{32u,55u,56u,63u,64u,65u,97u,128u,1024u,4096u}){
        auto bytes=readFile((directory/(std::to_string(size)+".source")).c_str());auto expected=readFile((directory/(std::to_string(size)+".sha256")).c_str());SourceIdentity id;
        assert(identifySource(std::as_bytes(std::span(bytes)),id)&&id.bytes==size&&expected.size()==32&&std::memcmp(id.sha256.data(),expected.data(),32)==0);++hashes;
        const auto preserved=id;bytes.back()^=1;SourceIdentity changed;
        if(size>32)assert(identifySource(std::as_bytes(std::span(bytes)),changed)&&changed!=preserved);
        bytes[0]=0;assert(!identifySource(std::as_bytes(std::span(bytes)),id)&&id.bytes==0);
    }
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
    for(unsigned flags:{0x10u,0x100u,0x110u}){data.flags=flags;buffer.mMtxNum=2;buffer.mCurrentViewNo=1;model.accesses=0;assert(!model.copyNativePalette(binding,palette)&&palette.count==0&&model.accesses==0);}
    data.flags=0;buffer.mMtxNum=1;buffer.mCurrentViewNo=0;
    data.billboard=true;assert(!model.copyNativePalette(binding,palette));data.billboard=false;
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
    std::printf("PASS %u independent SHA256 source identities; actual game archive capture before mutation and actual J3DModel palette copy with weighted/native IDs, unaligned input, numeric and unsupported-mode guards (host)\n",hashes);
}
void nativeGameAssets(const char* root){
    const std::filesystem::path directory(root);auto bytes=readFile((directory/"97.source").c_str());SourceIdentity source;assert(identifySource(std::as_bytes(std::span(bytes)),source));
    auto packed=readFile((directory/(sourceKey(source)+".dna")).c_str());AssetPack pack;assert(AssetPack::open(std::as_bytes(std::span(packed)),source,pack)==PackError::none&&pack.plan.size()==3&&pack.shapes[0]==5&&pack.shapes[1]==5&&pack.shapes[2]==8);
    unsigned rejects=0;
    for(unsigned size=0;size<128;++size){assert(AssetPack::open(std::as_bytes(std::span(packed)).first(size),source,pack)!=PackError::none&&pack.plan.size()==0);++rejects;}
    const auto put=[](std::vector<char>& b,unsigned at,unsigned value){for(unsigned n=0;n<4;++n)b[at+n]=char(value>>(8*n));};
    const auto repair=[&](std::vector<char>& b){put(b,124,crc32(std::as_bytes(std::span(b)).first(124),std::as_bytes(std::span(b)).subspan(128)));};
    const auto reject=[&](std::vector<char> b,bool recalc=true){if(recalc)repair(b);assert(AssetPack::open(std::as_bytes(std::span(b)),source,pack)!=PackError::none&&pack.plan.size()==0);++rejects;};
    for(unsigned at:{0u,8u,12u,16u,20u,24u,55u,56u,60u,64u,68u,72u,76u,80u,84u,88u,92u,100u,123u}){auto b=packed;b[at]^=1;reject(b);}
    {auto b=packed;put(b,96,2);reject(b);}
    {auto b=packed;b.back()^=1;reject(b,false);}
    {auto b=packed;put(b,56,0xfffffff0u);reject(b);}
    {auto b=packed;put(b,60,0xffffffffu);reject(b);}
    const auto get=[](const std::vector<char>& b,unsigned at){unsigned v=0;for(unsigned n=0;n<4;++n)v|=unsigned(static_cast<unsigned char>(b[at+n]))<<(8*n);return v;};
    {auto b=packed;put(b,get(b,88),4096);reject(b);}
    for(unsigned table:{56u,64u,72u,80u}){auto b=packed;const unsigned at=get(b,table);b[at]^=1;reject(b);}
    SourceIdentity wrong=source;wrong.sha256[0]^=1;assert(AssetPack::open(std::as_bytes(std::span(packed)),wrong,pack)!=PackError::none);
    auto r=std::make_unique<Renderer>();auto d=std::make_unique<GameDraw>();auto store=std::make_unique<GameAssets>();assert(r->initialize({})&&d->initialize(*r)&&store->initialize(*r,*d,root));
    struct Context{GameAssets* assets{};bool reenter{},fail{};unsigned calls{};};Context context{store.get()};
    GameSourceProducer producer=+[](const void* instance,const UniformBinding& binding,DrawUniforms& u,void* user) noexcept {
        auto& c=*static_cast<Context*>(user);++c.calls;if(c.reenter)assert(!c.assets->forget(instance));
        if(c.fail&&binding.material==29)return false;
        u=uniforms();u.palette.count=binding.paletteCount;u.material.suppliedMask=1;u.material.colors[0]=0x12345678;return true;
    };
    int a{},b{},c{};const auto allocated=nativeMock::allocationCalls;
    assert(!store->bind(&a,source,producer,&context,orderingInput)&&store->error()==GameAssetError::inputs&&nativeMock::allocationCalls==allocated&&store->empty());
    const auto tls=nativeMock::tls;int other{};nativeMock::tls=&other;assert(!store->bind(&a,source,producer,&context,63)&&store->error()==GameAssetError::owner);nativeMock::tls=tls;
    assert(!store->bind(&a,wrong,producer,&context,63)&&store->error()==GameAssetError::missing);
    const auto path=directory/(sourceKey(source)+".dna");
    {auto bad=packed;bad.back()^=1;saveBytes(path,bad);assert(!store->bind(&a,source,producer,&context,63)&&store->error()==GameAssetError::pack&&nativeMock::allocationCalls==allocated);saveBytes(path,packed);}
    {auto external=packed;put(external,96,1);repair(external);saveBytes(path,external);assert(!store->bind(&a,source,producer,&context,63)&&store->error()==GameAssetError::textureContext&&nativeMock::allocationCalls==allocated&&store->empty());saveBytes(path,packed);}
    nativeMock::failAllocationAt=nativeMock::allocationCalls+1;assert(!store->bind(&a,source,producer,&context,63)&&store->error()==GameAssetError::backend&&store->empty());nativeMock::failAllocationAt=0;
    assert(store->bind(&a,source,producer,&context,63)&&store->residentAssets()==1&&store->metadataBytes()>0&&store->metadataBytes()<=256u<<10);
    const auto resident=nativeMock::allocationCalls;assert(std::filesystem::remove(path));
    assert(store->bind(&b,source,producer,&context,63)&&nativeMock::allocationCalls==resident&&store->residentAssets()==1);saveBytes(path,packed);
    assert(!store->bind(&a,wrong,producer,&context,63));FogParameters fog;assert(!store->bind(&c,source,producer,&context,63,&fog)&&store->error()==GameAssetError::inputs);
    C3D_RenderTarget surface{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&d->beginExternalFrame(&surface));
    context.reenter=true;auto result=d->draw(&a,5);assert(!result&&!result.submitted&&store->residentAssets()==1);context.reenter=false;
    result=d->draw(&a,5);assert(result&&result.submitted==2);result=d->draw(&b,8);assert(result&&result.submitted==1&&nativeMock::allocationCalls==resident);
    assert(store->bind(&c,source,producer,&context,63)&&nativeMock::allocationCalls==resident);assert(store->forget(&c));
    assert(!d->draw(&a,999));assert(store->forget(&a)&&!store->empty()&&store->residentAssets()==1);assert(store->forget(&b)&&store->empty()&&d->empty());
    const auto retired=r->statistics().meshBytes;assert(retired);assert(d->endExternalFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);nativeMock::complete();
    assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&d->beginExternalFrame(&surface)&&r->statistics().meshBytes==0);assert(d->endExternalFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);nativeMock::complete();
    std::array<int,65> instances{};for(unsigned n=0;n<64;++n)assert(store->bind(&instances[n],source,producer,&context,63));
    assert(!store->bind(&instances[64],source,producer,&context,63)&&store->error()==GameAssetError::budget);
    for(auto& i:instances)assert(store->forget(&i));assert(store->empty()&&d->empty());
    std::array<SourceIdentity,17> sources{};
    for(unsigned n=0;n<17;++n){auto changed=bytes;changed.back()=char(n+1);assert(identifySource(std::as_bytes(std::span(changed)),sources[n]));auto copy=packed;
        std::memcpy(copy.data()+24,sources[n].sha256.data(),32);repair(copy);saveBytes(directory/(sourceKey(sources[n])+".dna"),copy);
        const bool loaded=store->bind(&instances[n],sources[n],producer,&context,63);
        if(n<16)assert(loaded&&store->residentAssets()==n+1&&store->metadataBytes()<=256u<<10);else assert(!loaded&&store->error()==GameAssetError::budget);
    }
    for(unsigned n=0;n<17;++n)assert(store->forget(&instances[n]));assert(store->empty()&&d->empty());
    assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::printf("PASS native asset envelope: %u malformed/truncated cases, source match, pre-upload input rejection, main-thread ownership, allocation rollback, shared resident instances, copied metadata, reentry refusal, 64-instance/16-asset budgets and completion-dependent retirement (SDK substitutes)\n",rejects);
}
}
