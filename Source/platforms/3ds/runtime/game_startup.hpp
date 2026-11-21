#pragma once
#include "boot_stage.hpp"
namespace dusk::native {
int runOriginalGame();
const BootStage* requestedBootStage();
// After a direct stage entry initialized an empty save: the equipment of the automatic test.
void applyBootStageLoadout();
// At the start of the PLAY_SCENE a developer travel requested: the empty save
// and test equipment of that travel (the previous scene is gone by then).
void applyTravelLoadout();
}
