#pragma once
#include "boot_stage.hpp"
namespace dusk::native {
int runOriginalGame();
const BootStage* requestedBootStage();
}
