// Mac-only proof for the model preparation survey (model_prep_survey.cpp):
// every display list of every room model runs through the runtime's own GX
// command processor (runtime/gx_commands.cpp), as the 3DS build configures it
// (game_startup.cpp: 64 KiB packed vertices, 16 Ki indices, 4096 vertices,
// deduplication on), and again with 16384-vertex caps.
//
// Checked for each shape:
//  1. the processor makes exactly the draws simulateDraws() predicts (the
//     survey's draw counts are the runtime's);
//  2. the triangles it hands to the draw sink, expanded to the packed vertex
//     tuples (array indices of every attribute: the inputs the vertex decoder
//     reads), are the triangles of the list in list order, under both caps:
//     the same triangles, the same vertex order in each, the same order of
//     triangles. Larger caps change the number of draws only.
// Built and run by Tools/3ds/model_prep.py (ASan/UBSan).
#include "model_prep_walk.hpp"
#include "../../Source/platforms/3ds/runtime/gx_commands.hpp"
#include "../../Source/platforms/3ds/runtime/gx_compact_cache.hpp"
#include <dolphin/gd.h>
#include <dolphin/gx/GXAurora.h>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace dusk::native::gx;
extern "C" void OSPanic(const char*, int, const char* message, ...) { throw std::runtime_error(message); }
extern "C" void DCFlushRange(void*, u32) {}
thread_local dusk::native::gx::StaticGeometryHint dusk::native::gx::staticGeometryHint{};

