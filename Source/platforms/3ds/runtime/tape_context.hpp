#pragma once
// What the command tapes (hud_tape_ctru.cpp, screen_tape_ctru.cpp) need from
// Citro3D 1.7.1 internals (source/internal.h): the head of its context up to
// the state flags, and the function its own restore path uses to mark every
// uniform changed. The layout is checked at the first use (tapeContextUsable);
// a library that does not match leaves the tapes off.
#include <3ds.h>
#include <citro3d.h>
extern "C" {
struct DuskC3DContextHead {
    gxCmdQueue_s gxQueue;
    u32* cmdBuf;
    size_t cmdBufSize;
    float cmdBufUsage;
    u32 flags;
};
extern DuskC3DContextHead __C3D_Context;
void C3Di_DirtyUniforms(GPU_SHADER_TYPE type);
void OSReport(const char*,...);
}
namespace dusk::native {
// Draws that are not J3D shapes go to the second-core worker's queue right now
// (gx_command_draw.cpp duskNativeDeferRuns): a recording made now would miss
// their command words, so none is started.
bool nativeRunsDeferred();
// Alpha 15: a retained screen recorded while the painter's runs go to the
// queue draws its own runs on the game thread (gx_command_draw.cpp); calls nest.
void suspendNativeRuns(bool on);
// C3DiF_* (internal.h): Active; then every group the APT restore hook marks:
// AttrInfo BufInfo Effect FrameBuf Viewport Scissor Program TexEnvBuf LightEnv
// VshCode GshCode FogLut TexAll TexEnvAll.
inline constexpr u32 tapeContextActive=BIT(0);
inline constexpr u32 tapeContextEverything=BIT(2)|BIT(3)|BIT(4)|BIT(5)|BIT(6)|BIT(7)|BIT(8)|BIT(9)|BIT(10)|BIT(11)|BIT(12)|BIT(17)|
    (7u<<23)|(0x3fu<<26);
// The context symbol really is Citro3D's: its queue is the one bound to GSP,
// its command buffer is the frame's and it is active.
inline bool tapeContextUsable(){
    static bool checked,okay;
    if(checked)return okay;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    const auto& c=__C3D_Context;
    okay=(c.flags&tapeContextActive)&&c.cmdBuf&&c.cmdBufSize>=8192&&buffer&&
        buffer>=c.cmdBuf&&buffer<c.cmdBuf+c.cmdBufSize&&c.gxQueue.entries&&c.gxQueue.maxEntries;
    checked=true;
    OSReport("Native command tapes: Citro3D context %s (flags=%08lx, command buffer %lu words)\n",okay?"recognised":"not recognised, tapes off",
        static_cast<unsigned long>(c.flags),static_cast<unsigned long>(c.cmdBufSize));
    return okay;
}
// Everything Citro3D tracks counts as changed: the next draw sends its whole
// record (what its APT restore hook does after the GPU state was lost).
inline void tapeMarkContextChanged(){
    __C3D_Context.flags|=tapeContextEverything;
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    C3Di_DirtyUniforms(GPU_GEOMETRY_SHADER);
}
}
