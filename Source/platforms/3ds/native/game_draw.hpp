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
private:
    friend class GameAssets;
    struct Binding {const void* instance{};Model* model{};GameUniformProducer producer{};void* user{};std::uint64_t revision{};std::array<std::uint16_t,64> shapes{};};
    bool fail(Error e){failure=e;return false;}
    Renderer* renderer{};
    std::array<Binding,64> bindings{};
    std::array<DrawUniforms,64> uniforms{}; // One shared bounded draw workspace.
    bool submitting{},invalidated{},externalOpen{};Error failure=Error::none;
};
namespace game {GameDraw& bindings();Renderer& renderer();}
}
