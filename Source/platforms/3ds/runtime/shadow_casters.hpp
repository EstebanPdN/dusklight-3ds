#pragma once
#include <array>
#include <span>
namespace dusk::native {
// Native model shadows. A shadow caster is not drawn a second time through GX:
// while the scene draws a tagged model, each of its PICA draws (vertex buffer
// already on the GPU, matrix palette already built) is remembered. The caster
// pass replays those draws white into the tile of the shadow image, replacing
// only the projection rows of their uniform block by camera space -> tile.
struct ShadowCasterView {
    bool used{};
    // Camera view space -> tile-local (s,t) in [0,1]; GX orientation, t down.
    std::array<std::array<float,4>,2> st{};
    unsigned x{},y{},size{}; // tile in image pixels
};
// Start remembering (once per frame, before the scene draws its casters).
void armGxShadowCasters();
// Render target is the resident texture of the copy image `imageIdentity`
// (width x height, RGBA8). views[slot-1] is the caster selected with slot.
// False: nothing usable was drawn (the image keeps its previous content).
bool drawGxShadowCasters(const void* imageIdentity,unsigned width,unsigned height,std::span<const ShadowCasterView> views);
void gxShadowCasterStatistics(unsigned& remembered,unsigned& dropped,unsigned& passes);
}
// Selects the caster the following rigid draws belong to (0 = none).
extern "C" void duskNativeShadowCaster(unsigned slot);
