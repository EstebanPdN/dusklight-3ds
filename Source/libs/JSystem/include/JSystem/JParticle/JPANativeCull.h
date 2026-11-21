#ifndef JPANATIVECULL_H
#define JPANATIVECULL_H

// Native 3DS particle drawing: the screen-space tests the emitter loop uses
// before it sets any GX state (JPAResource.cpp, JPANativeDrawGroup). Plain
// float math with no game headers, so the host test
// (Source/platforms/3ds/tests/particle_native_cull_test.cpp) checks the same
// code the game runs.
//
// The particle group's projection matrix (JPAEmitterWorkData::mPrjMtx, 3x4)
// maps a view-space point p to (s*q, t*q, q) with the visible picture at
// s, t in 0..1, for perspective and orthographic groups alike.
//
// A particle is bounded by a sphere around its view-space centre (twice its
// scaled quad: it covers the pivot shift and any rotation, see the batch
// loop). Two tests follow from it:
//  - outside: the sphere lies wholly beyond one side of the view (the quad
//    reaches no pixel);
//  - footprint: a rectangle in (s, t) that holds every point of the sphere
//    seen in front of the eye. Two emitters whose footprints do not meet
//    touch no common pixel, so the order in which they are drawn does not
//    change the picture (blending, depth test and depth writes act per
//    pixel).

#include <cmath>

namespace jpa_native {

struct CullPlanes {
    float plane[5][4];
};

// In front of the eye, s >= 0, s <= 1, t >= 0, t <= 1, each with a unit
// normal (distances in view-space units). An orthographic matrix has no
// plane through the eye: that plane never culls.
inline void makeCullPlanes(const float prj[3][4], CullPlanes& out) {
    for (int i = 0; i < 4; i++) {
        out.plane[0][i] = prj[2][i];
        out.plane[1][i] = prj[0][i];
        out.plane[2][i] = prj[2][i] - prj[0][i];
        out.plane[3][i] = prj[1][i];
        out.plane[4][i] = prj[2][i] - prj[1][i];
    }
    for (auto& plane : out.plane) {
        const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (length > 1.0e-12f) {
            for (float& value : plane) {
                value /= length;
            }
        } else {
            plane[0] = plane[1] = plane[2] = 0.0f;
            plane[3] = 1.0f;
        }
    }
}

inline bool sphereOutside(const CullPlanes& planes, const float centre[3], float radius) {
    for (const auto& plane : planes.plane) {
        if (plane[0] * centre[0] + plane[1] * centre[1] + plane[2] * centre[2] + plane[3] < -radius) {
            return true;
        }
    }
    return false;
}

// Screen rectangle {s0, t0, s1, t1}. Empty when s0 > s1.
struct Rect {
    float s0, t0, s1, t1;
};

inline Rect emptyRect() {
    return {1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f};
}

inline bool rectEmpty(const Rect& r) {
    return r.s0 > r.s1 || r.t0 > r.t1;
}

inline void rectAdd(Rect& into, const Rect& r) {
    into.s0 = r.s0 < into.s0 ? r.s0 : into.s0;
    into.t0 = r.t0 < into.t0 ? r.t0 : into.t0;
    into.s1 = r.s1 > into.s1 ? r.s1 : into.s1;
    into.t1 = r.t1 > into.t1 ? r.t1 : into.t1;
}

// The rectangle of a sphere in front of the eye. False when part of the
// sphere reaches the eye plane (q <= minQ): the footprint is then unknown
// (the quad may cover any part of the picture once clipped).
//
// Over the sphere, n = row . p + w lies within n(c) -+ r * |row| and the
// same holds for q; with q > 0 throughout, n / q is largest and smallest at
// the corners of that box.
inline bool sphereRect(const float prj[3][4], const float centre[3], float radius, Rect& out) {
    float n[3], spread[3];
    for (int row = 0; row < 3; row++) {
        n[row] = prj[row][0] * centre[0] + prj[row][1] * centre[1] + prj[row][2] * centre[2] + prj[row][3];
        spread[row] = radius * std::sqrt(prj[row][0] * prj[row][0] + prj[row][1] * prj[row][1] +
                                         prj[row][2] * prj[row][2]);
    }
    const float qLow = n[2] - spread[2], qHigh = n[2] + spread[2];
    if (!(qLow > 1.0e-4f * (qHigh > 1.0f ? qHigh : 1.0f))) {
        return false;
    }
    float low[2], high[2];
    for (int axis = 0; axis < 2; axis++) {
        const float nLow = n[axis] - spread[axis], nHigh = n[axis] + spread[axis];
        const float a = nLow / qLow, b = nLow / qHigh, c = nHigh / qLow, d = nHigh / qHigh;
        low[axis] = a < b ? a : b;
        high[axis] = c > d ? c : d;
    }
    out = {low[0], low[1], high[0], high[1]};
    return true;
}

// Rectangles that come closer than `margin` count as meeting: the margin
// covers rasterisation rounding (a pixel of the 400x240 picture is 0.0025 s
// and 0.0042 t; the default margin is several pixels).
constexpr float footprintMargin = 0.02f;

inline bool rectsMeet(const Rect& a, const Rect& b, float margin = footprintMargin) {
    if (rectEmpty(a) || rectEmpty(b)) {
        return false;
    }
    return a.s0 <= b.s1 + margin && b.s0 <= a.s1 + margin && a.t0 <= b.t1 + margin && b.t0 <= a.t1 + margin;
}

// One emitter of a particle group, as the run search sees it.
struct RunCandidate {
    enum Draws : unsigned char {
        nothing,  // draws no pixel (hidden, left out, no particle and no callback)
        own,      // draws at its place, never in a run
        runnable, // may share a batch with compatible emitters
    };
    Draws draws;
    bool barrier; // may draw anywhere (footprint unknown)
    bool done;    // drawn already (or pulled into an earlier run)
    Rect rect;    // footprint when not a barrier
};

// The emitters after `head` that are drawn together with it, in list order
// (members[0] = head). A candidate may be drawn at the head's place only if
// its footprint meets none of the emitters it passes, i.e. those still to be
// drawn after the run; a barrier ends the search. Members are marked done.
// Every pair of emitters whose order changes then has disjoint footprints,
// so the picture is the one the list order gives.
template <class Compatible>
unsigned gatherRun(RunCandidate* c, unsigned count, unsigned head, unsigned window, unsigned limit,
                   unsigned* members, Compatible compatible) {
    constexpr unsigned pendingLimit = 16;
    Rect pending[pendingLimit];
    unsigned pendingCount = 0, found = 1;
    members[0] = head;
    for (unsigned j = head + 1; j < count && j <= head + window && found < limit; j++) {
        RunCandidate& t = c[j];
        if (t.done || t.draws == RunCandidate::nothing) {
            continue;
        }
        if (t.draws == RunCandidate::runnable && compatible(head, j)) {
            bool meets = pendingCount != 0 && t.barrier;
            for (unsigned k = 0; k < pendingCount && !meets; k++) {
                meets = rectsMeet(t.rect, pending[k]);
            }
            if (!meets) {
                members[found++] = j;
                t.done = true;
                continue;
            }
        }
        if (t.barrier) {
            break;
        }
        if (pendingCount < pendingLimit) {
            pending[pendingCount++] = t.rect;
        } else {
            rectAdd(pending[pendingLimit - 1], t.rect);
        }
    }
    return found;
}

}  // namespace jpa_native

#endif
