#pragma once
#include "renderer.hpp"
namespace dusk::pica {
struct Quad {
    // Original order is preserved; the caller supplies native UV orientation.
    std::array<std::array<float,3>,4> corners{};
    std::array<std::array<std::array<float,2>,3>,4> uv{};
    std::array<std::array<std::uint8_t,4>,4> colors{{{255,255,255,255},{255,255,255,255},{255,255,255,255},{255,255,255,255}}};
};
// Owner-thread, ordered native UI/particle geometry builder. No implicit state
// sorting, CPU transforms, allocations, source-platform packets or fallback.
// Each begin has one material/texture/uniform state. End before changing passes.
class QuadBatch {
public:
    static constexpr unsigned capacity=32;
    explicit QuadBatch(Renderer& r):renderer(r){}
    bool begin(MaterialHandle,const std::array<TextureHandle,3>&,const DrawUniforms&);
    bool append(const Quad&);
    bool flush();
    bool end();
    unsigned pending()const{return count;}
    unsigned submissions()const{return emitted;}
    bool failed()const{return poisoned;}
private:
    bool current()const{return active&&renderer.frameActive()&&frame==renderer.statistics().frames&&state==renderer.stateVersion()&&expectedDraws==renderer.statistics().draws;}
    Renderer& renderer;
    std::array<Vertex,capacity*4> vertices{};
    std::array<std::uint16_t,capacity*6> indices{};
    MaterialHandle material{};std::array<TextureHandle,3> textures{};DrawUniforms uniforms{};
    std::uint64_t frame{},state{},expectedDraws{};unsigned count{},emitted{};bool active{},poisoned{};
};
}
