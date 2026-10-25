#pragma once
#include "runtime/texture_ia8.hpp"

namespace dusk::native {
// Isolated startup-resource proof, not a J2D/GX backend or game title screen.
struct PreviewImage { const std::uint8_t* rgba = nullptr; TextureExtent extent{}; };
bool initializePreview(const PreviewImage& image);
void drawPreview();
// Call only after the GPU has finished all frames (C3D_Fini at shell shutdown).
void destroyPreview();
}
