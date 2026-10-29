#pragma once
#include "model_plan.hpp"
namespace dusk::pica {
// One source-identified, immutable native asset. Bank bytes are borrowed until
// Model::load completes; plan/shape ownership metadata is copied.
class AssetPack {
public:
    static constexpr std::size_t maxBytes=16u<<20;
    static PackError open(std::span<const std::byte>,const SourceIdentity& expected,AssetPack&);
    MeshPack meshes;TexturePack images;MaterialPack materials;ModelPlan plan;
    std::array<unsigned,64> shapes{};
    bool requiresTextureContext{}; // Static source hash alone cannot select external image variants.
};
}
