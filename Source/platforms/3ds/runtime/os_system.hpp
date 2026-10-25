#pragma once
#include <cstdint>
namespace dusk::native {
struct ResetSignal {int mode;std::uint32_t code;bool forceMenu;};
std::uint8_t platformGameLanguage();
bool platformDigitalVideo();
}
