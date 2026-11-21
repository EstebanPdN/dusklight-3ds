// Host test of runtime/eh_globals.cpp and of the exit barrier's unwind shape.
//
// The console's C++ runtime keeps two chains of exception objects in the
// block __cxa_get_globals returns: the caught ones (catch blocks entered; the
// head is what `throw;` rethrows) and, on the ARM EH ABI, the propagating
// ones (cleanups running; the head is what __cxa_end_cleanup resumes). The
// worker barrier cancels every game worker at once, and each one's ThreadExit
// passes the same frames: a cleanup in checkpoint (its state-lock guard), the
// catch(...)/throw; of threadConditionWait, the catch(...)/throw; of
// OSWaitCond, then the catch in worker(). This runs those chain operations
// for six workers in lock step, in the worst order the console can produce
// (each step taken by every worker before the next step), once with the
// production per-thread block and once with one block for all, which is what
// libstdc++ uses on the console without pthread keys. The host's own C++
// runtime is not involved: it is per-thread already, so the console failure
// cannot be reproduced through real exceptions here.
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern "C" void* __wrap___cxa_get_globals() noexcept;
extern "C" void* __wrap___cxa_get_globals_fast() noexcept;

namespace {
struct Exception {
    Exception* nextCaught{};
    Exception* nextPropagating{};
    int handlers{};
};
struct Globals {
    Exception* caught;
    unsigned uncaught;
    Exception* propagating;
};
static_assert(sizeof(Globals) == 3 * sizeof(void*));

// The chain operations, as the ARM EH ABI C++ runtime performs them.
void raise(Globals& g) { ++g.uncaught; }
void beginCatch(Globals& g, Exception& e) {
    e.handlers = e.handlers < 0 ? -e.handlers + 1 : e.handlers + 1;
    --g.uncaught;
    if (g.caught != &e) { e.nextCaught = g.caught; g.caught = &e; }
}
Exception* rethrow(Globals& g) {
    auto* e = g.caught;
    ++g.uncaught;
    if (e) e->handlers = -e->handlers;
    return e;
}
void endCatch(Globals& g) {
    auto* e = g.caught;
    if (!e) return;
    if (e->handlers < 0) { if (++e->handlers == 0) g.caught = e->nextCaught; }
    else if (--e->handlers == 0) g.caught = e->nextCaught;
}
void beginCleanup(Globals& g, Exception& e) { e.nextPropagating = g.propagating; g.propagating = &e; }
Exception* endCleanup(Globals& g) {
    auto* e = g.propagating;
    if (e) g.propagating = e->nextPropagating;
    return e;
}

constexpr unsigned workers = 6;
enum class Source { perThread, shared };

// Returns how many chain operations handed a worker another worker's exception.
unsigned barrierUnwind(Source source) {
    Globals shared{};
    std::atomic<unsigned> turn{}, wrong{};
    std::barrier steps(workers);
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < workers; ++index) threads.emplace_back([&, index] {
        auto& g = source == Source::shared ? shared : *static_cast<Globals*>(__wrap___cxa_get_globals());
        Exception own;
        // One step: every worker in index order, then all together onward.
        const auto step = [&](auto operation) {
            while (turn.load(std::memory_order_acquire) % workers != index) std::this_thread::yield();
            operation();
            turn.fetch_add(1, std::memory_order_acq_rel);
            steps.arrive_and_wait();
        };
        const auto expect = [&](Exception* got) { if (got != &own) wrong.fetch_add(1, std::memory_order_relaxed); };
        step([&] { raise(g); });                                     // checkpoint: throw ThreadExit
        step([&] { beginCleanup(g, own); });                         // ~Guard in checkpoint
        step([&] { expect(endCleanup(g)); });
        for (unsigned frame = 0; frame < 2; ++frame) {               // threadConditionWait, OSWaitCond
            step([&] { beginCatch(g, own); });                       // catch (...) {
            step([&] { expect(rethrow(g)); });                       //     throw; }
            step([&] { beginCleanup(g, own); endCatch(g); });         // leaving the catch block
            step([&] { expect(endCleanup(g)); });
        }
        step([&] { beginCatch(g, own); });                           // worker(): catch (const ThreadExit&)
        step([&] { endCatch(g); });
        if (source == Source::perThread) assert(!g.caught && !g.uncaught && !g.propagating);
    });
    for (auto& thread : threads) thread.join();
    return wrong.load();
}
}

int main() {
    // One block per thread, the same through both entry points, zeroed at
    // thread start and stable for the thread's life.
    std::set<void*> blocks;
    std::vector<std::thread> threads;
    std::atomic<unsigned> checked{};
    std::mutex lock;
    std::barrier together(workers + 1);
    const auto probe = [&] {
        auto* block = __wrap___cxa_get_globals();
        assert(block && block == __wrap___cxa_get_globals_fast() && block == __wrap___cxa_get_globals());
        const auto* g = static_cast<const Globals*>(block);
        assert(!g->caught && !g->uncaught && !g->propagating);
        together.arrive_and_wait(); // all alive at once: no reuse of a finished thread's block
        { std::lock_guard guard(lock); assert(blocks.insert(block).second); }
        checked.fetch_add(1);
    };
    for (unsigned index = 0; index < workers; ++index) threads.emplace_back(probe);
    probe();
    for (auto& thread : threads) thread.join();
    assert(checked == workers + 1 && blocks.size() == workers + 1);

    const unsigned perThread = barrierUnwind(Source::perThread);
    const unsigned shared = barrierUnwind(Source::shared);
    std::printf("barrier unwind of %u workers: per-thread block %u foreign exceptions, one shared block %u\n",
                workers, perThread, shared);
    assert(perThread == 0); // the fix: every worker rethrows and resumes its own exception
    assert(shared > 0);     // negative control: the console's shared block hands them around
    std::puts("PASS: exception state per thread (runtime/eh_globals.cpp); the shared block of the Alpha 13/15 console exits mixes the workers' exceptions");
    return 0;
}
