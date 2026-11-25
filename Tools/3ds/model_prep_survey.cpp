// Mac-only: what offline preparation of the models could save at run time,
// computed on the user's own zelda.twilight (or GameCube image). Built and run
// by Tools/3ds/model_prep.py; nothing here is linked on the 3DS.
//
// Candidates, each counted in the draws the runtime makes (one per batch of
// the command processor, model_prep_bmd.hpp simulateDraws):
//   merge     room shapes merged with the shape drawn right before them when
//             they share the material (same MAT3 entry), the matrix, the
//             vertex format and fit one batch: exact (same triangles, same
//             order, same state).
//   reorder   bound for the same merge with the draw order given up inside a
//             buffer (not exact for blending or equal depths).
//   textures  bound for merging every shape of a buffer whose material samples
//             the same images (needs the materials made equal: not exact).
//   batches   the runtime's 4096-vertex batch cap against a larger one (same
//             triangles; restripping offline cannot beat the larger cap).
//   palette   skinned (multi-matrix) shapes: consecutive matrix groups merged
//             into one draw when the PICA's uniform palette holds P matrices
//             instead of GX's 10 slots (needs a new runtime draw path).
#include "model_prep_walk.hpp"
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace modelprep;
namespace {
struct Room {
    std::string stage;
    unsigned room = 0, models = 0, shapes = 0, opaque = 0, translucent = 0;
    unsigned draws = 0, drawsWideCaps = 0, merge = 0, reorder = 0, textures = 0;
    std::uint64_t dlBytes = 0, mergedDlBytes = 0;
};
struct Skin { std::uint64_t models = 0, shapes = 0, groups = 0, skinnedShapes = 0, skinnedGroups = 0, p16 = 0, p26 = 0, tiny = 0,
              sameMaterialPairs = 0, simplePairs = 0; };

// Texture identity of TEX1 entry t: the header without its image offset and
// the first 256 image bytes (enough to tell authored images apart here; the
// bound it feeds is an upper bound anyway).
std::uint64_t textureHash(const Bytes& f, unsigned t) {
    std::uint32_t n = 0;
    const auto* tex1 = blockOf(f, "TEX1", n);
    if (!tex1 || t >= be16(tex1 + 8)) return 0xdead;
    const auto* h = tex1 + be32(tex1 + 12) + t * 32;
    if (h + 32 > tex1 + n) return 0xdead;
    std::uint64_t x = 1469598103934665603ull;
    for (int i = 0; i < 28; ++i) x = (x ^ h[i]) * 1099511628211ull;
    const auto* img = h + be32(h + 28);
    for (int i = 0; i < 256 && img + i < tex1 + n; ++i) x = (x ^ img[i]) * 1099511628211ull;
    return x;
}
std::vector<std::vector<std::uint64_t>> textureSets(const Bytes& f, const Model& m) {
    std::uint32_t n = 0;
    const auto* mat3 = blockOf(f, "MAT3", n);
    std::vector<std::vector<std::uint64_t>> sets(m.materials.size());
    const auto initOff = be32(mat3 + 12), texNoOff = be32(mat3 + 0x48);
    for (std::size_t i = 0; i < m.materials.size(); ++i) {
        const auto* d = mat3 + initOff + m.materials[i].remap * 0x14C;
        for (int k = 0; k < 8; ++k) {
            const unsigned idx = be16(d + 0x84 + 2 * k);
            if (idx != 0xFFFF) sets[i].push_back(textureHash(f, be16(mat3 + texNoOff + 2 * idx)));
        }
    }
    return sets;
}
bool mergeable(const Shape& s) {
    if (s.mtxType != 0 || s.groups.size() != 1 || s.directAttributes) return false;
    const auto& g = s.groups[0];
    return g.clean && g.triangleKinds && simulateDraws(g, s.stride) == 1;
}
std::uint32_t shapeDraws(const Shape& s, const BatchCaps& caps = {}) {
    std::uint32_t d = 0;
    for (const auto& g : s.groups) d += simulateDraws(g, s.stride, caps);
    return d;
}
// Adjacent merge in draw order (exact); returns draws and the list bytes a
// merged copy would hold.
std::uint32_t adjacentMerge(const Model& m, const std::vector<DrawItem>& list, std::uint64_t& mergedBytes) {
    std::uint32_t draws = 0;
    int head = -1;
    std::uint32_t v = 0, ix = 0, bytes = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& d = list[i];
        const auto& s = m.shapes[d.shape];
        bool join = false;
        if (mergeable(s) && head >= 0) {
            const auto& h = m.shapes[list[head].shape];
            const auto& g = s.groups[0];
            join = m.materials[list[head].material].remap == m.materials[d.material].remap && h.vcd == s.vcd &&
                   h.groups[0].useMtx == g.useMtx && v + g.vertices <= 4096 && ix + g.indices <= 16384 &&
                   (v + g.vertices) * s.stride <= 65536;
            if (join) { v += g.vertices; ix += g.indices; bytes += g.dlUsed; mergedBytes += g.dlUsed; }
        }
        if (join) continue;
        draws += shapeDraws(s);
        if (mergeable(s)) { head = int(i); v = s.groups[0].vertices; ix = s.groups[0].indices; bytes = s.groups[0].dlUsed; }
        else head = -1;
    }
    (void)bytes;
    return draws;
}
// Matrix groups of a skinned shape with a palette of P matrices: consecutive
// groups join while the matrices their vertices reference fit P.
std::uint32_t paletteDraws(const Bytes& f, const Model& m, const Shape& s, unsigned P) {
    std::uint32_t n = 0;
    const auto* shp1 = blockOf(f, "SHP1", n);
    const auto mtxTable = be32(shp1 + 28), initOff = be32(shp1 + 12), mtxInitOff = be32(shp1 + 36);
    const auto* d = shp1 + initOff + s.initIndex * 0x28;
    const unsigned mtxInit = be16(d + 6);
    std::vector<std::array<int, 10>> loads;
    std::vector<std::set<int>> uses;
    for (std::size_t g = 0; g < s.groups.size(); ++g) {
        const auto* mi = shp1 + mtxInitOff + (mtxInit + g) * 8;
        const unsigned cnt = be16(mi + 2), first = be32(mi + 4);
        std::array<int, 10> L;
        L.fill(-2);
        for (unsigned i = 0; i < cnt && i < 10; ++i) {
            const unsigned v = be16(shp1 + mtxTable + 2 * (first + i));
            L[i] = v == 0xFFFF ? -2 : int(v);
        }
        loads.push_back(L);
        std::set<int> U;
        const auto& G = s.groups[g];
        for (const auto& p : G.primitives)
            for (unsigned v = 0; v < p.count; ++v) U.insert(f[G.dlOffset + p.offset + 3 + v * s.stride] / 3);
        uses.push_back(U);
    }
    (void)m;
    std::uint32_t draws = 0;
    std::size_t g = 0;
    std::array<int, 10> slots;
    slots.fill(-1);
    while (g < loads.size()) {
        ++draws;
        std::set<int> mats;
        std::size_t h = g;
        auto e = slots;
        for (; h < loads.size(); ++h) {
            auto e2 = e;
            for (int i = 0; i < 10; ++i) if (loads[h][i] != -2) e2[i] = loads[h][i];
            auto m2 = mats;
            for (const int x : uses[h]) m2.insert(x < 10 ? e2[x] : -100 - x);
            if (m2.size() > P && h > g) break;
            mats = m2;
            e = e2;
        }
        slots = e;
        g = h;
    }
    return draws;
}
bool hasMatrixIndex(const Shape& s) {
    for (std::size_t k = 0; k + 8 <= s.vcd.size(); k += 8)
        if (be32(s.vcd.data() + k) == 0 && be32(s.vcd.data() + k + 4) != 0) return true;
    return false;
}
std::string json(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}
}

