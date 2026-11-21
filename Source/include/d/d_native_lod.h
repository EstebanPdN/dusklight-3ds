#ifndef D_NATIVE_LOD_H
#define D_NATIVE_LOD_H

// Native New 3DS level of detail: every rule and threshold in one place.
//
// The owner's ask (2026-11-18): far things drawn with less, things too small
// on the 400x240 screen not drawn, the game complete and playable, the picture
// changed only subtly; shadows and fog stay. Each rule is decided per picture
// from what the camera sees and only changes what is drawn: no rule changes
// game logic, collision or what an actor computes.
//
//   far    actors, grass and other clipper users stop at this camera distance
//          (room geometry keeps the original one)                [Candidate 91]
//   bg     room shapes whose box projects under N px are hidden  [Candidate 91]
//   actor  actors whose cull volume projects under N px are not drawn (Link
//          and events exempt)                                     [Candidate 91]
//          (bg and actor use the Candidate 91 measure: radius x 240 / depth)
//   parts  a part (J3D shape) of an ordinary actor's model whose box projects
//          under N screen pixels (radius) is not entered: the eyes of a
//          villager 1200 units away, a fish a few pixels long. Link, Epona, the
//          locked-on actor and every actor during an event are exempt. Sizes
//          come from the SHP1 boxes, checked on all 13,371 shapes of the disc
//          (a one-off survey; its tool was not kept): a one-matrix shape's box is its
//          extent in its joint's space (scaled by the joint's world matrix); a
//          shape of several matrices has its bind-pose extent in the model's
//          space (taken 1.5 times: the largest bind-pose-to-box ratio of such
//          shapes on the disc is 1.40), placed by the model's base matrix and
//          widened by the root joint's animated offset.
//   shadow outside events, the model shadow of a caster other than Link and
//          Epona whose near edge is past this camera depth is drawn as a blob
//          of the same darkness (the original fades model shadows out between
//          depths 1000 and 2000; Alpha 13 made the blob past 1400)
//   grass  grass and flowers: half density past the first camera depth, a
//          quarter past the second, none past the third
//   skin   the skin (envelope) matrices of a model are made for the picture
//          that draws it, not on the logic ticks in between whose picture is
//          never drawn: the picture is the same, the ticks cost less
//   levels (switch `lod`, runtime/perf_features.hpp) a shape with levels of
//          detail (sdmc:/3ds/DuskLight/zelda.lod, Tools/3ds/model_lod.py) is
//          drawn with the coarsest level whose error (the largest distance
//          of the shape's surface to the reduced one, model units) projects
//          under N screen pixels at the near edge of the shape's bound;
//          going coarser needs 0.8 N, going back finer happens past N, so a
//          shape at the edge does not flicker. Never Link's models, never an
//          actor's during an event; room models too (they are no participant).
//   shadowlevels (switch `shadow`) every model shadow caster, Link and
//          Epona too, is replayed with the coarsest level whose error stays
//          under N pixels of the shadow image (its tile is 64 or 128 pixels
//          for a box of 0.8 x the caster's shadow size)
//   shadowpx (switch `shadow`) outside events, the model shadow of a caster
//          other than Link and Epona whose half-width projects under N
//          pixels at its near edge is a blob of the same darkness, as past
//          the shadow depth
//
// sdmc:/3ds/DuskLight/lod.txt, read once at boot: words separated by spaces or
// lines. "none" turns every rule off (the original game), "alpha13" restores
// Alpha 13 (the Candidate 91 rules and the 1400 shadow depth only); after them
// "key=value" words change one value: far=6000 bg=6 actor=8 parts=1.5
// shadow=1100 grass=500,1200,2400 skin=1 census=1 levels=0.75
// shadowlevels=0.75 shadowpx=16 (0 turns a rule off;
// shadow=2000 never makes a blob). The older draw-distance.txt
// ("FAR [BG [ACTOR]]") is read first and still works.
// census=1 writes two log lines every 300 drawn pictures ("Native LOD ...").

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dNativeLod {

// Defaults (Alpha 14): the Alpha 13 picture (parts off, shadow blob past 1400,
// grass 600/1400/2800) plus the exact skin rule. Measured in Azahar, Ordon
// village, one tick per picture: parts=1.5 shadow=1100 grass=500,1200,2400
// saved 2 draws of 275 and no time (94.1 against 93.5 ms), so the picture
// stays as it was; lod.txt still selects them.
struct Config {
    float drawFar = 6000.0f;
    float bgPixels = 6.0f;
    float actorPixels = 8.0f;
    float partPixels = 0.0f;
    float shadowDepth = 1400.0f;
    float grass[3] = {600.0f, 1400.0f, 2800.0f};
    bool skin = true;
    bool census = true;
    // Behind their switches (perf_features.hpp lod, shadow): measured in the
    // emulator against the reference picture before they were turned on.
    float levelPixels = 0.75f;
    float shadowLevelPixels = 0.75f;
    float shadowPixels = 16.0f;
};

inline Config noneConfig() {
    Config c;
    c.drawFar = 1.0e9f;
    c.bgPixels = c.actorPixels = c.partPixels = 0.0f;
    c.shadowDepth = 2000.0f;
    c.grass[0] = c.grass[1] = c.grass[2] = 0.0f;
    c.skin = false;
    c.levelPixels = c.shadowLevelPixels = c.shadowPixels = 0.0f;
    return c;
}

inline Config alpha13Config() {
    Config c;
    c.partPixels = 0.0f;
    c.shadowDepth = 1400.0f;
    c.grass[0] = 600.0f;
    c.grass[1] = 1400.0f;
    c.grass[2] = 2800.0f;
    c.skin = false;
    return c;
}

// Applies the words of lod.txt to c; returns the number of words not
// understood (each is left without effect).
inline int parseConfig(const char* text, Config& c) {
    int refused = 0;
    char word[64];
    const char* at = text;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n' || *at == ';') {
            ++at;
        }
        size_t n = 0;
        while (at[n] != '\0' && at[n] != ' ' && at[n] != '\t' && at[n] != '\r' && at[n] != '\n' &&
               at[n] != ';') {
            ++n;
        }
        if (n == 0) {
            break;
        }
        const size_t kept = n < sizeof(word) - 1 ? n : sizeof(word) - 1;
        std::memcpy(word, at, kept);
        word[kept] = '\0';
        at += n;
        if (!std::strcmp(word, "none") || !std::strcmp(word, "0") || !std::strcmp(word, "off")) {
            c = noneConfig();
            continue;
        }
        if (!std::strcmp(word, "alpha13")) {
            c = alpha13Config();
            continue;
        }
        char* value = std::strchr(word, '=');
        if (value == nullptr) {
            ++refused;
            continue;
        }
        *value++ = '\0';
        char* end = nullptr;
        const float v = std::strtof(value, &end);
        if (end == value) {
            ++refused;
            continue;
        }
        if (!std::strcmp(word, "far") && v >= 1000.0f && v <= 1.0e9f) {
            c.drawFar = v;
        } else if (!std::strcmp(word, "bg") && v >= 0.0f && v <= 64.0f) {
            c.bgPixels = v;
        } else if (!std::strcmp(word, "actor") && v >= 0.0f && v <= 64.0f) {
            c.actorPixels = v;
        } else if (!std::strcmp(word, "parts") && v >= 0.0f && v <= 16.0f) {
            c.partPixels = v;
        } else if (!std::strcmp(word, "shadow") && (v == 0.0f || (v >= 1000.0f && v <= 2000.0f))) {
            c.shadowDepth = v == 0.0f ? 2000.0f : v;
        } else if (!std::strcmp(word, "grass") && v >= 0.0f) {
            float steps[3] = {v, 0.0f, 0.0f};
            int count = 1;
            while (count < 3 && *end == ',') {
                const char* next = end + 1;
                steps[count] = std::strtof(next, &end);
                if (end == next || steps[count] < steps[count - 1]) {
                    break;
                }
                ++count;
            }
            if (v == 0.0f) {
                c.grass[0] = c.grass[1] = c.grass[2] = 0.0f;
            } else if (count == 3) {
                c.grass[0] = steps[0];
                c.grass[1] = steps[1];
                c.grass[2] = steps[2];
            } else {
                ++refused;
            }
        } else if (!std::strcmp(word, "skin") && (v == 0.0f || v == 1.0f)) {
            c.skin = v != 0.0f;
        } else if (!std::strcmp(word, "census") && (v == 0.0f || v == 1.0f)) {
            c.census = v != 0.0f;
        } else if (!std::strcmp(word, "levels") && v >= 0.0f && v <= 8.0f) {
            c.levelPixels = v;
        } else if (!std::strcmp(word, "shadowlevels") && v >= 0.0f && v <= 8.0f) {
            c.shadowLevelPixels = v;
        } else if (!std::strcmp(word, "shadowpx") && v >= 0.0f && v <= 64.0f) {
            c.shadowPixels = v;
        } else {
            ++refused;
        }
    }
    return refused;
}

