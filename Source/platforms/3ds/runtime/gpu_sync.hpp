#pragma once
#include <cstdint>
struct OSThread;

namespace dusk::native {
enum class GpuCompletion { complete, notReady, wrongThread, timedOut, exhausted };
// Flush queued PICA commands and wait on the actual bound Citro3D GX queue.
// The render owner retains command, vertex and texture storage through this wait.
GpuCompletion platformCompleteGpu(std::uint64_t timeoutNanoseconds);
// Native frame boundaries serialize against the original GSP queue callback.
// Queue waits are bounded; FrameSync retains the SDK's VBlank synchronization.
bool platformBeginGpuFrame(std::uint8_t flags);
bool platformEndGpuFrame(std::uint8_t flags);
// Capture the public GPU command span immediately after C3D_FrameBegin.
bool platformBeginGpuCommands();
// Keep a conservative complete-draw reserve. Reclaim only after real completion.
bool platformPrepareGpuDraw();
// Reserve consecutive public GX entries before a compound SDK operation. This
// drains/reclaims completed queue entries only; CPU command/geometry storage is
// retained. Main render owner only; no submission may intervene before use.
GpuCompletion platformReserveGpuEntries(unsigned count,std::uint64_t timeoutNanoseconds);
// Nonblocking observation of the real queue and pending CPU command buffer.
GpuCompletion platformQueryGpu();
// Sticky failure state: fatal containment must bypass APT/UI/GPU teardown.
// A deliberate nonblocking FrameBegin miss does not set it.
bool platformGpuFailurePending();
// Actual Dolphin TLS identity of the owner that bound the Citro3D queue.
// Null means no queue has been bound. Ownership transfer is not supported.
OSThread* platformGpuOwner();
// Render-owner cache invalidation after a successful queue completion. Marks
// actual converted images for reconversion on their next validated binding.
bool platformInvalidateGpuTextures();
}
