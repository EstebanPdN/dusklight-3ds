// Mac-only proof for the level-of-detail store (model_lod_build.cpp): the
// levels are index lists over the vertex records the runtime makes for each
// draw of a display list, so the tool's picture of those draws must be the
// runtime's. Every display list of every J3D model of the disc runs through
// runtime/gx_commands.cpp as the 3DS build configures it, and each draw the
// sink receives is compared with model_lod_batches.hpp:
//   - the same number of draws per list;
//   - the same vertex records, byte for byte, in the same order;
//   - the same index list (the sink's own, or the expansion of a lone
//     primitive, which is what the native shape record keeps).
// Then the store written by the tool is opened with the runtime's reader and
// every record parsed (bounds, indices below their batch's vertex count).
// Built and run by Tools/3ds/model_lod.py --test (ASan/UBSan).
#include "model_prep_walk.hpp"
#include "model_lod_batches.hpp"
#include "../../Source/platforms/3ds/runtime/gx_commands.hpp"
#include "../../Source/platforms/3ds/runtime/gx_compact_cache.hpp"
#include "../../Source/platforms/3ds/runtime/model_lod_store.hpp"
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
struct Draw { std::vector<u8> records; std::vector<u16> indices; bool expandable = true; };
struct Sink { std::vector<Draw> draws; unsigned stride = 0; bool failed = false; };
bool sink(const CommandDraw& draw, void* context) {
    auto& s = *static_cast<Sink*>(context);
    Draw d;
    if (draw.vertices.size() < std::size_t(draw.vertexCount) * s.stride) { s.failed = true; return false; }
    d.records.assign(draw.vertices.begin(), draw.vertices.begin() + std::size_t(draw.vertexCount) * s.stride);
    if (draw.indexCount) {
        for (u32 i = 0; i < draw.indexCount; ++i) {
            u16 v;
            if (!draw.triangleIndex(i, v)) { s.failed = true; return false; }
            d.indices.push_back(v);
        }
    } else {
        d.expandable = modellod::expandTriangles(std::uint8_t(draw.primitive), draw.vertexCount, [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            d.indices.push_back(u16(a)); d.indices.push_back(u16(b)); d.indices.push_back(u16(c));
        });
        if (!d.expandable) d.indices.clear();
    }
    s.draws.push_back(std::move(d));
    return true;
}
unsigned typeBytes(unsigned type) { return type == 0 || type == 1 ? 1 : type == 2 || type == 3 ? 2 : 4; }
unsigned arrayStride(unsigned attr, const modelprep::Model::Format& f) {
    if (attr == GX_VA_POS) return (f.count ? 3 : 2) * typeBytes(f.type);
    if (attr == GX_VA_NRM) return (f.count ? 9 : 3) * typeBytes(f.type);
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) { static const unsigned c[6] = {2, 3, 4, 2, 3, 4}; return f.type < 6 ? c[f.type] : 4; }
    return (f.count ? 2 : 1) * typeBytes(f.type);
}
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: model_lod_batch_test zelda.twilight zelda.lod\n"); return 2; }
    dusk::native::Disc disc;
    if (disc.open(argv[1]) != dusk::native::DiscStatus::ready) { std::fprintf(stderr, "disc not readable\n"); return 1; }
    std::vector<u8> vertices(64 * 1024);
    std::vector<u16> indices(16 * 1024), hash(8192), remap(4096);
    unsigned models = 0, lists = 0, skipped = 0, failures = 0;
    std::uint64_t draws = 0, triangles = 0;
    alignas(32) static u8 setup[4096];
    dusk::native::DiscDirectory root;
    if (disc.findDirectory("/", root) != dusk::native::DiscFileStatus::ready) return 1;
    const bool walked = modelprep::walkAllModels(disc, root, "", [&](const std::string& path, const modelprep::Bytes& bytes) {
        const auto m = modelprep::parseModel(bytes);
        if (!m.error.empty()) return;
        ++models;
        CommandMemory memory;
        MemoryHandle handle;
        if (!memory.add({bytes.data(), bytes.size()}, handle)) { ++failures; return; }
        for (std::size_t si = 0; si < m.shapes.size(); ++si) {
            const auto& s = m.shapes[si];
            if (s.directAttributes) { skipped += unsigned(s.groups.size()); continue; }
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
                GDSetArraySized(d.attr, const_cast<u8*>(bytes.data()) + m.arrayOffset[d.attr], m.arrayBytes[d.attr],
                                u8(arrayStride(d.attr, m.formats[d.attr])), false);
            }
            const std::span<const u8> setupBytes(setup, GDGetCurrOffset());
            GDSetCurrent(nullptr);
            for (const auto& g : s.groups) {
                ++lists;
                std::vector<modellod::RuntimeBatch> expected;
                const bool cut = modellod::runtimeBatches(bytes.data() + g.dlOffset, g.dlSize, s.stride, expected);
                CommandProcessor processor(memory);
                Sink result;
                result.stride = s.stride;
                processor.setSink(sink, &result);
                processor.setPrimitiveBatchScratch({vertices, indices, 4096, hash, remap});
                const std::span<const u8> list(bytes.data() + g.dlOffset, g.dlSize);
                const bool ok = processor.execute(setupBytes) && processor.execute(list) && !result.failed;
                if (!cut) { ++skipped; continue; }
                bool same = ok && result.draws.size() == expected.size();
                for (std::size_t d = 0; same && d < expected.size(); ++d) {
                    const auto& e = expected[d];
                    const auto& r = result.draws[d];
                    if (r.records.size() != e.tuple.size() * s.stride) { same = false; break; }
                    for (std::size_t v = 0; same && v < e.tuple.size(); ++v)
                        same = !std::memcmp(r.records.data() + v * s.stride, bytes.data() + g.dlOffset + e.tuple[v], s.stride);
                    same = same && r.indices == e.indices && r.expandable == e.triangles;
                    triangles += e.indices.size() / 3;
                }
                draws += result.draws.size();
                if (!same && ++failures <= 8)
                    std::fprintf(stderr, "%s shape %zu: ok %d draws %zu expected %zu error %u\n", path.c_str(), si, ok,
                                 result.draws.size(), expected.size(), unsigned(processor.error()));
            }
        }
    });
    std::printf("models %u, lists %u (not cut %u), draws %llu, triangles %llu: records and indices %s\n", models, lists, skipped,
                (unsigned long long)draws, (unsigned long long)triangles, failures ? "DIFFER" : "identical to the runtime's");
    // The store, through the runtime's reader.
    dusk::native::ModelLodStore store;
    const auto status = store.open(argv[2]);
    unsigned records = 0, bad = 0;
    if (status == dusk::native::ModelLodStore::Status::ready) {
        // Walk the index in key order through find (every key must be found).
        std::FILE* f = std::fopen(argv[2], "rb");
        std::vector<u8> header(64);
        if (!f || std::fread(header.data(), 1, 64, f) != 64) return 1;
        const std::uint64_t count = store.models();
        std::vector<u8> idx(count * 24);
        if (std::fseek(f, 64, SEEK_SET) || std::fread(idx.data(), 1, idx.size(), f) != idx.size()) return 1;
        std::fclose(f);
        for (std::uint64_t n = 0; n < count; ++n) {
            std::uint64_t key = 0;
            std::memcpy(&key, idx.data() + n * 24, 8);
            dusk::native::ModelLodEntry entry;
            std::vector<u8> record;
            if (!store.find(key, entry)) { ++bad; continue; }
            record.resize(entry.bytes);
            dusk::native::ModelLodRecord parsed;
            if (!store.read(entry, record.data(), record.size()) || !parsed.parse(record)) { ++bad; continue; }
            ++records;
        }
    }
    std::printf("store: %s, %u records parsed, %u bad\n", dusk::native::ModelLodStore::statusText(status), records, bad);
    const bool pass = walked && !failures && status == dusk::native::ModelLodStore::Status::ready && !bad && records;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