// Screen pixels per world unit at depth 1 for the radius of a sphere, from
// the clipper's cot(fovy) (mDoLib_clipper::getFovyRate): the vertical half
// field of view covers 120 of the 240 lines.
inline float pixelsPerUnit(float cotFovy) {
    if (!(cotFovy > -1.0e6f && cotFovy < 1.0e6f)) {
        return 208.0f;
    }
    return 120.0f * (cotFovy + std::sqrt(1.0f + cotFovy * cotFovy));
}

// A sphere of this radius at this depth (world units in front of the camera)
// covers less than minPixels of radius on the top screen. A sphere that
// reaches the camera plane never does.
inline bool partTooSmall(float radius, float depth, float unitPixels, float minPixels) {
    if (minPixels <= 0.0f || !(radius > 0.0f) || depth <= 1.0f) {
        return false;
    }
    return radius * unitPixels < minPixels * depth;
}

// Level of detail of a shape (rules "levels" and "shadowlevels"): the
// coarsest of `levels` whose error (model units, error[l - 1] for level l)
// times `pixelsPerUnit` stays under the limit; a level coarser than the one
// kept so far must stay under 0.8 of it. 0: the shape is drawn whole.
inline unsigned chooseLevel(const float* error, unsigned levels, float pixelsPerUnit, float limit, unsigned previous) {
    if (limit <= 0.0f || !(pixelsPerUnit > 0.0f) || !(pixelsPerUnit < 1.0e6f)) {
        return 0;
    }
    for (unsigned l = levels; l >= 1; --l) {
        const float allowed = l <= previous ? limit : 0.8f * limit;
        if (error[l - 1] * pixelsPerUnit <= allowed) {
            return l;
        }
    }
    return 0;
}

