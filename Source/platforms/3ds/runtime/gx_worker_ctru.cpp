#include "gx_worker_runtime.hpp"
#include "gx_worker_sync.hpp"
#include "native_packets.hpp"
#include "gx_texture_commands.hpp"
#include "gpu_sync.hpp"
#include "gpu_canary.hpp"
#include "screen_effects.hpp"
#include "bootstrap.hpp"
#include "os_thread.hpp"
#include <3ds.h>
#include <dolphin/os.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

extern "C" void OSReport(const char*,...);
extern "C" void _fini();
// gx_command_draw.cpp: conditional sections (gx_worker_sync.hpp).
namespace dusk::native {void nativeSectionStatistics(std::uint64_t& queued,std::uint64_t& skipped,std::uint64_t& shapesSkipped,std::uint64_t& runsDropped);
void nativeRunStatistics(std::uint64_t& queued,std::uint64_t& refusedForArrays);
void nativeWorkerStatistics(std::uint64_t& queued,std::uint64_t& lists,std::uint64_t& failures);}
extern "C" std::uint8_t __start__[];
namespace dusk::native {
namespace {
// The worker's priority between syncs: 12 (0x27), below the audio mixer
// (6, 0x20, audio_ndsp.cpp), which keeps its blocks on time; the game thread
// raises it to 5 (0x1e) only while it waits for the worker to stop between
// two shapes (workerUrgent). Measured in the emulator (Alpha 10, three
// zones): above the mixer the worker drew most shapes even in Death Mountain
// (63.3 ms a picture) but the mixer missed blocks there; at the mixer's
// priority (68.1 ms) or below it (68.6 ms) none were missed, and in Death
// Mountain the worker rests (gxWorkerPictureEnd). Moving the mixer off core
// 2 would give the worker the whole core: the core probe at the end of the
// automatic test measures what core 1 offers on a console.
constexpr int workerPriority=12;
OSThread workerRecord;
OSThread* gameRecord{};
bool threadRunning{};
Handle wakeEvent{},progressEvent{};
std::uint32_t gameFpscr{};
// The game thread's core (startGxWorker, before the worker thread is created).
std::int32_t gameCore{-1};
// Time the game thread spent waiting for the worker to stop between two
// shapes (sync) and the worker spent asleep, in system ticks.
std::atomic<std::uint64_t> gameWaitTicks{},workerIdleTicks{};
std::uint64_t reportedWait{},reportedIdle{},reportTick{};
gxworker::Counters reported{};
unsigned picturesSinceReport{};

__attribute__((target("arm"))) std::uint32_t readFpscr(){std::uint32_t value;__asm__ volatile("vmrs %0, fpscr":"=r"(value));return value;}
__attribute__((target("arm"))) void writeFpscr(std::uint32_t value){__asm__ volatile("vmsr fpscr, %0"::"r"(value));}

// A fatal error met while the worker draws (fatalRelayHook): the worker stops
// there for good and the game thread, which finds the text the next time it
// waits for the worker to stop between two shapes, ends the program as it
// does for its own fatal errors (message, restart of an automatic test).
std::atomic<const char*> workerFatal{};
std::atomic<int> workerHang{}; // 1: GPU stop, restart advice; 2: GPU stop, send the folder
char workerFatalText[360];
bool relayWorkerFatal(const char* text){
    if(!threadRunning||OSGetCurrentThread()!=&workerRecord)return false;
    std::snprintf(workerFatalText,sizeof(workerFatalText),"%s [second-core worker]",text?text:"");
    workerFatal.store(workerFatalText,std::memory_order_release);
    svcSignalEvent(progressEvent);
    return true;
}
bool relayWorkerGpuHang(const char* text,bool restart){
    if(!threadRunning||OSGetCurrentThread()!=&workerRecord)return false;
    std::snprintf(workerFatalText,sizeof(workerFatalText),"%s",text?text:"");
    workerHang.store(restart?1:2,std::memory_order_release);
    svcSignalEvent(progressEvent);
    return true;
}
void wakeWorker(){svcSignalEvent(wakeEvent);}
void waitForWorker(unsigned microseconds){
    const auto start=svcGetSystemTick();
    svcWaitSynchronization(progressEvent,s64(microseconds)*1000);
    gameWaitTicks.fetch_add(svcGetSystemTick()-start,std::memory_order_relaxed);
    if(const int hang=workerHang.load(std::memory_order_acquire))restartAfterGpuHang(workerFatalText,hang==1);
    if(const char* text=workerFatal.load(std::memory_order_acquire))fatal(text);
}
void workerProgress(){svcSignalEvent(progressEvent);}
void workerIdle(unsigned microseconds){
    const auto start=svcGetSystemTick();
    svcWaitSynchronization(wakeEvent,s64(microseconds)*1000);
    workerIdleTicks.fetch_add(svcGetSystemTick()-start,std::memory_order_relaxed);
}
// Verify mode: the game thread reached a GPU function with shapes still
// queued and no sync() of its own before it. The queue is drawn then, so the
// order is right, but anything that path did to Citro3D or to the draw
// adapter's tables before that call raced with the worker: every such path
// needs its own sync() at its entry. One line per place, with the code
// addresses found on the stack above this call (callers, and now and then a
// stale word); resolve them with addr2line against the image's ELF.
const std::uintptr_t* gameStackTop{};
// The code addresses found on the game thread's stack above the caller (the
// callers, now and then a stale word), and a sum of the first few.
unsigned stackCodeAddresses(std::uintptr_t (&found)[10],std::uint32_t& sum){
    const auto low=reinterpret_cast<std::uintptr_t>(__start__),high=reinterpret_cast<std::uintptr_t>(&_fini);
    const auto* words=static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    unsigned count=0;
    for(unsigned i=0;i<900&&words+i<gameStackTop&&count<10;++i){
        const auto value=words[i];
        if(value>=low&&value<high&&!(value&3)&&(!count||found[count-1]!=value))found[count++]=value;
    }
    sum=2166136261u;
    for(unsigned i=1;i<count&&i<6;++i)sum=(sum^std::uint32_t(found[i]))*16777619u;
    return count;
}
// Alpha 15: a draw of the game thread (the processor's draw barrier) that
// found shapes queued: the game thread draws all of them first. One line per
// place (stack signature), 16 places; resolve with addr2line on the ELF.
void reportDrawSync(bool overlapping){
    static std::uint32_t seen[16];static unsigned seenCount;
    std::uintptr_t found[10];std::uint32_t sum;
    const unsigned count=stackCodeAddresses(found,sum);
    for(unsigned i=0;i<seenCount;++i)if(seen[i]==sum)return;
    if(seenCount>=std::size(seen))return;
    seen[seenCount++]=sum;
    char text[160];int used=0;
    for(unsigned i=0;i<count&&used>=0&&std::size_t(used)<sizeof(text)-12;++i)used+=std::snprintf(text+used,sizeof(text)-used," %08lx",static_cast<unsigned long>(found[i]));
    OSReport("Native GX worker draw sync with shapes queued (%s): stack code addresses:%s\n",overlapping?"after the painter":"in the painter",text);
}
void reportOwnerSync(){
    static std::uint32_t seen[48];static unsigned seenCount,reports;
    const auto low=reinterpret_cast<std::uintptr_t>(__start__),high=reinterpret_cast<std::uintptr_t>(&_fini);
    const auto* words=static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    std::uintptr_t found[10];unsigned count=0;
    for(unsigned i=0;i<900&&words+i<gameStackTop&&count<10;++i){
        const auto value=words[i];
        if(value>=low&&value<high&&!(value&3)&&(!count||found[count-1]!=value))found[count++]=value;
    }
    std::uint32_t sum=2166136261u;
    for(unsigned i=1;i<count&&i<6;++i)sum=(sum^std::uint32_t(found[i]))*16777619u;
    for(unsigned i=0;i<seenCount;++i)if(seen[i]==sum)return;
    if(seenCount<std::size(seen))seen[seenCount++]=sum;
    if(reports>=40)return;
    ++reports;
    char text[160];int used=0;
    for(unsigned i=0;i<count&&used>=0&&std::size_t(used)<sizeof(text)-12;++i)used+=std::snprintf(text+used,sizeof(text)-used," %08lx",static_cast<unsigned long>(found[i]));
    OSReport("Native GX worker verify: GPU reached with shapes queued and no sync before it; stack code addresses:%s\n",text);
}
// Where the game thread had the queue drawn while shapes were queued: the
// code address of the function that asked (it called syncGxWorker() or asked
// for the GPU owner), how often, and how many shapes the game thread then
// drew in the worker's place. The periodic report prints them: these are the
// places to turn into queued jobs (gx_worker_sync.hpp) or to move.
// Alpha 14: and the game thread's time there (system ticks), the wait for the
// worker to stop and the drawing in its place together.
struct SyncSite {std::uintptr_t address{};std::uint32_t syncs{},shapes{};std::uint64_t ticks{};};
std::array<SyncSite,24> syncSites{};
// Alpha 15: the GPU uses of the game thread that ended an overlap (address,
// count), for the overlap line.
std::array<std::pair<std::uintptr_t,std::uint32_t>,6> overlapEndSites{};
void syncAt(void* site){
    auto& queue=gxworker::queue();
    if(!queue.busy()||gxworker::Queue::consuming()){queue.sync(gxworker::SyncCause::gpu);return;}
    if(queue.overlapping()){
        const auto address=reinterpret_cast<std::uintptr_t>(site);
        for(auto& entry:overlapEndSites)if(entry.first==address||!entry.first){entry.first=address;++entry.second;break;}
    }
    const auto before=queue.counters().drawnByGame;
    const auto start=svcGetSystemTick();
    queue.sync(gxworker::SyncCause::gpu);
    const auto ticks=svcGetSystemTick()-start;
    const auto address=reinterpret_cast<std::uintptr_t>(site);
    SyncSite* place=nullptr;
    for(auto& entry:syncSites){
        if(entry.address==address){place=&entry;break;}
        if(!entry.address&&!place)place=&entry;
    }
    if(place){place->address=address;++place->syncs;place->shapes+=std::uint32_t(queue.counters().drawnByGame-before);place->ticks+=ticks;}
}
void wanted(void* site){
    auto& queue=gxworker::queue();
    if(queue.verifying()&&queue.busy()&&!gxworker::Queue::consuming())reportOwnerSync();
    syncAt(site);
}
// Overlap syncs by where they came from (reason of gxOverlapSync).
std::uint64_t overlapHooks[5]{};
void gpuToWorker(){platformLendGpu(gameRecord,threadRunning?&workerRecord:nullptr,true,wanted);}
}
// The game processor's draw barrier (game_startup.cpp): it is about to draw.
void syncGxWorkerForDraw(){
    auto& queue=gxworker::queue();
    if(queue.busy()&&!gxworker::Queue::consuming()&&threadRunning)reportDrawSync(queue.overlapping());
    queue.sync(gxworker::SyncCause::draw);
}
namespace {
void gpuToGame(){platformLendGpu(gameRecord,threadRunning?&workerRecord:nullptr,false,wanted);}
void* run(void*){
    // The game thread's rounding mode, flush-to-zero and default-NaN bits:
    // the replay computes the same floats on either thread.
    writeFpscr((readFpscr()&0xf80000ffu)|(gameFpscr&~0xf80000ffu));
    OSReport("Native GX worker: running on core %ld, FPSCR %08lx (game thread %08lx)\n",static_cast<long>(svcGetProcessorID()),
        static_cast<unsigned long>(readFpscr()),static_cast<unsigned long>(gameFpscr));
    // Native packets pay only where this thread runs beside the game thread.
    packets::noteWorkerCore(svcGetProcessorID()!=gameCore);
    gxworker::queue().run();
    return nullptr;
}
const char* modeName(gxworker::Mode mode){
    return mode==gxworker::Mode::threaded?"worker on core 2":mode==gxworker::Mode::deferred?"deferred on the game thread":"off";
}
}
namespace {
// Development test (SD file gx-worker-test.txt, "fatal=N"): the worker meets
// a fatal error on the Nth shape it draws. Shows in the emulator what a real
// one does: the main thread ends the program with the message
// (fatalRelayHook), the GPU guard's session record says so, and the next
// start leaves the worker off (game_startup.cpp, gx-worker-learned.txt).
gxworker::Adapter testedAdapter{};
unsigned long testFatalAt{},testDrawn{};
void testedDraw(const gxworker::QueuedShape& shape,const gx::CommandRegisters& bank,const gx::CommandProcessor::ArrayBindings& arrays,
                const gx::TargetExtent& extent){
    if(OSGetCurrentThread()==&workerRecord&&++testDrawn==testFatalAt)
        OSPanic(__FILE__,__LINE__,"Second-core worker test: fatal error on shape %lu (gx-worker-test.txt)",testDrawn);
    testedAdapter.draw(shape,bank,arrays,extent);
}
}
bool startGxWorker(gxworker::Mode mode,bool verify,const gxworker::Adapter& given,void* storage,std::size_t bytes){
    if(mode==gxworker::Mode::off)return false;
    gxworker::Adapter adapter=given;
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gx-worker-test.txt","rb")){
        if(std::fscanf(file,"fatal=%lu",&testFatalAt)==1&&testFatalAt){
            testedAdapter=given;adapter.draw=testedDraw;
            OSReport("Native GX worker test: a fatal error on the worker's shape %lu (gx-worker-test.txt)\n",testFatalAt);
        }
        std::fclose(file);
    }
    gameRecord=OSGetCurrentThread();gameFpscr=readFpscr();gameCore=svcGetProcessorID();
    gameStackTop=static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    // Deferred mode has no worker, but the GPU functions still draw what is
    // queued before they go on (the same order as with the worker).
    gxworker::Platform platform{};platform.gpuToWorker=gpuToWorker;platform.gpuToGame=gpuToGame;
    if(mode==gxworker::Mode::threaded){
        const bool events=R_SUCCEEDED(svcCreateEvent(&wakeEvent,RESET_ONESHOT))&&R_SUCCEEDED(svcCreateEvent(&progressEvent,RESET_ONESHOT));
        if(!events){
            OSReport("Native GX worker: no kernel events; every draw stays on the game thread\n");
            return false;
        }else platform={wakeWorker,waitForWorker,workerProgress,workerIdle,gpuToWorker,gpuToGame,
            +[]{return std::uint64_t(svcGetSystemTick())*1000000ull/SYSCLOCK_ARM11;},
            +[]{fatal("Second-core worker did not stop between two shapes within 20 s");},
            // OS priority 5 is 0x1e: above the audio mixer for as long as
            // the game thread waits (a shape: microseconds to a few
            // milliseconds; the mixer keeps three blocks, 50 ms, queued).
            +[](bool on){OSSetThreadPriority(&workerRecord,on?5:workerPriority);},
            // Overlap: the worker drew the painter's last shape during the
            // next logic tick; it still has the GPU, the PICA starts on it.
            +[]{(void)platformSubmitGpu();}};
    }
    if(!gxworker::queue().start(mode,storage,bytes,adapter,platform,verify)){
        OSReport("Native GX worker: the queue did not start (%lu bytes of storage)\n",static_cast<unsigned long>(bytes));
        return false;
    }
    if(mode==gxworker::Mode::threaded){
        if(!createThreadOnCore(&workerRecord,run,nullptr,nullptr,128*1024,workerPriority,0,2)){
            // No thread: nothing is queued (queueing alone costs time).
            OSReport("Native GX worker: no thread on core 2; every draw stays on the game thread\n");
            gxworker::queue().accept(false);
            return true;
        }
        threadRunning=true;OSResumeThread(&workerRecord);
        // Every session record of this start says the worker was on: a fatal
        // end or a GPU stop then leaves it off on the next start
        // (game_startup.cpp).
        gpuCanarySessionWord("core2");
    }
    gxWorkerSyncHook=+[]{syncAt(__builtin_return_address(0));};
    fatalRelayHook=relayWorkerFatal;gpuHangRelayHook=relayWorkerGpuHang;
    gxWorkerConsumingHook=+[]{return gxworker::Queue::consuming();};
    reportTick=svcGetSystemTick();
    OSReport("Native GX worker: %s%s, %lu KiB of records\n",modeName(mode),verify?" with verify":"",static_cast<unsigned long>(bytes/1024));
    return true;
}
void stopGxWorker(){
    auto& queue=gxworker::queue();
    if(queue.mode()==gxworker::Mode::off)return;
    queue.sync();
    if(!threadRunning)return;
    queue.stop();
    const auto deadline=svcGetSystemTick()+2*SYSCLOCK_ARM11;
    while(!OSIsThreadTerminated(&workerRecord)&&svcGetSystemTick()<deadline)svcSleepThread(1000000);
    if(OSIsThreadTerminated(&workerRecord))OSJoinThread(&workerRecord,nullptr);
    else OSReport("Native GX worker: the thread did not end within 2 s\n");
    threadRunning=false;
}
// When the worker rests. A shape the game thread ends up drawing itself (at a
// sync) cost its queueing on top; where the audio mixer needs most of core 2
// (Death Mountain: 59 % of it in the emulator) the worker draws few shapes.
// Up to Alpha 13 the queue rested whenever the worker drew under half of the
// queued shapes in 24 pictures: a guess. Queueing a shape costs about a
// tenth of drawing it (Azahar village, profiled: 20 against 180 us), so a
// worker that draws a third of them still saves time, and the rule rested
// in the village where syncs, not the mixer, handed shapes back (Alpha 13).
//
// Alpha 14 measures instead. A low share only starts a trial: the queue rests
// for trialLength pictures, and the painter's time per draw over the trial is
// compared with the same measure over the pictures just before it, with the
// worker. The queue rests (180 pictures, doubling up to 1440) only when the
// trial was faster by more than trialMargin; otherwise the worker keeps
// drawing and no trial is started for a while (600 pictures, doubling up to
// 4800). A rest can therefore cost at most a trial, never a picture rate
// below the one without the worker. SD switch gx-rest-measured-off.txt:
// the Alpha 13 rule. The automatic test's onecore phase switches the queue
// off (testWants) and its still phase keeps it on without a rest
// (testMeasuring).
namespace {
bool testWants=true,testMeasuring{},resting{};
bool measuredRests{true};
unsigned restPictures{},lowPictures{},probePictures{},restLength{};
std::uint64_t picturesQueued{},picturesByWorker{},rests{},restedPictures{};
constexpr unsigned lowShareLimit=24,firstRest=180,longestRest=1440,probeLength=24;
// Measured rests.
constexpr unsigned trialLength=30,firstQuiet=600,longestQuiet=4800,onWindowPictures=96,leastPictures=12,leastDraws=16;
constexpr double trialMargin=0.04;
struct Window {
    std::uint64_t ticks{},draws{};unsigned pictures{};
    void add(std::uint64_t paint,std::uint32_t drawn){ticks+=paint;draws+=drawn;++pictures;}
    // The most recent pictures weigh most: halved when full.
    void age(unsigned most){if(pictures>=most){ticks/=2;draws/=2;pictures/=2;}}
    double perDraw()const{return draws?double(ticks)/double(draws):0.0;}
};
Window withWorker,resting_;
bool trial{};
unsigned trialLeft{},quietPictures{},quietLength{};
std::uint64_t trials{},trialsWon{};
}
void wantGxWorker(bool on,bool measuring){
    testWants=on;testMeasuring=measuring;
    if(measuring){resting=false;trial=false;restPictures=0;lowPictures=0;probePictures=0;}
    auto& queue=gxworker::queue();
    if(queue.mode()!=gxworker::Mode::off)queue.accept(testWants&&!resting&&!trial);
}
void setGxWorkerMeasuredRests(bool on){measuredRests=on;}
namespace {bool trialsHeld{};}
bool gxWorkerResting(){return resting||trial;}
void holdGxWorkerTrials(bool hold){trialsHeld=hold;}
namespace {
// The Alpha 10-13 rule.
void restByShare(gxworker::Queue& queue,std::uint64_t queued,std::uint64_t byWorker){
    if(resting){
        ++restedPictures;
        if(!--restPictures){resting=false;probePictures=probeLength;lowPictures=0;queue.accept(testWants);}
        return;
    }
    // Nothing to judge by (a menu, a loading screen), or a measurement.
    if(!testWants||testMeasuring||queued<16)return;
    // Less than half of the shapes: the picture was not faster than without.
    if(2*byWorker<queued)++lowPictures;else if(lowPictures)--lowPictures;
    if(probePictures&&!--probePictures&&!lowPictures)restLength=0; // the worker keeps up again
    if(trialsHeld)return; // a measurement runs: the rest waits for it
    if(lowPictures>=lowShareLimit){
        restLength=restLength?std::min(2*restLength,longestRest):firstRest;
        resting=true;restPictures=restLength;lowPictures=0;++rests;
        queue.accept(false);
        if(rests<=12||!(rests&(rests-1)))OSReport("Native GX worker: resting for %u pictures (it drew under half of the queued shapes in %u of the last pictures; "
            "the audio mixer shares its core); rest %llu\n",restLength,lowShareLimit,static_cast<unsigned long long>(rests));
    }
}
void restMeasured(gxworker::Queue& queue,std::uint64_t queued,std::uint64_t byWorker,std::uint64_t paintTicks,std::uint32_t draws){
    const auto us=[](double ticks){return ticks*1e6/double(SYSCLOCK_ARM11);};
    if(resting){
        ++restedPictures;
        if(!--restPictures){resting=false;lowPictures=0;withWorker={};queue.accept(testWants);}
        return;
    }
    if(trial){
        ++restedPictures;
        if(draws>=leastDraws)resting_.add(paintTicks,draws);
        if(--trialLeft)return;
        trial=false;++trials;
        const double off=resting_.perDraw(),on=withWorker.perDraw();
        const bool restWins=resting_.pictures>=leastPictures&&withWorker.pictures>=leastPictures&&off>0&&on>0&&off<on*(1.0-trialMargin);
        if(restWins){
            restLength=restLength?std::min(2*restLength,longestRest):firstRest;
            resting=true;restPictures=restLength;++rests;
            if(rests<=12||!(rests&(rests-1)))OSReport("Native GX worker: resting for %u pictures: a %u-picture trial without it painted %.1f us a draw, "
                "with it %.1f (it drew %llu of %llu shapes); rest %llu\n",restLength,trialLength,us(off),us(on),
                static_cast<unsigned long long>(byWorker),static_cast<unsigned long long>(queued),static_cast<unsigned long long>(rests));
            return;
        }
        // The worker is as fast or faster even with its low share.
        ++trialsWon;restLength=0;
        quietLength=quietLength?std::min(2*quietLength,longestQuiet):firstQuiet;quietPictures=quietLength;
        lowPictures=0;queue.accept(testWants);
        if(trialsWon<=12||!(trialsWon&(trialsWon-1)))OSReport("Native GX worker: keeps drawing: with it %.1f us a draw, a %u-picture trial without it %.1f "
            "(%u and %u pictures); next trial in %u pictures at the earliest; trial %llu\n",us(on),trialLength,us(off),withWorker.pictures,resting_.pictures,
            quietLength,static_cast<unsigned long long>(trials));
        return;
    }
    // Nothing to judge by (a menu, a loading screen), or a measurement.
    if(!testWants||testMeasuring||queued<16)return;
    if(draws>=leastDraws){withWorker.age(onWindowPictures);withWorker.add(paintTicks,draws);}
    if(2*byWorker<queued)++lowPictures;else if(lowPictures)--lowPictures;
    if(quietPictures){--quietPictures;return;}
    // A measurement runs: the trial waits for it (the share goes on counting).
    if(trialsHeld)return;
    if(lowPictures>=lowShareLimit&&withWorker.pictures>=leastPictures){
        trial=true;trialLeft=trialLength;resting_={};lowPictures=0;
        queue.accept(false);
    }
}
}
void gxWorkerPictureEnd(std::uint64_t paintTicks,std::uint32_t draws){
    auto& queue=gxworker::queue();
    if(queue.mode()!=gxworker::Mode::threaded||!threadRunning)return;
    const auto& c=queue.counters();
    const auto queued=c.queued-picturesQueued,byWorker=c.drawnByWorker-picturesByWorker;
    picturesQueued=c.queued;picturesByWorker=c.drawnByWorker;
    if(measuredRests)restMeasured(queue,queued,byWorker,paintTicks,draws);
    else restByShare(queue,queued,byWorker);
}
void describeGxWorker(char* text,std::size_t size){
    const auto& queue=gxworker::queue();const auto& c=queue.counters();
    const auto n=[](std::uint64_t value){return static_cast<unsigned long long>(value);};
    using Cause=gxworker::SyncCause;
    std::snprintf(text,size,"mode:%s queued:%llu runs:%llu run_bytes:%llu by_worker:%llu by_game:%llu syncs:%llu(draw:%llu gpu:%llu end:%llu full:%llu other:%llu) "
        "takeovers:%llu(draw:%llu gpu:%llu end:%llu) park_waits:%llu delta_words:%llu array_records:%llu whole_banks:%llu unmarked:%llu sum_mismatches:%llu "
        "damaged:%llu binding_refusals:%lu wait_ms:%.1f rests:%llu rested_pictures:%llu trials:%llu trials_kept:%llu",
        queue.mode()==gxworker::Mode::threaded?"w":queue.mode()==gxworker::Mode::deferred?"d":"off",
        n(c.queued),n(c.runs),n(c.runBytes),n(c.drawnByWorker),n(c.drawnByGame),n(c.syncs),n(c.syncsBy[unsigned(Cause::draw)]),n(c.syncsBy[unsigned(Cause::gpu)]),
        n(c.syncsBy[unsigned(Cause::painterEnd)]),n(c.syncsBy[unsigned(Cause::ringFull)]),n(c.syncsBy[unsigned(Cause::other)]),
        n(c.takeovers),n(c.takeoversBy[unsigned(Cause::draw)]),n(c.takeoversBy[unsigned(Cause::gpu)]),n(c.takeoversBy[unsigned(Cause::painterEnd)]),
        n(c.parkWaits),n(c.deltaWords),n(c.arrayRecords),n(c.fullBanks),n(c.unmarkedBlocks),n(c.sumMismatches),n(c.damaged),
        static_cast<unsigned long>(gx::queuedTextureBindingRefusals),double(gameWaitTicks.load(std::memory_order_relaxed))*1000.0/SYSCLOCK_ARM11,n(rests),n(restedPictures),
        n(trials),n(trialsWon));
}
void reportGxWorker(bool always){
    const auto& queue=gxworker::queue();
    if(queue.mode()==gxworker::Mode::off)return;
    if(!always&&++picturesSinceReport<300)return;
    const auto c=queue.counters();
    const auto wait=gameWaitTicks.load(std::memory_order_relaxed),idle=workerIdleTicks.load(std::memory_order_relaxed);
    const auto now=svcGetSystemTick();
    const double pictures=picturesSinceReport?double(picturesSinceReport):1.0;
    const auto ms=[](std::uint64_t ticks){return double(ticks)*1000.0/SYSCLOCK_ARM11;};
    const auto per=[&](std::uint64_t now,std::uint64_t before){return double(now-before)/pictures;};
    using Cause=gxworker::SyncCause;
    // Per picture: shapes queued (primitive runs among them, and their copied
    // bytes), drawn by the worker and by the game thread (at a sync, in the
    // worker's place), syncs that found shapes queued by
    // cause (another draw, a GPU call, the end of painting), those at which
    // the game thread took shapes over, words of bank deltas, the game
    // thread's wait for the worker to stop, and how much of the time the
    // worker was not asleep. The last three counters must stay zero.
    OSReport("Native GX worker (%s), last %u pictures, per picture: queued %.1f (runs %.1f, %.0f bytes) by-worker %.1f by-game %.1f; "
        "syncs %.1f (draw %.1f gpu %.1f end %.1f) takeovers %.1f; delta words %.0f; game waited %.3f ms, worker busy %.0f %%; unmarked=%llu sums=%llu damaged=%llu\n",
        modeName(queue.mode()),picturesSinceReport,per(c.queued,reported.queued),per(c.runs,reported.runs),per(c.runBytes,reported.runBytes),
        per(c.drawnByWorker,reported.drawnByWorker),
        per(c.drawnByGame,reported.drawnByGame),per(c.syncs,reported.syncs),
        per(c.syncsBy[unsigned(Cause::draw)],reported.syncsBy[unsigned(Cause::draw)]),per(c.syncsBy[unsigned(Cause::gpu)],reported.syncsBy[unsigned(Cause::gpu)]),
        per(c.syncsBy[unsigned(Cause::painterEnd)],reported.syncsBy[unsigned(Cause::painterEnd)]),per(c.takeovers,reported.takeovers),
        per(c.deltaWords,reported.deltaWords),ms(wait-reportedWait)/pictures,
        now>reportTick?100.0-100.0*double(idle-reportedIdle)/double(now-reportTick):0.0,
        static_cast<unsigned long long>(c.unmarkedBlocks),static_cast<unsigned long long>(c.sumMismatches),static_cast<unsigned long long>(c.damaged));
    {
        // address:syncs/shapes the game thread drew there/ms a picture it
        // spent there (addr2line on the image's ELF). Lines under 200 characters.
        char text[200];int used=0;
        for(auto& site:syncSites)if(site.syncs){
            char item[48];
            const int n=std::snprintf(item,sizeof(item)," %08lx:%lu/%lu/%.3f",static_cast<unsigned long>(site.address),
                static_cast<unsigned long>(site.syncs),static_cast<unsigned long>(site.shapes),ms(site.ticks)/pictures);
            site.syncs=site.shapes=0;site.ticks=0;
            if(n<=0)continue;
            if(used&&std::size_t(used+n)>=sizeof(text)){OSReport("Native GX worker sync sites:%s\n",text);used=0;}
            std::memcpy(text+used,item,std::size_t(n)+1);used+=n;
        }
        if(used>0)OSReport("Native GX worker sync sites:%s\n",text);
    }
    {
        // Alpha 14: the game thread's time in syncs that found shapes queued,
        // per picture, by cause: waiting for the worker to stop / drawing in
        // its place. "end" is the painter's end (the picture boundary).
        const auto us=[&](const std::uint64_t (&now)[unsigned(Cause::count)],const std::uint64_t (&before)[unsigned(Cause::count)],Cause cause){
            return double(now[unsigned(cause)]-before[unsigned(cause)])/1000.0/pictures;};
        std::uint64_t sections{},skipped{},shapesSkipped{},runsDropped{},bloomJobs{},bloomFailed{};
        nativeSectionStatistics(sections,skipped,shapesSkipped,runsDropped);gxBloomJobStatistics(bloomJobs,bloomFailed);
        static std::uint64_t lastSections,lastSkipped,lastShapes,lastRuns,lastBloom;
        OSReport("Native GX worker sync ms per picture (wait/drew): draw %.3f/%.3f gpu %.3f/%.3f end %.3f/%.3f full %.3f/%.3f other %.3f/%.3f "
            "frame %.3f/%.3f heap %.3f/%.3f delete %.3f/%.3f create %.3f/%.3f vertex %.3f/%.3f; "
            "sections %.1f (failed %.1f, shapes left out %.1f, runs %.1f); bloom jobs %.1f (failed %llu); trials %llu kept %llu\n",
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::draw),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::draw),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::gpu),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::gpu),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::painterEnd),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::painterEnd),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::ringFull),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::ringFull),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::other),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::other),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::frameEnd),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::frameEnd),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::heap),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::heap),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::deletion),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::deletion),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::creation),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::creation),
            us(c.waitMicrosBy,reported.waitMicrosBy,Cause::vertex),us(c.takeoverMicrosBy,reported.takeoverMicrosBy,Cause::vertex),
            double(sections-lastSections)/pictures,double(skipped-lastSkipped)/pictures,double(shapesSkipped-lastShapes)/pictures,
            double(runsDropped-lastRuns)/pictures,double(bloomJobs-lastBloom)/pictures,static_cast<unsigned long long>(bloomFailed),
            static_cast<unsigned long long>(trials),static_cast<unsigned long long>(trialsWon));
        lastSections=sections;lastSkipped=skipped;lastShapes=shapesSkipped;lastRuns=runsDropped;lastBloom=bloomJobs;
    }
    {
        // Alpha 15 overlap (gx-overlap.txt): pictures whose painter ended with
        // shapes left to the worker, what ended each overlap (frame = the
        // picture's close, the whole logic tick ran beside the worker; the
        // others are syncs before a free, a process deletion or creation, a
        // CPU vertex rewrite, a GPU use or a draw of the game thread), how many
        // of the frame ones found every shape drawn, the shapes the game
        // thread drew at the end of an overlap, and ring-full syncs that drew
        // only until the record fitted. Per picture except the counts.
        const auto d=[&](std::uint64_t now,std::uint64_t before){return static_cast<unsigned long long>(now-before);};
        const auto e=[&](Cause cause){return d(c.overlapEndsBy[unsigned(cause)],reported.overlapEndsBy[unsigned(cause)]);};
        if(c.overlaps!=reported.overlaps||c.partialDrains!=reported.partialDrains)
            OSReport("Native GX overlap, last %u pictures: overlaps %llu ended by frame %llu (all drawn %llu) heap %llu delete %llu create %llu vertex %llu "
                "gpu %llu draw %llu other %llu; game drew %.1f shapes a picture there; partial ring drains %llu; hooks heap %llu delete %llu create %llu vertex %llu\n",
                picturesSinceReport,d(c.overlaps,reported.overlaps),e(Cause::frameEnd),d(c.overlapsFinished,reported.overlapsFinished),
                e(Cause::heap),e(Cause::deletion),e(Cause::creation),e(Cause::vertex),e(Cause::gpu),e(Cause::draw),e(Cause::other),
                per(c.overlapShapesByGame,reported.overlapShapesByGame),d(c.partialDrains,reported.partialDrains),
                static_cast<unsigned long long>(overlapHooks[1]),static_cast<unsigned long long>(overlapHooks[2]),
                static_cast<unsigned long long>(overlapHooks[3]),static_cast<unsigned long long>(overlapHooks[4]));
        for(auto& hook:overlapHooks)hook=0;
        // Where GPU uses ended overlaps, and runs drawn on the game thread
        // because their arrays are writable memory (they sync first).
        std::uint64_t runs{},refused{};nativeRunStatistics(runs,refused);
        static std::uint64_t lastRefused;
        char text[200];int used=0;
        for(auto& entry:overlapEndSites)if(entry.first&&used>=0&&std::size_t(used)<sizeof(text)-24){
            used+=std::snprintf(text+used,sizeof(text)-used," %08lx:%lu",static_cast<unsigned long>(entry.first),static_cast<unsigned long>(entry.second));
            entry={};
        }
        // Queued shapes and runs the drawing side could not draw (they are
        // missing from their picture): must stay 0 (Alpha 15 flicker fix).
        std::uint64_t shapes{},lists{},failures{};nativeWorkerStatistics(shapes,lists,failures);
        static std::uint64_t lastFailures;
        if(c.overlaps!=reported.overlaps||refused!=lastRefused||failures!=lastFailures)
            OSReport("Native GX overlap ended by GPU use at:%s; runs refused for writable arrays %.1f a picture; queued draws not drawn %llu\n",
                used>0?text:" -",double(refused-lastRefused)/pictures,static_cast<unsigned long long>(failures-lastFailures));
        lastRefused=refused;lastFailures=failures;
    }
    // Native packets (native_packets.hpp): their lines over the same pictures.
    packets::report(picturesSinceReport);
    reported=c;reportedWait=wait;reportedIdle=idle;reportTick=now;picturesSinceReport=0;
}
}
namespace dusk::native {
// gxOverlapSyncHook (gx_worker_sync.hpp): game code that frees, creates or
// rewrites what a queued draw may read (1 an allocation ends or the heap tree
// changes, 2 a process deletion, 3 a creation is pending, 4 a model whose
// arrays the CPU rewrites). While the worker still draws the painter's shapes
// after the painter ended (Queue::beginOverlap), they are drawn first. Other
// threads (the loader thread frees too) and pictures without an overlap pass
// through.
void gxOverlapSync(int reason){
    auto& queue=gxworker::queue();
    if(!queue.overlapping()||!threadRunning||OSGetCurrentThread()!=gameRecord||gxworker::Queue::consuming())return;
    using Cause=gxworker::SyncCause;
    const Cause cause=reason==1?Cause::heap:reason==2?Cause::deletion:reason==3?Cause::creation:Cause::vertex;
    if(reason>=1&&reason<=4)++overlapHooks[reason];
    queue.sync(cause);
}
}
