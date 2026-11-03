#pragma once
#include "renderer.hpp"

namespace dusk::pica {
// One profile for the SDK allocation and renderer admission. The command
// arena is reclaimed after queue completion, independently of the frame cap.
inline constexpr Limits nativeGameLimits=[] {
    Limits value;
    value.drawsPerFrame=maxFrameDraws;
    value.commandBytes=32768u+(4u<<20);
    value.textureBytes=8u<<20;
    value.uniformBytes=512u<<10; // The real body uses 193 distinct recipes in one source shape.
    return value;
}();
}
