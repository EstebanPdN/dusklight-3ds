#pragma once
#include <cstdint>
namespace dusk::native {
// Call once per scanned input frame, before original PADRead/game execution.
void updateApplicationDumpInput(std::uint32_t held);
bool applicationDumpInputSuppressed();
bool applicationDumpActive();
bool applicationDumpRequested();
// Call only after C3D_FrameEnd. Returns true if the game clock needs reset.
bool serviceApplicationDump(std::uint64_t frame,const char* scene,int room);
}
