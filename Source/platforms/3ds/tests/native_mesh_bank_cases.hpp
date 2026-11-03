#pragma once
#include "native/renderer.hpp"
#include "native/pack.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <memory>
#include <vector>

// Include beside the other focused headers in native_engine_test.cpp and call
// nativeMeshBanks() after shader fixture registration, outside an active frame.
namespace {
namespace meshBankCases {
using namespace dusk::pica;
struct Chunk {std::array<Vertex,4> vertices{};std::array<std::uint16_t,6> indices{0,1,2,2,1,3};};
MeshView view(Chunk& chunk,unsigned n,unsigned count){
    return {(count-n)*17+3,std::as_bytes(std::span(chunk.vertices).first(n%2?4:3)),std::as_bytes(std::span(chunk.indices).first(n%2?6:3))};
}
DrawUniforms parameters(){DrawUniforms u;u.palette.count=4;for(unsigned n=0;n<4;++n)u.viewProjection[n][n]=1;return u;}
const void* allocation(const void* pointer){
    const auto address=reinterpret_cast<std::uintptr_t>(pointer);
    for(auto [base,size]:nativeMock::allocations){const auto start=reinterpret_cast<std::uintptr_t>(base);if(address>=start&&address-start<size)return base;}
    assert(false);return nullptr;
}
void put(std::vector<std::byte>& bytes,unsigned at,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes[at+n]=std::byte(value>>(8*n));}
void packCases(const MeshView& source){
    constexpr unsigned count=maxModelParts;
    const auto stride=align16(source.vertices.size())+align16(source.indices.size());
    std::vector<std::byte> bytes(32+count*32+count*stride);
    std::memcpy(bytes.data(),"DUSKPN01",8);put(bytes,8,2);put(bytes,12,unsigned(bytes.size()));put(bytes,16,count);put(bytes,20,32);put(bytes,24,count*32);
    for(unsigned n=0;n<count;++n){
        const unsigned at=32+n*32,vo=unsigned(32+count*32+n*stride),io=unsigned(vo+align16(source.vertices.size()));
        put(bytes,at,3+n*17);put(bytes,at+4,unsigned(source.vertices.size()/sizeof(Vertex)));put(bytes,at+8,unsigned(source.indices.size()/2));
        put(bytes,at+12,vo);put(bytes,at+16,io);put(bytes,at+20,unsigned(source.vertices.size()));put(bytes,at+24,unsigned(source.indices.size()));put(bytes,at+28,crc32(source.vertices,source.indices));
        std::memcpy(bytes.data()+vo,source.vertices.data(),source.vertices.size());std::memcpy(bytes.data()+io,source.indices.data(),source.indices.size());
    }
    const auto repair=[&]{put(bytes,28,crc32(std::span(bytes).first(28),std::span(bytes).subspan(32,count*32)));};repair();
    MeshPack pack;assert(MeshPack::open(bytes,pack)==PackError::none);
    const unsigned last=3+(count-1)*17;MeshView lastView;assert(pack.mesh(last,lastView)==PackError::none);
    assert(std::memcmp(lastView.vertices.data(),source.vertices.data(),source.vertices.size())==0&&std::memcmp(lastView.indices.data(),source.indices.data(),source.indices.size())==0);
    // Directory ownership and moves must not leave a live borrowed descriptor.
    put(bytes,32+(count-1)*32+12,0xffffffff);
    MeshPack moved=std::move(pack);assert(pack.size()==0&&pack.mesh(last,lastView)==PackError::header);
    assert(moved.mesh(last,lastView)==PackError::none&&lastView.vertices.size()==source.vertices.size());
    pack=std::move(moved);assert(moved.size()==0&&moved.mesh(last,lastView)==PackError::header);
    assert(pack.mesh(last,lastView)==PackError::none);
    const auto offset=std::size_t(lastView.vertices.data()-bytes.data());bytes[offset]^=std::byte{1};
    assert(pack.mesh(last,lastView)==PackError::checksum&&lastView.vertices.empty());bytes[offset]^=std::byte{1};
    put(bytes,16,count+1);assert(MeshPack::open(bytes,pack)==PackError::bounds&&pack.size()==0);
    put(bytes,16,count);put(bytes,32+(count-1)*32+12,0xffffffff);repair();
    assert(MeshPack::open(bytes,pack)==PackError::layout&&pack.size()==0);
}
}
void nativeMeshBanks(){
    using namespace dusk::pica;using namespace meshBankCases;
    constexpr unsigned count=384;
    std::vector<Chunk> chunks(count);std::vector<MeshView> views(count);std::size_t payloadBytes=0;
    for(unsigned n=0;n<count;++n){
        for(unsigned v=0;v<4;++v){auto& vertex=chunks[n].vertices[v];vertex.position={float(n+1),float(v),0};vertex.normal={0,0,1};
            vertex.boneRows[0]=std::uint8_t((n%4)*3);
            if(n%2){vertex.boneRows[0]=0;vertex.boneRows[1]=std::uint8_t((n%4)*3);vertex.weights={128,127,0,0};}
        }
        views[n]=view(chunks[n],n,count);GeometryInfo info;assert(inspectPackedGeometry(views[n].vertices,views[n].indices,info));payloadBytes+=info.bytes;
    }
    packCases(views.back());
    auto r=std::make_unique<Renderer>();Limits limits;limits.drawsPerFrame=maxModelParts;limits.commandBytes=32768+std::size_t(maxModelParts)*4096;
    assert(r->initialize(limits));const auto initialized=nativeMock::allocations.size();const auto before=nativeMock::allocationCalls;
    // A bad final range must fail before any GPU allocation or publication.
    auto invalid=views;invalid.back().id=0;assert(!r->createMeshBank(invalid)&&r->error()==Error::invalidAsset);
    invalid=views;invalid.back().id=invalid.front().id;assert(!r->createMeshBank(invalid)&&r->error()==Error::invalidAsset);
    invalid=views;invalid.back().vertices=invalid.back().vertices.first(1);assert(!r->createMeshBank(invalid)&&r->error()==Error::invalidAsset);
    const std::array<std::uint16_t,3> badIndices{0,1,4};invalid=views;invalid.back().indices=std::as_bytes(std::span(badIndices));
    assert(!r->createMeshBank(invalid)&&r->error()==Error::invalidAsset);
    assert(!r->createMeshBank({})&&r->error()==Error::invalidAsset);
    std::vector<MeshView> excessive(maxModelParts+1,views.front());assert(!r->createMeshBank(excessive)&&r->error()==Error::invalidAsset);
    assert(nativeMock::allocationCalls==before&&r->statistics().meshBytes==0&&nativeMock::allocations.size()==initialized);
    nativeMock::failAllocationAt=nativeMock::allocationCalls+1;assert(!r->createMeshBank(views)&&r->error()==Error::allocation);nativeMock::failAllocationAt=0;
    nativeMock::failPublication=true;assert(!r->createMeshBank(views)&&r->error()==Error::publication);nativeMock::failPublication=false;
    assert(r->statistics().meshBytes==0&&nativeMock::allocations.size()==initialized);
    const auto allocationsBeforeBank=nativeMock::allocationCalls;auto bank=r->createMeshBank(views);assert(bank);
    assert(nativeMock::allocationCalls==allocationsBeforeBank+1&&nativeMock::allocations.size()==initialized+1);
    const auto bankBytes=r->statistics().meshBytes;
    auto material=r->createMaterial(Material::vertexColor());assert(material);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    auto u=parameters();assert(!r->draw(bank,0,material,{},u)&&r->error()==Error::protocol);assert(r->begin(&target));
    const auto expected=chunks;
    for(auto& chunk:chunks)chunk={};for(auto& descriptor:views)descriptor={};
    const auto drawsBefore=nativeMock::draws,allocationsBeforeDraw=nativeMock::allocationCalls;
    const void* bankAllocation=nullptr;std::uintptr_t previousVertex=0;
    for(unsigned n=0;n<count;++n){
        if(n%4){auto shortPalette=u;shortPalette.palette.count=n%4;const auto draws=nativeMock::draws;
            assert(!r->draw(bank,n,material,{},shortPalette)&&r->error()==Error::invalidAsset&&nativeMock::draws==draws);}
        assert(r->draw(bank,n,material,{},u));
        const auto* vertex=static_cast<const Vertex*>(nativeMock::buffer.data);const auto* index=nativeMock::held[nativeMock::held.size()-4];
        if(!bankAllocation)bankAllocation=allocation(vertex);
        assert(allocation(vertex)==bankAllocation&&allocation(index)==bankAllocation&&nativeMock::allocations.at(const_cast<void*>(bankAllocation))==bankBytes);
        assert(reinterpret_cast<std::uintptr_t>(vertex)>previousVertex);previousVertex=reinterpret_cast<std::uintptr_t>(vertex);
        assert(std::memcmp(vertex,expected[n].vertices.data(),(n%2?4:3)*sizeof(Vertex))==0);
        assert(std::memcmp(index,expected[n].indices.data(),(n%2?6:3)*sizeof(std::uint16_t))==0);
        assert(nativeMock::selectedShader==(n%2?0u:1u));
        assert(nativeMock::attributes.count==9&&nativeMock::buffer.attributeCount==9&&nativeMock::attributes.permutation==0x876543210ULL&&nativeMock::buffer.order==0x876543210ULL);
    }
    assert(nativeMock::draws==drawsBefore+count&&nativeMock::allocationCalls==allocationsBeforeDraw);
    assert(r->statistics().triangles==count/2*3&&r->statistics().rigidDraws==count/2&&r->statistics().weightedDraws==count/2);
    const auto draws=nativeMock::draws;
    assert(!r->draw(bank,count,material,{},u)&&r->error()==Error::invalidAsset&&nativeMock::draws==draws);
    assert(!r->draw(bank,~0u,material,{},u)&&r->error()==Error::invalidAsset&&nativeMock::draws==draws);
    assert(!r->draw(MeshHandle{bank.value},material,{},u)&&r->error()==Error::invalidHandle);
    assert(!r->release(MeshHandle{bank.value})&&r->error()==Error::invalidHandle);
    chunks=expected;for(unsigned n=0;n<count;++n)views[n]=view(chunks[n],n,count);
    auto single=r->createMeshBytes(views[0].vertices,views[0].indices);assert(single);
    assert(!r->draw(MeshBankHandle{single.value},0,material,{},u)&&r->error()==Error::invalidHandle);
    assert(!r->release(MeshBankHandle{single.value})&&r->error()==Error::invalidHandle);
    assert(r->draw(single,material,{},u));assert(r->release(single));
    const auto freeBefore=nativeMock::frees;assert(r->release(bank));
    assert(!r->draw(bank,0,material,{},u)&&r->error()==Error::invalidHandle);
    assert(!r->release(bank)&&r->error()==Error::invalidHandle);assert(r->end());
    assert(!r->begin(&target)&&r->error()==Error::busy&&nativeMock::frees==freeBefore&&r->statistics().meshBytes>bankBytes);
    nativeMock::complete();assert(r->begin(&target)&&nativeMock::frees==freeBefore+2&&r->statistics().meshBytes==0);
    auto replacement=r->createMeshBank(views);assert(replacement&&replacement.value!=bank.value);
    assert(!r->draw(bank,0,material,{},u)&&r->error()==Error::invalidHandle);assert(r->draw(replacement,count-1,material,{},u));
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    // The directory is real charged storage, not an unbudgeted side allocation.
    limits.meshBytes=payloadBytes;assert(r->initialize(limits));const auto budgetBefore=nativeMock::allocationCalls;
    assert(!r->createMeshBank(views)&&r->error()==Error::budget&&nativeMock::allocationCalls==budgetBefore&&r->statistics().meshBytes==0);
    assert(r->releaseAfterContextDestroyed());limits.meshBytes=bankBytes;assert(r->initialize(limits));
    bank=r->createMeshBank(views);material=r->createMaterial(Material::vertexColor());assert(bank&&material&&r->begin(&target));
    assert(r->draw(bank,count-1,material,{},u)&&r->release(bank));const auto retiredBefore=nativeMock::allocationCalls;
    assert(!r->createMeshBank(views)&&r->error()==Error::budget&&nativeMock::allocationCalls==retiredBefore&&r->statistics().meshBytes==bankBytes);
    assert(r->end());assert(!r->begin(&target)&&r->statistics().meshBytes==bankBytes);nativeMock::complete();assert(r->begin(&target)&&r->statistics().meshBytes==0);
    replacement=r->createMeshBank(views);assert(replacement);assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    // The inclusive maximum is drawable, not merely a parser count allowance.
    limits.meshBytes=4<<20;assert(r->initialize(limits));std::vector<MeshView> maximum(maxModelParts,views.back());
    for(unsigned n=0;n<maximum.size();++n)maximum[n].id=19+n*13;
    bank=r->createMeshBank(maximum);material=r->createMaterial(Material::vertexColor());assert(bank&&material&&r->begin(&target));
    assert(r->draw(bank,maxModelParts-1,material,{},u));assert(std::memcmp(nativeMock::buffer.data,chunks.back().vertices.data(),sizeof(chunks.back().vertices))==0);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native mesh banks: 384 source-ordered copied ranges, local indices/palettes, one GPU allocation, full preflight, publication rollback, kind/stale guards, exact retired-byte budgets, inclusive 1024-range upload/draw and owned large MeshPack metadata (SDK substitutes)");
}
}
