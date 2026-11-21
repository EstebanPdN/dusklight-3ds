#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

namespace dusk::native {
// Called by the sole scanner, including when the benchmark is inactive.
// A captured hold stays owned until a sampled release, across mode changes.
class BenchmarkCaptureInput {
public:
    bool sample(bool benchmark, std::uint32_t held, std::uint32_t a) {
        const bool pressed=(held&a)!=0;
        if(!pressed)consuming=false;
        const bool capture=benchmark&&pressed&&!previous;
        if(benchmark&&pressed)consuming=true;
        previous=pressed;
        return capture;
    }

    bool ownsHold() const {return consuming;}

    std::uint32_t physical(std::uint32_t keys, std::uint32_t a) const {
        return consuming ? keys & ~a : keys;
    }

private:
    bool previous{},consuming{};
};

// One render producer and one SD writer. The slot stays occupied throughout
// writing, including any filesystem failure. Neither side waits for the other.
template <typename Snapshot>
class BenchmarkCaptureSlot {
public:
    Snapshot* reserve() {
        State expected = State::empty;
        return state.compare_exchange_strong(expected, State::filling, std::memory_order_acquire)
            ? &snapshot : nullptr;
    }
    void publish() { state.store(State::ready, std::memory_order_release); }
    const Snapshot* take() {
        State expected = State::ready;
        return state.compare_exchange_strong(expected, State::writing, std::memory_order_acquire)
            ? &snapshot : nullptr;
    }
    // After writing, before publication on capture failure, or after a failed
    // writer startup (when no consumer exists).
    void release() { state.store(State::empty, std::memory_order_release); }

private:
    enum class State : unsigned { empty, filling, ready, writing };
    std::atomic<State> state{State::empty};
    Snapshot snapshot{};
};

// Keep the normal slot resident. A second picture only needs storage while
// the SD writer still owns the first one. The writer frees that storage
// before admitting another overflow capture.
template<typename Snapshot>
class BenchmarkPictureQueue {
public:
    Snapshot* reserve() {
        if (auto* item = primary.reserve()) return item;
        if (extraOccupied.test_and_set(std::memory_order_acquire)) return nullptr;
        if (void* memory = std::malloc(sizeof(Snapshot))) {
            extraFilling = ::new (memory) Snapshot;
            return extraFilling;
        }
        extraOccupied.clear(std::memory_order_release);
        return nullptr;
    }

    void publish() {
        if (extraFilling) {
            extraReady.store(extraFilling, std::memory_order_release);
            extraFilling = nullptr;
        } else {
            primary.publish();
        }
    }

    template<typename Writer>
    void drain(Writer write) {
        if (const auto* item = primary.take()) {
            write(*item);
            primary.release();
        }
        if (auto* item = extraReady.exchange(nullptr, std::memory_order_acquire)) {
            write(*item);
            std::destroy_at(item);
            std::free(item);
            extraOccupied.clear(std::memory_order_release);
        }
    }

private:
    BenchmarkCaptureSlot<Snapshot> primary;
    Snapshot* extraFilling{}; // Only the render thread accesses this pointer.
    std::atomic<Snapshot*> extraReady{};
    std::atomic_flag extraOccupied = ATOMIC_FLAG_INIT;
};
enum class BenchmarkCaptureSource : unsigned { manual, automatic };

template<typename Record>
struct BenchmarkPictureTriggers {
    // One automatic request and the whole sampler queue can refer to a
    // single slow picture. They share attachments and keep distinct IDs.
    std::array<Record,33> records;
    unsigned count{};

    bool append(const Record& trigger) {
        if(count==records.size()||(count&&records[0].frame!=trigger.frame))return false;
        records[count++]=trigger;
        return true;
    }
};

// Owned trigger records for pictures that arrive while the attachment slot
// is busy. Only the producer updates the tail and only the writer reads it.
template<typename Record, std::size_t Capacity>
class BenchmarkTriggerQueue {
public:
    bool push(const Record& record) {
        const auto tail=written.load(std::memory_order_relaxed);
        if(tail-read.load(std::memory_order_acquire)==Capacity)return false;
        records[tail%Capacity]=record;
        written.store(tail+1,std::memory_order_release);
        return true;
    }

    bool pop(Record& record) {
        const auto head=read.load(std::memory_order_relaxed);
        if(head==written.load(std::memory_order_acquire))return false;
        record=records[head%Capacity];
        read.store(head+1,std::memory_order_release);
        return true;
    }

private:
    static_assert(Capacity && (Capacity & (Capacity-1))==0);
    std::array<Record,Capacity> records{};
    std::atomic<unsigned> written{},read{};
};

}
