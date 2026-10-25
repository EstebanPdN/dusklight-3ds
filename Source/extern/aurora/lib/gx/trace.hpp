#pragma once
#if DUSK_GX_TRACE
#include <dolphin/gx.h>
namespace aurora::gx::trace {
void initialize();
void shutdown();
void begin_frame();
void end_frame();
void draw(GXPrimitive primitive, GXVtxFmt format, u32 vertices, u32 indices = 0);
void efb_copy();
void target_switch();
}
#endif