namespace {
using Tuple = std::vector<u8>;
using Triangle = std::array<Tuple, 3>;
struct Sink {
    std::vector<Triangle> triangles;
    unsigned draws = 0;
    unsigned stride = 0;
    bool failed = false;
};
template <class F>
bool expand(unsigned primitive, unsigned count, F emit) {
    switch (primitive) {
    case GX_TRIANGLES: for (unsigned i = 0; i + 2 < count; i += 3) emit(i, i + 1, i + 2); return count % 3 == 0;
    case GX_TRIANGLESTRIP:
        for (unsigned i = 2; i < count; ++i) { if (i & 1) emit(i - 1, i - 2, i); else emit(i - 2, i - 1, i); }
        return count >= 3;
    case GX_TRIANGLEFAN: for (unsigned i = 2; i < count; ++i) emit(0, i - 1, i); return count >= 3;
    case GX_QUADS: for (unsigned i = 0; i + 3 < count; i += 4) { emit(i, i + 1, i + 2); emit(i + 2, i + 3, i); } return count % 4 == 0;
    }
    return false;
}
bool sink(const CommandDraw& draw, void* context) {
    auto& s = *static_cast<Sink*>(context);
    ++s.draws;
    const auto tuple = [&](unsigned v) {
        return Tuple(draw.vertices.begin() + std::size_t(v) * s.stride, draw.vertices.begin() + std::size_t(v + 1) * s.stride);
    };
    if (draw.vertices.size() < std::size_t(draw.vertexCount) * s.stride) { s.failed = true; return false; }
    if (draw.indexCount) {
        for (u32 i = 0; i + 2 < draw.indexCount; i += 3) {
            u16 a, b, c;
            if (!draw.triangleIndex(i, a) || !draw.triangleIndex(i + 1, b) || !draw.triangleIndex(i + 2, c)) { s.failed = true; return false; }
            s.triangles.push_back({tuple(a), tuple(b), tuple(c)});
        }
        return true;
    }
    if (!expand(draw.primitive, draw.vertexCount, [&](unsigned a, unsigned b, unsigned c) {
            s.triangles.push_back({tuple(a), tuple(b), tuple(c)});
        })) s.failed = true;
    return !s.failed;
}
// The reference: the list's own primitives, expanded in order (the
// processor's own expansion rule, aurora::gx::dl::expand_triangles).
std::vector<Triangle> referenceTriangles(const modelprep::Bytes& f, const modelprep::Group& g, unsigned stride) {
    std::vector<Triangle> out;
    for (const auto& p : g.primitives) {
        const u8* base = f.data() + g.dlOffset + p.offset + 3;
        const auto tuple = [&](unsigned v) { return Tuple(base + std::size_t(v) * stride, base + std::size_t(v + 1) * stride); };
        expand(p.opcode, p.count, [&](unsigned a, unsigned b, unsigned c) { out.push_back({tuple(a), tuple(b), tuple(c)}); });
    }
    return out;
}
unsigned typeBytes(unsigned type) { return type == 0 || type == 1 ? 1 : type == 2 || type == 3 ? 2 : 4; }
unsigned arrayStride(unsigned attr, const modelprep::Model::Format& f) {
    if (attr == GX_VA_POS) return (f.count ? 3 : 2) * typeBytes(f.type);
    if (attr == GX_VA_NRM) return (f.count ? 9 : 3) * typeBytes(f.type);
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) { static const unsigned c[6] = {2, 3, 4, 2, 3, 4}; return f.type < 6 ? c[f.type] : 4; }
    return (f.count ? 2 : 1) * typeBytes(f.type);
}
struct Config {
    const char* name;
    modelprep::BatchCaps caps;
    std::vector<u8> vertices;
    std::vector<u16> indices, hash, remap;
    explicit Config(const char* n, modelprep::BatchCaps c, unsigned hashSlots)
        : name(n), caps(c), vertices(c.vertexBytes), indices(c.maxIndices), hash(hashSlots), remap(c.maxVertices) {}
};
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: model_prep_batch_test zelda.twilight\n"); return 2; }
    dusk::native::Disc disc;
    if (disc.open(argv[1]) != dusk::native::DiscStatus::ready) { std::fprintf(stderr, "disc not readable\n"); return 1; }
    Config runtime("runtime", {4096, 64 * 1024, 16 * 1024}, 8192);
    Config wide("wide", {16384, 256 * 1024, 64 * 1024}, 32768);
    unsigned archives = 0, shapes = 0, lists = 0, skipped = 0, failures = 0;
    std::uint64_t triangles = 0, draws[2]{}, predicted[2]{};
    alignas(32) static u8 setup[4096];
    const bool walked = modelprep::walkRoomModels(disc, [&](modelprep::RoomModel& rm) {
        const auto m = modelprep::parseModel(rm.bytes);
        if (!m.error.empty()) { ++failures; return; }
        CommandMemory memory;
        MemoryHandle handle;
        if (!memory.add({rm.bytes.data(), rm.bytes.size()}, handle)) { ++failures; return; }
        for (std::size_t si = 0; si < m.shapes.size(); ++si) {
            const auto& s = m.shapes[si];
            ++shapes;
            if (s.directAttributes) { ++skipped; continue; }
            // The shape's vertex setup, as J3DShape::makeVcdVatCmd, and its arrays.
            std::vector<GXVtxDescList> desc;
            std::vector<GXVtxAttrFmtList> fmt;
            for (std::size_t k = 0; k + 8 <= s.vcd.size(); k += 8) {
                const auto attr = modelprep::be32(s.vcd.data() + k), type = modelprep::be32(s.vcd.data() + k + 4);
                if (attr == 0xFF) break;
                desc.push_back({GXAttr(attr), GXAttrType(type)});
            }
            desc.push_back({GX_VA_NULL, GX_NONE});
            for (unsigned a = GX_VA_POS; a <= GX_VA_TEX7; ++a)
                if (m.formats[a].present)
                    fmt.push_back({GXAttr(a), GXCompCnt(m.formats[a].count), GXCompType(m.formats[a].type), m.formats[a].frac});
            fmt.push_back({GX_VA_NULL, GX_POS_XY, GX_U8, 0});
            GDLObj gd;
            GDInitGDLObj(&gd, setup, sizeof(setup));
            GDSetCurrent(&gd);
            GDSetVtxDescv(desc.data());
            GDSetVtxAttrFmtv(GX_VTXFMT0, fmt.data());
            for (const auto& d : desc) {
                if (d.attr < GX_VA_POS || d.attr > GX_VA_TEX7 || (d.type != GX_INDEX8 && d.type != GX_INDEX16)) continue;
                if (!m.arrayBytes[d.attr]) continue;
                GDSetArraySized(d.attr, const_cast<u8*>(rm.bytes.data()) + m.arrayOffset[d.attr], m.arrayBytes[d.attr],
                                u8(arrayStride(d.attr, m.formats[d.attr])), false);
            }
            const std::span<const u8> setupBytes(setup, GDGetCurrOffset());
            GDSetCurrent(nullptr);
            for (const auto& g : s.groups) {
                ++lists;
                const auto reference = referenceTriangles(rm.bytes, g, s.stride);
                triangles += reference.size();
                unsigned c = 0;
                for (auto* config : {&runtime, &wide}) {
                    CommandProcessor processor(memory);
                    Sink result;
                    result.stride = s.stride;
                    processor.setSink(sink, &result);
                    processor.setPrimitiveBatchScratch({config->vertices, config->indices, config->caps.maxVertices, config->hash, config->remap});
                    const std::span<const u8> list(rm.bytes.data() + g.dlOffset, g.dlSize);
                    const bool ok = processor.execute(setupBytes) && processor.execute(list) && !result.failed;
                    const auto expected = modelprep::simulateDraws(g, s.stride, config->caps);
                    draws[c] += result.draws;
                    predicted[c] += expected;
                    if (!ok || result.draws != expected || result.triangles != reference) {
                        if (++failures <= 8)
                            std::fprintf(stderr, "%s r%u %s shape %zu (%s caps): ok %d draws %u predicted %u triangles %zu reference %zu error %u\n",
                                         rm.stage.c_str(), rm.room, rm.file.c_str(), si, config->name, ok, result.draws, expected,
                                         result.triangles.size(), reference.size(), unsigned(processor.error()));
                    }
                    ++c;
                }
            }
        }
    }, archives);
    std::printf("room archives %u, shapes %u, lists %u (skipped %u), triangles %llu\n", archives, shapes, lists, skipped,
                (unsigned long long)triangles);
    std::printf("runtime caps: %llu draws (predicted %llu); 16384-vertex caps: %llu draws (predicted %llu); triangle streams identical to the lists: %s\n",
                (unsigned long long)draws[0], (unsigned long long)predicted[0], (unsigned long long)draws[1],
                (unsigned long long)predicted[1], failures ? "NO" : "yes");
    std::printf("%s (%u failures)\n", walked && !failures ? "PASS" : "FAIL", failures);
    return walked && !failures ? 0 : 1;
}
