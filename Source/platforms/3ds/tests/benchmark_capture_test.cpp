#include "benchmark_capture.hpp"
#include "benchmark_input.hpp"
#include "clock.hpp"
#include "frame_telemetry.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>

using namespace dusk::native;

void inputTest() {
    constexpr std::uint32_t a=1,b=2,l=4,r=8;
    BenchmarkCaptureInput input;
    assert(!input.sample(false,a,a));
    assert(input.physical(a,a)==a);
    assert(!input.sample(true,a,a)); // A was already held on entry.
    assert(input.ownsHold()&&input.physical(a,a)==0);
    assert(!input.sample(true,0,a));

    BenchmarkTriggerQueue<BenchmarkInputSite,4> edges;
    BenchmarkInputSite site{137,1000,"F_SP121",10,3};
    // Neither tap is held on either render tick. The sole scanner sees both.
    for(const auto raw:{a|b|l|r,a|b,b,a|b,b}){
        ++site.requestTick;
        if(input.sample(true,raw,a))assert(edges.push(site));
        assert(input.physical(raw,a)==(raw&~a));
        assert((input.physical(raw,a)|a)==(raw|a)); // Scripted A overlaps.
    }
    BenchmarkInputSite edge;
    assert(edges.pop(edge)&&edge.previousFrame==137&&edge.requestTick==1001);
    assert(edges.pop(edge)&&edge.previousFrame==137&&edge.requestTick==1004);
    assert(!benchmarkInputReady(edge,1003)); // A prior submission cannot use this edge.
    assert(benchmarkInputReady(edge,1004));
    assert(!edges.pop(edge));

    assert(input.sample(true,a,a));
    assert(input.ownsHold()&&input.physical(a,a)==0);
    assert(!input.sample(false,a,a)); // Continue consuming across tour exit.
    assert(input.ownsHold()&&input.physical(a,a)==0);
    assert(!input.sample(false,0,a));
    assert(!input.ownsHold());
    assert(!input.sample(false,a,a)); // A new post-release press belongs to play.
    assert(input.physical(a,a)==a);
    assert(!input.sample(false,a,a));
    assert(!input.sample(true,a,a));
    assert(!input.sample(true,0,a));
    assert(input.sample(true,a,a));
}

void queueTest() {
    BenchmarkCaptureSlot<std::array<unsigned,2>> slot;
    assert(!slot.take());
    auto* cancelled=slot.reserve();
    assert(cancelled&&!slot.reserve()&&!slot.take());
    slot.release(); // Failed to snapshot.
    assert(slot.reserve());
    slot.publish();
    slot.release(); // Writer startup failed; no consumer.
    assert(!slot.take());

    constexpr unsigned attempts=10000;
    std::atomic<unsigned> writerOwns{},producerChecked{};
    std::thread writer([&]{
        for(unsigned n=1;n<=attempts;++n){
            const std::array<unsigned,2>* picture{};
            while(!(picture=slot.take()))std::this_thread::yield();
            assert((*picture)[0]==n&&(*picture)[1]==n*2);
            writerOwns.store(n,std::memory_order_release);
            while(producerChecked.load(std::memory_order_acquire)!=n)std::this_thread::yield();
            slot.release(); // Same release path after successful and failed writes.
        }
    });
    for(unsigned n=1;n<=attempts;++n){
        std::array<unsigned,2>* picture{};
        while(!(picture=slot.reserve()))std::this_thread::yield();
        *picture={n,n*2};
        assert(!slot.take());
        slot.publish();
        while(writerOwns.load(std::memory_order_acquire)!=n)std::this_thread::yield();
        assert(!slot.reserve());
        producerChecked.store(n,std::memory_order_release);
    }
    writer.join();
    assert(!slot.take());
    assert(slot.reserve());
    slot.release();
}

