#ifndef D_NATIVE_LOD_H
#define D_NATIVE_LOD_H

// Native New 3DS cost reduction: screen-size level of detail and a shorter
// actor draw distance, without fog changes. Defaults may be overridden by an
// optional SD file sdmc:/3ds/DuskLight/draw-distance.txt containing
// "FAR [BG_PIXELS [ACTOR_PIXELS]]".
#if defined(DUSK_TARGET_3DS)

#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <cmath>
#include <cstdio>

namespace dNativeLod {

struct Config {
    f32 drawFar;     // actors, grass and other clipper users (world units)
    f32 bgPixels;    // room shapes projecting smaller than this are skipped
    f32 actorPixels; // actors projecting smaller than this are skipped
};

inline const Config& config() {
    static const Config value = [] {
        Config result = {6000.0f, 6.0f, 8.0f};
        if (FILE* file = std::fopen("sdmc:/3ds/DuskLight/draw-distance.txt", "rb")) {
            float drawFar = 0.0f, bgPixels = 0.0f, actorPixels = 0.0f;
            const int count = std::fscanf(file, "%f %f %f", &drawFar, &bgPixels, &actorPixels);
            std::fclose(file);
            if (count >= 1 && drawFar >= 1000.0f && drawFar <= 1000000.0f) {
                result.drawFar = drawFar;
            }
            if (count >= 2 && bgPixels >= 0.0f && bgPixels <= 64.0f) {
                result.bgPixels = bgPixels;
            }
            if (count >= 3 && actorPixels >= 0.0f && actorPixels <= 64.0f) {
                result.actorPixels = actorPixels;
            }
        }
        OSReport("Native LOD drawFar=%.0f bgPixels=%.1f actorPixels=%.1f\n", result.drawFar,
                 result.bgPixels, result.actorPixels);
        return result;
    }();
    return value;
}

// True when a sphere (local center/radius under mtx) projects smaller than the
// pixel threshold. 240 approximates the vertical projection scale at 400x240.
inline bool tooSmall(const Mtx mtx, const Vec& center, f32 radius, f32 minPixels) {
    if (minPixels <= 0.0f) {
        return false;
    }
    const f32 scale = std::sqrt(mtx[0][0] * mtx[0][0] + mtx[1][0] * mtx[1][0] +
                                mtx[2][0] * mtx[2][0]);
    const f32 z = mtx[2][0] * center.x + mtx[2][1] * center.y + mtx[2][2] * center.z + mtx[2][3];
    const f32 r = radius * scale;
    const f32 distance = -z - r;
    if (distance <= 0.0f) {
        return false;
    }
    return r * 240.0f < minPixels * distance;
}

inline bool tooSmall(const Mtx mtx, const Vec& min, const Vec& max, f32 minPixels) {
    const Vec center = {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    const f32 dx = max.x - min.x, dy = max.y - min.y, dz = max.z - min.z;
    return tooSmall(mtx, center, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), minPixels);
}

// Grass/flower level of detail in steps: full density near the camera, half
// past 600 units, a quarter past 1400, none past 2800.
inline bool hideFoliage(const Mtx view, const Vec& pos, int index) {
    const f32 distance =
        -(view[2][0] * pos.x + view[2][1] * pos.y + view[2][2] * pos.z + view[2][3]);
    if (distance > 2800.0f) {
        return true;
    }
    if (distance > 1400.0f) {
        return (index & 3) != 0;
    }
    return distance > 600.0f && (index & 1) != 0;
}

}  // namespace dNativeLod

#endif
#endif
