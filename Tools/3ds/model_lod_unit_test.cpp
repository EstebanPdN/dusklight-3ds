// Mac-only unit test of the level-of-detail pieces that need no disc
// (Tools/3ds/model_lod.py --unit): the simplifier keeps every promise its
// header makes on small meshes whose answer is known, and the runtime's
// record parser refuses every malformed record it is shown.
#include "model_lod_batches.hpp"
#include "model_lod_simplify.hpp"
#include "../../Source/platforms/3ds/runtime/model_lod_store.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

using namespace modellod;

namespace {
// An n x n grid of quads on z = height(x, y), one record per point.
Mesh grid(unsigned n, double (*height)(double, double)) {
    Mesh m;
    for (unsigned y = 0; y <= n; ++y)
        for (unsigned x = 0; x <= n; ++x) {
            m.position.push_back({double(x), double(y), height(x, y)});
            m.skin.push_back(0);
            m.locked.push_back(0);
            m.normal.push_back({0, 0, 1});
            m.color.push_back({1, 1, 1, 1});
        }
    for (unsigned y = 0; y < n; ++y)
        for (unsigned x = 0; x < n; ++x) {
            const std::uint32_t a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            m.triangles.push_back({a, b, d});
            m.triangles.push_back({a, d, c});
        }
    return m;
}
double flat(double, double) { return 0; }
double bumpy(double x, double y) { return ((int(x) * 7 + int(y) * 13) % 5) * 0.3; }
std::set<std::uint32_t> used(const Level& l) {
    std::set<std::uint32_t> v;
    for (const auto& t : l.triangles) v.insert(t.begin(), t.end());
    return v;
}
void putLe(std::vector<std::uint8_t>& o, std::uint64_t v, unsigned bytes) { for (unsigned i = 0; i < bytes; ++i) o.push_back(std::uint8_t(v >> (8 * i))); }
// One shape, one group, one batch of 4 records; full list of 2 triangles,
// level 1 one triangle.
std::vector<std::uint8_t> record(std::uint16_t badIndex = 0, bool badCount = false) {
    std::vector<std::uint8_t> r;
    putLe(r, dusk::native::modelLodRecordMagic, 4); putLe(r, 1, 2); putLe(r, 1, 2); putLe(r, 1, 4); putLe(r, 3, 4);
    putLe(r, 0, 2); putLe(r, 1, 1); putLe(r, 1, 1);
    const float e = 0.5f; std::uint32_t ev; std::memcpy(&ev, &e, 4); putLe(r, ev, 4); putLe(r, 0, 4); putLe(r, 0, 4);
    putLe(r, 0x1122334455667788ull, 8); putLe(r, 64, 4); putLe(r, 0, 2); putLe(r, 1, 2);
    const std::uint16_t full[6] = {0, 1, 2, 0, 2, 3};
    putLe(r, 4, 2); putLe(r, 0, 2); putLe(r, 6, 4); putLe(r, dusk::native::modelLodIndexHash(full, 6), 4);
    putLe(r, badCount ? 4 : 3, 4); putLe(r, 0, 4); putLe(r, 0, 4);
    putLe(r, 0, 2); putLe(r, 1, 2); putLe(r, badIndex ? badIndex : 3, 2);
    return r;
}
}