// Rule "shadowpx": a model shadow whose half-width (0.4 x the caster's
// shadow size, setReal) projects under minPixels at the near edge of its
// volume. A volume that reaches the camera plane never does.
inline bool shadowTooSmall(float size, float nearDepth, float unitPixels, float minPixels) {
    if (minPixels <= 0.0f || !(size > 0.0f) || !(nearDepth > 1.0f)) {
        return false;
    }
    return 0.4f * size * unitPixels < minPixels * nearDepth;
}

// Grass and flower density in steps, by camera depth; a step at 0 is off.
inline bool foliageHidden(const Config& c, float depth, int index) {
    if (c.grass[2] > 0.0f && depth > c.grass[2]) {
        return true;
    }
    if (c.grass[1] > 0.0f && depth > c.grass[1]) {
        return (index & 3) != 0;
    }
    return c.grass[0] > 0.0f && depth > c.grass[0] && (index & 1) != 0;
}


}  // namespace dNativeLod

#if defined(DUSK_TARGET_3DS)

#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <cstdio>

namespace dNativeLod {

inline const Config& config() {
    static const Config value = [] {
        Config result;
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
        int refused = 0;
        if (FILE* file = std::fopen("sdmc:/3ds/DuskLight/lod.txt", "rb")) {
            char text[512] = {};
            (void)std::fread(text, 1, sizeof(text) - 1, file);
            std::fclose(file);
            refused = parseConfig(text, result);
        }
        OSReport("Native LOD far=%.0f bg=%.1f actor=%.1f parts=%.1f px shadow=%.0f "
                 "grass=%.0f,%.0f,%.0f skin=%s census=%s levels=%.2f shadowlevels=%.2f shadowpx=%.1f%s "
                 "(SD switch lod.txt: none, alpha13, key=value)\n",
                 result.drawFar, result.bgPixels, result.actorPixels, result.partPixels,
                 result.shadowDepth, result.grass[0], result.grass[1], result.grass[2],
                 result.skin ? "on" : "off", result.census ? "on" : "off", result.levelPixels,
                 result.shadowLevelPixels, result.shadowPixels, refused ? "; words refused in lod.txt" : "");
        return result;
    }();
    return value;
}

// What the rules did, counted over the pictures of one census window (game
// thread only: the draw phase and the logic ticks).
struct Census {
    unsigned pictures, eventPictures;
    unsigned shapes, groups;          // entered by the model entry helpers
    unsigned partShapes, partGroups;  // left out by the parts rule
    unsigned actors, actorGroups;     // tiny actors (actor rule), groups they did not enter
    unsigned shadowKept, shadowBlobs; // model shadows of other casters: kept, made blobs
    unsigned foliageTested, foliageHidden;
    unsigned groupsByProc[800];
};
inline Census census{};

// The process whose draw method is running (f_pc_draw.cpp), or null, and
// whether it is an actor too small to enter its models (f_op_actor.cpp).
inline const void* drawOwner = nullptr;
inline short drawOwnerName = -1;
inline bool drawOwnerTiny = false;

// Draw phase of one picture (m_Do_ext.cpp): per-picture values, the census.
void beginDrawPhase();
// Counts draw phases: a shape packet's level is used by the painter only
// when the packet was entered in the latest one (J3DPacket.cpp).
inline unsigned drawPhase = 0;
// Screen pixels per world unit at depth 1 of this draw phase (pixelsPerUnit).
inline float unitPixels = 208.0f;

// True when a sphere (local center/radius under mtx) projects smaller than the
// pixel threshold (Candidate 91 measure: 240 x radius / depth).
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

// Grass/flower level of detail in steps (see "grass" above).
inline bool hideFoliage(const Mtx view, const Vec& pos, int index) {
    const f32 depth =
        -(view[2][0] * pos.x + view[2][1] * pos.y + view[2][2] * pos.z + view[2][3]);
    const bool hidden = foliageHidden(config(), depth, index);
    ++census.foliageTested;
    census.foliageHidden += hidden;
    return hidden;
}

}  // namespace dNativeLod

#endif
#endif
