#include "j2d_picture_probe.hpp"
#include "runtime/gx_state.hpp"
#include <JSystem/J2DGraph/J2DPicture.h>
#include <JSystem/J2DGraph/J2DOrthoGraph.h>
#include <array>

namespace dusk::native {
namespace {
std::array<J2DPicture*, 3> pictures{};
std::array<gx::TextureHandle, 3> textures{};
bool ready;
const char* status = "Original J2DPicture pending";
}
bool initializeJ2dPictureProbe(ResourceSpan nintendo, ResourceSpan warning, ResourceSpan prompt) {
    if (ready) return true;
    const ResourceSpan resources[]{nintendo, warning, prompt};
    for (unsigned i = 0; i < 3; ++i) {
        if (!gx::registerTexture({resources[i].data, resources[i].size}, textures[i])) {
            status = "J2DPicture texture admission failed"; destroyJ2dPictureProbe(); return false;
        }
        pictures[i] = new J2DPicture;
        if (!pictures[i]) { status = "J2DPicture allocation failed"; destroyJ2dPictureProbe(); return false; }
        pictures[i]->initiate(reinterpret_cast<const ResTIMG*>(resources[i].data), nullptr);
        if (!pictures[i]->getTexture(0)) {
            status = "J2DPicture original initiate failed"; destroyJ2dPictureProbe(); return false;
        }
    }
    pictures[0]->setWhite(JUtility::TColor(0xc80000ff));
    ready = true;
    status = "Original J2DPicture resources ready";
    return true;
}
void drawJ2dPictureProbe(std::uint64_t frame) {
    if (!ready) return;
    // Explicit diagnostic alternation, not an emulation of the game's boot
    // scene. Each image is drawn by the original J2DPicture implementation.
    if ((frame / 300) % 2) {
        J2DOrthoGraph graph(0, 0, 400, 240, -1, 1);
        graph.setPort();
        pictures[0]->draw(12, 68, 376, 104, false, false, false);
        status = "Original J2DPicture IA8 + red tint";
    } else {
        // Keep the 608x448 resource aspect in the 400x240 native display.
        J2DOrthoGraph graph(37, 0, 326, 240, -1, 1);
        graph.setOrtho(0, 0, 608, 448, -1, 1);
        graph.setPort();
        pictures[1]->draw(0, 0, 608, 448, false, false, false);
        const unsigned phase = frame % 60;
        const unsigned alpha = phase < 30 ? phase * 255 / 29 : (59 - phase) * 255 / 29;
        pictures[2]->setAlpha(alpha);
        pictures[2]->draw(0, 359, 608, 48, false, false, false);
        status = "Original J2DPicture I4 + prompt fade";
    }
}
const char* j2dPictureProbeStatus() { return status; }
void destroyJ2dPictureProbe() {
    ready = false;
    for (unsigned i = 0; i < 3; ++i) {
        delete pictures[i]; pictures[i] = nullptr;
        if (textures[i]) gx::unregisterTexture(textures[i]);
        textures[i] = 0;
    }
}
}
