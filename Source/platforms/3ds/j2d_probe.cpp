#include "gx_pica.hpp"
#include <JSystem/J2DGraph/J2DOrthoGraph.h>
#include <JSystem/JUtility/JUTResource.h>
namespace dusk::native {
void drawJ2dPrimitiveProbe() {
    // These draw calls execute the unchanged original J2D implementation.
    // Colored test geometry is deliberately labeled a probe, not game content.
    J2DOrthoGraph graph(0, 0, 400, 240, -1, 1);
    graph.setPort();
    graph.setColor(JUtility::TColor(0x335577ff));
    graph.fillBox(JGeometry::TBox2<f32>(8, 8, 392, 44));
    graph.setColor(JUtility::TColor(0x45b97cff));
    graph.fillBox(JGeometry::TBox2<f32>(16, 16, 184, 36));
    graph.setColor(JUtility::TColor(0xe2a24d80));
    graph.fillBox(JGeometry::TBox2<f32>(152, 16, 304, 36));
    graph.scissor(JGeometry::TBox2<f32>(316, 16, 376, 36));
    graph.setScissor();
    graph.setColor(JUtility::TColor(0xb67fcfff));
    graph.fillBox(JGeometry::TBox2<f32>(308, 8, 384, 44));
}
}
