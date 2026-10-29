#pragma once
#include "model.hpp"
#include "game_source.hpp"
namespace dusk::pica {
// Fully owned native metadata. Contains enqueue order, not a scene sort or
// original hierarchy. File storage may be released after open. Source-derived
// plans require external native producers before normal binding access.
class ModelPlan {
public:
    static PackError open(std::span<const std::byte>,ModelPlan&);
    unsigned size()const{return count;}
    unsigned uniformCount()const{return uniformsCount;}
    std::uint32_t requiredInputs()const{return requirements;}
    std::span<const PartBinding> bindings(std::uint32_t providedInputs)const{
        return count&&!(providedInputs&~63u)&&(providedInputs&requirements)==requirements?std::span<const PartBinding>(parts.data(),count):std::span<const PartBinding>{};
    }
    // Inspection/resource preparation does not claim all producer requirements.
    bool entry(unsigned,PartBinding&,unsigned& pass)const;
    const UniformBinding* uniform(unsigned index)const{return index<uniformsCount?&uniforms[index]:nullptr;}
private:
    std::array<PartBinding,64> parts{};
    std::array<UniformBinding,64> uniforms{};
    std::array<std::uint8_t,64> passes{};
    unsigned count{},uniformsCount{};std::uint32_t requirements{};
};
}
