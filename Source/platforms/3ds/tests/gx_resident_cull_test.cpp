#include "gx_resident_cull.hpp"

#include <cassert>
#include <limits>
#include <vector>

namespace rc = dusk::native::gx::resident_cull;

static rc::Box box(float low, float high, float z = -.5f) {
    rc::Bounds bounds;
    const float a[]{low, 0.f, z}, b[]{high, 0.f, z};
    assert(bounds.add(a) && bounds.add(b));
    return bounds.box();
}

static std::array<rc::Uniform, 94> projection() {
    std::array<rc::Uniform, 94> uniforms{};
    uniforms[0][0] = uniforms[1][1] = uniforms[2][2] = 1.f;
    uniforms[31][0] = uniforms[32][1] = uniforms[33][2] = 1.f;
    uniforms[6][0] = uniforms[7][1] = uniforms[8][2] = uniforms[9][3] = 1.f;
    return uniforms;
}

static void checkClipping() {
    auto uniforms = projection();
    rc::Planes planes;
    assert(rc::makePlanes(uniforms, 0, rc::allPlanes, planes));
    assert(rc::classify(planes, box(2.f, 6.f), rc::allPlanes).outside == 1);
    assert(rc::classify<true>(planes, box(-2.f, 2.f), rc::allPlanes).crossing == 3);
    assert(rc::classify(planes, box(1.f, 1.f), rc::allPlanes).outside == 0);
    assert(rc::classify(planes, box(0.f, 0.f, .01f), rc::allPlanes).outside == 16);
    assert(rc::classify(planes, box(0.f, 0.f, .01f), rc::sidePlanes).outside == 0);
    assert(rc::classify(planes, box(0.f, 0.f, -2.f), rc::allPlanes).outside == 0);

    // Projection subtracts nearly equal eye-space terms. A margin built
    // from the already combined object-space row would miss that error.
    uniforms[0] = {0, 0, 0, 100001.f};
    uniforms[1] = {0, 0, 0, 100000.f};
    uniforms[6] = {1, -1, 0, .25f};
    uniforms[7] = {};
    assert(rc::makePlanes(uniforms, 0, rc::allPlanes, planes));
    assert(rc::classify(planes, box(0.f, 0.f), rc::allPlanes).outside == 0);
    uniforms[0][3] = std::numeric_limits<float>::infinity();
    assert(!rc::makePlanes(uniforms, 0, rc::allPlanes, planes));
    assert(!rc::makePlanes(uniforms, 92, rc::allPlanes, planes));
    rc::Bounds bounds;
    const float invalid[]{std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f};
    assert(!bounds.add(invalid));
}

static void checkPalette() {
    auto uniforms = projection();
    // Vertices from two bones can form one triangle across the screen.
    uniforms[31][3] = -4.f;
    const rc::MatrixBox boxes[]{{box(2.f, 2.f), 0}, {box(2.f, 2.f), 31}};
    assert(!rc::outsidePalette(uniforms, boxes, rc::allPlanes));
    uniforms[31][3] = 4.f;
    assert(rc::outsidePalette(uniforms, boxes, rc::allPlanes));
    uniforms[31][3] = -1.f;
    assert(!rc::outsidePalette(uniforms, boxes, rc::allPlanes));
}

static void checkRuns() {
    const auto uniforms = projection();
    rc::Planes planes;
    assert(rc::makePlanes(uniforms, 0, rc::allPlanes, planes));
    const auto inside = box(0.f, 0.f), outside = box(3.f, 3.f), crossing = box(0.f, 3.f);
    constexpr unsigned count = rc::coarseIndices + 6;
    const rc::Box coarse[]{crossing, crossing};
    rc::MatrixBox fine[]{{outside, 0}, {inside, 0}, {outside, 0}, {outside, 0}, {inside, 0}};
    rc::Runs runs;
    assert(rc::refine(planes, rc::allPlanes, crossing, coarse, fine, count, runs));
    // The two removed interior sub-blocks are retained to avoid another draw.
    assert(runs.size == 1 && runs.ranges[0].first == 48 && runs.ranges[0].count == 150);
    assert(runs.indices == 150);

    // A larger gap would cost a draw while saving fewer than 192 indices.
    const rc::Box largeCoarse[]{crossing, crossing, crossing};
    rc::MatrixBox largeFine[12];
    for (auto& part : largeFine) part = {inside, 0};
    for (unsigned i = 2; i < 5; ++i) largeFine[i] = {outside, 0};
    runs.size = runs.indices = 0;
    assert(rc::refine(planes, rc::allPlanes, crossing, largeCoarse, largeFine, 576, runs));
    assert(runs.size == 1 && runs.indices == 576);

    // A sufficiently large gap is removed without changing draw order.
    largeFine[5] = {outside, 0};
    runs.size = runs.indices = 0;
    assert(rc::refine(planes, rc::allPlanes, crossing, largeCoarse, largeFine, 576, runs));
    assert(runs.size == 2 && runs.indices == 384);
    assert(runs.ranges[0].first == 0 && runs.ranges[0].count == 96);
    assert(runs.ranges[1].first == 288 && runs.ranges[1].count == 288);

    runs.size = runs.indices = 0;
    assert(rc::refine(planes, rc::allPlanes, inside, largeCoarse, largeFine, 576, runs));
    assert(runs.size == 1 && runs.indices == 576);
    runs.size = runs.indices = 0;
    assert(rc::refine(planes, rc::allPlanes, outside, largeCoarse, largeFine, 576, runs));
    assert(runs.size == 0 && runs.indices == 0);

    constexpr unsigned many = 2 * rc::maxRuns + 1;
    std::vector<rc::Box> manyCoarse(many, outside);
    const std::vector<rc::MatrixBox> manyFine(many * 4, {outside, 0});
    for (unsigned i = 0; i < many; i += 2) manyCoarse[i] = inside;
    runs.size = runs.indices = 0;
    assert(!rc::refine(planes, rc::allPlanes, crossing, manyCoarse, manyFine, many * rc::coarseIndices, runs));
}

int main() {
    checkClipping();
    checkPalette();
    checkRuns();
}
