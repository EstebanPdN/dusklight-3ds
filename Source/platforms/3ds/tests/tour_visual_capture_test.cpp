#include "tour_visual_capture.hpp"
#include <cassert>
#include <thread>

using namespace dusk::native;

int main() {
    TourVisualSchedule schedule;
    using Reason = TourVisualReason;
    const auto interval = TourVisualSchedule::interval;
    const auto entry = 123 * nativeTickFrequency;
    schedule.enter();
    assert(schedule.sample(entry - 1, false) == Reason::none);
    assert(schedule.sample(entry, true) == Reason::entry);
    assert(schedule.sample(entry + interval - 1, true) == Reason::none);
    assert(schedule.sample(entry + interval, true) == Reason::periodic);
    assert(schedule.sample(entry + interval, true) == Reason::none);
    // A 265-second cinematic with steady presentation still yields 26 samples.
    unsigned cinematicSamples = 1;
    for (unsigned seconds = 11; seconds <= 265; ++seconds)
        cinematicSamples += schedule.sample(entry + seconds * nativeTickFrequency, true) == Reason::periodic;
    assert(cinematicSamples == 26);
    // After a stall, emit once and start a fresh interval.
    assert(schedule.sample(entry + 300 * nativeTickFrequency, false) == Reason::periodic);
    assert(schedule.sample(entry + 300 * nativeTickFrequency + 1, true) == Reason::none);
    const auto nextZone = entry + 302 * nativeTickFrequency;
    schedule.enter();
    assert(schedule.sample(nextZone - 1, false) == Reason::none);
    assert(schedule.sample(nextZone, true) == Reason::entry);
    assert(schedule.sample(nextZone + interval - 1, true) == Reason::none);
    assert(schedule.sample(nextZone + interval, true) == Reason::periodic);

    struct Picture { unsigned zone, frame, reasons; };
    TourVisualQueue<Picture> queue;
    auto* first = queue.reserve();
    assert(first);
    *first = {20, 98, unsigned(TourVisualReason::exit)};
    queue.drain([](const auto&) { assert(false); }, [] { assert(false); }); // no partially copied picture
    queue.publish();
    auto* second = queue.reserve();
    assert(second && second != first);
    *second = {21, 110, unsigned(TourVisualReason::entry)};
    queue.publish();

    std::atomic<bool> writing{}, release{};
    std::atomic<unsigned> releases{};
    std::thread consumer([&] {
        unsigned seen = 0;
        queue.drain([&](const Picture& picture) {
            ++seen;
            if (seen == 1) {
                writing.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
                assert(picture.zone == 20 && picture.frame == 98 && picture.reasons == 8);
            } else {
                assert(picture.zone == 21 && picture.frame == 110 && picture.reasons == 1);
            }
        }, [&] { ++releases; releases.notify_one(); });
        assert(seen == 2);
    });
    while (!writing.load(std::memory_order_acquire)) std::this_thread::yield();
    assert(!queue.reserve()); // Bounded even while writer holds a previous zone.
    // A third scheduled picture waits at admission. The writer's first
    // picture is still intact until the producer has observed the full queue.
    auto* third = queue.reserveWhenAvailable([&] {
        release.store(true, std::memory_order_release);
        releases.wait(0);
    });
    assert(third == first);
    *third = {21, 111, unsigned(TourVisualReason::periodic)};
    queue.publish();
    consumer.join();
    assert(releases == 2);
    queue.drain([](const Picture& p) { assert(p.zone == 21 && p.frame == 111 && p.reasons == 2); },
                [&] { ++releases; });
    assert(releases == 3);

    CaptureWriterActivity writer;
    assert(!writer.sinceLastSubmission());
    writer.transition();
    assert(writer.sinceLastSubmission());
    assert(writer.sinceLastSubmission());
    writer.transition();
    assert(writer.sinceLastSubmission());
    assert(!writer.sinceLastSubmission());
    writer.transition();
    writer.transition();
    assert(writer.sinceLastSubmission()); // Whole write between submissions.
    assert(!writer.sinceLastSubmission());
}