void triggerQueueTest() {
    struct Trigger {
        unsigned frame;
        BenchmarkCaptureSource source;
        std::array<char,9> stage;
        int room;
    };
    const Trigger manual{42,BenchmarkCaptureSource::manual,{"F_SP121"},10};
    const Trigger automatic{42,BenchmarkCaptureSource::automatic,{"F_SP121"},10};
    const Trigger next{44,BenchmarkCaptureSource::manual,{"F_SP122"},8};
    BenchmarkPictureTriggers<Trigger> pair;
    assert(pair.append(manual));
    assert(!pair.append(next)); // Another frame must go to the fallback queue.
    assert(pair.append(automatic));
    for(unsigned n=1;n<32;++n)assert(pair.append(manual)); // Distinct taps on one slow frame.
    assert(!pair.append(manual));
    assert(pair.records[0].source==BenchmarkCaptureSource::manual);
    assert(pair.records[1].source==BenchmarkCaptureSource::automatic);
    assert(pair.records[0].frame==pair.records[1].frame);

    BenchmarkTriggerQueue<Trigger,4> queue;
    Trigger read{};
    assert(!queue.pop(read));
    assert(queue.push(manual)&&queue.push(automatic)&&queue.push(next));
    auto mutableTrigger=next;
    mutableTrigger.frame=46;
    assert(queue.push(mutableTrigger));
    mutableTrigger.frame=48;
    mutableTrigger.stage={"F_SP116"};
    assert(!queue.push(mutableTrigger));
    assert(queue.pop(read)&&read.frame==42&&read.source==BenchmarkCaptureSource::manual&&read.room==10);
    assert(queue.push(mutableTrigger)); // Wrap and reuse the released slot.
    assert(queue.pop(read)&&read.frame==42&&read.source==BenchmarkCaptureSource::automatic);
    assert(queue.pop(read)&&read.frame==44&&read.stage==next.stage&&read.room==8);
    assert(queue.pop(read)&&read.frame==46&&read.stage==next.stage);
    assert(queue.pop(read)&&read.frame==48&&read.stage==mutableTrigger.stage);
    assert(!queue.pop(read));

    constexpr unsigned count=10000;
    std::thread writer([&]{
        for(unsigned frame=1;frame<=count;++frame){
            Trigger trigger{};
            while(!queue.pop(trigger))std::this_thread::yield();
            assert(trigger.frame==frame&&trigger.stage==manual.stage&&trigger.room==manual.room);
        }
    });
    for(unsigned frame=1;frame<=count;++frame){
        mutableTrigger=manual;
        mutableTrigger.frame=frame;
        while(!queue.push(mutableTrigger))std::this_thread::yield();
    }
    writer.join();
    assert(!queue.pop(read));
}

void overlappingPicturesTest() {
    BenchmarkPictureQueue<unsigned> queue;
    auto* first=queue.reserve();
    assert(first);
    *first=73;
    queue.publish();
    std::atomic<bool> writing{},secondReady{};
    std::array<unsigned,2> seen{};
    unsigned count=0;
    std::thread writer([&]{
        queue.drain([&](const unsigned& value){
            assert(count<seen.size());
            seen[count++]=value;
            writing.store(true,std::memory_order_release);
            while(!secondReady.load(std::memory_order_acquire))std::this_thread::yield();
        });
    });
    while(!writing.load(std::memory_order_acquire))std::this_thread::yield();
    auto* second=queue.reserve();
    assert(second&&second!=first);
    *second=74;
    queue.publish();
    assert(!queue.reserve());
    secondReady.store(true,std::memory_order_release);
    writer.join();
    assert(count==2&&seen[0]==73&&seen[1]==74);
    assert(queue.reserve()==first);
    queue.publish();
    assert(queue.reserve());
    queue.publish();
    queue.drain([](const unsigned&){});
}

void telemetryTest() {
    std::array<FrameTelemetryRow,4> owned{};
    assert(copyRecentFrameTelemetry(owned)==0);
    initializeFrameTelemetry();
    setFrameTelemetryTag(17,2);
    FrameTelemetryInput sample;
    sample.stage="F_SP121";
    sample.room=10;
    sample.loopTicks=nativeTickFrequency/30;
    sample.ticks=2;
    for(unsigned n=1;n<=4100;++n){sample.frame=n;recordFrameTelemetry(sample);}
    const auto count=copyRecentFrameTelemetry(owned);
    assert(count==owned.size());
    assert(owned.front().frame==4097&&owned.back().frame==4100);
    assert(owned.back().zone==17&&owned.back().phase==2);
    std::array<char,2048> regular{};
    const auto bytes=formatRecentFrameTelemetry(regular.data(),regular.size(),count);
    sample.frame=4101;recordFrameTelemetry(sample);
    assert(owned.back().frame==4100); // Writer sees its own copy after the ring advances.
    auto* file=std::tmpfile();
    assert(file&&writeFrameTelemetryRows(file,owned));
    std::rewind(file);
    std::array<char,2048> manual{};
    assert(std::fread(manual.data(),1,manual.size(),file)==bytes);
    assert(std::fclose(file)==0);
    assert(std::string(regular.data(),bytes)==std::string(manual.data(),bytes));
}

int main() {
    inputTest();
    queueTest();
    overlappingPicturesTest();
    triggerQueueTest();
    telemetryTest();
}
