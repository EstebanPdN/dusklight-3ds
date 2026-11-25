// Host test of the native particle emitter loop's screen math
// (Source/libs/JSystem/include/JSystem/JParticle/JPANativeCull.h), the same
// header JPAResource.cpp uses on the console.
//
// 1. The bounding sphere of a particle (twice its scaled template quad) holds
//    every corner the original draw functions build, for every quad shape
//    (billboard, rotated and Y billboards, direction with its plane types,
//    rotation with its axis types, direction billboard, cross shapes) and
//    every template centre offset of the original vertex table.
// 2. A sphere found outside the view has every corner outside the same side:
//    the quad reaches no pixel, so leaving the emitter out changes nothing.
// 3. The footprint rectangle of a sphere holds the projection of every corner.
// 4. End to end: random particle groups drawn in list order and in the order
//    the run search gives (gatherRun) produce the same picture, bit for bit,
//    with a blend that depends on order; the search does merge emitters.
//
// Build: c++ -std=c++20 -O1 -I Source/libs/JSystem/include \
//        Source/platforms/3ds/tests/particle_native_cull_test.cpp
// (Tools/3ds/test_public_host.py runs it as "particle-cull").
#include "JSystem/JParticle/JPANativeCull.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

using Mtx = float[3][4];
using jpa_native::Rect;

int failures = 0;
#define CHECK(condition, ...)                                   \
    do {                                                        \
        if (!(condition)) {                                     \
            if (failures++ < 20) {                              \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                std::printf(__VA_ARGS__);                       \
                std::printf("\n");                              \
            }                                                   \
        }                                                       \
    } while (0)

// The original template quads (JPAResource.cpp, jpa_pos): s8 positions.
const std::uint8_t jpa_pos[324] = {
    0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0xE7, 0x00, 0x00, 0x19,
    0x00, 0x00, 0x19, 0xCE, 0x00, 0xE7, 0xCE, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCE,
    0x00, 0xCE, 0xCE, 0x00, 0x00, 0x19, 0x00, 0x32, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x00, 0xE7, 0x00,
    0xE7, 0x19, 0x00, 0x19, 0x19, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0xE7, 0x00, 0xCE, 0x19, 0x00, 0x00,
    0x19, 0x00, 0x00, 0xE7, 0x00, 0xCE, 0xE7, 0x00, 0x00, 0x32, 0x00, 0x32, 0x32, 0x00, 0x32, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x32, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00,
    0xCE, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32,
    0x00, 0x00, 0x32, 0x00, 0x32, 0x00, 0x00, 0x32, 0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0x00,
    0x32, 0xE7, 0x00, 0x32, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x32,
    0x00, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x00, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0x19,
    0x00, 0xE7, 0x19, 0x00, 0x19, 0xE7, 0x00, 0x19, 0xCE, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0x00, 0x00,
    0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE7, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xCE, 0x00, 0xCE, 0x00,
    0x00, 0xCE, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0xCE,
    0x32, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0xE7,
    0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0xCE, 0x00, 0x19, 0x00, 0x00,
    0x19, 0x32, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x00, 0x00, 0x19, 0xE7, 0x00, 0x19, 0x19, 0x00, 0xE7,
    0x19, 0x00, 0xE7, 0xE7, 0x00, 0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0xCE,
    0x00, 0x32, 0x00, 0x00, 0x32, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x32, 0xE7, 0x00,
    0x32, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x32, 0xCE, 0x00, 0x32, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xCE,
};

struct Template {
    float v[8][3];
    int count;
    float radius; // the batch's sphere radius per unit scale: twice the largest corner
};

// JPAResource::initBatchInfo: centre offset (scale centre x + 3 y) * 0xC,
// base plane type * 0x6C, cross shapes add the four vertices at 72.
Template makeTemplate(int centreX, int centreY, int planeType, bool cross) {
    Template t{};
    const auto* pos = reinterpret_cast<const std::int8_t*>(jpa_pos) + (centreX + 3 * centreY) * 0xC + planeType * 0x6C;
    t.count = cross ? 8 : 4;
    float largest = 0.0f;
    for (int i = 0; i < t.count; i++) {
        const int index = i < 4 ? i : 72 + (i - 4);
        for (int k = 0; k < 3; k++) {
            t.v[i][k] = pos[index * 3 + k];
        }
        largest = std::fmax(largest, std::sqrt(t.v[i][0] * t.v[i][0] + t.v[i][1] * t.v[i][1] + t.v[i][2] * t.v[i][2]));
    }
    t.radius = 2.0f * largest;
    return t;
}

