#pragma once
struct OSThread;
struct OSThreadQueue;
namespace dusk::native {
// Original JKR owns its OSThread storage; release the actual finished native
// handle/stack/TLS before freeing that record. Live workers must be cancelled
// and joined first. The implementation belongs to the native platform.
void releaseTerminatedThread(OSThread* thread);
// Atomically test the original pause flag while linking the real wait queue.
// A clear flag returns immediately; a set flag waits for wakeup/cancellation.
void sleepThreadWhile(OSThreadQueue* queue, const bool* paused);
}
