#pragma once
#include "contracts.hpp"
#include <citro3d.h>

namespace dusk::pica {
struct Limits {
    std::size_t meshBytes=4<<20,textureBytes=4<<20,transientBytes=256<<10;
    unsigned drawsPerFrame=64;
    // Caller must initialize Citro3D with at least this command-buffer size.
    std::size_t commandBytes=512<<10;
};
struct Statistics {
    std::uint64_t frames{},draws{},triangles{},rigidDraws{},weightedDraws{},uvDraws{},projectiveDraws{};
    unsigned frameDraws{};
    std::size_t meshBytes{},textureBytes{},transientBytes{};
    float previousGpuMilliseconds{},previousCpuMilliseconds{};
};
// Exclusive Citro3D owner, allocated at a stable address for its entire context
// lifetime. No calls from a worker, no interleaved legacy renderer or C3D calls.
// GameDraw is the explicit shared-game-frame adapter: its platform caller owns
// prior-work completion and invalidates the transitional backend state after
// each native model dispatch. Arbitrary interleaved public draws remain forbidden.
// initialize AFTER C3D_Init; releaseAfterContextDestroyed AFTER C3D_Fini.
// Targets are native caller-owned objects, retained through GPU completion.
class Renderer {
public:
    Renderer()=default;
    Renderer(const Renderer&)=delete;
    Renderer& operator=(const Renderer&)=delete;
    bool initialize(const Limits&);
    bool begin(C3D_RenderTarget*); // NONBLOCK: busy returns without recycling.
    bool target(C3D_RenderTarget*); // Full target viewport/scissor, no implicit clear.
    bool clear(std::uint32_t color,std::uint32_t depth,bool colorBuffer=true,bool depthBuffer=true);
    bool scissor(unsigned left,unsigned top,unsigned right,unsigned bottom);
    bool end();
    bool releaseAfterContextDestroyed(); // Does not wait or destroy Citro3D.
    MeshHandle createMesh(std::span<const Vertex>,std::span<const std::uint16_t>);
    MeshHandle createMeshBytes(std::span<const std::byte> nativeVertices,std::span<const std::byte> nativeIndices);
    TextureHandle createTexture(const TextureInfo&,std::span<const std::byte> nativeTiledMipChain);
    MaterialHandle createMaterial(const Material&);
    bool release(MeshHandle);
    bool release(TextureHandle);
    bool release(MaterialHandle);
    bool draw(MeshHandle,MaterialHandle,const std::array<TextureHandle,3>&,const DrawUniforms&);
    bool drawTransient(std::span<const Vertex>,std::span<const std::uint16_t>,MaterialHandle,const std::array<TextureHandle,3>&,const DrawUniforms&);
    Error error()const{return failure;}
    bool frameActive()const{return open;}
    std::uint64_t stateVersion()const{return stateSerial;}
    const Statistics& statistics()const{return stats;}
private:
    friend class Model;
    friend class Scene;
    friend class GameDraw;
    friend class GameAssets;
    struct Mesh {void* data{};GeometryInfo info{};};
    struct Texture {C3D_Tex value{};std::size_t bytes{};};
    struct Prepared {Material state{};std::array<C3D_TexEnv,6> env{};C3D_FogLut fog{};std::uint8_t parameterMask{},parameterStages{};};
    template<class T> struct Slot {T payload{};std::uint32_t generation=1;bool live{},retired{};};
    std::array<Slot<Mesh>,resourceSlots> meshes{};
    std::array<Slot<Texture>,resourceSlots> textures{};
    std::array<Slot<Prepared>,resourceSlots> materials{};
    template<class T> static Slot<T>* lookup(std::array<Slot<T>,resourceSlots>& pool,std::uint32_t h){
        unsigned i=(h&255);if(!i||i>resourceSlots)return nullptr;
        auto& s=pool[i-1];return s.live&&s.generation==(h>>8)?&s:nullptr;
    }
    template<class T> static int vacant(const std::array<Slot<T>,resourceSlots>& pool){
        for(unsigned i=0;i<resourceSlots;++i)
            if(!pool[i].live&&!pool[i].retired&&pool[i].generation<0xffffff)return int(i);
        return -1;
    }
    template<class T> static std::uint32_t token(const Slot<T>& s,unsigned i){return s.generation<<8|(i+1);}
    template<class T> static void recycle(Slot<T>& s){s.payload={};s.retired=false;++s.generation;}
    template<class T> bool retire(std::array<Slot<T>,resourceSlots>& pool,std::uint32_t handle){
        if(!owner())return false;
        auto* s=lookup(pool,handle);if(!s)return fail(Error::invalidHandle);
        s->live=false;s->retired=true;return true;
    }
    bool owner();bool writable();bool fail(Error e){failure=e;return false;}
    void collect();
    void discardShaders();
    MeshHandle uploadMesh(std::span<const std::byte>,std::span<const std::byte>,const GeometryInfo&);
    bool emit(const Vertex*,const std::uint16_t*,const GeometryInfo&,Prepared&,const std::array<TextureHandle,3>&,const DrawUniforms&);
    Limits limits{};Statistics stats{};Error failure=Error::none;
    const void* ownerTls{};
    bool ready{},open{},emitted{},hasDepth{},passEmitted{};
    C3D_RenderTarget* activeTarget{};
    unsigned width{},height{};
    std::uint64_t stateSerial{};
    std::byte* transient{};
    C3D_Tex unusedSampler{}; // Real native white image; never replaces missing consumed textures.
    std::array<C3D_Tex,3> samplerViews{}; // Borrowed per-unit views; never separately delete shared pixels.
    static constexpr unsigned programCount=6;
    std::array<DVLB_s*,programCount> binaries{};
    std::array<shaderProgram_s,programCount> programs{};
    std::array<bool,programCount> programInitialized{};
    std::array<int,programCount> uniformLocations{-1,-1,-1,-1,-1,-1};
};
}
