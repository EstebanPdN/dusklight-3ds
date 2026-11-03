#pragma once
#include "renderer.hpp"
#include "pack.hpp"
#include "texture_pack.hpp"
#include "material_pack.hpp"
#include "storage.hpp"
namespace dusk::pica {
struct PartBinding {
    std::uint32_t mesh{},material{};
    unsigned uniformSlot{}; // Native pose/environment parameter block,0..maxModelParts-1.
    std::array<Sampler,3> samplers{}; // Explicit for each consumed material unit.
    std::uint8_t samplerMask{}; // Must exactly match consumed units, no inferred defaults.
};
struct Submission {unsigned submitted{};Error error=Error::none;explicit operator bool()const{return error==Error::none;}};
struct ModelResources {unsigned meshes{},materials{},images{};};
// Native resident model only: no original model parsing, source state stream,
// hierarchy traversal, matrix calculation, animation or CPU vertex work.
// Owns resource handles; unload before renderer destruction. No implicit GPU
// work in the destructor. Keep Renderer alive at the same address throughout.
class Model {
public:
    Model()=default;
    Model(const Model&)=delete;Model& operator=(const Model&)=delete;
    // Preloads immutable image storage; capture roles retain their authored IDs
    // without allocation and borrow the live provider only on submission.
    bool load(Renderer&,const MeshPack&,const MaterialPack&,const TexturePack&,
              std::span<const PartBinding>,const FogParameters* environment=nullptr);
    bool unload(); // Retires handles; renderer owns completion-dependent freeing.
    // Image overrides name resident bank IDs; samplers merge by provided unit.
    // Renderer submissions contain handles only, never unresolved bank IDs.
    Submission draw(std::span<const DrawUniforms>);
    // Explicit native visibility/order. Repeated indices are separate draws.
    Submission drawParts(std::span<const unsigned> indices,std::span<const DrawUniforms>);
    // Direct per-instance parameters; ignores the pack's uniform-slot number.
    Submission drawPart(unsigned index,const DrawUniforms&);
    unsigned parts()const{return loaded?partCount:0;}
    ModelResources resources()const{return {meshCount,materialCount,imageCount};}
    std::size_t metadataBytes()const{return metadataBytesForParts(partCount);}
    static std::size_t metadataBytesForParts(unsigned count);
    Error error()const{return failure;}
private:
    friend class Scene;
    friend class GameDraw;
    template<class H>struct Resource {std::uint32_t id{};H handle{};};
    template<class H>static int index(std::array<Resource<H>,64>& resources,unsigned& count,std::uint32_t id){
        for(unsigned n=0;n<count;++n)if(resources[n].id==id)return int(n);
        if(count==64)return -1;
        resources[count].id=id;return int(count++);
    }
    struct MeshResource {std::uint32_t id{};std::uint8_t highestBone{};};
    struct Part {std::uint16_t mesh{},uniform{};std::uint8_t material{},textureMask{},lookupMask{},fogMask{};std::array<std::uint8_t,3> images{255,255,255};std::array<Sampler,3> samplers{};std::uint8_t highestBone{},parameters{};bool depth{},fogCarrier{};};
    Error preflightPart(unsigned,const DrawUniforms&)const;
    bool submitPart(unsigned,const DrawUniforms&);
    int selectedImage(const Part&,unsigned,const DrawUniforms&)const;
    bool fail(Error e){failure=e;return false;}
    bool cancel(Error);
    Renderer* renderer{};
    MeshBankHandle meshBank{};
    Storage<MeshResource> meshes{};
    std::array<Resource<MaterialHandle>,64> materials{};
    std::array<Resource<TextureHandle>,64> images{};
    Storage<Part> sequence{};
    std::array<std::uint8_t,64> imageLevels{};
    std::array<bool,64> framebufferImages{};
    unsigned partCount{},meshCount{},materialCount{},imageCount{};
    bool loaded{};Error failure=Error::none;std::uint64_t revision{};
};
}
