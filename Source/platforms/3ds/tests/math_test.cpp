#include <dolphin/mtx.h>
#include <dolphin/ppc_math.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

static void near(float actual, float expected, float tolerance = 0.00002f) {
    assert(std::isfinite(actual));
    assert(std::fabs(actual - expected) <= tolerance);
}

static void identity(const Mtx m) {
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 4; ++c)
            near(m[r][c], r == c ? 1.0f : 0.0f);
}

int main() {
    // Link C implementation units through the exact C++ declarations used by JSystem.
    Mtx translation, scale, transform, inverse, product;
    PSMTXTrans(translation, 10, -2, 7);
    PSMTXScale(scale, 2, 3, 4);
    PSMTXConcat(translation, scale, transform);
    Vec v{1, 2, 3};
    PSMTXMultVec(transform, &v, &v);
    near(v.x, 12); near(v.y, 4); near(v.z, 19);
    assert(PSMTXInverse(transform, inverse) == 1);
    PSMTXConcat(transform, inverse, product);
    identity(product);
    // In-place concatenation and inversion occur in model and camera code.
    PSMTXConcat(transform, inverse, inverse);
    identity(inverse);
    PSMTXCopy(transform, inverse);
    assert(PSMTXInverse(inverse, inverse) == 1);
    PSMTXConcat(inverse, transform, inverse);
    identity(inverse);
    PSMTXScale(product, 0, 1, 1);
    assert(PSMTXInverse(product, inverse) == 0);

    Mtx stackStorage[3];
    MtxStack stack{};
    stack.stackBase = stackStorage[0];
    MTXInitStack(&stack, 3);
    assert(MTXGetStackPtr(&stack) == nullptr);
    MTXPush(&stack, translation);
    MTXPushFwd(&stack, scale);
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 4; ++c)
            near(MTXGetStackPtr(&stack)[r][c], transform[r][c]);
    assert(MTXPop(&stack) == stackStorage[0]);
    assert(MTXPop(&stack) == nullptr);
    assert(MTXPop(&stack) == nullptr);

    Vec length5{3, 4, 0}, zero{};
    near(PSVECMag(&length5), 5);
    near(PSVECMag(&zero), 0);
    PSVECNormalize(&length5, &length5);
    near(length5.x, .6f); near(length5.y, .8f);
    Vec axis{0, 0, 1};
    Quaternion rotation, rotationInverse, quaternionIdentity;
    QUATRotAxisRad(&rotation, &axis, 1.5707963267948966f);
    QUATInverse(&rotation, &rotationInverse);
    QUATMultiply(&rotation, &rotationInverse, &quaternionIdentity);
    near(quaternionIdentity.x, 0); near(quaternionIdentity.y, 0);
    near(quaternionIdentity.z, 0); near(quaternionIdentity.w, 1);
    PSMTXQuat(product, &rotation);
    v = {1, 0, 0};
    PSMTXMultVecSR(product, &v, &v);
    near(v.x, 0); near(v.y, 1); near(v.z, 0);

    // Aurora retains GX depth [-1, 0], not PICA's projection convention.
    Mtx44 projection;
    MTXPerspective(projection, 60, 4.0f / 3.0f, 1, 100);
    const auto depth = [&](float z) {
        return (projection[2][2] * z + projection[2][3]) / (-z);
    };
    near(depth(-1), -1); near(depth(-100), 0);
    near(projection[3][2], -1);

    // Exercise the portable PPC estimate's non-finite and subnormal branches.
    assert(std::isinf(frsqrte(0.0)) && !std::signbit(frsqrte(0.0)));
    assert(std::isinf(frsqrte(-0.0)) && std::signbit(frsqrte(-0.0)));
    assert(frsqrte(std::numeric_limits<double>::infinity()) == 0);
    assert(std::isnan(frsqrte(-1)));
    assert(std::isnan(frsqrte(-std::numeric_limits<double>::infinity())));
    assert(std::isnan(frsqrte(std::numeric_limits<double>::quiet_NaN())));
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double estimate = frsqrte(tiny);
    assert(std::isfinite(estimate));
    assert(std::fabs(estimate * std::sqrt(tiny) - 1.0) < .001);
    for (float x : {0.0001f, 0.1f, 1.0f, 2.0f, 25.0f, 10000.0f})
        near(ppc_rsqrte(x) * std::sqrt(x), 1);
    std::puts("PASS: C/C++ math linkage, transforms, aliasing, stack, quaternion, GX projection and PPC estimate edges");
}
