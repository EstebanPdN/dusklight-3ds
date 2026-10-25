#pragma once
#include <cstdint>
struct OSThread;

namespace dusk::native {
// Cooperative requests are observed at native OS waits/locks/yields. A foreign
// suspend waits for acknowledgement; cancellation joins before freeing storage.
// Same suspended-start lifetime/ownership as OSCreateThread; explicit native core.
// -2 inherits the title, 0 is main app core, 2 is the additional New3DS app core.
bool createThreadOnCore(OSThread*,void* (*)(void*),void*,void*,std::uint32_t,int,std::uint16_t,int core);
void threadCheckpoint();
void threadLock(std::int32_t* lock);
void threadConditionWait(std::int32_t* condition, std::int32_t* lock);
void releaseTerminatedThread(OSThread* thread);
struct ThreadStats { unsigned workers; std::uint32_t stackBytes; unsigned waitingQueues; };
ThreadStats threadStats();
enum class WorkerShutdownStatus { complete, wrongOwner, timedOut, retainedOwnership };
struct WorkerShutdownResult {
    WorkerShutdownStatus status;
    ThreadStats remaining;
    unsigned joined; // Collected by this call; existing joiners may collect others.
    unsigned joining; // Outstanding claims, included in remaining.workers.
};
const char* workerShutdownStatusName(WorkerShutdownStatus status);
// Main libctru thread only. Permanently closes creation, cancels every slot
// before waiting, and kernel-joins/frees under ONE total budget (at most 2 s).
// Idempotent; a timed-out call may be retried, without reopening admission.
// Only complete permits shared storage/service teardown or runtime return.
// Failure retains every unjoined handle/stack/record and never calls fatal/exit.
// Keep records, queues, mutexes, heaps and services alive through cleanup. An
// uncooperative I/O callback can time out: parent must contain that failure via
// kernel process termination without libctru heap teardown, not std::exit.
// During aggregate stop, mutex acquisition/destructor cancellation may finish
// normally before the next explicit wait/yield observes cancellation. Do not
// hold a worker-needed mutex across this call; contention can time out safely.
WorkerShutdownResult shutdownWorkers(std::uint64_t timeoutNanoseconds = 2000000000);
}
