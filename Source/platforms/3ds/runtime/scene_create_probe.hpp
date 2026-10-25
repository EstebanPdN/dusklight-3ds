#ifndef DUSK_SCENE_CREATE_PROBE_HPP
#define DUSK_SCENE_CREATE_PROBE_HPP
#include <cstddef>
#include <cstdint>
namespace dusk::native {
// All entry points belong exclusively to the original game thread. No UI/worker access.
constexpr unsigned sceneProbeSlots = 128, sceneProbeEvents = 512, sceneProbeEndFrame = 480;
struct SceneProbeIdentity {
    std::uint32_t id, layer, parent, parentParam, param;
    int requested, name, profile, parentProfile, room, parentRoom, init;
};
enum class SceneProbeKind : unsigned {
    start, phase, lifecycle, freed, heapCallback, heapFinal, link, background, room, layer
};
bool sceneProbeEnabled();
// Strict byte parser; embedded NUL, trailing data, alternate whitespace fail closed.
bool parseSceneProbeConfig(const char*, std::size_t);
// Call once on the game thread before fapGm_Create. No rearming in this process.
void initializeSceneProbeFromSd();
void initializeSceneProbe(bool enabled);
// begin(frame+1) before fapGm_Execute; end() AFTER it. Frames must be 1..480 in order.
void sceneProbeBegin(std::uint32_t frame);
void sceneProbeEnd();
// Optional early orderly shutdown, on the SAME game thread; emits incomplete window.
void sceneProbeClose();
void sceneProbeStart(const SceneProbeIdentity&);
void sceneProbeRefresh(std::uint32_t, const SceneProbeIdentity&);
void sceneProbePhase(std::uint32_t, const SceneProbeIdentity&, int phase, int ret);
void sceneProbeChildren(std::uint32_t, int count);
void sceneProbeActor(std::uint32_t, int room, std::uint32_t parentActor);
void sceneProbeLifecycle(std::uint32_t, int outcome);
void sceneProbeFreed(std::uint32_t);
void sceneProbeDetail(std::uint32_t, SceneProbeKind, int a, int b, int c);
void sceneProbeLayer(int room, int effectiveLayer);
bool sceneProbeGate(std::uint32_t, int reason, bool blocked, int startMode, int startPoint);
}
#endif