void mul(const Mtx a, const Mtx b, Mtx out) {
    Mtx r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
        }
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            out[i][j] = r[i][j];
        }
    }
}

void apply(const Mtx m, const float v[3], float out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3];
    }
}

void applySR(const Mtx m, const float v[3], float out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
}

void normalize(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; i++) {
        v[i] /= length;
    }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    const float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    for (int i = 0; i < 3; i++) {
        out[i] = r[i];
    }
}

// A rigid camera (rotation + translation), as the view matrices are.
void makeCamera(std::mt19937& rng, Mtx cam) {
    std::uniform_real_distribution<float> angle(-3.14159f, 3.14159f), shift(-500.0f, 500.0f);
    const float a = angle(rng), b = angle(rng), c = angle(rng);
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
    const float rx[3][3] = {{1, 0, 0}, {0, ca, -sa}, {0, sa, ca}};
    const float ry[3][3] = {{cb, 0, sb}, {0, 1, 0}, {-sb, 0, cb}};
    const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
    float t[3][3], r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t[i][j] = ry[i][0] * rz[0][j] + ry[i][1] * rz[1][j] + ry[i][2] * rz[2][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = rx[i][0] * t[0][j] + rx[i][1] * t[1][j] + rx[i][2] * t[2][j];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            cam[i][j] = r[i][j];
        }
        cam[i][3] = shift(rng);
    }
}

