#include "native/renderer.hpp"
#include "native_shader_fixture.hpp"
#include <cassert>
#include <cstring>
#include <memory>
#include <cstdio>
using namespace dusk::pica;

// Link contracts.cpp + renderer.cpp with native_engine_stubs and the assembled
// native_shader_fixture.hpp emitted by test_native_engine.py. Host lifetime
// proof only: the separate GPU scenario owns normal/billboard pixel acceptance.
int main(){
    auto renderer=std::make_unique<Renderer>();
    assert(renderer->initialize({}));
    std::array<Vertex,3> vertices{};
    vertices[1].position[0]=1;vertices[2].position[1]=1;
    for(auto& vertex:vertices)vertex.normal={0,0,1};
    const std::array<std::uint16_t,3> indices{0,1,2};
    std::array<std::byte,256> pixels{};pixels.fill(std::byte{0x35});
    auto material=Material::textured();material.fog=true;material.fogValues.fill(.25f);
    auto mesh=renderer->createMesh(vertices,indices);
    auto texture=renderer->createTexture({8,8,TextureFormat::rgba8},pixels);
    auto state=renderer->createMaterial(material);
    assert(mesh&&texture&&state);
    DrawUniforms uniforms;
    for(unsigned i=0;i<4;++i)uniforms.viewProjection[i][i]=1;
    // Normal texgen on a newly resident part must coexist with queued parts.
    auto& uv=uniforms.uvTransforms[0];uv.source=6;
    uv.rows={{{.5f,0,0},{0,-.5f,0}}};uv.translation={.5f,.5f};
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(renderer->begin(&target));
    assert(renderer->draw(mesh,state,{texture,{},{}},uniforms));
    const void* oldVertices=nativeMock::buffer.data;
    const auto* oldTextureBinding=nativeMock::textures[0];
    const void* oldImage=oldTextureBinding->data;
    const auto* oldFog=nativeMock::fog;
    const auto oldFogValues=oldFog->values;
    std::array<std::byte,sizeof(vertices)> oldVertexBytes{};
    std::memcpy(oldVertexBytes.data(),oldVertices,oldVertexBytes.size());
    assert(renderer->release(mesh)&&renderer->release(texture)&&renderer->release(state));
    vertices[0].position[2]=2;pixels.fill(std::byte{0x79});material.fogValues.fill(.75f);
    auto newMesh=renderer->createMeshBytes(std::as_bytes(std::span(vertices)),std::as_bytes(std::span(indices)));
    auto newTexture=renderer->createTexture({8,8,TextureFormat::rgba8},pixels);
    auto newState=renderer->createMaterial(material);
    assert(newMesh&&newTexture&&newState);
    assert(nativeMock::textures[0]==oldTextureBinding&&oldTextureBinding->data==oldImage);
    assert(nativeMock::fog==oldFog&&oldFog->values==oldFogValues);
    assert(nativeMock::contains(oldVertices)&&nativeMock::contains(oldImage));
    assert(std::memcmp(oldVertices,oldVertexBytes.data(),oldVertexBytes.size())==0);
    for(unsigned i=0;i<256;++i)assert(static_cast<const std::byte*>(oldImage)[i]==std::byte{0x35});
    assert(renderer->draw(newMesh,newState,{newTexture,{},{}},uniforms));
    assert(nativeMock::buffer.data!=oldVertices&&nativeMock::textures[0]->data!=oldImage&&nativeMock::fog!=oldFog);
    assert(oldFog->values==oldFogValues&&nativeMock::contains(oldVertices)&&nativeMock::contains(oldImage));
    assert(renderer->end());
    assert(!renderer->begin(&target)&&renderer->error()==Error::busy);
    assert(nativeMock::contains(oldVertices)&&nativeMock::contains(oldImage)&&oldFog->values==oldFogValues);
    nativeMock::complete();assert(renderer->begin(&target));
    assert(!nativeMock::contains(oldVertices)&&!nativeMock::contains(oldImage));
    assert(renderer->draw(newMesh,newState,{newTexture,{},{}},uniforms));
    assert(renderer->end());nativeMock::complete();
    assert(renderer->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS normal-texgen resource admission preserves queued mesh/image/LUT storage until successful FrameBegin (host only)");
}
