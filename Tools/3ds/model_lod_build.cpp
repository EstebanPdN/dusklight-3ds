// Mac-only: levels of detail for every J3D model of the owner's disc
// (Tools/3ds/model_lod.py builds and runs it). Writes the store the runtime
// reads (runtime/model_lod_store.hpp, sdmc:/3ds/DuskLight/zelda.lod) and a
// JSON report with, per stage, the triangles before and at each level.
//
// Per model: every display list is cut into the runtime's draws
// (model_lod_batches.hpp); each draw is reduced on its own vertex records
// (model_lod_simplify.hpp) with
//   - positions in bind-pose model space (rigid draw matrices through their
//     joint's bind transform, envelopes as stored), so that triangles across
//     joints measure true distances;
//   - every point shared with another draw of the model locked (material
//     borders between shapes, skinning groups, batches of one list);
//   - in alpha-tested materials, every card (a piece of at most four points)
//     locked: cards are never thinned out;
//   - billboards (they face the camera) and shapes under 64 triangles left
//     whole.
// A shape gets up to three levels (1/2, 1/4, 1/8 of its triangles); a level
// is kept only when it has at most 3/4 of the indices of the one before.
// Deterministic: models in disc order, keys sorted, no clocks, no hashing of
// pointers.
#include "model_prep_walk.hpp"
#include "model_lod_batches.hpp"
#include "model_lod_simplify.hpp"
#include "../../Source/platforms/3ds/runtime/model_lod_store.hpp"
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace modelprep;
using dusk::native::modelLodLevels;

