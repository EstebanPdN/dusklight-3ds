#ifndef HELPERS_NATIVE_EFFECTS_H
#define HELPERS_NATIVE_EFFECTS_H

// Native 3DS effect paths. These effects are not the GameCube sequences run
// through the GX translator: each one is drawn in the cheapest form the PICA
// can show with (nearly) the same picture. The platform sets the switches from
// the SD card before the game starts and prints the counters with the
// profiled frame; the game code only reads the switches and adds to counters.
namespace dusk::native_effects {
// 0 = no shadows, 1 = blob shadows only, 2 = blob + projected model shadows.
inline int shadows = 2;
// 0 = no world particles, 1 = one draw per particle (original), 2 = one draw
// per emitter where the emitter allows it.
inline int particles = 2;
// 0 = no bloom, 1 = PICA bloom (one frame copy, two small targets).
inline int bloom = 1;

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
    unsigned particleRefraction;   // particles drawn as screen refraction
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
// matrix; bits 8-11 shape type, 12-14 colour formula, 16-17 blend mode.
struct EmitterRow {
    const void* resource;
    unsigned id, flags, emitters, particles, batches, singles;
    unsigned long long ticks;
};
inline bool emitterReport = false;
inline EmitterRow emitterRows[40]{};
inline unsigned emitterRowCount = 0;
}

#endif
