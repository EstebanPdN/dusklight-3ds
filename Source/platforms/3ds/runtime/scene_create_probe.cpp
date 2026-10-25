#include "scene_create_probe.hpp"
#if defined(DUSK_TARGET_3DS) && DUSK_SCENE_CREATE_PROBE
#include <cstring>
extern "C" void OSReport(const char*, ...);
namespace dusk::native {
namespace {
struct Row {
    SceneProbeIdentity who;
    std::uint32_t first, since, calls, parentActor, detailSince;
    int phase, ret, children, lifecycle, detailA, detailB, detailC;
    SceneProbeKind detailKind;
    bool active, hasDetail;
};
struct Event {
    SceneProbeIdentity who;
    std::uint32_t frame, age;
    SceneProbeKind kind;
    int a, b, c;
};
struct State {
    Row rows[sceneProbeSlots];
    Event events[sceneProbeEvents];
    std::uint32_t frame, previous, eventCount, droppedEvents, droppedStarts, missingDetails;
    std::uint32_t started, freed, reports, protocolErrors;
    bool initialized, enabled, inFrame;
};
State state{};
// Only the initializing game thread may access state. Other threads short-circuit on their own TLS.
thread_local bool owner = false;
static_assert(sizeof(State) <= 64 * 1024, "Diagnostic BSS bound exceeded");
void increment(std::uint32_t& n) { if (n != UINT32_MAX) ++n; }
Row* find(std::uint32_t id) {
    for (auto& row : state.rows) if (row.active && row.who.id == id) return &row;
    return nullptr;
}
void event(const SceneProbeIdentity& who, SceneProbeKind kind, int a, int b, int c, unsigned age) {
    if (state.eventCount == sceneProbeEvents) { increment(state.droppedEvents); return; }
    state.events[state.eventCount++] = {who, state.frame, age, kind, a, b, c};
}
void rowEvent(const Row& row, SceneProbeKind kind, int a, int b, int c) {
    event(row.who, kind, a, b, c, state.frame - row.first);
}
void printIdentity(const char* tag, const SceneProbeIdentity& w, unsigned frame, unsigned age,
                   unsigned kind, int a, int b, int c, unsigned sequence) {
    // Both lines fit the existing native OSReport 256-byte buffer even at scalar extrema.
    OSReport("SCP1 %s seq=%u f=%u age=%u kind=%u id=%u req=%d name=%d prof=%d init=%d a=%d b=%d c=%d\n",
        tag, sequence, frame, age, kind, w.id, w.requested, w.name, w.profile, w.init, a, b, c);
    OSReport("SCP1 OWNER seq=%u layer=%u parent=%u pprof=%d pparam=%08x proom=%d param=%08x room=%d\n",
        sequence, w.layer, w.parent, w.parentProfile, w.parentParam, w.parentRoom, w.param, w.room);
}
void flush(bool final, bool completeWindow) {
    ++state.reports;
    unsigned active = 0;
    for (const auto& r : state.rows) if (r.active) ++active;
    OSReport("SCP1 BEGIN report=%u f=%u scope=standard_requests slots=%u events=%u bss=%u active=%u\n",
        state.reports, state.frame, sceneProbeSlots, sceneProbeEvents, unsigned(sizeof(State)), active);
    OSReport("SCP1 STATS started=%u freed=%u drop_start=%u drop_event=%u missing_detail=%u protocol_error=%u\n",
        state.started, state.freed, state.droppedStarts, state.droppedEvents, state.missingDetails, state.protocolErrors);
    for (unsigned i = 0; i < state.eventCount; ++i) {
        const auto& e = state.events[i];
        printIdentity("EVENT", e.who, e.frame, e.age, unsigned(e.kind), e.a, e.b, e.c, i);
    }
    unsigned sequence = state.eventCount;
    for (const auto& r : state.rows) if (r.active) {
        printIdentity("PENDING", r.who, state.frame, state.frame-r.first,
            unsigned(SceneProbeKind::phase), r.phase, r.ret, r.children, sequence++);
        OSReport("SCP1 WAIT id=%u phase_age=%u calls=%u lifecycle=%d actor_parent=%u has_detail=%u detail_age=%u kind=%u a=%d b=%d c=%d\n",
            r.who.id, state.frame-r.since, r.calls, r.lifecycle, r.parentActor,
            unsigned(r.hasDetail), r.hasDetail ? state.frame-r.detailSince : 0, unsigned(r.detailKind), r.detailA, r.detailB, r.detailC);
    }
    OSReport("SCP1 END report=%u f=%u final=%u window_complete=%u records_complete=%u event_count=%u pending=%u\n",
        state.reports, state.frame, unsigned(final), unsigned(completeWindow),
        unsigned(!state.droppedStarts && !state.droppedEvents && !state.missingDetails && !state.protocolErrors),
        state.eventCount, active);
    state.eventCount = 0;
}
}
bool sceneProbeEnabled() { return owner && state.enabled; }
bool parseSceneProbeConfig(const char* text, std::size_t bytes) {
    static constexpr char token[] = "scene-create-probe=1\n";
    return text && bytes == sizeof(token)-1 && std::memcmp(text, token, sizeof(token)-1) == 0;
}
void initializeSceneProbe(bool enabled) {
    if (state.initialized) return;
    state.initialized = true; owner = true; state.enabled = enabled;
    if (enabled) OSReport("SCP1 CONFIG enabled=1 end=480 boundaries=60,180,300,480 scope=standard_requests age=game_frames ready_is_not_gameplay=1\n");
}
void sceneProbeBegin(std::uint32_t frame) {
    if (!sceneProbeEnabled()) return;
    if (state.inFrame || frame != state.previous+1 || frame > sceneProbeEndFrame) {
        increment(state.protocolErrors); flush(true, false); state.enabled = false; return;
    }
    state.frame = frame; state.inFrame = true;
}
void sceneProbeEnd() {
    if (!sceneProbeEnabled()) return;
    if (!state.inFrame) { increment(state.protocolErrors); flush(true, false); state.enabled = false; return; }
    state.previous = state.frame; state.inFrame = false;
    if (state.frame == 60 || state.frame == 180 || state.frame == 300 || state.frame == 480)
        flush(state.frame == 480, state.frame == 480);
    if (state.frame == sceneProbeEndFrame) state.enabled = false;
}
void sceneProbeClose() {
    if (!sceneProbeEnabled()) return;
    flush(true, false); state.enabled = false;
}
void sceneProbeStart(const SceneProbeIdentity& who) {
    if (!sceneProbeEnabled()) return;
    increment(state.started);
    if (find(who.id)) { increment(state.protocolErrors); return; }
    for (auto& row : state.rows) if (!row.active) {
        row = {}; row.who = who; row.active = true;
        row.first = row.since = state.frame; row.phase = row.ret = row.children = -1;
        row.parentActor = ~0u;
        rowEvent(row, SceneProbeKind::start, 0, 0, 0); return;
    }
    increment(state.droppedStarts);
}
void sceneProbeRefresh(std::uint32_t id, const SceneProbeIdentity& who) {
    if (!sceneProbeEnabled()) return;
    if (auto* r = find(id)) {
        const int requested = r->who.requested, room = r->who.room;
        r->who = who; r->who.requested = requested;
        if (who.room == -1) r->who.room = room;
    }
}
void sceneProbePhase(std::uint32_t id, const SceneProbeIdentity& who, int phase, int ret) {
    if (!sceneProbeEnabled()) return;
    auto* r = find(id); if (!r) return; // start overflow is already explicit.
    sceneProbeRefresh(id, who); increment(r->calls);
    if (r->phase != phase || r->ret != ret) {
        // Phase age means time since this observed phase/return pair began, not total creation age.
        r->since = state.frame; r->phase = phase; r->ret = ret;
        rowEvent(*r, SceneProbeKind::phase, phase, ret, r->children);
    }
}
void sceneProbeChildren(std::uint32_t id, int count) {
    if (!sceneProbeEnabled()) return;
    if (auto* r = find(id)) r->children = count;
}
void sceneProbeActor(std::uint32_t id, int room, std::uint32_t parentActor) {
    if (!sceneProbeEnabled()) return;
    if (auto* r = find(id)) { r->who.room = room; r->parentActor = parentActor; }
    else increment(state.missingDetails);
}
void sceneProbeLifecycle(std::uint32_t id, int outcome) {
    if (!sceneProbeEnabled()) return;
    if (auto* r = find(id)) {
        // Preserve an execute-queue rejection when cancellation follows it.
        if (!(outcome == 3 && r->lifecycle == 2)) r->lifecycle = outcome;
        rowEvent(*r, SceneProbeKind::lifecycle, outcome, r->phase, r->ret);
    }
}
void sceneProbeFreed(std::uint32_t id) {
    if (!sceneProbeEnabled()) return;
    if (auto* r = find(id)) {
        rowEvent(*r, SceneProbeKind::freed, r->lifecycle, r->phase, r->ret);
        r->active = false; increment(state.freed);
    }
}
void sceneProbeDetail(std::uint32_t id, SceneProbeKind kind, int a, int b, int c) {
    if (!sceneProbeEnabled()) return;
    auto* r = find(id); if (!r) { increment(state.missingDetails); return; }
    // Callback invocations must never be deduplicated: they are the actual retry outcomes.
    const bool changed = !r->hasDetail || r->detailKind != kind || r->detailA != a || r->detailB != b || r->detailC != c;
    if (changed) r->detailSince = state.frame;
    if (kind == SceneProbeKind::heapCallback || kind == SceneProbeKind::heapFinal || changed)
        rowEvent(*r, kind, a, b, c);
    r->hasDetail = true; r->detailKind = kind; r->detailA = a; r->detailB = b; r->detailC = c;
}
void sceneProbeLayer(int room, int effectiveLayer) {
    if (!sceneProbeEnabled()) return;
    // A scalar event from the exact existing layer selection, no additional selector call.
    SceneProbeIdentity who{~0u, ~0u, ~0u, 0, 0, -1, -1, -1, -1, room, -1, -1};
    event(who, SceneProbeKind::layer, room, effectiveLayer, 0, 0);
}
bool sceneProbeGate(std::uint32_t id, int reason, bool blocked, int mode, int point) {
    if (sceneProbeEnabled() && blocked) sceneProbeDetail(id, SceneProbeKind::link, reason, mode, point);
    return blocked;
}
}
#endif
