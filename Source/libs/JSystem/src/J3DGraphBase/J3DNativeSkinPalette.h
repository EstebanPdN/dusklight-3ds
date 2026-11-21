#pragma once
// Merged matrix groups of skinned shapes, the game thread's part
// (features::Id::skinPalette; runtime/skin_palette.hpp has the whole
// picture). Included once, by J3DShape.cpp on the 3DS, after the instance
// batches (it shares their picture count).
//
// A shape is planned once per model data: its groups split into runs whose
// matrices fit the palette, each run's merged list built in the arena with a
// header for the drawing side. Per picture a run is one draw when every
// entry's normal is exact or within the bound (skin_palette.hpp); otherwise
// its groups are drawn the original way, with J3D's normals. Per picture each run costs one matrix product
// per palette entry (the groups loaded a slot again whenever they named a
// bone the slot no longer held), one XF write and one queue entry, where its
// groups cost a load and a queue entry each.
#include "../../../../platforms/3ds/runtime/skin_palette.hpp"
#include "../../../../platforms/3ds/runtime/perf_features.hpp"
#include "JSystem/J3DGraphBase/J3DShapeDraw.h"
#include "JSystem/J3DGraphBase/J3DTransform.h"
#include <cmath>
#include <new>

extern "C" void GXNativeLoadXf(u16 address, const u32* values, u32 count);
extern "C" void OSReport(const char*, ...);
// Counters of the drawing side (gx_command_draw.cpp), for the report line.
extern "C" void duskNativeSkinDrawReport(void);

