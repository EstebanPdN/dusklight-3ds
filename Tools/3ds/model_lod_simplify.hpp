// Mac-only: reduced index lists for one draw of a model (model_lod_build.cpp).
//
// The runtime draws a J3D display list as one or more batches, each a set of
// unique vertex records and an index list over them (gx_commands.cpp). A
// level of detail here is another index list over the same records: every
// collapse moves a point onto a point that already exists (half-edge
// collapse), so positions, normals, colours, texture coordinates and matrix
// indices stay the authored ones and the runtime keeps one vertex buffer for
// every level and for the shadow caster.
//
// Collapse order: quadric error (Garland-Heckbert, area weighted) plus a small
// penalty for the normal and colour a vertex loses. Never collapsed:
//   - a locked vertex (shared with another draw of the model: material,
//     skinning-group and batch borders; or part of an alpha-tested card);
//   - across draw matrices (a point follows one joint or envelope);
//   - a border point off its border (open edges keep their outline);
//   - a point whose wedges (records of one position with other attributes:
//     texture seams, hard normals) cannot each follow an edge to one wedge of
//     the target (the seam would tear);
//   - when a remaining triangle would flip or become degenerate.
// Deterministic: ties break on point indices, no hashing order is used.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <queue>
#include <tuple>
#include <vector>

namespace modellod {
using Vec3 = std::array<double, 3>;
using Triangle = std::array<std::uint32_t, 3>;

struct Mesh {
    std::vector<Vec3> position;          // per vertex record, model space (bind pose)
    std::vector<std::uint32_t> skin;     // per vertex: the draw matrix its position follows
    std::vector<std::uint8_t> locked;    // per vertex
    std::vector<Vec3> normal;            // per vertex (unit, or zero when absent)
    std::vector<std::array<double, 4>> color; // per vertex RGBA in [0,1] (zero when absent)
    std::vector<Triangle> triangles;     // distinct vertices, draw order
};
struct Level {
    std::vector<Triangle> triangles;     // a subset of the input order, vertices remapped
    double error = 0;                    // largest distance of an input point to the reduced surface
};
struct Options {
    std::vector<double> targets{0.5, 0.25, 0.125}; // fractions of the input triangles
    double maxError = 1e30;              // no collapse whose estimated error is larger
    double flipCos = 0.25;               // smallest cosine between a face normal before and after
    double attributeWeight = 0.0;        // squared length per unit of attribute change (0: none)
    // Allow a point to collapse onto a neighbour that follows another draw
    // matrix (never onto one at the same position: a deliberate seam). Such
    // a collapse costs four times as much and the level's error is at least
    // the length of every such edge: animated, the moved surface follows the
    // neighbour's joints instead of its own.
    bool crossSkin = false;
    // Optional: why candidate collapses were refused (tool diagnostics).
    std::array<std::uint64_t, 8>* refusals = nullptr; // locked, skin, no edge, border, link, wedge, flip, done
};

namespace detail {
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
// Symmetric 4x4 quadric: xx xy xz xw yy yz yw zz zw ww.
struct Quadric {
    std::array<double, 10> q{};
    double weight = 0;
    void addPlane(const Vec3& n, double d, double w) {
        const double a = n[0], b = n[1], c = n[2];
        q[0] += w * a * a; q[1] += w * a * b; q[2] += w * a * c; q[3] += w * a * d;
        q[4] += w * b * b; q[5] += w * b * c; q[6] += w * b * d;
        q[7] += w * c * c; q[8] += w * c * d; q[9] += w * d * d;
        weight += w;
    }
    void add(const Quadric& o) { for (int i = 0; i < 10; ++i) q[i] += o.q[i]; weight += o.weight; }
    double eval(const Vec3& p) const {
        const double x = p[0], y = p[1], z = p[2];
        const double e = q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y +
                         2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
        return e > 0 ? e : 0;
    }
};
// Distance from p to triangle abc.
inline double pointTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return length(ap);
    const Vec3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return length(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double v = d1 / (d1 - d3);
        return length(sub(p, {a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]}));
    }
    const Vec3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return length(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double w = d2 / (d2 - d6);
        return length(sub(p, {a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]}));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const Vec3 bc = sub(c, b);
        return length(sub(p, {b[0] + w * bc[0], b[1] + w * bc[1], b[2] + w * bc[2]}));
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    return length(sub(p, {a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w}));
}
}  // namespace detail