// C_MTXLightPerspective(fovY, aspect, 0.5, -0.5, 0.5, 0.5) (JPADrawInfo).
void makePerspective(float fovY, float aspect, Mtx prj) {
    const float cot = 1.0f / std::tan(fovY * 0.5f * 3.14159265f / 180.0f);
    const float m[3][4] = {{cot / aspect * 0.5f, 0.0f, -0.5f, 0.0f}, {0.0f, cot * -0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f, 0.0f}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            prj[i][j] = m[i][j];
}

// C_MTXLightOrtho(0, 448, 0, 608, 0.5, 0.5, 0.5, 0.5) (the 2D groups).
void makeOrtho(Mtx prj) {
    const float t = 0.0f, b = 448.0f, l = 0.0f, r = 608.0f;
    const float m[3][4] = {{2.0f / (r - l) * 0.5f, 0.0f, 0.0f, -(r + l) / (r - l) * 0.5f + 0.5f},
                           {0.0f, 2.0f / (t - b) * 0.5f, 0.0f, -(t + b) / (t - b) * 0.5f + 0.5f},
                           {0.0f, 0.0f, 0.0f, 1.0f}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            prj[i][j] = m[i][j];
}

struct Particle {
    float corners[8][3]; // view space
    int count;
    float centre[3];     // view space
    float radius;
};

// The view-space position matrix of the original draw functions
// (JPABaseShape.cpp) for one random particle of shape `kind`.
Particle makeParticle(std::mt19937& rng, const Mtx cam, int kind, const float world[3]) {
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f), scale(0.01f, 3.0f), base(0.2f, 4.0f);
    std::uniform_int_distribution<int> centre(0, 2), plane(0, 2), rotType(0, 3);
    const float globalX = base(rng), globalY = base(rng);
    const float ptclX = scale(rng) * (unit(rng) < -0.8f ? -1.0f : 1.0f), ptclY = scale(rng);
    const float sx = globalX * ptclX, sy = globalY * ptclY;
    const float angle = unit(rng) * 3.14159f, sinR = std::sin(angle), cosR = std::cos(angle);
    Mtx pos{};
    bool cross = false;
    int planeType = 0;
    float p[3];
    apply(cam, world, p);
    switch (kind) {
    case 0: // JPADrawBillboard
        pos[0][0] = sx; pos[0][3] = p[0];
        pos[1][1] = sy; pos[1][3] = p[1];
        pos[2][2] = 1.0f; pos[2][3] = p[2];
        break;
    case 1: // JPADrawRotBillboard
        pos[0][0] = cosR * sx; pos[0][1] = -sinR * sy; pos[0][3] = p[0];
        pos[1][0] = sinR * sx; pos[1][1] = cosR * sy; pos[1][3] = p[1];
        pos[2][2] = 1.0f; pos[2][3] = p[2];
        break;
    case 2: // JPADrawYBillboard (mYBBCamMtx from the camera's y axis)
    case 3: { // JPADrawRotYBillboard
        float v[3] = {0.0f, cam[1][1], cam[2][1]};
        normalize(v);
        const float y11 = v[1], y12 = -v[2], y21 = v[2], y22 = v[1];
        if (kind == 2) {
            pos[0][0] = sx; pos[0][3] = p[0];
            pos[1][1] = y11 * sy; pos[1][2] = y12; pos[1][3] = p[1];
            pos[2][1] = y21 * sy; pos[2][2] = y22; pos[2][3] = p[2];
        } else {
            pos[0][0] = cosR * sx; pos[0][1] = -sinR * sy; pos[0][3] = p[0];
            pos[1][0] = sinR * sx * y11; pos[1][1] = cosR * sy * y11; pos[1][2] = -y21; pos[1][3] = p[1];
            pos[2][0] = sinR * sx * y21; pos[2][1] = cosR * sy * y21; pos[2][2] = y11; pos[2][3] = p[2];
        }
        break;
    }
    case 4: // JPADrawDirection / JPADrawRotDirection, any plane type, cross or not
    case 5: {
        planeType = plane(rng);
        cross = unit(rng) > 0.0f;
        float axisY[3] = {unit(rng), unit(rng), unit(rng)}, baseAxis[3] = {unit(rng), unit(rng), unit(rng)}, axisZ[3];
        normalize(axisY);
        cross3(baseAxis, axisY, axisZ);
        normalize(axisZ);
        cross3(axisY, axisZ, baseAxis);
        normalize(baseAxis);
        Mtx frame = {{baseAxis[0], axisY[0], axisZ[0], world[0]},
                     {baseAxis[1], axisY[1], axisZ[1], world[1]},
                     {baseAxis[2], axisY[2], axisZ[2], world[2]}};
        Mtx local = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        if (kind == 5) { // rotTypeY/X/Z/XYZ
            const int r = rotType(rng);
            const float s = sinR, c = cosR;
            if (r == 0) {
                const Mtx m = {{c, 0, -s, 0}, {0, 1, 0, 0}, {s, 0, c, 0}};
                std::copy(&m[0][0], &m[0][0] + 12, &local[0][0]);
            } else if (r == 1) {
                const Mtx m = {{1, 0, 0, 0}, {0, c, -s, 0}, {0, s, c, 0}};
                std::copy(&m[0][0], &m[0][0] + 12, &local[0][0]);
            } else if (r == 2) {
                const Mtx m = {{c, -s, 0, 0}, {s, c, 0, 0}, {0, 0, 1, 0}};
                std::copy(&m[0][0], &m[0][0] + 12, &local[0][0]);
            } else {
                float f31 = 0.33333298563957214f * (1.0f - c), f30 = 0.57735f * s;
                const float f29 = f31 + f30;
                f30 = f31 - f30;
                f31 += c;
                const Mtx m = {{f31, f30, f29, 0}, {f29, f31, f30, 0}, {f30, f29, f31, 0}};
                std::copy(&m[0][0], &m[0][0] + 12, &local[0][0]);
            }
        }
        // basePlaneTypeXY / XZ / X on the local matrix
        const int pt = cross ? 2 : planeType;
        for (int i = 0; i < 3; i++) {
            local[i][0] *= sx;
            if (pt == 0) {
                local[i][1] *= sy;
            } else if (pt == 1) {
                local[i][2] *= sy;
            } else {
                local[i][1] *= sy;
                local[i][2] *= sx;
            }
        }
        mul(frame, local, pos);
        mul(cam, pos, pos);
        break;
    }
    case 6: { // JPADrawRotation (rotation about the world axes, plane types)
        planeType = plane(rng);
        cross = unit(rng) > 0.0f;
        Mtx local = {{cosR, -sinR, 0, 0}, {sinR, cosR, 0, 0}, {0, 0, 1, 0}};
        const int pt = cross ? 2 : planeType;
        for (int i = 0; i < 3; i++) {
            local[i][0] *= sx;
            if (pt == 0) {
                local[i][1] *= sy;
            } else if (pt == 1) {
                local[i][2] *= sy;
            } else {
                local[i][1] *= sy;
                local[i][2] *= sx;
            }
        }
        local[0][3] = world[0];
        local[1][3] = world[1];
        local[2][3] = world[2];
        mul(cam, local, pos);
        break;
    }
    default: { // JPADrawDBillboard
        float d[3] = {unit(rng), unit(rng), unit(rng)}, axis[3] = {cam[2][0], cam[2][1], cam[2][2]};
        cross3(d, axis, d);
        normalize(d);
        applySR(cam, d, d);
        pos[0][0] = d[0] * sx; pos[0][1] = -d[1] * sy; pos[0][3] = p[0];
        pos[1][0] = d[1] * sx; pos[1][1] = d[0] * sy; pos[1][3] = p[1];
        pos[2][2] = 1.0f; pos[2][3] = p[2];
        break;
    }
    }
    // Direction and rotation shapes read the plane type (types 3 and 7);
    // the others the XY templates. Any of the nine scale centres.
    const Template t = makeTemplate(centre(rng), centre(rng), (kind >= 4 && !cross) ? (planeType == 1 ? 1 : 0) : 0, cross);
    Particle out{};
    out.count = t.count;
    for (int i = 0; i < t.count; i++) {
        apply(pos, t.v[i], out.corners[i]);
    }
    for (int i = 0; i < 3; i++) {
        out.centre[i] = p[i];
    }
    // The batch's radius: twice the largest template corner times the
    // larger of |global * particle| scale (JPAResource.cpp, offScreen).
    out.radius = t.radius * std::fmax(std::fabs(globalX * ptclX), std::fabs(globalY * ptclY));
    return out;
}

void project(const Mtx prj, const float v[3], float& s, float& t, float& q) {
    const float sq = prj[0][0] * v[0] + prj[0][1] * v[1] + prj[0][2] * v[2] + prj[0][3];
    const float tq = prj[1][0] * v[0] + prj[1][1] * v[1] + prj[1][2] * v[2] + prj[1][3];
    q = prj[2][0] * v[0] + prj[2][1] * v[1] + prj[2][2] * v[2] + prj[2][3];
    s = sq / q;
    t = tq / q;
}

void testShapes() {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> world(-2000.0f, 2000.0f);
    unsigned outside = 0, rects = 0;
    for (int trial = 0; trial < 200000; trial++) {
        Mtx cam, prj;
        makeCamera(rng, cam);
        if (trial % 5 == 0) {
            makeOrtho(prj);
        } else {
            makePerspective(20.0f + float(trial % 60), 1.6667f, prj);
        }
        const float w[3] = {world(rng), world(rng), world(rng)};
        const Particle p = makeParticle(rng, cam, trial % 8, w);
        for (int i = 0; i < p.count; i++) {
            const float dx = p.corners[i][0] - p.centre[0], dy = p.corners[i][1] - p.centre[1], dz = p.corners[i][2] - p.centre[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            CHECK(distance <= p.radius * 1.0001f + 1e-4f, "shape %d corner %d at %g outside radius %g", trial % 8, i, distance, p.radius);
        }
        jpa_native::CullPlanes planes;
        jpa_native::makeCullPlanes(prj, planes);
        if (jpa_native::sphereOutside(planes, p.centre, p.radius)) {
            outside++;
            bool one = false;
            for (const auto& plane : planes.plane) {
                bool all = true;
                for (int i = 0; i < p.count && all; i++) {
                    all = plane[0] * p.corners[i][0] + plane[1] * p.corners[i][1] + plane[2] * p.corners[i][2] + plane[3] < 0.0f;
                }
                one = one || all;
            }
            CHECK(one, "shape %d culled with a corner inside every side", trial % 8);
            continue;
        }
        Rect rect;
        if (jpa_native::sphereRect(prj, p.centre, p.radius, rect)) {
            rects++;
            for (int i = 0; i < p.count; i++) {
                float s, t, q;
                project(prj, p.corners[i], s, t, q);
                CHECK(q > 0.0f, "corner behind the eye inside a footprint");
                const float slack = 1e-4f * (1.0f + std::fabs(s) + std::fabs(t));
                CHECK(s >= rect.s0 - slack && s <= rect.s1 + slack && t >= rect.t0 - slack && t <= rect.t1 + slack,
                      "shape %d corner %d at %g,%g outside footprint %g..%g, %g..%g", trial % 8, i, s, t, rect.s0, rect.s1,
                      rect.t0, rect.t1);
            }
        }
    }
    CHECK(outside > 1000 && rects > 1000, "too few cases: outside %u footprints %u", outside, rects);
    std::printf("shapes: %u culled, %u footprints checked\n", outside, rects);
}

// --- End to end: list order against the run order --------------------------

constexpr int kWidth = 400, kHeight = 240;

struct Emitter {
    int resource;
    std::vector<Particle> particles;
    float colour[3];
    float alpha;
};

// Pixel centres inside a triangle (both windings), blended "over": the
// result depends on the order of the draws that cover a pixel.
void drawTriangle(std::vector<float>& image, const float a[2], const float b[2], const float c[2], const Emitter& e) {
    const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f) {
        return;
    }
    const int x0 = std::max(0, int(std::floor(std::fmin(a[0], std::fmin(b[0], c[0])))));
    const int x1 = std::min(kWidth - 1, int(std::ceil(std::fmax(a[0], std::fmax(b[0], c[0])))));
    const int y0 = std::max(0, int(std::floor(std::fmin(a[1], std::fmin(b[1], c[1])))));
    const int y1 = std::min(kHeight - 1, int(std::ceil(std::fmax(a[1], std::fmax(b[1], c[1])))));
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            const float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            const float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            const bool inside = area > 0.0f ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) {
                float* pixel = &image[(y * kWidth + x) * 3];
                for (int k = 0; k < 3; k++) {
                    pixel[k] = e.alpha * e.colour[k] + (1.0f - e.alpha) * pixel[k];
                }
            }
        }
    }
}

