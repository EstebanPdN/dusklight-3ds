#pragma once
#include <cstdint>
#include <cstdio>
namespace dusk::native {
struct FrameTelemetryInput;
enum class BenchmarkCaptureSource : unsigned;
enum class TourVisualReason : unsigned;
struct DevZoneCaptureContext;
struct BenchmarkInputSite;
void captureBenchmarkPicture(const FrameTelemetryInput&, float fps, int speed,
    const DevZoneCaptureContext&, const char* tour, const char* loadout, BenchmarkCaptureSource,
    const BenchmarkInputSite* input = nullptr, bool completed = true);
// Finalize the owned picture at the next ordinary GPU boundary.
void finishBenchmarkPicture(bool gpuAlreadyIdle = false);
void writeBenchmarkCapturePending(); // SD writer only
// Called before FrameEnd to identify the SDK transfer destination for this picture.
void noteTourVisualTarget();
void captureTourVisualPicture(const FrameTelemetryInput&, const DevZoneCaptureContext&, TourVisualReason);
void finishTourVisualPicture(bool gpuAlreadyIdle = false);
bool captureWriterOverlappedSubmission();

// Call once per scanned input frame, before original PADRead/game execution.
void updateApplicationDumpInput(std::uint32_t held);
bool applicationDumpInputSuppressed();
bool applicationDumpActive();
bool applicationDumpRequested();
// Same capture as the held chord, requested by the developer console.
void requestApplicationDump(bool waitForInput = true);
// Call only after C3D_FrameEnd. Returns true if the game clock needs reset.
bool serviceApplicationDump(std::uint64_t frame,const char* scene,int room);
// Extra lines for the next dump's info.txt (frame rate, game speed, player).
void setApplicationDumpNote(const char* text);
// Writes the game state (camera, player, heaps, actors...) into info.txt.
// Called on the main thread after a completed frame.
void setApplicationDumpReport(void (*report)(std::FILE*),void (*scene)(std::FILE*));
}
