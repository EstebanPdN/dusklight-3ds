#pragma once
#include "boot_stage.hpp"
namespace dusk::native {
int runOriginalGame();
const BootStage* requestedBootStage();
// After a direct stage entry initialized an empty save: the equipment of the automatic test.
void applyBootStageLoadout();
}
