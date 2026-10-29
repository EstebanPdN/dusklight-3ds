#pragma once
#include "game_source.hpp"
#include "camera.hpp"
// Game-facing boundary: no SDK/source-platform graphics types or command data.
namespace dusk::pica::game {
bool initialize(void* nativeTarget); // After Citro3D init; stable native target.
bool beginFrame();
bool endFrame();
bool frameActive();
bool target(void* nativeTarget);
// Explicit high-level camera publication. Only the retained main display target
// is admitted yet. Projection is authored clip [-W,0], not a command/register.
bool publishCamera(void* nativeTarget,const Matrix&,const CameraRegion&);
void invalidateCamera(); // Unconverted pass/state changes invalidate, never translate.
bool applyCamera(DrawUniforms&); // Pure parameter copy; no SDK calls.
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
