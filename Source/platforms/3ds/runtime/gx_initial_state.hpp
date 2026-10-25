#pragma once
namespace dusk::native::gx {
// Original GXInit's supported state, encoded through the real native command
// API. Resource bindings remain explicit; this does not invent game textures.
void initializeGxDefaultState();
}
