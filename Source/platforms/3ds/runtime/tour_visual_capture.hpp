#pragma once

#include "clock.hpp"
#include <array>
#include <atomic>

namespace dusk::native {
enum class TourVisualReason : unsigned { none = 0, entry = 1, periodic = 2, evidence = 4, exit = 8 };

// Real elapsed time still advances in cutscenes and under slow rendering.
// A late frame consumes one deadline. There is no catch-up burst.
class TourVisualSchedule {
public:
    static constexpr auto interval = 10 * nativeTickFrequency;

    void enter() { entryPending = true; }
    TourVisualReason sample(std::uint64_t tick, bool visible) {
        if (entryPending) {
            if (!visible) return TourVisualReason::none;
            entryPending = false;
            last = tick;
            return TourVisualReason::entry;
        }
        if (tick - last < interval) return TourVisualReason::none;
        last = tick;
        return TourVisualReason::periodic;
    }

private:
    std::uint64_t last{};
    bool entryPending{true};
};

// Single render producer and single SD consumer. The consumer's current
// write occupies a slot until its files close. Only a full queue needs to wait.
template<class Picture>
class TourVisualQueue {
public:
    static constexpr unsigned capacity = 2;

    Picture* reserve() {
        const auto tail = written.load(std::memory_order_relaxed);
        if (tail - read.load(std::memory_order_acquire) == capacity) return nullptr;
        return &pictures[tail % capacity];
    }
    template<class Wait>
    Picture* reserveWhenAvailable(Wait wait) {
        while (true) {
            if (auto* picture = reserve()) return picture;
            wait();
        }
    }
    void publish() { written.fetch_add(1, std::memory_order_release); }
    template<class Write, class Released>
    void drain(Write write, Released released) {
        for (unsigned i = 0; i < capacity; ++i) {
            const auto head = read.load(std::memory_order_relaxed);
            if (head == written.load(std::memory_order_acquire)) return;
            write(pictures[head % capacity]);
            read.store(head + 1, std::memory_order_release);
            released();
        }
    }

private:
    std::array<Picture, capacity> pictures{};
    std::atomic<unsigned> written{}, read{};
};

// Increment at both edges so a write entirely between two submissions is
// visible as well as a write spanning either or both submissions.
class CaptureWriterActivity {
public:
    void transition() { epoch.fetch_add(1, std::memory_order_acq_rel); }
    bool sinceLastSubmission() {
        const auto current = epoch.load(std::memory_order_acquire);
        const bool overlap = current != previous || (current & 1);
        previous = current;
        return overlap;
    }

private:
    std::atomic<unsigned> epoch{};
    unsigned previous{}; // render owner only
};
}
