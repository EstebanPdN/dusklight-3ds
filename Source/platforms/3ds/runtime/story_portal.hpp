#pragma once

#include <cmath>
#include <cstdint>

namespace dusk::native::story {

enum class WarpInput { wait, openMap, showPortals, moveCursor, select, chooseYes, confirm };

struct PortalWarpSample {
    WarpInput input{};
    float cursorDx{}, cursorDy{};
    bool confirmed{}, departing{};
};

struct PortalWarpProof {
    bool confirmed{};
    unsigned departurePictures{};

    void record(const PortalWarpSample& sample) {
        confirmed |= sample.confirmed;
        if (confirmed && sample.departing) ++departurePictures;
    }

    bool witnessed() const { return confirmed && departurePictures >= 3; }
};

inline void steerMap(float dx, float dy, std::int16_t& x, std::int16_t& y) {
    const float distance = std::hypot(dx, dy);
    if (!std::isfinite(distance) || distance < 1.0f) return;
    const float scale = (distance > 36.0f ? 150.0f : 75.0f) / distance;
    x = std::int16_t(dx * scale);
    y = std::int16_t(-dy * scale);
}

}
