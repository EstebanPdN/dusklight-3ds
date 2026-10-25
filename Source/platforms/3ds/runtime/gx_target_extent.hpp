#pragma once
#include <cstdint>
namespace dusk::native::gx {
// Logical GX pixels are independent from the rotated PICA allocation.
// Defaults retain the diagnostic's native 400x240 coordinate system.
struct TargetExtent {
    unsigned logicalWidth=400, logicalHeight=240;
    unsigned pixelWidth=400, pixelHeight=240;
    bool valid() const {
        return logicalWidth && logicalHeight && logicalWidth<=1024 && logicalHeight<=1024 &&
               pixelWidth && pixelHeight && pixelWidth<=1024 && pixelHeight<=1024;
    }
};
inline thread_local TargetExtent currentTargetExtent{};
}
