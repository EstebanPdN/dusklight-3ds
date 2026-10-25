#include "../runtime/os_sync.hpp"
#include <dolphin/os.h>
#include <JSystem/JAudio2/JASCriticalSection.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <latch>
#include <thread>

static OSMessage number(uintptr_t value) { return reinterpret_cast<OSMessage>(value); }

int main() {
    unsigned audioCounter=0;
    std::array<std::thread,8> audioWorkers;
    // Race the first-use initialization, nest scopes like track/pool calls,
    // and verify that leaving an exceptional scope releases the shared lock.
    std::latch audioStart(8);
    for(auto& worker:audioWorkers)worker=std::thread([&] {
        audioStart.count_down();audioStart.wait();
        for(unsigned i=0;i<5000;++i) {
            JASCriticalSection outer;
            JASCriticalSection inner;
            ++audioCounter;
        }
        try { JASCriticalSection scope; throw 1; }catch(int) {}
        JASCriticalSection after; ++audioCounter;
    });
    for(auto& worker:audioWorkers)worker.join();
    assert(audioCounter==40008);
    OSMutex mutex{};
    OSCond condition{};
    OSInitMutex(&mutex); OSInitCond(&condition);
    OSLockMutex(&mutex); OSLockMutex(&mutex);
    assert(OSTryLockMutex(&mutex));
    assert(mutex.count == 3 && mutex.thread == OSGetCurrentThread());
    std::thread contender([&] {
        assert(!OSTryLockMutex(&mutex));
        OSUnlockMutex(&mutex); // Non-owner must not release it.
        assert(!OSTryLockMutex(&mutex));
    });
    contender.join(); OSUnlockMutex(&mutex);
    bool ready = false;
    std::thread notifier([&] {
        OSLockMutex(&mutex);
        ready = true;
        OSSignalCond(&condition);
        OSUnlockMutex(&mutex);
    });
    while (!ready) OSWaitCond(&condition, &mutex);
    assert(mutex.count == 2 && mutex.thread == OSGetCurrentThread());
    OSUnlockMutex(&mutex); OSUnlockMutex(&mutex);
    notifier.join();

    OSMessageQueue queue{};
    std::array<OSMessage, 7> storage;
    OSInitMessageQueue(&queue, storage.data(), storage.size());
    OSMessage value = number(99);
    assert(!OSReceiveMessage(&queue, &value, OS_MESSAGE_NOBLOCK));
    assert(value == number(99));
    assert(OSSendMessage(&queue, number(2), OS_MESSAGE_NOBLOCK));
    assert(OSJamMessage(&queue, number(1), OS_MESSAGE_NOBLOCK));
    assert(OSReceiveMessage(&queue, &value, OS_MESSAGE_NOBLOCK) && value == number(1));
    assert(OSReceiveMessage(&queue, &value, OS_MESSAGE_NOBLOCK) && value == number(2));
    for (unsigned i = 0; i < storage.size(); ++i)
        assert(OSSendMessage(&queue, number(i), OS_MESSAGE_NOBLOCK));
    assert(!OSSendMessage(&queue, nullptr, OS_MESSAGE_NOBLOCK));
    assert(!OSJamMessage(&queue, nullptr, OS_MESSAGE_NOBLOCK));
    for (unsigned i = 0; i < storage.size(); ++i)
        assert(OSReceiveMessage(&queue, &value, OS_MESSAGE_NOBLOCK) && value == number(i));

    constexpr unsigned count = 10000;
    std::thread producer([&] {
        for (unsigned i = 1; i <= count; ++i)
            assert(OSSendMessage(&queue, number(i), OS_MESSAGE_BLOCK));
    });
    for (unsigned i = 1; i <= count; ++i)
        assert(OSReceiveMessage(&queue, &value, OS_MESSAGE_BLOCK) && value == number(i));
    producer.join();

    // Repeated wrap-around under four producers; each message must arrive once.
    std::array<std::thread, 4> producers;
    for (unsigned p = 0; p < producers.size(); ++p) producers[p] = std::thread([&, p] {
        for (unsigned i = 0; i < count; ++i)
            assert(OSSendMessage(&queue, number(1 + p * count + i), OS_MESSAGE_BLOCK));
    });
    std::array<bool, 4 * count> seen{};
    for (unsigned i = 0; i < seen.size(); ++i) {
        assert(OSReceiveMessage(&queue, &value, OS_MESSAGE_BLOCK));
        const auto index = reinterpret_cast<uintptr_t>(value) - 1;
        assert(index < seen.size() && !seen[index]); seen[index] = true;
    }
    for (auto& thread : producers) thread.join();

    std::latch receiverStarted(1);
    std::thread receiver([&] {
        receiverStarted.count_down();
        OSMessage unchanged = number(88);
        assert(!OSReceiveMessage(&queue, &unchanged, OS_MESSAGE_BLOCK));
        assert(unchanged == number(88));
    });
    receiverStarted.wait();
    dusk::native::closeMessageQueue(&queue);
    receiver.join();
    assert(!OSSendMessage(&queue, number(3), OS_MESSAGE_BLOCK));

    OSMessageQueue full{};
    OSMessage single;
    OSInitMessageQueue(&full, &single, 1);
    assert(OSSendMessage(&full, number(10), OS_MESSAGE_BLOCK));
    std::latch senderStarted(1);
    std::thread sender([&] {
        senderStarted.count_down();
        assert(!OSSendMessage(&full, number(11), OS_MESSAGE_BLOCK));
    });
    senderStarted.wait();
    dusk::native::closeMessageQueue(&full);
    sender.join();
    assert(OSReceiveMessage(&full, &value, OS_MESSAGE_BLOCK) && value == number(10));
    assert(!OSReceiveMessage(&full, &value, OS_MESSAGE_BLOCK));
    std::puts("PASS: JAS recursive scopes/first-use contention/exception release, recursive mutex, atomic condition wait, FIFO/jam/full/empty, 50000 messages and close/join");
}
