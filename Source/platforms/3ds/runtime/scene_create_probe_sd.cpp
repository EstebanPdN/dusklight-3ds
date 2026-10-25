#include "scene_create_probe.hpp"
#if defined(DUSK_TARGET_3DS) && DUSK_SCENE_CREATE_PROBE
#include <cstdio>
namespace dusk::native {
void initializeSceneProbeFromSd() {
    static bool read = false;
    if (read) return;
    read = true;
    // Explicit exception: one bounded startup read, never from an ordinary frame hook.
    auto* file = std::fopen("sdmc:/3ds/dusklight/scene-create-probe.txt", "rb");
    if (!file) { initializeSceneProbe(false); return; }
    char token[22]; // Exact token is 21 bytes; one extra byte detects any suffix.
    const auto n = std::fread(token, 1, sizeof(token), file);
    bool okay = !std::ferror(file);
    if (std::fclose(file)) okay = false;
    initializeSceneProbe(okay && parseSceneProbeConfig(token, n));
}
}
#endif
