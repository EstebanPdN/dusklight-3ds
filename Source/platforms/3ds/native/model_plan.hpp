#pragma once
#include "model.hpp"
#include "game_source.hpp"
#include <utility>
namespace dusk::pica {
// Fully owned native metadata. Contains enqueue order, not a scene sort or
// original hierarchy. File storage may be released after open. Source-derived
// plans require external native producers before normal binding access.
class ModelPlan {
public:
    ModelPlan()=default;
    ModelPlan(const ModelPlan&)=delete;ModelPlan& operator=(const ModelPlan&)=delete;
    ModelPlan(ModelPlan&& other) noexcept { *this=std::move(other); }
    ModelPlan& operator=(ModelPlan&& other) noexcept {
        if(this!=&other){
            parts=std::move(other.parts);uniforms=std::move(other.uniforms);passes=std::move(other.passes);
            count=std::exchange(other.count,0);uniformsCount=std::exchange(other.uniformsCount,0);requirements=std::exchange(other.requirements,0);
        }
        return *this;
    }
    static PackError open(std::span<const std::byte>,ModelPlan&);
    unsigned size()const{return count;}
    unsigned uniformCount()const{return uniformsCount;}
    std::uint32_t requiredInputs()const{return requirements;}
    std::size_t metadataBytes()const{return std::size_t(count)*(sizeof(PartBinding)+sizeof(std::uint8_t))+std::size_t(uniformsCount)*sizeof(UniformBinding);}
    std::span<const PartBinding> bindings(std::uint32_t providedInputs)const{
        return count&&!(providedInputs&~63u)&&(providedInputs&requirements)==requirements?std::span<const PartBinding>(parts.get(),count):std::span<const PartBinding>{};
    }
    // Inspection/resource preparation does not claim all producer requirements.
    bool entry(unsigned,PartBinding&,unsigned& pass)const;
    const UniformBinding* uniform(unsigned index)const{return index<uniformsCount?&uniforms[index]:nullptr;}
private:
    Storage<PartBinding> parts{};
    Storage<UniformBinding> uniforms{};
    Storage<std::uint8_t> passes{};
    unsigned count{},uniformsCount{};std::uint32_t requirements{};
};
}
