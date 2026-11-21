#pragma once
// Levels of detail of the loaded models (runtime/model_lod_store.hpp holds
// the file). Game thread: the models' levels are looked up when a model is
// first entered and kept until the stage changes; the choice of a level per
// model and shape is in include/d/d_native_lod.h. The thread that draws (the
// second-core worker, or the game thread after a sync) swaps the index list
// of a recorded batch for the level's (gx_command_draw.cpp).
//
// Switches (runtime/perf_features.hpp): `lod` draws far shapes with their
// levels, `shadow` replays every shadow caster with the coarsest level that
// leaves its shadow the same at the shadow image's resolution. Without
// zelda.lod on the SD both do nothing.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

class J3DModelData;

namespace dusk::native::model_lod {
inline constexpr unsigned levels = 3;
// One draw (batch) of a display list.
struct Batch {
    std::uint16_t vertexCount{};
    std::uint32_t fullIndices{}, fullHash{};
    std::array<const std::uint16_t*, levels> source{}; // level indices, CPU memory (0 count: the full list)
    std::array<std::uint32_t, levels> count{};
    // The thread that draws only: the level's copy in GPU-readable memory
    // (the resident index pool, valid while `epoch` is its epoch), and
    // whether the runtime's batch is the one the levels were cut for.
    std::array<const std::uint16_t*, levels> resident{};
    std::array<std::uint32_t, levels> epoch{};
    std::uint8_t checked{}; // 0 not yet, 1 same batch, 2 another batch: never reduced
};
// One display list (a matrix group of a shape).
struct Group {
    const void* list{};
    std::uint32_t listBytes{};
    std::uint16_t batches{};
    Batch* batch{};
};
struct Shape {
    std::uint8_t levels{};                          // 0: drawn whole
    std::array<float, model_lod::levels> error{};   // model units, per level
    std::uint16_t groups{};
    Group* group{};                                 // null for a shape without levels
};
struct Model {
    std::uint16_t shapes{};
    Shape* shape{};
};

// Boot: opens sdmc:/3ds/DuskLight/zelda.lod. Logs what it found.
void open();
bool available();
// Game thread, draw phase: the levels of a loaded model, or null (no file, no
// record, a list that is not the recorded one). Read from the SD the first
// time a model data is seen (by its vertex-data generation, never reused).
const Model* find(J3DModelData* data);
// Game thread, at the start of a picture (the worker has drawn everything
// queued): a new stage retires every model's levels (the packets entered in
// the last draw phase still point at them while they are painted), and
// retired levels of the boundary before are freed.
void frameBegin(bool stageChanged);
// Periodic log line (every 300 pictures) with what the levels did.
void report();

// What the level choice did, for the log (game thread).
struct Counters {
    std::uint32_t models{}, missing{}, mismatched{}, reads{}, readFailures{}, full{};
    std::uint64_t readBytes{}, heldBytes{}, readMicroseconds{};
    std::uint32_t shapesSeen{}, shapesReduced{}, levelChoices[levels + 1]{};
    std::uint32_t shadowShapes{}, shadowReduced{};
};
Counters& counters();
// Drawing side: batches that had a level, refused (another batch, no pool
// room); draws that used a level and the indices saved, against what the
// full list submits after the off-screen cull (scene) or replays (casters).
struct DrawCounters {
    std::uint32_t batches{}, reduced{}, refusedBatch{}, refusedPool{}, shadowReduced{};
    std::uint64_t indicesSaved{}, shadowIndicesSaved{};
};
// Written by the thread that draws, taken by the game thread's report.
namespace detail {
inline std::atomic<std::uint32_t> drawBatches{}, drawReduced{}, drawRefusedBatch{}, drawRefusedPool{}, drawShadowReduced{};
inline std::atomic<std::uint64_t> drawSaved{}, drawShadowSaved{};
}
inline void countDraw(const DrawCounters& add) {
    using namespace detail;
    drawBatches.fetch_add(add.batches, std::memory_order_relaxed);
    drawReduced.fetch_add(add.reduced, std::memory_order_relaxed);
    drawRefusedBatch.fetch_add(add.refusedBatch, std::memory_order_relaxed);
    drawRefusedPool.fetch_add(add.refusedPool, std::memory_order_relaxed);
    drawShadowReduced.fetch_add(add.shadowReduced, std::memory_order_relaxed);
    drawSaved.fetch_add(add.indicesSaved, std::memory_order_relaxed);
    drawShadowSaved.fetch_add(add.shadowIndicesSaved, std::memory_order_relaxed);
}
inline DrawCounters takeDrawCounters() {
    using namespace detail;
    DrawCounters d;
    d.batches = drawBatches.exchange(0, std::memory_order_relaxed);
    d.reduced = drawReduced.exchange(0, std::memory_order_relaxed);
    d.refusedBatch = drawRefusedBatch.exchange(0, std::memory_order_relaxed);
    d.refusedPool = drawRefusedPool.exchange(0, std::memory_order_relaxed);
    d.shadowReduced = drawShadowReduced.exchange(0, std::memory_order_relaxed);
    d.indicesSaved = drawSaved.exchange(0, std::memory_order_relaxed);
    d.shadowIndicesSaved = drawShadowSaved.exchange(0, std::memory_order_relaxed);
    return d;
}
}
// J3D side (J3DShape.cpp) before a matrix group is drawn: the group's levels
// and the levels chosen for the scene and the shadow caster (0 = whole);
// (null, 0) after it. The queued shape carries the choice to the worker.
extern "C" void duskNativeLodSelect(const void* group, unsigned sceneLevel, unsigned shadowLevel);
