#pragma once
#include <cstdint>
namespace dusk::native {
#if defined(DUSK_GAME_DIAGNOSTICS)
constexpr bool betaScreenEnabled() { return false; }
inline void startBetaScreen() {}
inline void updateBetaScreen(float, int) {}
inline void touchBetaScreen(unsigned, unsigned, bool) {}
inline void betaDumpProgress(std::uint64_t, std::uint64_t) {}
inline void betaDumpFinished(bool) {}
#else
bool betaScreenEnabled();
void startBetaScreen();
void updateBetaScreen(float frameMs, int speed);
void touchBetaScreen(unsigned x, unsigned y, bool allowDump);
void betaDumpProgress(std::uint64_t done, std::uint64_t total);
void betaDumpFinished(bool success);
#endif
}