int main() {
    // A flat plane loses triangles at no error, and its corners never move
    // (they are border points that cannot slide anywhere).
    {
        const Mesh m = grid(8, flat);
        const auto levels = simplify(m, {});
        assert(levels.size() == 3);
        for (const auto& l : levels) {
            assert(l.error < 1e-9);
            const auto v = used(l);
            for (std::uint32_t corner : {0u, 8u, 72u, 80u}) assert(v.count(corner));
            for (const auto& t : l.triangles) for (auto i : t) assert(i < m.position.size());
        }
        assert(levels[0].triangles.size() <= 64 && levels[2].triangles.size() <= 16);
    }
    // Locked points stay; collapses never cross skins.
    {
        Mesh m = grid(8, bumpy);
        for (std::uint32_t v = 0; v < m.position.size(); ++v) {
            m.locked[v] = (v % 9) == 4;
            m.skin[v] = m.position[v][0] < 4 ? 1 : 2;
        }
        const auto levels = simplify(m, {});
        assert(!levels.empty());
        for (const auto& l : levels) {
            const auto v = used(l);
            for (std::uint32_t p = 0; p < m.position.size(); ++p) if (m.locked[p]) assert(v.count(p));
            assert(l.error > 0);
        }
    }
    // A texture seam: the grid's middle column has two records per point
    // (left and right halves); every reduced triangle keeps its side's record.
    {
        Mesh m = grid(8, flat);
        const std::uint32_t base = std::uint32_t(m.position.size());
        for (unsigned y = 0; y <= 8; ++y) {
            m.position.push_back(m.position[y * 9 + 4]);
            m.skin.push_back(0); m.locked.push_back(0); m.normal.push_back({0, 0, 1}); m.color.push_back({1, 1, 1, 1});
        }
        // Triangles right of x = 4 use the right records of the seam.
        for (auto& t : m.triangles) {
            double cx = 0;
            for (auto i : t) cx += m.position[i][0] / 3;
            if (cx > 4) for (auto& i : t) if (m.position[i][0] == 4 && i < base) i = base + std::uint32_t(m.position[i][1]);
        }
        const auto levels = simplify(m, {});
        assert(!levels.empty());
        for (const auto& l : levels)
            for (const auto& t : l.triangles) {
                double cx = 0;
                for (auto i : t) cx += m.position[i][0] / 3;
                for (auto i : t) if (m.position[i][0] == 4) assert((cx > 4) == (i >= base));
            }
    }
    // Determinism.
    {
        const Mesh m = grid(12, bumpy);
        const auto a = simplify(m, {}), b = simplify(m, {});
        assert(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) assert(a[i].triangles == b[i].triangles && a[i].error == b[i].error);
    }
    // The runtime's batching of a list: two strips are one draw with shared
    // records; a lone strip keeps its raw vertices.
    {
        // Stride 1 (one INDEX8 position). Strip 0 1 2 3, strip 2 3 4 5.
        const std::uint8_t two[] = {0x98, 0, 4, 0, 1, 2, 3, 0x98, 0, 4, 2, 3, 4, 5, 0, 0};
        std::vector<RuntimeBatch> out;
        assert(runtimeBatches(two, sizeof(two), 1, out) && out.size() == 1);
        assert(out[0].tuple.size() == 6 && out[0].indices.size() == 12);
        assert((out[0].indices == std::vector<std::uint16_t>{0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5}));
        const std::uint8_t one[] = {0x98, 0, 4, 9, 8, 9, 7};
        assert(runtimeBatches(one, sizeof(one), 1, out) && out.size() == 1 && out[0].tuple.size() == 4);
        assert((out[0].indices == std::vector<std::uint16_t>{0, 1, 2, 2, 1, 3}));
    }
    // Record parser: the good record, then each defect refused.
    {
        dusk::native::ModelLodRecord r;
        auto good = record();
        assert(r.parse(good) && r.shapes() == 1 && r.groups() == 1 && r.batches() == 1);
        const auto b = r.batch(0);
        assert(b.vertexCount == 4 && b.fullIndices == 6 && b.count[0] == 3 && b.indices[0][2] == 3 && !b.indices[1]);
        assert(r.shape(0).levels == 1 && r.shape(0).error[0] == 0.5f && r.group(0).listBytes == 64);
        auto badIndex = record(4);
        assert(!r.parse(badIndex));                       // index past the batch's records
        auto badCount = record(0, true);
        assert(!r.parse(badCount));                       // not whole triangles / past the words
        auto shortRecord = good;
        shortRecord.pop_back();
        assert(!r.parse(shortRecord));
        auto badMagic = good;
        badMagic[0] ^= 1;
        assert(!r.parse(badMagic));
        auto noBatches = good;
        noBatches[8] = 0;                                 // a shape with levels needs its batches
        assert(!r.parse(noBatches));
    }
    std::printf("model_lod_unit_test: ok\n");
    return 0;
}