int main(int argc, char** argv) {
    std::string discPath, reportPath;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--disc") discPath = argv[i + 1];
        else if (a == "--report") reportPath = argv[i + 1];
    }
    if (discPath.empty() || reportPath.empty()) {
        std::fprintf(stderr, "usage: model_prep_survey --disc zelda.twilight --report out.json\n");
        return 2;
    }
    dusk::native::Disc disc;
    if (disc.open(discPath.c_str()) != dusk::native::DiscStatus::ready) { std::fprintf(stderr, "disc not readable\n"); return 1; }
    std::map<std::pair<std::string, unsigned>, Room> rooms;
    unsigned archives = 0, failed = 0;
    BatchCaps wide{16384, 256 * 1024, 64 * 1024};
    const bool walked = walkRoomModels(disc, [&](RoomModel& rm) {
        const auto m = parseModel(rm.bytes);
        if (!m.error.empty()) { ++failed; std::fprintf(stderr, "%s r%u %s: %s\n", rm.stage.c_str(), rm.room, rm.file.c_str(), m.error.c_str()); return; }
        auto& r = rooms[{rm.stage, rm.room}];
        r.stage = rm.stage; r.room = rm.room; ++r.models;
        const auto order = drawOrder(m);
        const auto sets = textureSets(rm.bytes, m);
        r.shapes += unsigned(m.shapes.size());
        r.opaque += unsigned(order.opaque.size());
        r.translucent += unsigned(order.translucent.size());
        for (const auto* list : {&order.opaque, &order.translucent}) {
            std::set<std::tuple<unsigned, Bytes, unsigned>> keys;
            std::set<std::vector<std::uint64_t>> texKeys;
            unsigned others = 0, textureOthers = 0;
            for (const auto& d : *list) {
                const auto& s = m.shapes[d.shape];
                r.draws += shapeDraws(s);
                r.drawsWideCaps += shapeDraws(s, wide);
                for (const auto& g : s.groups) r.dlBytes += g.dlSize;
                if (mergeable(s)) {
                    keys.insert({m.materials[d.material].remap, s.vcd, s.groups[0].useMtx});
                    texKeys.insert(sets[d.material]);
                } else { others += shapeDraws(s); textureOthers += shapeDraws(s); }
            }
            r.merge += adjacentMerge(m, *list, r.mergedDlBytes);
            r.reorder += unsigned(keys.size()) + others;
            r.textures += unsigned(texKeys.size()) + textureOthers;
        }
    }, archives);
    if (!walked) { std::fprintf(stderr, "disc walk failed\n"); return 1; }

    // Every model of the disc: skinned matrix groups and same-material neighbours.
    Skin all, objects;
    std::map<std::string, Skin> perArchive;
    dusk::native::DiscDirectory root;
    if (disc.findDirectory("/", root) != dusk::native::DiscFileStatus::ready) return 1;
    walkAllModels(disc, root, "", [&](const std::string& path, const Bytes& bytes) {
        const auto m = parseModel(bytes);
        if (!m.error.empty()) return;
        const auto order = drawOrder(m);
        const auto archive = path.substr(0, path.find(':'));
        const bool object = path.rfind("/res/Object/", 0) == 0;
        for (auto* s : {&all, object ? &objects : nullptr, &perArchive[archive]}) {
            if (!s) continue;
            ++s->models;
            for (const auto& shape : m.shapes) {
                ++s->shapes;
                s->groups += shape.groups.size();
                for (const auto& g : shape.groups) if (g.vertices < 64) ++s->tiny;
                if (shape.mtxType == 3 && hasMatrixIndex(shape) && !shape.directAttributes) {
                    ++s->skinnedShapes;
                    s->skinnedGroups += shape.groups.size();
                    s->p16 += paletteDraws(bytes, m, shape, 16);
                    s->p26 += paletteDraws(bytes, m, shape, 26);
                }
            }
            for (const auto* list : {&order.opaque, &order.translucent})
                for (std::size_t i = 1; i < list->size(); ++i) {
                    const auto& a = (*list)[i - 1];
                    const auto& b = (*list)[i];
                    if (m.materials[a.material].remap != m.materials[b.material].remap) continue;
                    ++s->sameMaterialPairs;
                    const auto& A = m.shapes[a.shape];
                    const auto& B = m.shapes[b.shape];
                    if (mergeable(A) && mergeable(B) && A.vcd == B.vcd && A.groups[0].useMtx == B.groups[0].useMtx) ++s->simplePairs;
                }
        }
    });

    Room total;
    for (const auto& [key, r] : rooms) {
        total.models += r.models; total.shapes += r.shapes; total.opaque += r.opaque; total.translucent += r.translucent;
        total.draws += r.draws; total.drawsWideCaps += r.drawsWideCaps; total.merge += r.merge; total.reorder += r.reorder;
        total.textures += r.textures; total.dlBytes += r.dlBytes; total.mergedDlBytes += r.mergedDlBytes;
    }
    FILE* out = std::fopen(reportPath.c_str(), "wx");
    if (!out) { std::fprintf(stderr, "report exists or cannot be written\n"); return 1; }
    auto roomJson = [&](const Room& r) {
        std::fprintf(out, "{\"stage\":%s,\"room\":%u,\"models\":%u,\"shapes\":%u,\"opaque\":%u,\"translucent\":%u,"
                     "\"draws\":%u,\"draws_wide_caps\":%u,\"merge_exact\":%u,\"reorder_bound\":%u,\"texture_bound\":%u,"
                     "\"dl_bytes\":%llu,\"merged_dl_bytes\":%llu}",
                     json(r.stage).c_str(), r.room, r.models, r.shapes, r.opaque, r.translucent, r.draws, r.drawsWideCaps,
                     r.merge, r.reorder, r.textures, (unsigned long long)r.dlBytes, (unsigned long long)r.mergedDlBytes);
    };
    auto skinJson = [&](const Skin& s) {
        std::fprintf(out, "{\"models\":%llu,\"shapes\":%llu,\"groups\":%llu,\"groups_under_64_vertices\":%llu,"
                     "\"skinned_shapes\":%llu,\"skinned_groups\":%llu,\"palette16\":%llu,\"palette26\":%llu,"
                     "\"same_material_neighbours\":%llu,\"simple_same_material_neighbours\":%llu}",
                     (unsigned long long)s.models, (unsigned long long)s.shapes, (unsigned long long)s.groups,
                     (unsigned long long)s.tiny, (unsigned long long)s.skinnedShapes, (unsigned long long)s.skinnedGroups,
                     (unsigned long long)s.p16, (unsigned long long)s.p26, (unsigned long long)s.sameMaterialPairs,
                     (unsigned long long)s.simplePairs);
    };
    std::fprintf(out, "{\"archives\":%u,\"rooms\":%zu,\"failed_models\":%u,\"room_total\":", archives, rooms.size(), failed);
    roomJson(total);
    std::fprintf(out, ",\"all_models\":");
    skinJson(all);
    std::fprintf(out, ",\"object_models\":");
    skinJson(objects);
    std::fprintf(out, ",\"rooms_detail\":[");
    bool first = true;
    for (const auto& [key, r] : rooms) { if (!first) std::fputc(',', out); first = false; std::fputc('\n', out); roomJson(r); }
    std::fprintf(out, "],\"archives_detail\":{");
    first = true;
    for (const auto& [name, s] : perArchive) {
        if (!s.skinnedGroups && !s.simplePairs) continue;
        if (!first) std::fputc(',', out);
        first = false;
        std::fprintf(out, "\n%s:", json(name).c_str());
        skinJson(s);
    }
    std::fprintf(out, "}}\n");
    std::fclose(out);

    std::printf("rooms %zu (archives %u, models %u, failed %u): shapes %u (opaque %u, translucent %u)\n", rooms.size(), archives,
                total.models, failed, total.shapes, total.opaque, total.translucent);
    std::printf("room draws at the runtime caps %u (%.1f per room); 16384-vertex caps %u\n", total.draws,
                double(total.draws) / rooms.size(), total.drawsWideCaps);
    std::printf("merge (exact, same material, adjacent) %u draws (-%u); reorder bound %u (-%u); texture bound %u (-%u)\n",
                total.merge, total.draws - total.merge, total.reorder, total.draws - total.reorder, total.textures,
                total.draws - total.textures);
    std::printf("room display lists %.1f MiB; a merged copy would hold %llu bytes\n", total.dlBytes / 1048576.0,
                (unsigned long long)total.mergedDlBytes);
    std::printf("all models %llu: shapes %llu, draws (groups) %llu, groups under 64 vertices %llu\n",
                (unsigned long long)all.models, (unsigned long long)all.shapes, (unsigned long long)all.groups,
                (unsigned long long)all.tiny);
    std::printf("skinned shapes %llu, groups %llu -> palette 16: %llu, palette 26: %llu\n", (unsigned long long)all.skinnedShapes,
                (unsigned long long)all.skinnedGroups, (unsigned long long)all.p16, (unsigned long long)all.p26);
    std::printf("same-material neighbours in draw order %llu (simple concatenation %llu)\n",
                (unsigned long long)all.sameMaterialPairs, (unsigned long long)all.simplePairs);
    for (const char* k : {"F_SP104/1", "F_SP103/0", "F_SP103/1", "F_SP00/0", "R_SP01/4", "F_SP108/1", "F_SP109/0", "D_MN05/0", "F_SP122/16"}) {
        const std::string s = k;
        const auto slash = s.find('/');
        const auto it = rooms.find({s.substr(0, slash), unsigned(std::stoul(s.substr(slash + 1)))});
        if (it == rooms.end()) continue;
        const auto& r = it->second;
        std::printf("  %-8s r%-2u shapes %3u draws %3u wide-caps %3u merge %3u reorder %3u textures %3u\n", r.stage.c_str(), r.room,
                    r.shapes, r.draws, r.drawsWideCaps, r.merge, r.reorder, r.textures);
    }
    for (const char* k : {"/res/Object/Cow.arc", "/res/Object/Kmdl.arc", "/res/Object/Alink.arc", "/res/Object/Horse.arc", "/res/Object/Midna.arc"}) {
        const auto it = perArchive.find(k);
        if (it == perArchive.end()) continue;
        const auto& s = it->second;
        std::printf("  %-22s shapes %3llu groups %3llu skinned groups %3llu -> p16 %3llu p26 %3llu, simple same-material neighbours %llu\n", k,
                    (unsigned long long)s.shapes, (unsigned long long)s.groups, (unsigned long long)s.skinnedGroups,
                    (unsigned long long)s.p16, (unsigned long long)s.p26, (unsigned long long)s.simplePairs);
    }
    return failed ? 1 : 0;
}
