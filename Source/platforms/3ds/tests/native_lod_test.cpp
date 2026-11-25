// Host check of the level-of-detail rules' arithmetic and of lod.txt
// (include/d/d_native_lod.h, the part shared by every target).
//   clang++ -std=c++20 -Wall -Wextra -fsanitize=address,undefined -I Source/include \
//       Source/platforms/3ds/tests/native_lod_test.cpp -o native_lod_test && ./native_lod_test
#include "d/d_native_lod.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace dNativeLod;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f * (1.0f + std::fabs(b)); }

int main() {
    // Defaults and presets.
    Config c;
    assert(c.drawFar == 6000.0f && c.bgPixels == 6.0f && c.actorPixels == 8.0f);
    assert(c.partPixels == 0.0f && c.shadowDepth == 1400.0f && c.skin && c.census);
    assert(c.grass[0] == 600.0f && c.grass[1] == 1400.0f && c.grass[2] == 2800.0f);
    Config none = c;
    assert(parseConfig("none", none) == 0);
    assert(none.partPixels == 0.0f && none.bgPixels == 0.0f && none.actorPixels == 0.0f);
    assert(none.shadowDepth == 2000.0f && !none.skin && none.grass[2] == 0.0f && none.drawFar > 1.0e8f);
    Config zero = c;
    assert(parseConfig("0\n", zero) == 0 && zero.partPixels == 0.0f && !zero.skin);
    Config a13 = c;
    assert(parseConfig("alpha13", a13) == 0);
    assert(a13.partPixels == 0.0f && a13.shadowDepth == 1400.0f && !a13.skin);
    assert(a13.grass[0] == 600.0f && a13.grass[1] == 1400.0f && a13.grass[2] == 2800.0f);
    assert(a13.bgPixels == 6.0f && a13.actorPixels == 8.0f && a13.drawFar == 6000.0f);

    // key=value words, in order, after a preset; bad words are counted and ignored.
    Config k = c;
    assert(parseConfig("alpha13 parts=2.5\r\nshadow=1250 grass=400,900,1800 skin=1 census=0", k) == 0);
    assert(k.partPixels == 2.5f && k.shadowDepth == 1250.0f && k.skin && !k.census);
    assert(k.grass[0] == 400.0f && k.grass[1] == 900.0f && k.grass[2] == 1800.0f);
    Config bad = c;
    assert(parseConfig("parts=99 shadow=500 grass=900,400,100 grass=300 skin=2 what far=10 x=", bad) == 8);
    assert(bad.partPixels == 0.0f && bad.shadowDepth == 1400.0f && bad.grass[0] == 600.0f && bad.skin);
    assert(bad.drawFar == 6000.0f);
    Config off = c;
    assert(parseConfig("grass=0 shadow=0 parts=0 skin=0", off) == 0);
    assert(off.grass[0] == 0.0f && off.grass[2] == 0.0f && off.shadowDepth == 2000.0f && !off.skin);

    // Pixels per unit from cot(fovy): 60 degrees -> 120 / tan(30) = 207.85.
    const float cot60 = 1.0f / std::tan(60.0f * 3.14159265f / 180.0f);
    assert(near(pixelsPerUnit(cot60), 207.846f));
    const float cot45 = 1.0f / std::tan(45.0f * 3.14159265f / 180.0f);
    assert(near(pixelsPerUnit(cot45), 120.0f / std::tan(22.5f * 3.14159265f / 180.0f)));
    assert(pixelsPerUnit(NAN) == 208.0f);

    // A villager's eyes (box half diagonal 8.5) at 1.5 px on a 60 degree
    // camera: hidden past 8.5 * 207.85 / 1.5 = 1178 units of depth.
    const float k60 = pixelsPerUnit(cot60);
    assert(!partTooSmall(8.5f, 1170.0f, k60, 1.5f));
    assert(partTooSmall(8.5f, 1190.0f, k60, 1.5f));
    assert(!partTooSmall(8.5f, 5000.0f, k60, 0.0f));   // rule off
    assert(!partTooSmall(8.5f, 0.5f, k60, 1.5f));      // reaches the camera plane
    assert(!partTooSmall(0.0f, 5000.0f, k60, 1.5f));   // unknown size: kept
    assert(!partTooSmall(NAN, 5000.0f, k60, 1.5f));

    // Grass steps: full, half, quarter, none; a 0 step is off.
    int shown[4] = {};
    const float depths[4] = {400.0f, 800.0f, 1500.0f, 3000.0f};
    for (int d = 0; d < 4; ++d)
        for (int i = 0; i < 1000; ++i) shown[d] += !foliageHidden(c, depths[d], i);
    assert(shown[0] == 1000 && shown[1] == 500 && shown[2] == 250 && shown[3] == 0);
    for (int i = 0; i < 64; ++i) assert(!foliageHidden(none, 1.0e6f, i));

    // Levels of detail: the coarsest level under the limit; going coarser
    // needs 0.8 of it (no flicker at the edge); 0 when nothing fits or off.
    {
        const float error[3] = {2.0f, 5.0f, 12.0f};
        const float limit = c.levelPixels;
        assert(limit == 0.75f && c.shadowLevelPixels == 0.75f && c.shadowPixels == 16.0f);
        assert(none.levelPixels == 0.0f && none.shadowLevelPixels == 0.0f && none.shadowPixels == 0.0f);
        // Error 12 at a villager 4500 units away: 12 * 208 / 4500 = 0.55 px.
        assert(chooseLevel(error, 3, k60 / 4500.0f, limit, 0) == 3);
        assert(chooseLevel(error, 3, k60 / 1000.0f, limit, 0) == 1);   // 2 * 0.208 = 0.42 px, 5 * 0.208 = 1.04 px
        assert(chooseLevel(error, 3, k60 / 300.0f, limit, 0) == 0);    // close: drawn whole
        assert(chooseLevel(error, 3, k60 / 4500.0f, 0.0f, 0) == 0);    // rule off
        assert(chooseLevel(error, 3, NAN, limit, 0) == 0);
        assert(chooseLevel(error, 2, k60 / 1.0e6f, limit, 0) == 2);    // never past its own levels
        // Hysteresis: level 1 at 2 units needs 2 * ppu <= 0.6 to be taken,
        // and is kept until 2 * ppu > 0.75.
        const float edge = 0.7f / 2.0f;                                // 0.7 px for level 1
        assert(chooseLevel(error, 3, edge, limit, 0) == 0);            // coming closer from whole: stays whole
        assert(chooseLevel(error, 3, edge, limit, 1) == 1);            // kept once taken
        assert(chooseLevel(error, 3, 0.8f / 2.0f, limit, 1) == 0);     // past the limit: back to whole
        // Shadow blobs by size: half-width 0.4 * 600 = 240 units at depth
        // 3000 is 16.6 px (kept), at 3200 15.6 px (blob).
        assert(!shadowTooSmall(600.0f, 3000.0f, k60, 16.0f));
        assert(shadowTooSmall(600.0f, 3200.0f, k60, 16.0f));
        assert(!shadowTooSmall(600.0f, 0.5f, k60, 16.0f));             // reaches the camera
        assert(!shadowTooSmall(600.0f, 1.0e5f, k60, 0.0f));            // rule off
        Config words;
        assert(parseConfig("levels=1.5 shadowlevels=0.5 shadowpx=24", words) == 0);
        assert(words.levelPixels == 1.5f && words.shadowLevelPixels == 0.5f && words.shadowPixels == 24.0f);
        assert(parseConfig("levels=9", words) == 1 && words.levelPixels == 1.5f);
    }

    std::printf("native_lod_test: ok\n");
    return 0;
}
