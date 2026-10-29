#pragma once
#include "pack.hpp"
namespace dusk::pica {
struct MaterialView {
    std::uint32_t id{};
    Material state{};
    std::array<std::uint32_t,3> imageIds{};
    bool requiresFog{};
    bool initialSnapshotOnly{};
};
struct FogParameters {
    bool enabled{},depthFlip{};
    std::uint32_t color{};
    std::array<float,256> values{};
};
// A compiled fragment template cannot silently drop a required environment
// binding. This receives only native parameters, never original fog enums.
bool instantiateMaterial(const MaterialView&,const FogParameters*,Material&,bool allowInitialSnapshot=false);
// Native fragment state only. Texgen/lighting/animation parameter bindings
// belong to native scene data, not to a runtime source-material decoder.
class MaterialPack {
public:
    static PackError open(std::span<const std::byte>,MaterialPack&);
    PackError material(std::uint32_t,MaterialView&)const;
    unsigned size()const{return count;}
private:
    struct Entry {std::uint32_t id{},offset{},size{},checksum{};std::array<std::uint32_t,3> images{};bool requiresFog{},initialSnapshot{};};
    std::array<Entry,64> entries{};
    std::span<const std::byte> bytes{};
    unsigned count{},version{};
};
}
