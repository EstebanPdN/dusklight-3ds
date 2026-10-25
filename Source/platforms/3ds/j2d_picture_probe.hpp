#pragma once
#include "runtime/resource_archive.hpp"
#include <cstdint>
namespace dusk::native {
bool initializeJ2dPictureProbe(ResourceSpan nintendo, ResourceSpan warning, ResourceSpan prompt);
void drawJ2dPictureProbe(std::uint64_t frame);
const char* j2dPictureProbeStatus();
void destroyJ2dPictureProbe(); // After GPU drain, before releasing borrowed archive.
}
