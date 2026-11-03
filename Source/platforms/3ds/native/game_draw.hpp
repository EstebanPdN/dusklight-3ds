#pragma once
#include "model.hpp"
namespace dusk::pica {
// Supplies native parameters for one native model uniform slot. No GPU calls,
// registry changes, reentry, or object destruction during this synchronous call.
using GameUniformProducer=bool(*)(const void* gameInstance,unsigned nativeSlot,DrawUniforms&,void* user) noexcept;
class GameDraw {
public:
    GameDraw()=default;GameDraw(const GameDraw&)=delete;GameDraw& operator=(const GameDraw&)=delete;
    bool initialize(Renderer&);
    // Existing game queue owns SDK FrameBegin/End. Admission must follow its
    // verified prior-work completion, before retiring/reusing native storage.
    bool beginExternalFrame(C3D_RenderTarget*);
    bool endExternalFrame();
    // sourceShapeForEachPart is compiled ownership metadata, not a display list.
    // This mapping is copied; model and producer/user storage remain borrowed.
    bool bind(const void* instance,Model&,std::span<const unsigned> sourceShapeForEachPart,GameUniformProducer,void* user);
    bool forget(const void* instance);
    Submission draw(const void* instance,unsigned shape);
    bool empty()const;
    bool frameActive()const{return externalOpen;}
    bool onOwnerThread();
    void invalidateInputs(){if(submitting)invalidated=true;} // Owner only; poisons reentrant producers.
    Error error()const{return failure;}
    std::size_t metadataBytes()const{return bindingBytes;}
    std::size_t uniformBytes()const{return std::size_t(uniformCapacity)*sizeof(DrawUniforms);}
private:
    friend class GameAssets;
    struct Binding {const void* instance{};Model* model{};GameUniformProducer producer{};void* user{};std::uint64_t revision{};Storage<std::uint16_t> shapes{};unsigned count{};};
    bool fail(Error e){failure=e;return false;}
    Renderer* renderer{};
    std::array<Binding,64> bindings{};
    Storage<DrawUniforms> uniforms{}; // Dense uniforms for the largest bound source shape.
    unsigned uniformCapacity{};std::size_t bindingBytes{};
    static constexpr std::size_t maxBindingBytes=64u*maxModelParts*sizeof(std::uint16_t);
    std::array<std::uint16_t,maxModelParts> selection{},denseSlots{}; // Shared, never allocated during draw.
    bool submitting{},invalidated{},externalOpen{};Error failure=Error::none;
};
namespace game {GameDraw& bindings();Renderer& renderer();}
}