namespace {
namespace skin = dusk::native::skin;

struct SkinShape {
    const J3DShape* shape;
    const void* firstList;  // group 0's list when planned
    u32 vertexGeneration;   // the model data's arrays when planned
    u32 lastUse;            // picture
    u8 runs, capacity;
    const char* refused;    // why the shape stays on the original path; null: planned
    skin::MergedHeader* run[skin::maxRuns];
};
const u32 kSkinShapes = 256, kSkinProbe = 8;
SkinShape sSkinShapes[kSkinShapes];
// Merged lists, each behind its header. A list is never written again once
// built: a shape planned again gets new lists with new generations, and the
// arena empties only when none of its shapes was drawn in the last three
// pictures (the second core reads a list one picture late).
const u32 kSkinArena = 512 * 1024;
alignas(32) u8 sSkinArena[kSkinArena];
u32 sSkinArenaUsed;
u32 sSkinGeneration;

struct SkinCounts {
    u32 shapes, runs, groups, entries;            // shapes on this path and their runs (per report period)
    u32 mergedShapes, mergedRuns, mergedGroups;   // runs queued as one draw
    u32 notQueued;                                // runs drawn group by group: not queued
    u32 boundShapes, boundRuns;                   // runs drawn group by group: an entry over the normal limit
    u32 exactEntries, boundedEntries;             // entries of merged runs
    f32 boundedSum, boundedMax;                   // their bounds (sine of the angle)
    u32 buckets[6];                               // runs by worst bound: <=1, 2, 4, 8, 16 degrees, more
    u32 state, refusedShapes;                     // shapes drawn the original way, by reason
    u32 planned, planRefusals, replans, arenaResets;
    const char* lastRefusal;
};
SkinCounts sSkinCounts;

bool skinStateAllows() {
    // loadSkinMatrixRuns's conditions: pipeline 0 without differed texgen,
    // NBT or a normal-to-texture load writes only position and normal
    // matrices, which is all a merged draw replaces.
    return J3DShapeMtx::sCurrentPipeline == 0 && J3DDifferedTexMtx::sTexGenBlock == NULL && !J3DShapeMtx::sNBTFlag &&
           J3DShapeMtx::sTexMtxLoadType != 0x2000 && !J3DShapeMtx::getLODFlag();
}

// Palette entries a draw of `material` holds (runtime/skin_palette.hpp
// capacityForUnits): its texture units take the rows above the palette.
u8 skinCapacity(J3DMaterial* material) {
    unsigned units = 0;
    if (material != NULL) {
        for (u32 i = 0; i < 8; i++)
            units += material->getTexNo(i) != 0xFFFF;
    }
    return u8(skin::capacityForUnits(units));
}

void skinArenaReset() {
    for (u32 i = 0; i < kSkinShapes; i++)
        sSkinShapes[i] = SkinShape{};
    sSkinArenaUsed = 0;
    sSkinCounts.arenaResets++;
}

// The shape's runs and their merged lists; the reason when it stays on the
// original path.
const char* skinPlan(J3DShape* shape, u32 generation, u8 capacity, SkinShape& e) {
    const u16 groupNum = shape->getMtxGroupNum();
    if (groupNum < 2 || groupNum > skin::maxGroups)
        return "groups";
    skin::VertexLayout layout{};
    bool matrix = false;
    for (const GXVtxDescList* d = shape->getVtxDesc(); d != NULL && d->attr != GX_VA_NULL; d++) {
        if (d->type == GX_NONE)
            continue;
        if (d->attr == GX_VA_PNMTXIDX) {
            if (d->type != GX_DIRECT)
                return "layout";
            layout.matrixAt = layout.stride;
            layout.stride += 1;
            matrix = true;
        } else if (d->attr >= GX_VA_TEX0MTXIDX && d->attr <= GX_VA_TEX7MTXIDX) {
            return "texture matrix per bone";
        } else if (d->attr < GX_VA_POS || d->attr > GX_VA_TEX7 || (d->type != GX_INDEX8 && d->type != GX_INDEX16)) {
            return "layout";
        } else {
            layout.stride += d->type == GX_INDEX8 ? 1 : 2;
        }
    }
    if (!matrix)
        return "layout";
    static skin::GroupInput groups[skin::maxGroups];
    for (u16 i = 0; i < groupNum; i++) {
        J3DShapeMtx* mtx = shape->getShapeMtx(i);
        J3DShapeDraw* draw = shape->getShapeDraw(i);
        if (mtx == NULL || draw == NULL || mtx->getType() != 'SMMC' || draw->getDisplayList() == NULL)
            return "groups";
        groups[i].list = static_cast<const u8*>(draw->getDisplayList());
        groups[i].bytes = draw->getDisplayListSize();
        groups[i].loads.fill(0xFFFF);
        const u16 use = mtx->getUseMtxNum();
        for (u16 s = 0; s < use && s < skin::slots; s++)
            groups[i].loads[s] = mtx->getUseMtxIndex(s);
    }
    J3DModelData* data = j3dSys.getModel()->getModelData();
    // A palette entry is the matrix J3DShapeMtxMultiConcatView loads for a
    // use-matrix index: the joint or envelope table, and its index there.
    auto keyOf = [data](u16 use) -> u32 { return (u32(data->getDrawMtxFlag(use)) << 16) | data->getDrawMtxIndex(use); };
    static skin::ShapePlan plan;
    const std::span<const skin::GroupInput> input(groups, groupNum);
    const skin::Refusal why = skin::planShape(input, layout, keyOf, capacity, plan);
    if (why != skin::Refusal::none)
        return skin::refusalName(why);
    const u32 headerBytes = (sizeof(skin::MergedHeader) + 31) & ~31u;
    u32 need = 0;
    for (u32 r = 0; r < plan.runs; r++)
        need += headerBytes + skin::mergedBytes(plan.run[r]);
    if (sSkinArenaUsed + need > kSkinArena)
        return "arena";
    for (u32 r = 0; r < plan.runs; r++) {
        const skin::Run& run = plan.run[r];
        u8* list = sSkinArena + sSkinArenaUsed + headerBytes;
        if (!skin::writeMerged(input, layout, plan, r, list))
            return "list";
        auto* header = new (list - sizeof(skin::MergedHeader)) skin::MergedHeader{};
        header->magic = skin::mergedMagic;
        header->generation = skin::generationBit | (++sSkinGeneration & 0x3FFFFFFFu);
        header->groups = u8(run.groups);
        header->entries = run.matrices;
        header->listBytes = skin::mergedBytes(run);
        for (u32 g = 0; g < run.groups; g++) {
            const u32 group = run.firstGroup + g;
            header->group[g].list = groups[group].list;
            header->group[g].bytes = groups[group].bytes;
            std::memcpy(header->group[g].entry, plan.entry[group].data(), skin::slots);
        }
        for (u32 k = 0; k < run.matrices; k++)
            header->keys[k] = run.keys[k];
        e.run[r] = header;
        sSkinArenaUsed += headerBytes + header->listBytes;
    }
    e.runs = u8(plan.runs);
    sSkinCounts.planned++;
    return NULL;
}

SkinShape* skinShapeOf(const J3DShape* shape) {
    const u32 home = u32((reinterpret_cast<uintptr_t>(shape) >> 3) * 2654435761u) % kSkinShapes;
    SkinShape* free = NULL;
    for (u32 n = 0; n < kSkinProbe; n++) {
        SkinShape& e = sSkinShapes[(home + n) % kSkinShapes];
        if (e.shape == shape)
            return &e;
        if (e.shape == NULL && free == NULL)
            free = &e;
    }
    if (free == NULL) {
        // Full: an entry not drawn for three pictures gives its place (its
        // lists stay in the arena until it empties).
        for (u32 n = 0; n < kSkinProbe && free == NULL; n++) {
            SkinShape& e = sSkinShapes[(home + n) % kSkinShapes];
            if (e.lastUse + 3 <= sNativePicture)
                free = &e;
        }
        if (free == NULL)
            return NULL;
    }
    *free = SkinShape{};
    free->shape = shape;
    return free;
}

// Plans `e` (again) with `capacity`; an arena without room empties first
// when nothing in it can still be read.
void skinReplan(J3DShape* shape, u32 generation, u8 capacity, SkinShape*& e) {
    e->refused = skinPlan(shape, generation, capacity, *e);
    if (e->refused != NULL && !std::strcmp(e->refused, "arena")) {
        bool quiet = true;
        for (u32 i = 0; i < kSkinShapes && quiet; i++)
            quiet = sSkinShapes[i].shape == NULL || sSkinShapes[i].lastUse + 3 <= sNativePicture;
        if (quiet) {
            skinArenaReset();
            e = skinShapeOf(shape);
            if (e == NULL)
                return;
            e->refused = skinPlan(shape, generation, capacity, *e);
        }
    }
    e->firstList = shape->getShapeDraw(0)->getDisplayList();
    e->vertexGeneration = generation;
    e->capacity = capacity;
    if (e->refused != NULL) {
        e->runs = 0;
        sSkinCounts.planRefusals++;
        sSkinCounts.lastRefusal = e->refused;
    }
}

// Draws the groups of `run` as the original loop would: the slots their
// vertices name hold the run's matrices and J3D's normal matrices for them
// (the position 3x3 for scale flag 1, else its inverse transpose, as
// loadSkinMatrixRuns makes them), then each group's list.
void skinDrawGroups(J3DShape* shape, const skin::MergedHeader& run, u32 firstGroup, const u32* palette, u32 generation,
                    const u8* scale) {
    for (u32 g = 0; g < run.groups; g++) {
        for (u32 s = 0; s < skin::slots; s++) {
            const u8 entry = run.group[g].entry[s];
            if (entry == 0xFF)
                continue;
            const u32* position = palette + 12 * entry;
            u32 normal[9];
            if (scale[run.keys[entry] & 0xFFFF] == 1) {
                for (u32 row = 0; row < 3; row++)
                    for (u32 c = 0; c < 3; c++)
                        normal[3 * row + c] = position[4 * row + c];
            } else {
                Mtx m;
                Mtx33 inverseTranspose{};
                std::memcpy(m, position, sizeof(m));
                J3DPSCalcInverseTranspose(m, inverseTranspose);
                std::memcpy(normal, inverseTranspose, sizeof(normal));
            }
            GXNativeLoadXf(u16(12 * s), position, 12);
            GXNativeLoadXf(u16(0x400 + 9 * s), normal, 9);
        }
        J3DShapeDraw* draw = shape->getShapeDraw(u16(firstGroup + g));
        if (!duskNativeDrawShape(generation, draw->getDisplayList(), draw->getDisplayListSize())) {
            duskNativeStaticGeometry(generation, draw->getDisplayList());
            draw->draw();
            duskNativeStaticGeometry(0, NULL);
        }
    }
}

// Census buckets of a run's worst bound: sin 1, 2, 4, 8, 16 degrees.
const f32 kSkinBucketSines[5] = {0.017452f, 0.034899f, 0.069756f, 0.139173f, 0.275637f};

void skinPicture() {
    if (sNativePicture % 300 != 0)
        return;
    SkinCounts& c = sSkinCounts;
    if (c.shapes != 0 || c.planned != 0 || c.planRefusals != 0) {
        const f32 n = 1.0f / 300;
        const f32 degrees = 57.29578f;
        // Three lines: OSReport cuts one line at its buffer size.
        OSReport("Native skin palette, last 300 pictures, per picture: shapes %.1f in %.1f runs for %.1f groups, %.1f entries; "
                 "merged %.1f runs (%.1f queue entries saved), not queued %.1f; original way: state %.1f, refused %.1f\n",
                 c.shapes * n, c.runs * n, c.groups * n, c.entries * n, c.mergedRuns * n, (c.mergedGroups - c.mergedRuns) * n,
                 c.notQueued * n, c.state * n, c.refusedShapes * n);
        OSReport("Native skin palette normals, per picture: shapes merged %.1f, over the limit %.1f (%.1f runs); merged entries exact "
                 "%.1f, bounded %.1f: largest %.2f deg, mean %.2f deg; runs by worst bound <=1 %.1f <=2 %.1f <=4 %.1f <=8 %.1f "
                 "<=16 %.1f more %.1f\n",
                 c.mergedShapes * n, c.boundShapes * n, c.boundRuns * n, c.exactEntries * n, c.boundedEntries * n,
                 std::asin(c.boundedMax) * degrees,
                 c.boundedEntries != 0 ? std::asin(c.boundedSum / c.boundedEntries) * degrees : 0.0f, c.buckets[0] * n,
                 c.buckets[1] * n, c.buckets[2] * n, c.buckets[3] * n, c.buckets[4] * n, c.buckets[5] * n);
        OSReport("Native skin palette, shapes planned %u, refused %u (last: %s), planned again %u; arena %u/%u bytes, emptied %u\n",
                 c.planned, c.planRefusals, c.lastRefusal != NULL ? c.lastRefusal : "-", c.replans, sSkinArenaUsed, kSkinArena,
                 c.arenaResets);
        duskNativeSkinDrawReport();
    }
    const char* last = c.lastRefusal;
    c = SkinCounts{};
    c.lastRefusal = last;
}

// J3D's shape getters are not const; drawMtxGroups is.
bool J3DNativeSkinDraw(J3DShape* shape, u32 generation) {
    if (!dusk::native::features::on(dusk::native::features::Id::skinPalette) || shape->getMtxGroupNum() < 2)
        return false;
    if (!skinStateAllows()) {
        sSkinCounts.state++;
        return false;
    }
    SkinShape* e = skinShapeOf(shape);
    if (e == NULL)
        return false;
    J3DShapeDraw* first = shape->getShapeDraw(0);
    if (first == NULL)
        return false;
    if (e->runs == 0 && e->refused == NULL)
        skinReplan(shape, generation, skinCapacity(shape->getMaterial()), e);
    else if (e->firstList != first->getDisplayList() || e->vertexGeneration != generation) {
        // Another model's data at this shape's address, or its arrays made again.
        *e = SkinShape{};
        e->shape = shape;
        skinReplan(shape, generation, skinCapacity(shape->getMaterial()), e);
    }
    if (e == NULL)
        return false;
    e->lastUse = sNativePicture;
    if (e->refused != NULL) {
        sSkinCounts.refusedShapes++;
        return false;
    }
    // What the drawing side found: fewer entries fit this material's rows
    // (plan again with them), or it refused for another reason (more lights
    // than the palette leaves room for: original path from now on).
    for (u32 r = 0; r < e->runs; r++) {
        if (e->run[r]->refusals.load(std::memory_order_relaxed) == 0)
            continue;
        const u32 fits = e->run[r]->capacity.load(std::memory_order_relaxed);
        if (fits >= skin::slots && fits < e->capacity) {
            sSkinCounts.replans++;
            const u32 used = e->lastUse;
            *e = SkinShape{};
            e->shape = shape;
            skinReplan(shape, generation, u8(fits), e);
            if (e == NULL)
                return false;
            e->lastUse = used;
            if (e->refused != NULL) {
                sSkinCounts.refusedShapes++;
                return false;
            }
        } else {
            e->refused = "drawing side";
            e->runs = 0;
            sSkinCounts.refusedShapes++;
            return false;
        }
        break;
    }

    static const auto profileIndex = dusk::native::registerGxNamedProfile("j3d.skinPalette");
    dusk::native::GxNamedProfileScope profile(profileIndex);
    J3DModel* model = j3dSys.getModel();
    const u8* scale = J3DShapeMtx::sCurrentScaleFlag;
    MtxP base = *j3dSys.getShapePacket()->getBaseMtxPtr();
    Mtx* tables[2] = {reinterpret_cast<Mtx*>(model->getMtxBuffer()->getUserAnmMtx(0)),
                      reinterpret_cast<Mtx*>(model->getWeightAnmMtx(0))};
    const f32 limit2 = skin::normalSineLimit * skin::normalSineLimit;
    u32 words[4 + 12 * skin::maxMatrices];
    f32 bounds[skin::maxMatrices];
    u32 group = 0;
    bool merged = false, overLimit = false;
    for (u32 r = 0; r < e->runs; r++) {
        const skin::MergedHeader& run = *e->run[r];
        words[0] = (skin::headerMagic << 16) | run.entries;
        words[1] = run.generation;
        words[2] = words[3] = 0;
        // Entries whose J3D normal matrix is not their position 3x3 light
        // within the bound of skin_palette.hpp, or the run is drawn the
        // original way (skinDrawGroups, J3D's own normals).
        u32 bounded = 0;
        f32 worst = 0.0f;
        for (u32 k = 0; k < run.entries; k++) {
            Mtx m;
            MTXConcat(base, tables[run.keys[k] >> 16][run.keys[k] & 0xFFFF], m);
            std::memcpy(words + 4 + 12 * k, m, sizeof(m));
            bounds[k] = -1.0f;
            if (scale[run.keys[k] & 0xFFFF] != 1) {
                bounds[k] = skin::normalSineSquared(m);
                worst = bounds[k] > worst ? bounds[k] : worst;
                bounded++;
            }
        }
        if (bounded != 0) {
            const f32 sine = std::sqrt(worst);
            u32 bucket = 0;
            while (bucket < 5 && sine > kSkinBucketSines[bucket])
                bucket++;
            sSkinCounts.buckets[bucket]++;
        }
        const u8* list = reinterpret_cast<const u8*>(&run) + sizeof(skin::MergedHeader);
        if (worst > limit2) {
            sSkinCounts.boundRuns++;
            overLimit = true;
            skinDrawGroups(shape, run, group, words + 4, generation, scale);
        } else {
            GXNativeLoadXf(skin::xfHeader, words, 4 + 12 * run.entries);
            if (duskNativeDrawShape(run.generation, list, run.listBytes)) {
                merged = true;
                sSkinCounts.mergedRuns++;
                sSkinCounts.mergedGroups += run.groups;
                sSkinCounts.exactEntries += run.entries - bounded;
                sSkinCounts.boundedEntries += bounded;
                for (u32 k = 0; k < run.entries; k++) {
                    if (bounds[k] < 0.0f)
                        continue;
                    const f32 sine = std::sqrt(bounds[k]);
                    sSkinCounts.boundedSum += sine;
                    sSkinCounts.boundedMax = sine > sSkinCounts.boundedMax ? sine : sSkinCounts.boundedMax;
                }
            } else {
                // Neither queued nor drawn from a record (the worker rests and
                // this list was never drawn whole): its groups.
                sSkinCounts.notQueued++;
                skinDrawGroups(shape, run, group, words + 4, generation, scale);
            }
        }
        group += run.groups;
        sSkinCounts.runs++;
        sSkinCounts.groups += run.groups;
        sSkinCounts.entries += run.entries;
    }
    sSkinCounts.shapes++;
    sSkinCounts.mergedShapes += merged;
    sSkinCounts.boundShapes += overLimit;
    return true;
}
}  // namespace
