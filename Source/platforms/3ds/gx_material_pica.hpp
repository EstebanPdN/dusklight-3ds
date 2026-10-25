#pragma once
#include "runtime/gx_pica_material.hpp"
#include "runtime/gx_pica_tev.hpp"
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/texenv.h>
}
namespace dusk::native {
// Draw-local snapshots for repeated base/fog alternation. They own only packed
// register values, never texture pointers or mutable Citro3D state.
struct PreparedPicaMaterial {
    std::array<C3D_TexEnv,6> stages{};
    u32 bufferColor{};
    unsigned bufferUpdateRgb{};
};
PreparedPicaMaterial preparePicaMaterial(const gx::PicaMaterialPlan&);
void applyPreparedPicaMaterial(const PreparedPicaMaterial&);
// Apply a successfully planned material to Citro3D combiner state. Texture/UV
// bindings, raster color, and the rest of draw state are the caller's contract.
// Resets all six stages and programs buffer color/update masks each draw.
void applyPicaMaterial(const gx::PicaMaterialPlan&);
// Legacy J2D draw path uses the same source/operand/function mapping.
void applyPicaTev(const gx::PicaTevPlan&);
}