namespace {
struct Joint { int parent = -1; double local[12]{}; double world[12]{}; };

void identity(double m[12]) { for (int i = 0; i < 12; ++i) m[i] = (i % 5 == 0) ? 1.0 : 0.0; }
void concat(const double a[12], const double b[12], double out[12]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = c == 3 ? a[r * 4 + 3] : 0.0;
            for (int k = 0; k < 3; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}
modellod::Vec3 transformPoint(const double m[12], const modellod::Vec3& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

// What model_prep's parse leaves out: joint bind transforms, the matrix
// tables of the shape groups, the materials' alpha test.
struct Extra {
    std::vector<Joint> joints;
    std::vector<std::uint16_t> mtxTable;
    std::vector<std::vector<std::uint32_t>> groupFirst;   // per shape, per group: first table entry
    std::vector<std::uint8_t> alphaTest;                  // per material
    bool ok = false;
};
Extra parseExtra(const Bytes& f, const Model& m) {
    Extra x;
    std::uint32_t inf1n = 0, jnt1n = 0, shp1n = 0, mat3n = 0;
    const auto* inf1 = blockOf(f, "INF1", inf1n);
    const auto* jnt1 = blockOf(f, "JNT1", jnt1n);
    const auto* shp1 = blockOf(f, "SHP1", shp1n);
    const auto* mat3 = blockOf(f, "MAT3", mat3n);
    if (!inf1 || !jnt1 || !shp1 || !mat3) return x;
    // Joints: J3DJointInitData (0x40 bytes), J3DGetTranslateRotateMtx, scale.
    const unsigned joints = be16(jnt1 + 8);
    const auto initOff = be32(jnt1 + 0x0C), indexOff = be32(jnt1 + 0x10);
    x.joints.resize(joints);
    for (unsigned j = 0; j < joints; ++j) {
        if (indexOff + 2 * j + 2 > jnt1n) return x;
        const std::uint64_t at = initOff + std::uint64_t(be16(jnt1 + indexOff + 2 * j)) * 0x40;
        if (at + 0x40 > jnt1n) return x;
        const auto* d = jnt1 + at;
        const double sx = bef(d + 4), sy = bef(d + 8), sz = bef(d + 12);
        const double k = 3.14159265358979323846 / 32768.0;
        const double rx = std::int16_t(be16(d + 16)) * k, ry = std::int16_t(be16(d + 18)) * k, rz = std::int16_t(be16(d + 20)) * k;
        const double tx = bef(d + 24), ty = bef(d + 28), tz = bef(d + 32);
        const double cx = std::cos(rx), sxr = std::sin(rx), cy = std::cos(ry), syr = std::sin(ry), cz = std::cos(rz), szr = std::sin(rz);
        double r[12];
        r[8] = -syr; r[0] = cz * cy; r[4] = szr * cy; r[9] = cy * sxr; r[10] = cy * cx;
        r[1] = sxr * cz * syr - cx * szr; r[6] = cx * szr * syr - sxr * cz;
        r[2] = cx * cz * syr + sxr * szr; r[5] = sxr * szr * syr + cx * cz;
        r[3] = tx; r[7] = ty; r[11] = tz;
        for (int row = 0; row < 3; ++row) { r[row * 4 + 0] *= sx; r[row * 4 + 1] *= sy; r[row * 4 + 2] *= sz; }
        std::copy(r, r + 12, x.joints[j].local);
    }
    // Parents from the INF1 hierarchy (the joint that opened the scope).
    {
        std::uint32_t at = be32(inf1 + 20);
        std::vector<int> scope;
        int pJoint = -1, curJoint = -1;
        for (unsigned guard = 0; guard < 1u << 20; ++guard) {
            if (at + 4 > inf1n) return x;
            const unsigned type = be16(inf1 + at), value = be16(inf1 + at + 2);
            at += 4;
            if (type == 0) break;
            if (type == 1) { scope.push_back(pJoint); pJoint = curJoint; continue; }
            if (type == 2) { if (scope.empty()) break; curJoint = pJoint; pJoint = scope.back(); scope.pop_back(); continue; }
            if (type == 0x10) { if (value >= joints) return x; x.joints[value].parent = pJoint; curJoint = int(value); }
        }
    }
    std::vector<std::uint8_t> done(joints, 0);
    for (unsigned pass = 0; pass < joints; ++pass)
        for (unsigned j = 0; j < joints; ++j) {
            if (done[j]) continue;
            const int p = x.joints[j].parent;
            if (p < 0) { std::copy(x.joints[j].local, x.joints[j].local + 12, x.joints[j].world); done[j] = 1; }
            else if (unsigned(p) < joints && done[p]) { concat(x.joints[p].world, x.joints[j].local, x.joints[j].world); done[j] = 1; }
        }
    for (unsigned j = 0; j < joints; ++j) if (!done[j]) identity(x.joints[j].world);
    // Shape matrix tables (J3DShapeMtxInitData: u16 use, u16 count, u32 first).
    {
        const auto initOffS = be32(shp1 + 12), tableOff = be32(shp1 + 28), mtxInitOff = be32(shp1 + 36);
        const unsigned count = be16(shp1 + 8);
        const auto indexOffS = be32(shp1 + 16);
        for (std::uint32_t at = tableOff; at + 2 <= shp1n && at < mtxInitOff; at += 2) x.mtxTable.push_back(be16(shp1 + at));
        x.groupFirst.resize(count);
        for (unsigned s = 0; s < count; ++s) {
            const std::uint64_t init = initOffS + std::uint64_t(be16(shp1 + indexOffS + 2 * s)) * 0x28;
            const auto* d = shp1 + init;
            const unsigned groups = be16(d + 2), mtxInit = be16(d + 6);
            for (unsigned g = 0; g < groups; ++g) {
                const std::uint64_t mi = mtxInitOff + std::uint64_t(mtxInit + g) * 8;
                if (mi + 8 > shp1n) return x;
                x.groupFirst[s].push_back(be32(shp1 + mi + 4));
            }
        }
    }
    // Alpha test: J3DMaterialInitData alpha comp index at 0x146, table at 0x6C.
    {
        const auto initOffM = be32(mat3 + 12), alphaOff = be32(mat3 + 0x6C);
        for (const auto& mat : m.materials) {
            const std::uint64_t init = initOffM + std::uint64_t(mat.remap) * 0x14C;
            bool tested = false;
            if (alphaOff && init + 0x148 <= mat3n) {
                const unsigned idx = be16(mat3 + init + 0x146);
                const std::uint64_t a = alphaOff + std::uint64_t(idx) * 8;
                if (idx != 0xFFFF && a + 5 <= mat3n) {
                    const unsigned comp0 = mat3[a], op = mat3[a + 2], comp1 = mat3[a + 3];
                    const bool always = (op == 0 && comp0 == 7 && comp1 == 7) || (op == 1 && (comp0 == 7 || comp1 == 7));
                    tested = !always;
                }
            }
            x.alphaTest.push_back(tested);
        }
    }
    x.ok = true;
    return x;
}

double component(const std::uint8_t* p, unsigned type, unsigned frac) {
    double v = 0;
    switch (type) {
    case 0: v = p[0]; break;
    case 1: v = std::int8_t(p[0]); break;
    case 2: v = be16(p); break;
    case 3: v = std::int16_t(be16(p)); break;
    case 4: return bef(p);
    default: return 0;
    }
    return v / double(1u << frac);
}
unsigned componentBytes(unsigned type) { return type <= 1 ? 1 : type <= 3 ? 2 : 4; }

// One draw of a shape: what the simplifier needs and how to write it back.
struct Unit {
    unsigned shape = 0, group = 0, batch = 0;
    modellod::RuntimeBatch draw;
    modellod::Mesh mesh;
    std::vector<modellod::Level> levels;
};
struct ShapeOut {
    unsigned levels = 0;
    std::array<double, modelLodLevels> error{};
    std::vector<unsigned> units;   // indices into the model's units, in group/batch order
    std::vector<std::array<std::vector<std::uint16_t>, modelLodLevels>> levelIndices; // per unit
};
struct Totals {
    std::uint64_t models = 0, shapes = 0, shapesWithLevels = 0, full = 0;
    std::array<std::uint64_t, modelLodLevels> level{};
    std::uint64_t lockedPoints = 0, points = 0, cards = 0;
};

std::string stageOf(const std::string& path) {
    // "/res/Stage/F_SP103/R00_00.arc:..." -> F_SP103; "/res/Object/Cow.arc:..." -> Object
    const std::string stagePrefix = "/res/Stage/";
    if (path.rfind(stagePrefix, 0) == 0) {
        const auto end = path.find('/', stagePrefix.size());
        return path.substr(stagePrefix.size(), end == std::string::npos ? std::string::npos : end - stagePrefix.size());
    }
    const auto a = path.find('/', 1), b = a == std::string::npos ? a : path.find('/', a + 1);
    return b == std::string::npos ? path : path.substr(a + 1, b - a - 1);
}

void putLe(std::vector<std::uint8_t>& o, std::uint64_t v, unsigned bytes) { for (unsigned i = 0; i < bytes; ++i) o.push_back(std::uint8_t(v >> (8 * i))); }
void putF(std::vector<std::uint8_t>& o, float f) { std::uint32_t v; std::memcpy(&v, &f, 4); putLe(o, v, 4); }

struct Built { std::uint64_t key = 0; std::uint32_t lists = 0, listBytes = 0; std::vector<std::uint8_t> record; };

// Levels of one model. Returns false when it has none worth storing.
bool crossSkin = false;
bool buildModel(const Bytes& f, const Model& m, Built& out, Totals& totals, const std::string& name, std::FILE* log) {
    const Extra x = parseExtra(f, m);
    if (!x.ok) return false;
    // Key over every list, in shape and group order (what J3DModelData holds).
    std::uint64_t key = dusk::native::modelLodKeySeed;
    std::uint32_t lists = 0, listBytes = 0;
    for (const auto& s : m.shapes)
        for (const auto& g : s.groups) {
            key = dusk::native::modelLodCombine(key, dusk::native::modelLodListHash(f.data() + g.dlOffset, g.dlSize), g.dlSize);
            ++lists; listBytes += g.dlSize;
        }
    out.key = key; out.lists = lists; out.listBytes = listBytes;
    ++totals.models;
    totals.shapes += m.shapes.size();

    const auto& fmtPos = m.formats[9];
    const auto& fmtNrm = m.formats[10];
    const auto& fmtClr = m.formats[11];
    if (!fmtPos.present || !m.arrayBytes[9]) return false;

    std::vector<Unit> units;
    std::vector<ShapeOut> shapesOut(m.shapes.size());
    // A shape whose lists are not all cut into draws the runtime records
    // keeps its full lists (its batch numbers would not match).
    std::vector<std::uint8_t> broken(m.shapes.size(), 0);
    // Pass 1: units, decoded points.
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        const auto& s = m.shapes[si];
        if (s.directAttributes || (s.mtxType != 0 && s.mtxType != 3) || s.material < 0) continue;
        // Tuple layout in attribute order.
        int offPnmtx = -1, offPos = -1, offNrm = -1, offClr = -1;
        unsigned sizePos = 0, sizeNrm = 0, sizeClr = 0;
        {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> vcd;
            for (std::size_t k = 0; k + 8 <= s.vcd.size(); k += 8) {
                const auto attr = be32(s.vcd.data() + k), type = be32(s.vcd.data() + k + 4);
                if (attr == 0xFF) break;
                vcd.push_back({attr, type});
            }
            std::sort(vcd.begin(), vcd.end());
            unsigned at = 0;
            for (const auto& [attr, type] : vcd) {
                bool direct = false;
                const unsigned n = attributeBytes(attr, type, direct);
                if (attr == 0) offPnmtx = int(at);
                if (attr == 9) { offPos = int(at); sizePos = n; }
                if (attr == 10) { offNrm = int(at); sizeNrm = n; }
                if (attr == 11) { offClr = int(at); sizeClr = n; }
                at += n;
            }
            if (at != s.stride || offPos < 0) continue;
        }
        const bool tested = unsigned(s.material) < x.alphaTest.size() && x.alphaTest[s.material];
        std::array<int, 10> slotDrw;
        slotDrw.fill(-1);
        for (unsigned gi = 0; gi < s.groups.size(); ++gi) {
            const auto& g = s.groups[gi];
            // The draw matrix of every slot (multi: table entries, 0xFFFF keeps the slot).
            if (s.mtxType == 3) {
                const std::uint32_t first = gi < x.groupFirst[si].size() ? x.groupFirst[si][gi] : 0;
                for (unsigned k = 0; k < g.useMtxCount && k < 10; ++k)
                    if (first + k < x.mtxTable.size() && x.mtxTable[first + k] != 0xFFFF) slotDrw[k] = x.mtxTable[first + k];
            } else slotDrw.fill(int(g.useMtx));
            std::vector<modellod::RuntimeBatch> draws;
            if (!modellod::runtimeBatches(f.data() + g.dlOffset, g.dlSize, s.stride, draws)) { broken[si] = 1; continue; }
            for (unsigned bi = 0; bi < draws.size(); ++bi) {
                Unit u;
                u.shape = si; u.group = gi; u.batch = bi;
                u.draw = std::move(draws[bi]);
                const auto& t = u.draw.tuple;
                auto& mesh = u.mesh;
                bool bad = !u.draw.triangles;
                for (const auto offset : t) {
                    const std::uint8_t* tuple = f.data() + g.dlOffset + offset;
                    const unsigned posIndex = sizePos == 2 ? be16(tuple + offPos) : tuple[offPos];
                    const unsigned slot = offPnmtx >= 0 ? tuple[offPnmtx] / 3 : 0;
                    const int drw = slot < 10 ? slotDrw[slot] : -1;
                    const unsigned comps = fmtPos.count ? 3 : 2, cb = componentBytes(fmtPos.type);
                    const std::uint64_t at = m.arrayOffset[9] + std::uint64_t(posIndex) * comps * cb;
                    if (at + comps * cb > f.size()) { bad = true; break; }
                    modellod::Vec3 p{component(f.data() + at, fmtPos.type, fmtPos.frac),
                                     component(f.data() + at + cb, fmtPos.type, fmtPos.frac),
                                     comps == 3 ? component(f.data() + at + 2 * cb, fmtPos.type, fmtPos.frac) : 0.0};
                    if (drw >= 0 && unsigned(drw) < m.drawWeighted.size() && !m.drawWeighted[drw] &&
                        m.drawIndex[drw] < x.joints.size()) p = transformPoint(x.joints[m.drawIndex[drw]].world, p);
                    mesh.position.push_back(p);
                    mesh.skin.push_back(drw < 0 ? 0xFFFFFFFFu : std::uint32_t(drw));
                    mesh.locked.push_back(0);
                    modellod::Vec3 n{0, 0, 0};
                    if (offNrm >= 0 && fmtNrm.present && m.arrayBytes[10]) {
                        const unsigned ni = sizeNrm == 2 ? be16(tuple + offNrm) : tuple[offNrm];
                        const unsigned nb = componentBytes(fmtNrm.type);
                        const std::uint64_t na = m.arrayOffset[10] + std::uint64_t(ni) * 3 * nb;
                        if (na + 3 * nb <= f.size())
                            n = {component(f.data() + na, fmtNrm.type, fmtNrm.frac), component(f.data() + na + nb, fmtNrm.type, fmtNrm.frac),
                                 component(f.data() + na + 2 * nb, fmtNrm.type, fmtNrm.frac)};
                        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (l > 0) n = {n[0] / l, n[1] / l, n[2] / l};
                    }
                    mesh.normal.push_back(n);
                    std::array<double, 4> c{0, 0, 0, 0};
                    if (offClr >= 0 && fmtClr.present && m.arrayBytes[11]) {
                        const unsigned ci = sizeClr == 2 ? be16(tuple + offClr) : tuple[offClr];
                        static const unsigned bytesOf[6] = {2, 3, 4, 2, 3, 4};
                        const unsigned cb2 = fmtClr.type < 6 ? bytesOf[fmtClr.type] : 4;
                        const std::uint64_t ca = m.arrayOffset[11] + std::uint64_t(ci) * cb2;
                        if (ca + cb2 <= f.size()) {
                            const auto* q = f.data() + ca;
                            if (fmtClr.type == 1 || fmtClr.type == 2) c = {q[0] / 255.0, q[1] / 255.0, q[2] / 255.0, 1.0};
                            else if (fmtClr.type == 5) c = {q[0] / 255.0, q[1] / 255.0, q[2] / 255.0, q[3] / 255.0};
                            else if (fmtClr.type == 0) { const unsigned v = be16(q); c = {(v >> 11) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0, 1.0}; }
                            else if (fmtClr.type == 3) { const unsigned v = be16(q); c = {(v >> 12) / 15.0, ((v >> 8) & 15) / 15.0, ((v >> 4) & 15) / 15.0, (v & 15) / 15.0}; }
                            else { const unsigned v = (unsigned(q[0]) << 16) | (unsigned(q[1]) << 8) | q[2];
                                   c = {(v >> 18) / 63.0, ((v >> 12) & 63) / 63.0, ((v >> 6) & 63) / 63.0, (v & 63) / 63.0}; }
                        }
                    }
                    mesh.color.push_back(c);
                }
                if (bad) { broken[si] = 1; continue; }
                for (std::size_t i = 0; i + 2 < u.draw.indices.size(); i += 3) {
                    const auto a = u.draw.indices[i], b = u.draw.indices[i + 1], c = u.draw.indices[i + 2];
                    if (a == b || b == c || a == c) continue;
                    mesh.triangles.push_back({a, b, c});
                }
                // Cards of alpha-tested materials: pieces of at most four points.
                if (tested) {
                    std::vector<std::uint32_t> parent(mesh.position.size());
                    for (std::uint32_t v = 0; v < parent.size(); ++v) parent[v] = v;
                    auto root = [&](std::uint32_t v) { while (parent[v] != v) v = parent[v] = parent[parent[v]]; return v; };
                    for (const auto& tr : mesh.triangles) { parent[root(tr[1])] = root(tr[0]); parent[root(tr[2])] = root(tr[0]); }
                    std::map<std::uint32_t, std::set<std::tuple<double, double, double>>> piece;
                    for (std::uint32_t v = 0; v < parent.size(); ++v)
                        piece[root(v)].insert({mesh.position[v][0], mesh.position[v][1], mesh.position[v][2]});
                    for (std::uint32_t v = 0; v < parent.size(); ++v)
                        if (piece[root(v)].size() <= 4) mesh.locked[v] = 1;
                    for (const auto& [r, pts] : piece) if (pts.size() <= 4) ++totals.cards;
                }
                units.push_back(std::move(u));
                shapesOut[si].units.push_back(unsigned(units.size() - 1));
            }
        }
    }
    // Lock points shared with another shape (material borders): shapes are
    // reduced one by one. Inside a shape, borders between its skinning
    // groups and batches move only where every draw that holds the point
    // moves with it (the simplifier's wedge rule over the joint mesh).
    {
        std::map<std::tuple<long long, long long, long long>, std::set<unsigned>> owners;
        auto q = [](const modellod::Vec3& p) {
            return std::make_tuple(std::llround(p[0] * 256), std::llround(p[1] * 256), std::llround(p[2] * 256));
        };
        for (unsigned ui = 0; ui < units.size(); ++ui)
            for (const auto& p : units[ui].mesh.position) owners[q(p)].insert(units[ui].shape);
        for (unsigned ui = 0; ui < units.size(); ++ui) {
            auto& mesh = units[ui].mesh;
            for (std::size_t v = 0; v < mesh.position.size(); ++v) {
                if (owners[q(mesh.position[v])].size() > 1) mesh.locked[v] = 1;
                totals.lockedPoints += mesh.locked[v];
                ++totals.points;
            }
        }
    }
    // Pass 2: simplify per shape, all its draws as one mesh.
    bool any = false;
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        auto& so = shapesOut[si];
        std::uint64_t shapeFull = 0;
        for (const auto ui : so.units) shapeFull += units[ui].draw.indices.size();
        totals.full += shapeFull / 3;
        // Every list of the shape must be cut (or the shape draws whole).
        bool complete = !so.units.empty();
        {
            std::set<unsigned> groupsSeen;
            for (const auto ui : so.units) groupsSeen.insert(units[ui].group);
            complete = complete && !broken[si] && groupsSeen.size() == m.shapes[si].groups.size();
        }
        if (!complete || shapeFull / 3 < 64) { for (unsigned l = 0; l < modelLodLevels; ++l) totals.level[l] += shapeFull / 3; continue; }
        const double radius = std::max<double>(m.shapes[si].radius, 1e-3);
        modellod::Options options;
        // Coarse levels are chosen only where their error projects under a
        // pixel: a villager 12 pixels tall may lose 10% of its outline.
        options.maxError = 0.4 * radius;
        options.attributeWeight = (0.02 * radius) * (0.02 * radius);
        options.crossSkin = crossSkin;
        std::array<std::uint64_t, 8> refusals{};
        if (log) options.refusals = &refusals;
        modellod::Mesh joint;
        std::vector<std::uint32_t> base, unitOf;
        for (const auto ui : so.units) {
            const auto& mesh = units[ui].mesh;
            base.push_back(std::uint32_t(joint.position.size()));
            joint.position.insert(joint.position.end(), mesh.position.begin(), mesh.position.end());
            joint.skin.insert(joint.skin.end(), mesh.skin.begin(), mesh.skin.end());
            joint.locked.insert(joint.locked.end(), mesh.locked.begin(), mesh.locked.end());
            joint.normal.insert(joint.normal.end(), mesh.normal.begin(), mesh.normal.end());
            joint.color.insert(joint.color.end(), mesh.color.begin(), mesh.color.end());
            unitOf.insert(unitOf.end(), mesh.position.size(), ui);
            for (const auto& t : mesh.triangles) joint.triangles.push_back({t[0] + base.back(), t[1] + base.back(), t[2] + base.back()});
        }
        const auto jointLevels = modellod::simplify(joint, options);
        for (unsigned n = 0; n < so.units.size(); ++n) {
            auto& lv = units[so.units[n]].levels;
            lv.assign(jointLevels.size(), {});
            for (std::size_t l = 0; l < jointLevels.size(); ++l) {
                lv[l].error = jointLevels[l].error;
                for (const auto& t : jointLevels[l].triangles)
                    if (unitOf[t[0]] == so.units[n])
                        lv[l].triangles.push_back({t[0] - base[n], t[1] - base[n], t[2] - base[n]});
            }
        }
        // Shape level l: each draw at its own level min(l, its last); its
        // error the largest of them.
        std::array<std::uint64_t, modelLodLevels> count{};
        std::array<double, modelLodLevels> error{};
        for (unsigned l = 0; l < modelLodLevels; ++l)
            for (const auto ui : so.units) {
                const auto& lv = units[ui].levels;
                if (lv.empty()) { count[l] += units[ui].draw.indices.size(); continue; }
                const auto& use = lv[std::min<std::size_t>(l, lv.size() - 1)];
                count[l] += use.triangles.size() * 3;
                error[l] = std::max(error[l], use.error);
            }
        std::vector<unsigned> kept;
        std::uint64_t previous = shapeFull;
        for (unsigned l = 0; l < modelLodLevels; ++l)
            if (count[l] * 4 <= previous * 3) { kept.push_back(l); previous = count[l]; }
        if (kept.empty()) { for (unsigned l = 0; l < modelLodLevels; ++l) totals.level[l] += shapeFull / 3; continue; }
        so.levels = unsigned(kept.size());
        so.levelIndices.resize(so.units.size());
        for (unsigned k = 0; k < kept.size(); ++k) {
            so.error[k] = error[kept[k]];
            for (unsigned n = 0; n < so.units.size(); ++n) {
                const auto& lv = units[so.units[n]].levels;
                if (lv.empty()) continue; // full list at every level
                const auto& use = lv[std::min<std::size_t>(kept[k], lv.size() - 1)];
                if (use.triangles.size() * 3 >= units[so.units[n]].draw.indices.size()) continue;
                auto& out = so.levelIndices[n][k];
                for (const auto& t : use.triangles) for (int c = 0; c < 3; ++c) out.push_back(std::uint16_t(t[c]));
            }
        }
        for (unsigned l = 0; l < modelLodLevels; ++l) totals.level[l] += (l < kept.size() ? count[kept[l]] : count[kept.back()]) / 3;
        ++totals.shapesWithLevels;
        any = true;
        if (log) std::fprintf(log, "%s shape %u: %llu tris, levels", name.c_str(), si, (unsigned long long)(shapeFull / 3));
        if (log) for (unsigned k = 0; k < kept.size(); ++k) std::fprintf(log, " %llu (%.2f)", (unsigned long long)(count[kept[k]] / 3), error[kept[k]]);
        if (log) std::fprintf(log, "  refused: locked %llu skin %llu edge %llu border %llu link %llu wedge %llu flip %llu\n",
                              (unsigned long long)refusals[0], (unsigned long long)refusals[1], (unsigned long long)refusals[2],
                              (unsigned long long)refusals[3], (unsigned long long)refusals[4], (unsigned long long)refusals[5],
                              (unsigned long long)refusals[6]);
    }
    if (!any) return false;
    // Record: every shape and group (the runtime checks each list), batches
    // only for shapes with levels.
    auto& r = out.record;
    unsigned groupTotal = 0, batchTotal = 0;
    std::uint32_t words = 0;
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        groupTotal += unsigned(m.shapes[si].groups.size());
        if (shapesOut[si].levels) {
            batchTotal += unsigned(shapesOut[si].units.size());
            for (const auto& li : shapesOut[si].levelIndices) for (const auto& v : li) words += std::uint32_t(v.size());
        }
    }
    if (groupTotal > 0xFFFF || m.shapes.size() > 0xFFFF) return false;
    putLe(r, dusk::native::modelLodRecordMagic, 4);
    putLe(r, m.shapes.size(), 2);
    putLe(r, groupTotal, 2);
    putLe(r, batchTotal, 4);
    putLe(r, words, 4);
    unsigned g0 = 0;
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        putLe(r, g0, 2);
        putLe(r, m.shapes[si].groups.size(), 1);
        putLe(r, shapesOut[si].levels, 1);
        for (unsigned l = 0; l < modelLodLevels; ++l) putF(r, l < shapesOut[si].levels ? float(shapesOut[si].error[l]) : 0.0f);
        g0 += unsigned(m.shapes[si].groups.size());
    }
    unsigned b0 = 0;
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        const auto& so = shapesOut[si];
        for (unsigned gi = 0; gi < m.shapes[si].groups.size(); ++gi) {
            const auto& g = m.shapes[si].groups[gi];
            unsigned n = 0;
            if (so.levels) for (const auto ui : so.units) n += units[ui].group == gi;
            putLe(r, dusk::native::modelLodListHash(f.data() + g.dlOffset, g.dlSize), 8);
            putLe(r, g.dlSize, 4);
            putLe(r, b0, 2);
            putLe(r, n, 2);
            b0 += n;
        }
    }
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        const auto& so = shapesOut[si];
        if (!so.levels) continue;
        for (unsigned n = 0; n < so.units.size(); ++n) {
            const auto& u = units[so.units[n]];
            putLe(r, u.draw.tuple.size(), 2);
            putLe(r, 0, 2);
            putLe(r, u.draw.indices.size(), 4);
            putLe(r, dusk::native::modelLodIndexHash(u.draw.indices.data(), std::uint32_t(u.draw.indices.size())), 4);
            for (unsigned l = 0; l < modelLodLevels; ++l) putLe(r, l < so.levels ? so.levelIndices[n][l].size() : 0, 4);
        }
    }
    for (unsigned si = 0; si < m.shapes.size(); ++si) {
        const auto& so = shapesOut[si];
        if (!so.levels) continue;
        for (unsigned n = 0; n < so.units.size(); ++n)
            for (unsigned l = 0; l < so.levels; ++l)
                for (const auto v : so.levelIndices[n][l]) putLe(r, v, 2);
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    std::string discPath, storePath, reportPath, logPath, only;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--disc") discPath = argv[i + 1];
        else if (a == "--store") storePath = argv[i + 1];
        else if (a == "--report") reportPath = argv[i + 1];
        else if (a == "--log") logPath = argv[i + 1];
        else if (a == "--cross-skin") crossSkin = std::string(argv[i + 1]) == "1";
        else if (a == "--only") only = argv[i + 1];
    }
    if (discPath.empty() || storePath.empty() || reportPath.empty()) {
        std::fprintf(stderr, "usage: model_lod_build --disc zelda.twilight --store zelda.lod --report report.json [--log shapes.txt]\n");
        return 2;
    }
    dusk::native::Disc disc;
    if (disc.open(discPath.c_str()) != dusk::native::DiscStatus::ready) { std::fprintf(stderr, "disc not readable\n"); return 1; }
    std::FILE* log = logPath.empty() ? nullptr : std::fopen(logPath.c_str(), "wb");
    std::map<std::uint64_t, Built> records;
    std::map<std::string, Totals> perStage;
    unsigned files = 0, unreadable = 0, duplicates = 0;
    dusk::native::DiscDirectory root;
    if (disc.findDirectory("/", root) != dusk::native::DiscFileStatus::ready) { std::fprintf(stderr, "disc root?\n"); return 1; }
    const bool walked = walkAllModels(disc, root, "", [&](const std::string& path, const Bytes& bytes) {
        ++files;
        if (!only.empty() && path.find(only) == std::string::npos) return;
        const auto m = parseModel(bytes);
        if (!m.error.empty()) { ++unreadable; return; }
        auto& totals = perStage[stageOf(path)];
        Built b;
        const bool has = buildModel(bytes, m, b, totals, path, log);
        if (!has) return;
        auto [it, fresh] = records.emplace(b.key, std::move(b));
        if (!fresh) {
            ++duplicates;
            if (it->second.lists != b.lists || it->second.listBytes != b.listBytes) std::fprintf(stderr, "key collision at %s\n", path.c_str());
        }
    });
    if (log) std::fclose(log);
    if (!walked) { std::fprintf(stderr, "disc walk failed\n"); return 1; }

    // Store.
    std::vector<std::uint8_t> out(dusk::native::modelLodHeaderBytes, 0);
    const std::size_t indexAt = out.size();
    out.resize(indexAt + records.size() * dusk::native::modelLodIndexBytes);
    while (out.size() % 16) out.push_back(0);
    const std::size_t dataAt = out.size();
    std::uint32_t largest = 0;
    std::size_t n = 0;
    for (const auto& [key, b] : records) {
        while (out.size() % 4) out.push_back(0);
        const std::uint64_t offset = out.size() - dataAt;
        out.insert(out.end(), b.record.begin(), b.record.end());
        std::vector<std::uint8_t> e;
        putLe(e, key, 8); putLe(e, b.lists, 4); putLe(e, b.listBytes, 4); putLe(e, offset, 4); putLe(e, b.record.size(), 4);
        std::copy(e.begin(), e.end(), out.begin() + indexAt + n * dusk::native::modelLodIndexBytes);
        largest = std::max<std::uint32_t>(largest, std::uint32_t(b.record.size()));
        ++n;
    }
    while (out.size() % 4) out.push_back(0);
    const std::uint64_t fileBytes = out.size() + dusk::native::modelLodTrailerBytes;
    {
        std::vector<std::uint8_t> h;
        h.insert(h.end(), dusk::native::modelLodMagic, dusk::native::modelLodMagic + 8);
        putLe(h, dusk::native::modelLodFormatVersion, 4); putLe(h, dusk::native::modelLodBatchRule, 4);
        putLe(h, records.size(), 4); putLe(h, largest, 4);
        putLe(h, dusk::native::modelLodHeaderBytes, 8); putLe(h, dataAt, 8); putLe(h, fileBytes, 8);
        putLe(h, 0, 8);
        putLe(h, dusk::native::modelLodCrc32(0, out.data() + indexAt, records.size() * dusk::native::modelLodIndexBytes), 4);
        putLe(h, 0, 4);
        if (h.size() != dusk::native::modelLodHeaderBytes) { std::fprintf(stderr, "header layout\n"); return 1; }
        const auto crc = dusk::native::modelLodCrc32(0, h.data(), h.size());
        for (int i = 0; i < 4; ++i) h[60 + i] = std::uint8_t(crc >> (8 * i));
        std::copy(h.begin(), h.end(), out.begin());
    }
    {
        std::vector<std::uint8_t> t;
        t.insert(t.end(), dusk::native::modelLodTrailerMagic, dusk::native::modelLodTrailerMagic + 8);
        putLe(t, dusk::native::modelLodFormatVersion, 4); putLe(t, 0, 4); putLe(t, fileBytes, 8);
        t.resize(dusk::native::modelLodTrailerBytes, 0);
        const auto crc = dusk::native::modelLodCrc32(0, t.data(), t.size());
        for (int i = 0; i < 4; ++i) t[60 + i] = std::uint8_t(crc >> (8 * i));
        out.insert(out.end(), t.begin(), t.end());
    }
    if (std::FILE* s = std::fopen(storePath.c_str(), "wbx")) {
        const bool ok = std::fwrite(out.data(), 1, out.size(), s) == out.size();
        std::fclose(s);
        if (!ok) { std::fprintf(stderr, "store write failed\n"); return 1; }
    } else { std::fprintf(stderr, "store exists or cannot be created: %s\n", storePath.c_str()); return 1; }

    // Report.
    Totals all;
    for (const auto& [stage, t] : perStage) {
        all.models += t.models; all.shapes += t.shapes; all.shapesWithLevels += t.shapesWithLevels; all.full += t.full;
        for (unsigned l = 0; l < modelLodLevels; ++l) all.level[l] += t.level[l];
        all.lockedPoints += t.lockedPoints; all.points += t.points; all.cards += t.cards;
    }
    if (std::FILE* r = std::fopen(reportPath.c_str(), "wbx")) {
        std::fprintf(r, "{\n \"files\": %u, \"unreadable\": %u, \"records\": %zu, \"duplicates\": %u, \"store_bytes\": %zu,\n",
                     files, unreadable, records.size(), duplicates, out.size());
        auto line = [&](const Totals& t) {
            std::fprintf(r, "{\"models\": %llu, \"shapes\": %llu, \"shapes_with_levels\": %llu, \"triangles\": %llu, \"level1\": %llu, \"level2\": %llu, \"level3\": %llu, \"points\": %llu, \"locked\": %llu, \"cards\": %llu}",
                         (unsigned long long)t.models, (unsigned long long)t.shapes, (unsigned long long)t.shapesWithLevels,
                         (unsigned long long)t.full, (unsigned long long)t.level[0], (unsigned long long)t.level[1],
                         (unsigned long long)t.level[2], (unsigned long long)t.points, (unsigned long long)t.lockedPoints,
                         (unsigned long long)t.cards);
        };
        std::fprintf(r, " \"all\": ");
        line(all);
        std::fprintf(r, ",\n \"stages\": {\n");
        std::size_t k = 0;
        for (const auto& [stage, t] : perStage) {
            std::fprintf(r, "  \"%s\": ", stage.c_str());
            line(t);
            std::fprintf(r, "%s\n", ++k < perStage.size() ? "," : "");
        }
        std::fprintf(r, " }\n}\n");
        std::fclose(r);
    } else { std::fprintf(stderr, "report exists: %s\n", reportPath.c_str()); return 1; }
    std::printf("models %llu (files %u, unreadable %u), records %zu (+%u duplicates), store %zu bytes\n",
                (unsigned long long)all.models, files, unreadable, records.size(), duplicates, out.size());
    std::printf("triangles %llu -> level1 %llu, level2 %llu, level3 %llu; shapes with levels %llu of %llu; locked points %llu of %llu; cards %llu\n",
                (unsigned long long)all.full, (unsigned long long)all.level[0], (unsigned long long)all.level[1],
                (unsigned long long)all.level[2], (unsigned long long)all.shapesWithLevels, (unsigned long long)all.shapes,
                (unsigned long long)all.lockedPoints, (unsigned long long)all.points, (unsigned long long)all.cards);
    return 0;
}
