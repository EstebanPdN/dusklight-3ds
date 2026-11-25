#ifndef HELPERS_NATIVE_EFFECTS_H
#define HELPERS_NATIVE_EFFECTS_H

// Native 3DS effect paths. These effects are not the GameCube sequences run
// through the GX translator: each one is drawn in the cheapest form the PICA
// can show with (nearly) the same picture. The platform sets the switches from
// the SD card before the game starts and prints the counters with the
// profiled frame; the game code only reads the switches and adds to counters.
namespace dusk::native_effects {
// 0 = no shadows, 1 = blob shadows only, 2 = blob + projected model shadows.
// The console stopped in the model-shadow pass in Alpha 4 and Alpha 5; the
// pass replayed draws whose arena geometry had been reclaimed (nearly once
// per pass in the opening cutscene). Those replays are left out since Alpha 6.
// If the pass still stops the PICA, the GPU guard records a shadow-stop and
// the next start keeps blob shadows. shadows.txt (0, 1 or 2) overrides.
inline int shadows = 2;
// 0 = no world particles, 1 = one draw per particle (original), 2 = one draw
// per emitter where the emitter allows it.
inline int particles = 2;
// 0 = no bloom, 1 = PICA bloom (one frame copy, two small targets).
inline int bloom = 1;
// Frame-copy ("dummy") particles: 0 = left out (Alpha 6), 1 = those the
// original draws without a warp, 2 = also the warped ones, with a sway in
// place of the warp (default); 1 and 2 only in frames where another draw
// already made the frame copy; 3 = always (a copy each frame).
// particleMask: 1 = batched masks read the particle's own coordinates
// (original, default), 0 = Alpha 6. Both defined in JPAResource.cpp;
// refraction.txt and particle-mask.txt override.
extern int refraction;
extern int particleMask;
// Emitter loop of the particle groups (JPAResource.cpp, JPANativeDrawGroup):
// 0 = every emitter draws on its own (Alpha 11); 1 = emitters with nothing on
// screen set no GX state; 2 = also emitters of one resource share one batch
// where that cannot change the picture (default). particle-runs.txt overrides.
// particleCullPixels: an emitter whose whole footprint is smaller than this
// many pixels in both directions is left out too; 0 = off (default),
// particle-size-cull.txt overrides.
extern int particleRuns;
extern float particleCullPixels;

struct Counters {
    unsigned simpleShadows;        // blob quads drawn
    unsigned simpleShadowDraws;    // GX draws used for them
    unsigned realShadows;          // model shadows with an image
    unsigned realShadowTriangles;  // receiver triangles after clipping
    unsigned realShadowDemoted;    // model shadows drawn as a blob (over the limit)
    unsigned particleBatches;      // emitter draws that carried several particles
    unsigned particleBatched;      // particles inside those draws
    unsigned particleSingle;       // particles drawn one by one
    unsigned particleCulled;       // batched particles left out: off screen
    unsigned particleDirect;       // batched particles drawn without their indirect warp
    unsigned particleEmitters;     // emitters drawn
    // Particles of emitters that could not be one draw: 0 draw function kind
    // (points, lines, per-particle texture...), 1 projected texture,
    // 2 particle callback, 3 colour formula.
    unsigned particleUnbatched[4];
    // Extra draws of a batched emitter: 0 the environment colour changed,
    // 1 the texture changed.
    unsigned particleBreaks[2];
    unsigned particleRefraction;   // frame-copy (refraction) particles left out
    unsigned bloomPasses;          // frames with the bloom filter
};
inline Counters counters{};

// Heat haze (the original draws nine screen-sized particles that sample the
// frame copy through an indirect warp). The actor leaves its strength here
// each frame; the screen-effect pass draws the frame copy back through a
// swaying grid and clears the request.
struct Haze {
    unsigned alpha;   // 0 = none, 255 = the warped frame replaces the frame
    float amplitude;  // largest shift, as a fraction of the screen
};
inline Haze haze{};

// Profiled frame only: one row per particle resource drawn in that frame, so
// the report says which effect costs what. Flags: 1 frame-buffer texture,
// 2 indirect warp, 4 projected texture, 8 child shape, 16 particle callback,
// 32 emitter callback, 64 per-particle texture, 128 per-particle texture
// matrix; bits 8-11 shape type, 12-14 colour formula, 16-17 blend mode,
// 18 refraction left out, 19 refraction drawn with a sway.
struct EmitterRow {
    const void* resource;
    unsigned id, flags, emitters, particles, batches, singles;
    unsigned long long ticks;
};
inline bool emitterReport = false;
inline EmitterRow emitterRows[40]{};
inline unsigned emitterRowCount = 0;
}

#if defined(DUSK_TARGET_3DS)
// Second-core worker (platforms/3ds/runtime/gx_worker.hpp): draws issued
// through GX can be handed to a second processor core, which reads them
// after the game thread went on.
// duskNativeDeferRuns: the painter, around its 3D lists. Outside, every draw
// that is not a J3D shape is drawn before the call that issued it returns.
// duskNativeArraysStable: the worker reads the vertex arrays a draw indexes
// later. Arrays in constant data are always safe. Code whose arrays are
// writable memory that nothing writes until the picture is painted (tables
// converted once at start) declares that around its draws; without the
// declaration such draws stay on the game thread. Never declare it for an
// array that is filled again before the picture ends.
extern "C" void duskNativeDeferRuns(int on);
extern "C" void duskNativeArraysStable(int on);
namespace dusk::native_effects {
struct StableArrays {
    StableArrays() { duskNativeArraysStable(1); }
    ~StableArrays() { duskNativeArraysStable(0); }
    StableArrays(const StableArrays&) = delete;
    StableArrays& operator=(const StableArrays&) = delete;
};
}
#endif

#endif
