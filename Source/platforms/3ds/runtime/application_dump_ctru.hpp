#pragma once
#include <cstdint>
#include <cstdio>
namespace dusk::native {
// Call once per scanned input frame, before original PADRead/game execution.
void updateApplicationDumpInput(std::uint32_t held);
bool applicationDumpInputSuppressed();
bool applicationDumpActive();
bool applicationDumpRequested();
// Same capture as the held chord, requested by the developer console.
void requestApplicationDump();
// Call only after C3D_FrameEnd. Returns true if the game clock needs reset.
bool serviceApplicationDump(std::uint64_t frame,const char* scene,int room);
// Extra lines for the next dump's info.txt (frame rate, game speed, player).
void setApplicationDumpNote(const char* text);
// Writes the game state (camera, player, heaps, actors...) into info.txt.
// Called on the main thread after a completed frame.
void setApplicationDumpReport(void (*report)(std::FILE*));
}
