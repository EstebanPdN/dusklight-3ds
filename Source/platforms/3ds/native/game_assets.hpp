#pragma once
#include "asset_pack.hpp"
#include "game_draw.hpp"
namespace dusk::pica {
enum class GameAssetError {none,protocol,owner,identity,missing,io,allocation,pack,inputs,budget,backend,textureContext};
// Bounded main-render-thread resource factory. No original-file decoding and no
// GPU work at archive/worker load time. Paths use only a captured SHA256 key.
// All borrowed producer/user/instance storage must outlive its binding.
class GameAssets {
public:
    GameAssets()=default;GameAssets(const GameAssets&)=delete;GameAssets& operator=(const GameAssets&)=delete;
    bool initialize(Renderer&,GameDraw&,const char* directory);
    bool bind(const void* instance,const SourceIdentity&,GameSourceProducer,void* user,
              unsigned availableInputs,const FogParameters* stableEnvironment=nullptr);
    bool forget(const void* instance);
    bool empty()const;
    GameAssetError error()const{return failure;}
    unsigned residentAssets()const{return resident;}
    std::size_t metadataBytes()const;
private:
    struct Asset {SourceIdentity source{};Model model;ModelPlan plan;std::array<unsigned,64> shapes{};FogParameters environment{};bool hasEnvironment{};unsigned references{};};
    struct Instance {const void* identity{};Asset* asset{};GameSourceProducer producer{};void* user{};unsigned inputs{};GameAssets* owner{};};
    static bool produce(const void*,unsigned,DrawUniforms&,void*) noexcept;
    bool fail(GameAssetError e){failure=e;return false;}
    Renderer* renderer{};GameDraw* draw{};std::array<char,192> directory{};
    std::array<Asset*,16> assets{};std::array<Instance,64> instances{};
    unsigned resident{};bool busy{},invalidated{};GameAssetError failure=GameAssetError::none;
};
}
