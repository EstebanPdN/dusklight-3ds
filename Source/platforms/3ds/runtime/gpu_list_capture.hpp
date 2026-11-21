#pragma once
#include <cstdint>

namespace dusk::native {
// Request the next ordinary picture. Its commands are copied at submission
// and matched to completed GSP jobs after the following frame admission.
void requestGpuListCapture(const char* directory,const char* phase,const char* stage,int room);
void recordSubmittedGpuList(const std::uint32_t* list,std::uint32_t bytes);
bool gpuListCaptureBoundary(std::uint64_t frame);
void closeGpuListCapture(std::uint64_t frame,std::uint64_t submitTick,const char* stage,int room);
// A validated capture stays exclusively owned until its file and receipt close.
bool gpuListCaptureWritePending();
void writePendingGpuListCapture(); // Sole SD consumer, or fallback when thread creation failed.
void waitForGpuListCaptureWrite(); // Room receipts and the next scheduled capture.
void finishGpuListCaptures(); // Render owner after final GPU completion.
}
