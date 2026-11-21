#include "model_lod.hpp"
#include "model_lod_store.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DShapeDraw.h"
#include "JSystem/J3DGraphBase/J3DVertex.h"
#include <memory>
#include <cstdlib>
#include <cstring>
#include <new>
#include <3ds.h>

extern "C" void OSReport(const char*, ...);

namespace dusk::native::model_lod {
namespace {
// What one model's levels cost in memory is one block: the runtime structs
// followed by the record as read (the level indices point into it).
struct Held {
    Held* next;
    std::size_t bytes;
};
ModelLodStore store;
bool opened = false;
Held* current = nullptr;   // levels of the models of this stage
Held* retired = nullptr;   // levels of the stage before, freed at the next boundary
Counters count;
// Generation of a model data -> its levels, or absent. Open addressing, the
// generation is never 0 and never reused (J3DVertexData).
struct Slot { std::uint32_t generation; const Model* model; };
constexpr unsigned slotCount = 2048, probeLimit = 32;
Slot slots[slotCount];
unsigned slotsUsed = 0;
const Model* const absent = reinterpret_cast<const Model*>(1);
// Level data a stage may hold (main heap, not linear memory).
constexpr std::size_t heldLimit = 3u << 20;

void freeList(Held*& list) {
    while (list) {
        Held* next = list->next;
        count.heldBytes -= list->bytes;
        std::free(list);
        list = next;
    }
}
const Model* remember(std::uint32_t generation, const Model* model) {
    unsigned slot = (generation * 2654435761u) % slotCount;
    for (unsigned n = 0; n < probeLimit; ++n, slot = (slot + 1) % slotCount)
        if (!slots[slot].generation) {
            slots[slot] = {generation, model};
            ++slotsUsed;
            return model;
        }
    ++count.full; // no slot: asked again next time (bounded by the probe)
    return model;
}
std::uint64_t ticksToMicroseconds(std::uint64_t ticks) { return ticks * 1000000ull / SYSCLOCK_ARM11; }

// Reads and checks the record of `data`; null when there is none for it.
const Model* load(J3DModelData* data) {
    const unsigned shapes = data->getShapeNum();
    if (!shapes) return nullptr;
    std::uint64_t key = modelLodKeySeed;
    for (unsigned s = 0; s < shapes; ++s) {
        J3DShape* shape = data->getShapeNodePointer(u16(s));
        if (!shape) return nullptr;
        for (unsigned g = 0; g < shape->getMtxGroupNum(); ++g) {
            J3DShapeDraw* draw = shape->getShapeDraw(u16(g));
            if (!draw || !draw->getDisplayList()) return nullptr;
            key = modelLodCombine(key, modelLodListHash(draw->getDisplayList(), draw->getDisplayListSize()), draw->getDisplayListSize());
        }
    }
    ModelLodEntry entry;
    if (!store.find(key, entry)) { ++count.missing; return nullptr; }
    if (count.heldBytes + entry.bytes > heldLimit) { ++count.full; return nullptr; }
    // Struct sizes from the index entry's upper bounds, checked after parsing.
    const std::uint64_t start = svcGetSystemTick();
    std::unique_ptr<std::uint8_t[]> raw(new (std::nothrow) std::uint8_t[entry.bytes]);
    if (!raw || !store.read(entry, raw.get(), entry.bytes)) { ++count.readFailures; return nullptr; }
    ++count.reads;
    count.readBytes += entry.bytes;
    ModelLodRecord record;
    if (!record.parse({raw.get(), entry.bytes}) || record.shapes() != shapes) { ++count.mismatched; return nullptr; }
    const std::size_t bytes = sizeof(Held) + sizeof(Model) + sizeof(Shape) * record.shapes() + sizeof(Group) * record.groups() +
                              sizeof(Batch) * record.batches() + entry.bytes + 16;
    auto* block = static_cast<std::uint8_t*>(std::malloc(bytes));
    if (!block) { ++count.full; return nullptr; }
    auto* held = new (block) Held{nullptr, bytes};
    auto* model = new (block + sizeof(Held)) Model{};
    auto* shapeArray = reinterpret_cast<Shape*>(block + sizeof(Held) + sizeof(Model));
    auto* groupArray = reinterpret_cast<Group*>(shapeArray + record.shapes());
    auto* batchArray = reinterpret_cast<Batch*>(groupArray + record.groups());
    auto* bytesCopy = reinterpret_cast<std::uint8_t*>(batchArray + record.batches());
    bytesCopy += (16 - (reinterpret_cast<std::uintptr_t>(bytesCopy) & 15)) & 15;
    std::memcpy(bytesCopy, raw.get(), entry.bytes);
    // Views into the copy: parse it again where it now lives.
    if (!record.parse({bytesCopy, entry.bytes})) { std::free(block); ++count.mismatched; return nullptr; }
    model->shapes = std::uint16_t(shapes);
    model->shape = shapeArray;
    bool usable = false;
    for (unsigned s = 0; s < shapes; ++s) {
        const auto sv = record.shape(s);
        J3DShape* shape = data->getShapeNodePointer(u16(s));
        auto& out = *new (&shapeArray[s]) Shape{};
        if (sv.groups != shape->getMtxGroupNum()) { std::free(block); ++count.mismatched; return nullptr; }
        // Every list must be the recorded one, or the shape is drawn whole.
        bool same = true;
        for (unsigned g = 0; g < sv.groups && same; ++g) {
            const auto gv = record.group(sv.firstGroup + g);
            J3DShapeDraw* draw = shape->getShapeDraw(u16(g));
            same = gv.listBytes == draw->getDisplayListSize() &&
                   gv.listHash == modelLodListHash(draw->getDisplayList(), draw->getDisplayListSize());
        }
        if (!same) { ++count.mismatched; continue; }
        if (!sv.levels) continue;
        out.levels = sv.levels;
        out.error = sv.error;
        out.groups = sv.groups;
        out.group = &groupArray[sv.firstGroup];
        for (unsigned g = 0; g < sv.groups; ++g) {
            const auto gv = record.group(sv.firstGroup + g);
            J3DShapeDraw* draw = shape->getShapeDraw(u16(g));
            auto& group = *new (&groupArray[sv.firstGroup + g]) Group{};
            group.list = draw->getDisplayList();
            group.listBytes = gv.listBytes;
            group.batches = gv.batches;
            group.batch = &batchArray[gv.firstBatch];
            for (unsigned b = 0; b < gv.batches; ++b) {
                const auto bv = record.batch(gv.firstBatch + b);
                auto& batch = *new (&batchArray[gv.firstBatch + b]) Batch{};
                batch.vertexCount = bv.vertexCount;
                batch.fullIndices = bv.fullIndices;
                batch.fullHash = bv.fullHash;
                for (unsigned l = 0; l < levels; ++l) { batch.source[l] = bv.indices[l]; batch.count[l] = bv.count[l]; }
            }
        }
        usable = true;
    }
    if (!usable) { std::free(block); return nullptr; }
    held->next = current;
    current = held;
    count.heldBytes += bytes;
    ++count.models;
    count.readMicroseconds += ticksToMicroseconds(svcGetSystemTick() - start);
    return model;
}
}

Counters& counters() { return count; }

void open() {
    const auto status = store.open("sdmc:/3ds/DuskLight/zelda.lod");
    opened = status == ModelLodStore::Status::ready;
    OSReport("Native model levels: %s%s (sdmc:/3ds/DuskLight/zelda.lod, %lu models, index %lu KiB; made by Tools/3ds/model_lod.py)\n",
             ModelLodStore::statusText(status), opened ? "" : ": every model is drawn whole",
             static_cast<unsigned long>(store.models()), static_cast<unsigned long>(store.indexBytes() / 1024));
}
bool available() { return opened; }

const Model* find(J3DModelData* data) {
    if (!opened || !data) return nullptr;
    const std::uint32_t generation = data->getVertexData().mNativeGeneration;
    if (!generation) return nullptr;
    unsigned slot = (generation * 2654435761u) % slotCount;
    for (unsigned n = 0; n < probeLimit; ++n, slot = (slot + 1) % slotCount) {
        if (slots[slot].generation == generation) return slots[slot].model == absent ? nullptr : slots[slot].model;
        if (!slots[slot].generation) break;
    }
    const Model* model = load(data);
    remember(generation, model ? model : absent);
    return model;
}

void frameBegin(bool stageChanged) {
    freeList(retired);
    if (!stageChanged && slotsUsed < slotCount * 3 / 4) return;
    // Everything found so far is looked up again from now on; what the
    // packets entered last still point at is freed at the next boundary.
    retired = current;
    current = nullptr;
    std::memset(slots, 0, sizeof(slots));
    slotsUsed = 0;
}

void report() {
    if (!opened) return;
    const DrawCounters d = takeDrawCounters();
    Counters& c = count;
    // Two lines: one OSReport line is cut at the log's line length.
    OSReport("Native model levels: models %lu (records missing %lu, lists changed %lu, read %lu: %lu KiB in %lu ms, failed %lu, "
             "table/heap full %lu), held %lu KiB\n",
             static_cast<unsigned long>(c.models), static_cast<unsigned long>(c.missing), static_cast<unsigned long>(c.mismatched),
             static_cast<unsigned long>(c.reads), static_cast<unsigned long>(c.readBytes / 1024),
             static_cast<unsigned long>(c.readMicroseconds / 1000), static_cast<unsigned long>(c.readFailures),
             static_cast<unsigned long>(c.full), static_cast<unsigned long>(c.heldBytes / 1024));
    OSReport("Native model levels, last pictures: shapes seen %lu reduced %lu (level 0/1/2/3: %lu/%lu/%lu/%lu), shadow shapes %lu "
             "reduced %lu; batches %lu reduced %lu shadow %lu refused batch %lu pool %lu; indices saved %llu shadow %llu\n",
             static_cast<unsigned long>(c.shapesSeen), static_cast<unsigned long>(c.shapesReduced),
             static_cast<unsigned long>(c.levelChoices[0]), static_cast<unsigned long>(c.levelChoices[1]),
             static_cast<unsigned long>(c.levelChoices[2]), static_cast<unsigned long>(c.levelChoices[3]),
             static_cast<unsigned long>(c.shadowShapes), static_cast<unsigned long>(c.shadowReduced),
             static_cast<unsigned long>(d.batches), static_cast<unsigned long>(d.reduced), static_cast<unsigned long>(d.shadowReduced),
             static_cast<unsigned long>(d.refusedBatch), static_cast<unsigned long>(d.refusedPool),
             static_cast<unsigned long long>(d.indicesSaved), static_cast<unsigned long long>(d.shadowIndicesSaved));
    c.shapesSeen = c.shapesReduced = c.shadowShapes = c.shadowReduced = 0;
    for (auto& v : c.levelChoices) v = 0;
}
}
