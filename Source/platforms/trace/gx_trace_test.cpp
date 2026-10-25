#include "gx/trace.hpp"
#include "gx/gx.hpp"
#include <aurora/gx_trace.h>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    setenv("DUSK_GX_TRACE_PATH", argv[1], 1);
    using namespace aurora::gx;
    trace::initialize();
    trace::begin_frame();
    dusk_gx_trace_scene("SYNTHETIC", 0);
    dusk_gx_trace_cpu(0, 1.25);
    dusk_gx_trace_cpu(1, 2.5);
    g_gxState.numTevStages = 1;
    trace::draw(GX_TRIANGLES, GX_VTXFMT0, 3);
    trace::draw(GX_QUADS, GX_VTXFMT0, 4);
    trace::draw(GX_TRIANGLESTRIP, GX_VTXFMT0, 5);
    trace::draw(GX_TRIANGLEFAN, GX_VTXFMT0, 4);
    trace::draw(GX_TRIANGLES, GX_VTXFMT0, 4, 6);
    trace::draw(GX_TRIANGLESTRIP, GX_VTXFMT0, 0);
    // Inactive stages are normalized away by Aurora's program builder.
    g_gxState.tevStages[10].kcSel = GX_TEV_KCSEL_K0;
    trace::draw(GX_TRIANGLES, GX_VTXFMT0, 3);
    g_gxState.depthUpdate = false;
    trace::draw(GX_TRIANGLES, GX_VTXFMT0, 3);
    g_gxState.texCopyDstWidth = 64;
    g_gxState.texCopyDstHeight = 32;
    trace::efb_copy();
    trace::target_switch();
    trace::end_frame();
    trace::shutdown();
    return 0;
}
