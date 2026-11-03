#pragma once
#include "game_source.hpp"
#include "camera.hpp"
// Game-facing boundary: no SDK/source-platform graphics types or command data.
namespace dusk::pica::game {
bool initialize(void* nativeTarget); // After Citro3D init; stable native target.
// Supplies the current borrowed capture only on the render owner, after all
// producer callbacks and immediately before the consuming native draw.
bool setFramebufferProvider(bool(*provider)(void*&,unsigned&,unsigned&));
bool beginFrame();
bool endFrame();
bool frameActive();
bool target(void* nativeTarget);
// Explicit high-level camera publication. Only the retained main display target
// is admitted yet. Projection is authored clip [-W,0], not a command/register.
bool publishCamera(void* nativeTarget,const Matrix&,const CameraRegion&);
bool publishCamera(void* nativeTarget,const Matrix&,const CameraRegion&,const CameraScissor&);
// Narrowly scoped mirror model overrides. Material/shape identities must match
// every producer binding; no material templates or later draws are modified.
bool beginMirrorDraw(const void* model,unsigned shape,unsigned material,
                     const std::array<std::uint8_t,4>& ambient);
bool applyMirrorDraw(const void* model,const UniformBinding&,DrawUniforms&);
bool endMirrorDraw();
void invalidateCamera(); // Unconverted pass/state changes invalidate, never translate.
bool applyCamera(DrawUniforms&, unsigned fogCenter=0); // Copy camera and declared fog-range mapping; no SDK calls.
bool clear(unsigned char r,unsigned char g,unsigned char b,unsigned char a);
bool drawShape(const void* gameModel,unsigned shape);
// Source-identified automatic factory; missing producer inputs refuse before
// native GPU allocation. No source command decoding or legacy fallback.
bool drawSourceShape(const void* gameModel,unsigned shape,const SourceIdentity&,
                     GameSourceProducer,void* user,unsigned availableInputs);
void forget(const void* gameModel); // Game model destruction; idempotent.
const char* failure();
unsigned submittedParts();
bool releaseAfterContextDestroyed();
}