// Levels for the targets, coarser one after the other (each continues the
// previous one). A level is left out when the collapses stopped before its
// target (error limit or nothing valid left): the caller keeps only levels
// that pay.
inline std::vector<Level> simplify(const Mesh& mesh, const Options& options) {
    using namespace detail;
    std::vector<Level> levels;
    const std::size_t vertexCount = mesh.position.size();
    if (mesh.triangles.size() < 2 || vertexCount < 4) return levels;

    // Points: records of one position following one matrix.
    std::vector<std::uint32_t> pointOf(vertexCount);
    std::vector<Vec3> pointPos;
    std::vector<std::uint32_t> pointSkin;
    std::vector<std::uint8_t> pointLocked;
    std::vector<std::vector<std::uint32_t>> wedges;
    {
        std::map<std::tuple<double, double, double, std::uint32_t>, std::uint32_t> ids;
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            const auto key = std::make_tuple(mesh.position[v][0], mesh.position[v][1], mesh.position[v][2], mesh.skin[v]);
            auto [it, fresh] = ids.emplace(key, std::uint32_t(pointPos.size()));
            if (fresh) {
                pointPos.push_back(mesh.position[v]);
                pointSkin.push_back(mesh.skin[v]);
                pointLocked.push_back(0);
                wedges.emplace_back();
            }
            pointOf[v] = it->second;
            pointLocked[it->second] |= mesh.locked[v];
            wedges[it->second].push_back(v);
        }
    }
    const std::size_t points = pointPos.size();
    std::vector<Triangle> tri = mesh.triangles;
    std::vector<std::uint8_t> alive(tri.size(), 1);
    std::size_t aliveCount = tri.size();
    std::vector<std::vector<std::uint32_t>> around(points);
    for (std::uint32_t t = 0; t < tri.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            auto& list = around[pointOf[tri[t][k]]];
            if (list.empty() || list.back() != t) list.push_back(t);
        }

    // Quadrics, borders, non-manifold edges.
    std::vector<Quadric> quadric(points);
    std::vector<std::uint8_t> border(points, 0);
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edges;
    for (std::uint32_t t = 0; t < tri.size(); ++t) {
        const std::uint32_t p[3] = {pointOf[tri[t][0]], pointOf[tri[t][1]], pointOf[tri[t][2]]};
        const Vec3 n = cross(sub(pointPos[p[1]], pointPos[p[0]]), sub(pointPos[p[2]], pointPos[p[0]]));
        const double area2 = length(n);
        if (area2 > 0) {
            const Vec3 u = {n[0] / area2, n[1] / area2, n[2] / area2};
            const double d = -dot(u, pointPos[p[0]]);
            for (auto q : p) quadric[q].addPlane(u, d, area2 * 0.5);
        }
        for (int k = 0; k < 3; ++k) {
            const auto a = p[k], b = p[(k + 1) % 3];
            if (a == b) continue;
            edges[{std::min(a, b), std::max(a, b)}].push_back(t);
        }
    }
    for (const auto& [edge, list] : edges) {
        if (list.size() > 2) { pointLocked[edge.first] = pointLocked[edge.second] = 1; continue; }
        if (list.size() != 1) continue;
        border[edge.first] = border[edge.second] = 1;
        // A plane through the open edge, perpendicular to its triangle: the
        // outline may slide along itself only.
        const auto& t = tri[list[0]];
        const Vec3 tn = cross(sub(mesh.position[t[1]], mesh.position[t[0]]), sub(mesh.position[t[2]], mesh.position[t[0]]));
        const Vec3 e = sub(pointPos[edge.second], pointPos[edge.first]);
        Vec3 m = cross(e, tn);
        const double ml = length(m);
        if (ml <= 0) continue;
        m = {m[0] / ml, m[1] / ml, m[2] / ml};
        const double d = -dot(m, pointPos[edge.first]);
        const double w = 10.0 * dot(e, e);
        quadric[edge.first].addPlane(m, d, w);
        quadric[edge.second].addPlane(m, d, w);
    }

    auto contains = [&](const Triangle& t, std::uint32_t point) {
        return pointOf[t[0]] == point || pointOf[t[1]] == point || pointOf[t[2]] == point;
    };
    // Which record of q each record of p becomes; false when a wedge of p
    // has no unique partner (or the collapse is not allowed at all).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> mapping;
    auto plan = [&](std::uint32_t p, std::uint32_t q, double& cost) {
        auto refuse = [&](int why) { if (options.refusals) ++(*options.refusals)[why]; return false; };
        if (p == q || pointLocked[p]) return refuse(0);
        const bool otherSkin = pointSkin[p] != pointSkin[q];
        if (otherSkin && (!options.crossSkin || pointPos[p] == pointPos[q])) return refuse(1);
        unsigned shared = 0;
        std::vector<std::uint32_t> opposite, ringP, ringQ;
        for (auto t : around[p]) {
            if (!alive[t]) continue;
            const bool both = contains(tri[t], q);
            shared += both;
            for (int k = 0; k < 3; ++k) {
                const auto r = pointOf[tri[t][k]];
                if (r == p || r == q) continue;
                ringP.push_back(r);
                if (both) opposite.push_back(r);
            }
        }
        if (!shared) return refuse(2);
        if (border[p] && shared != 1) return refuse(3);  // a border point moves along its border
        if (border[p] && !border[q]) return refuse(3);
        // Link condition: the only points next to both are the ones across
        // the collapsed edge, or the result is not a manifold any more.
        for (auto t : around[q]) {
            if (!alive[t]) continue;
            for (int k = 0; k < 3; ++k) {
                const auto r = pointOf[tri[t][k]];
                if (r != p && r != q) ringQ.push_back(r);
            }
        }
        std::sort(ringP.begin(), ringP.end());
        ringP.erase(std::unique(ringP.begin(), ringP.end()), ringP.end());
        std::sort(ringQ.begin(), ringQ.end());
        ringQ.erase(std::unique(ringQ.begin(), ringQ.end()), ringQ.end());
        std::sort(opposite.begin(), opposite.end());
        opposite.erase(std::unique(opposite.begin(), opposite.end()), opposite.end());
        std::vector<std::uint32_t> common;
        std::set_intersection(ringP.begin(), ringP.end(), ringQ.begin(), ringQ.end(), std::back_inserter(common));
        if (common != opposite) return refuse(4);
        mapping.clear();
        for (auto vp : wedges[p]) {
            std::uint32_t partner = UINT32_MAX;
            bool used = false;
            for (auto t : around[p]) {
                if (!alive[t]) continue;
                const auto& tt = tri[t];
                if (tt[0] != vp && tt[1] != vp && tt[2] != vp) continue;
                used = true;
                for (int k = 0; k < 3; ++k)
                    if (pointOf[tt[k]] == q) {
                        if (partner != UINT32_MAX && partner != tt[k]) return refuse(5);
                        partner = tt[k];
                    }
            }
            if (!used) continue;
            if (partner == UINT32_MAX) return refuse(5);
            mapping.push_back({vp, partner});
        }
        // Remaining triangles must keep their orientation and some area.
        for (auto t : around[p]) {
            if (!alive[t] || contains(tri[t], q)) continue;
            Vec3 a[3], b[3];
            for (int k = 0; k < 3; ++k) {
                a[k] = pointPos[pointOf[tri[t][k]]];
                b[k] = pointOf[tri[t][k]] == p ? pointPos[q] : a[k];
            }
            const Vec3 n0 = cross(sub(a[1], a[0]), sub(a[2], a[0]));
            const Vec3 n1 = cross(sub(b[1], b[0]), sub(b[2], b[0]));
            const double l0 = length(n0), l1 = length(n1);
            if (l1 <= 1e-12 * (l0 + 1e-30) || l1 == 0) return refuse(6);
            if (dot(n0, n1) < options.flipCos * l0 * l1) return refuse(6);
        }
        cost = quadric[p].eval(pointPos[q]) + quadric[q].eval(pointPos[q]);
        if (otherSkin) {
            const Vec3 d = sub(pointPos[p], pointPos[q]);
            cost = 4 * cost + quadric[p].weight * dot(d, d);
        }
        if (options.attributeWeight > 0) {
            double change = 0;
            for (const auto& [from, to] : mapping) {
                const Vec3 dn = sub(mesh.normal[from], mesh.normal[to]);
                change += dot(dn, dn);
                for (int c = 0; c < 4; ++c) {
                    const double dc = mesh.color[from][c] - mesh.color[to][c];
                    change += dc * dc;
                }
            }
            cost += options.attributeWeight * quadric[p].weight * change;
        }
        return true;
    };
    auto estimate = [&](std::uint32_t p, double cost) {
        const double w = quadric[p].weight > 0 ? quadric[p].weight : 1;
        return std::sqrt(cost / w);
    };

    struct Candidate {
        double cost;
        std::uint32_t p, q, stampP, stampQ;
        bool operator>(const Candidate& o) const { return std::tie(cost, p, q) > std::tie(o.cost, o.p, o.q); }
    };
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> heap;
    std::vector<std::uint32_t> stamp(points, 0);
    std::vector<std::uint8_t> dead(points, 0);
    std::vector<std::uint32_t> rep(points);
    double skinError = 0;
    for (std::uint32_t i = 0; i < points; ++i) rep[i] = i;
    auto push = [&](std::uint32_t p, std::uint32_t q) {
        double cost = 0;
        if (dead[p] || dead[q] || !plan(p, q, cost)) return;
        heap.push({cost, p, q, stamp[p], stamp[q]});
    };
    for (const auto& [edge, list] : edges) {
        push(edge.first, edge.second);
        push(edge.second, edge.first);
    }

    auto snapshot = [&]() {
        Level level;
        for (std::uint32_t t = 0; t < tri.size(); ++t)
            if (alive[t]) level.triangles.push_back(tri[t]);
        // One-sided distance: every input point to the reduced surface near
        // the point that now stands for it (the triangles of that point and
        // of its neighbours: the patch its own triangles were folded into).
        double worst = 0;
        std::vector<std::uint32_t> patch, ring;
        for (std::uint32_t i = 0; i < points; ++i) {
            std::uint32_t r = i;
            while (rep[r] != r) r = rep[r];
            if (r == i) continue;
            ring.clear();
            patch.clear();
            for (auto t : around[r])
                if (alive[t]) for (int k = 0; k < 3; ++k) ring.push_back(pointOf[tri[t][k]]);
            std::sort(ring.begin(), ring.end());
            ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
            for (auto n : ring) for (auto t : around[n]) if (alive[t]) patch.push_back(t);
            std::sort(patch.begin(), patch.end());
            patch.erase(std::unique(patch.begin(), patch.end()), patch.end());
            double best = 1e300;
            for (auto t : patch) {
                const auto& tt = tri[t];
                best = std::min(best, pointTriangle(pointPos[i], mesh.position[tt[0]], mesh.position[tt[1]], mesh.position[tt[2]]));
            }
            if (best < 1e300) worst = std::max(worst, best);
        }
        level.error = std::max(worst, skinError);
        return level;
    };

    const std::size_t original = tri.size();
    for (const double target : options.targets) {
        const auto goal = std::size_t(std::floor(target * double(original)));
        while (aliveCount > goal && !heap.empty()) {
            const Candidate c = heap.top();
            heap.pop();
            if (dead[c.p] || dead[c.q]) continue;
            if (c.stampP != stamp[c.p] || c.stampQ != stamp[c.q]) { push(c.p, c.q); continue; }
            double cost = 0;
            if (!plan(c.p, c.q, cost)) continue;
            if (estimate(c.p, cost) > options.maxError) { heap = {}; break; }
            // Collapse p onto q.
            const auto p = c.p, q = c.q;
            if (pointSkin[p] != pointSkin[q]) skinError = std::max(skinError, length(sub(pointPos[p], pointPos[q])));
            for (auto t : around[p]) {
                if (!alive[t]) continue;
                if (contains(tri[t], q)) { alive[t] = 0; --aliveCount; continue; }
                for (int k = 0; k < 3; ++k)
                    if (pointOf[tri[t][k]] == p)
                        for (const auto& [from, to] : mapping) if (from == tri[t][k]) { tri[t][k] = to; break; }
                around[q].push_back(t);
            }
            around[p].clear();
            quadric[q].add(quadric[p]);
            dead[p] = 1;
            rep[p] = q;
            ++stamp[q];
            // Every edge around q changes cost.
            std::vector<std::uint32_t> neighbours;
            {
                auto& list = around[q];
                std::sort(list.begin(), list.end());
                list.erase(std::unique(list.begin(), list.end()), list.end());
                list.erase(std::remove_if(list.begin(), list.end(), [&](std::uint32_t t) { return !alive[t]; }), list.end());
                for (auto t : list)
                    for (int k = 0; k < 3; ++k) {
                        const auto r = pointOf[tri[t][k]];
                        if (r != q) neighbours.push_back(r);
                    }
                std::sort(neighbours.begin(), neighbours.end());
                neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
            }
            for (auto r : neighbours) { ++stamp[r]; }
            for (auto r : neighbours) { push(q, r); push(r, q); }
        }
        if (aliveCount > goal) {
            // Out of valid collapses before the target: the last level still
            // counts when it is clearly coarser than the one before.
            const std::size_t before = levels.empty() ? original : levels.back().triangles.size();
            if (aliveCount * 4 <= before * 3) levels.push_back(snapshot());
            break;
        }
        levels.push_back(snapshot());
    }
    return levels;
}
}  // namespace modellod