void drawEmitter(std::vector<float>& image, const Mtx prj, const Emitter& e) {
    for (const Particle& p : e.particles) {
        float screen[8][2];
        bool front = true;
        for (int i = 0; i < p.count; i++) {
            float s, t, q;
            project(prj, p.corners[i], s, t, q);
            front = front && q > 1e-3f;
            screen[i][0] = s * kWidth;
            screen[i][1] = t * kHeight;
        }
        if (!front) {
            continue; // a barrier in the search: never reordered, same in both orders
        }
        for (int first = 0; first < p.count; first += 4) {
            drawTriangle(image, screen[first], screen[first + 1], screen[first + 2], e);
            drawTriangle(image, screen[first], screen[first + 2], screen[first + 3], e);
        }
    }
}

void testRunOrder() {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    unsigned merged = 0, runs = 0, scenes = 0, culledEmitters = 0;
    for (int scene = 0; scene < 400; scene++) {
        Mtx cam = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, prj;
        makePerspective(60.0f, 1.6667f, prj);
        const int count = 4 + int(unit(rng) * 40);
        const int resources = 1 + int(unit(rng) * 4);
        std::vector<Emitter> emitters(count);
        for (Emitter& e : emitters) {
            e.resource = int(unit(rng) * resources);
            // Emitters cluster around a spot (torches), some off screen,
            // some close to the eye.
            const float cx = (unit(rng) - 0.5f) * 3000.0f, cy = (unit(rng) - 0.5f) * 1500.0f;
            const float cz = -(unit(rng) < 0.05f ? 30.0f + unit(rng) * 50.0f : 300.0f + unit(rng) * 3000.0f);
            const int particles = 1 + int(unit(rng) * 8);
            for (int k = 0; k < particles; k++) {
                const float w[3] = {cx + (unit(rng) - 0.5f) * 120.0f, cy + (unit(rng) - 0.5f) * 120.0f, cz + (unit(rng) - 0.5f) * 60.0f};
                e.particles.push_back(makeParticle(rng, cam, int(unit(rng) * 8), w));
            }
            for (float& c : e.colour) {
                c = unit(rng);
            }
            e.alpha = 0.25f + 0.5f * unit(rng);
        }
        // The survey (JPAResource.cpp, native_survey) and the draw loop.
        jpa_native::CullPlanes planes;
        jpa_native::makeCullPlanes(prj, planes);
        std::vector<jpa_native::RunCandidate> candidates(count);
        for (int i = 0; i < count; i++) {
            jpa_native::RunCandidate& c = candidates[i];
            c = {jpa_native::RunCandidate::nothing, false, false, jpa_native::emptyRect()};
            for (const Particle& p : emitters[i].particles) {
                if (jpa_native::sphereOutside(planes, p.centre, p.radius)) {
                    continue;
                }
                c.draws = jpa_native::RunCandidate::runnable;
                Rect r;
                if (!c.barrier && jpa_native::sphereRect(prj, p.centre, p.radius, r)) {
                    jpa_native::rectAdd(c.rect, r);
                } else {
                    c.barrier = true;
                }
            }
            // Some emitters draw at their place only (callbacks, stripes).
            if (c.draws == jpa_native::RunCandidate::runnable && unit(rng) < 0.1f) {
                c.draws = jpa_native::RunCandidate::own;
                c.barrier = c.barrier || unit(rng) < 0.5f;
            }
        }
        std::vector<int> order;
        for (int i = 0; i < count; i++) {
            if (candidates[i].done) {
                continue;
            }
            candidates[i].done = true;
            if (candidates[i].draws == jpa_native::RunCandidate::nothing) {
                culledEmitters++;
                continue;
            }
            if (candidates[i].draws == jpa_native::RunCandidate::own) {
                order.push_back(i);
                continue;
            }
            unsigned members[32];
            const unsigned found = jpa_native::gatherRun(candidates.data(), unsigned(count), unsigned(i), 64, 32, members,
                [&](unsigned a, unsigned b) { return emitters[a].resource == emitters[b].resource; });
            for (unsigned k = 0; k < found; k++) {
                order.push_back(int(members[k]));
            }
            if (found > 1) {
                runs++;
                merged += found - 1;
            }
        }
        std::vector<float> listImage(kWidth * kHeight * 3, 0.25f), runImage(kWidth * kHeight * 3, 0.25f);
        for (int i = 0; i < count; i++) {
            drawEmitter(listImage, prj, emitters[i]);
        }
        for (int i : order) {
            drawEmitter(runImage, prj, emitters[i]);
        }
        unsigned differing = 0;
        for (size_t k = 0; k < listImage.size(); k++) {
            differing += listImage[k] != runImage[k];
        }
        CHECK(differing == 0, "scene %d: %u values differ between list order and run order", scene, differing);
        scenes++;
    }
    CHECK(merged > 200, "the search merged only %u emitters", merged);
    CHECK(culledEmitters > 50, "only %u emitters left out", culledEmitters);
    std::printf("run order: %u scenes identical, %u runs carried %u extra emitters, %u emitters left out\n", scenes, runs,
                merged, culledEmitters);
}

void testRects() {
    const Rect a = {0.1f, 0.1f, 0.2f, 0.2f}, b = {0.25f, 0.1f, 0.3f, 0.2f}, c = {0.21f, 0.1f, 0.3f, 0.2f};
    CHECK(!jpa_native::rectsMeet(a, b), "disjoint rectangles meet");
    CHECK(jpa_native::rectsMeet(a, c), "rectangles within the margin do not meet");
    CHECK(jpa_native::rectsMeet(c, a), "rectsMeet is not symmetric");
    CHECK(!jpa_native::rectsMeet(a, jpa_native::emptyRect()), "an empty rectangle meets");
}

} // namespace

int main() {
    testRects();
    testShapes();
    testRunOrder();
    if (failures != 0) {
        std::printf("particle cull test: %d failures\n", failures);
        return 1;
    }
    std::printf("particle cull test: pass\n");
    return 0;
}
