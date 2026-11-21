#pragma once
// Second-core worker: the 3D shapes of a picture are drawn on another core.
//
// The game thread walks the J3D lists and runs the material side of every
// packet on its own command processor (register writes only). The shape of a
// packet used to be drawn right there: find its record, rebuild lighting,
// texture and matrix state from the GX registers, submit; or, without a
// record, run its display list through the vertex decoder. That is the
// largest single part of painting a picture. With the worker on, the game
// thread only queues the shape with the words its processor changed in the
// register bank since the last queued shape (gx_bank_delta.hpp). A thread on
// core 2 keeps a copy of the bank up to date with them and draws the shape as
// the game thread would have: by its record, or by running its display list
// on a second processor that adopts that copy.
//
// Rules that keep this equal to the single-thread path:
// - One thread at a time owns the GPU, Citro3D and every table the draw
//   adapter keeps (the consumer). The game thread hands them to the worker
//   with the first shape it queues and takes them back with sync(). Every
//   path of the game thread that draws or touches the GPU in any other way
//   calls sync() first (the processor's draw barrier, the draw sink, the GPU
//   queue functions, frame end), so the draws keep the order the game issued
//   them in.
// - sync() does not wait for the worker to finish what is queued: it asks
//   the worker to stop after the shape it is drawing (park) and the game
//   thread draws the rest itself, from the same records, with the same code.
//   The game thread has nothing else to do at that point, and core 2 is
//   shared with the audio mixer: a picture can then never take much longer
//   than with everything drawn by the game thread.
// - While the game thread updates actors nothing is queued. Up to Alpha 14
//   the painter ended with sync() and the worker slept meanwhile. With the
//   overlap (Alpha 15, beginOverlap) the worker goes on drawing the painter's
//   shapes during the next logic tick; game code that frees, creates or
//   rewrites what they read syncs first, and the picture's close syncs.
//
// Modes (game_startup.cpp reads the SD switch gx-worker.txt): off; `d` the
// same records consumed by the game thread itself at sync() (no second
// thread: proves the records); `w` the worker on core 2; `v` adds the verify
// checks to either.
#include "gx_bank_delta.hpp"
#include "gx_target_extent.hpp"
#include <atomic>

namespace dusk::native::gxworker {
// One J3D shape as the game thread issued it, or a run of primitive packets
// the game thread's command processor handed over (CommandProcessor::
// setRunDeferral: grass, sky, particles and other draws that are not J3D
// shapes). A shape's display list is game memory that stays as it is while
// the picture is painted; a run is copied into the queue, and `list` then
// points at that copy while the shape is drawn.
struct QueuedShape {
    const void* list{};std::uint32_t listBytes{};
    std::uint32_t generation{}; // vertex-data generation of a static shape, 0 for a shape with CPU-written arrays
    std::uint32_t casterSlot{}; // model-shadow caster selected when the shape was issued
    std::uint32_t bankSum{};    // verify mode: sum of the bank's words when queued
    // Not a shape but a job of the draw adapter (gx_worker_sync.hpp, gxJob...):
    // work that belongs between two shapes of the picture. Its arguments are
    // copied into the queue like a run (`list`, `listBytes`).
    std::uint32_t job{};
    // The process whose model the shape belongs to (profile name + 1, 0 none):
    // measurement only (gx_profile.hpp), never read by the draw itself.
    std::uint32_t owner{};
    // Levels of detail chosen for this matrix group (runtime/model_lod.hpp):
    // its level data (game-thread memory, kept until a stage boundary after
    // the queue drained) and the scene level | shadow level << 4.
    const void* lodGroup{};
    std::uint32_t lodLevels{};
};
enum class Mode:std::uint8_t {off,deferred,threaded};
// Why the game thread asked for the GPU back.
// Alpha 15 (overlap, gx-overlap.txt): frameEnd is the picture's close in the
// game loop; heap, deletion, creation and vertex are the targeted syncs of
// game code that frees, creates or rewrites what a queued draw may read while
// the worker still draws the painter's shapes during the next logic tick.
enum class SyncCause:std::uint8_t {draw,gpu,painterEnd,ringFull,other,frameEnd,heap,deletion,creation,vertex,count};
// What the queue needs from the draw adapter (gx_command_draw.cpp).
struct Adapter {
    // The consumer (the worker, or the game thread at sync()): draw the shape
    // under `bank` and `arrays` on a target of `extent`.
    void (*draw)(const QueuedShape&,const gx::CommandRegisters& bank,const gx::CommandProcessor::ArrayBindings& arrays,
                 const gx::TargetExtent& extent);
    // Optional, native packets (runtime/native_packets.hpp). `mirror` is a
    // processor that only the consumer uses: its bank is the queue's copy of
    // the game bank and its bindings the copy of the array bindings. A
    // native record carries the register operations of a whole J3D packet
    // (the game thread did not run them); `native` runs them on `mirror` in
    // their order and draws each shape of the packet through `draw` under
    // the bank as it stands at that point.
    gx::CommandProcessor* mirror{};
    void (*native)(const std::uint32_t* body,std::size_t words,gx::CommandProcessor& mirror,const gx::TargetExtent& extent){};
};
// What the queue needs from the system (gx_worker_ctru.cpp; the host test
// supplies its own).
struct Platform {
    void (*wakeWorker)();                      // there are records, or a park request
    void (*waitForWorker)(unsigned microseconds); // game thread: until the worker reports progress, at most this long
    void (*workerProgress)();                  // worker: it parked
    void (*workerIdle)(unsigned microseconds); // worker: nothing to do, sleep until woken, at most this long
    void (*gpuToWorker)();void (*gpuToGame)();
    // Optional. The game thread has waited stuckMicroseconds for the worker
    // to stop between two shapes: `now` is a clock in microseconds and
    // `workerStuck` ends the program with a record of it (it does not return;
    // the next start then leaves the worker off).
    std::uint64_t (*now)(){};void (*workerStuck)(){};
    // Optional. The game thread is about to wait for the worker to stop
    // between two shapes (true) and has it stopped (false): the worker shares
    // its core with a thread of higher priority (the audio mixer), and a
    // worker that is not running does not finish its shape.
    void (*workerUrgent)(bool on){};
    // Optional, worker: it drew the last queued shape after the painter ended
    // (beginOverlap); it still has the GPU: let it start on what was drawn.
    void (*drained)(){};
};
struct Counters {
    std::uint64_t queued{},drawnByWorker{},drawnByGame{},syncs{},takeovers{},parkWaits{},ringFull{},fullBanks{},deltaWords{},records{},wraps{};
    std::uint64_t arrayRecords{}; // shapes that carried the array bindings
    std::uint64_t runs{},runBytes{}; // primitive runs among the shapes, and the bytes copied for them
    std::uint64_t syncsBy[unsigned(SyncCause::count)]{};     // sync() calls that found shapes queued, by cause
    std::uint64_t takeoversBy[unsigned(SyncCause::count)]{}; // ... and found shapes the worker had not drawn yet
    std::uint64_t unmarkedBlocks{},sumMismatches{},damaged{}; // verify mode and record checks: must stay zero
    std::uint64_t natives{},nativeWords{}; // native packet records (Adapter::native) and their body words
    // Alpha 14: microseconds the game thread spent in syncs that found shapes
    // queued (Platform::now; zero without it), by cause: waiting for the
    // worker to stop between two records, and drawing what it had not drawn.
    std::uint64_t waitMicrosBy[unsigned(SyncCause::count)]{},takeoverMicrosBy[unsigned(SyncCause::count)]{};
    // Alpha 15. partialDrains: ring-full syncs that drew only until the record
    // fitted (setPartialDrain), the rest left to the worker. overlaps:
    // pictures whose painter ended with shapes still queued (beginOverlap);
    // overlapEndsBy: the sync that ended each one, by cause (frameEnd = the
    // whole next logic tick ran beside the worker); overlapsFinished: of the
    // frameEnd ones, those that found every shape drawn; overlapShapesByGame:
    // shapes the game thread drew at the sync that ended an overlap.
    std::uint64_t partialDrains{},overlaps{},overlapsFinished{},overlapShapesByGame{};
    std::uint64_t overlapEndsBy[unsigned(SyncCause::count)]{};
    // unmarkedBlocks: verify found a bank word the log and the tail left
    // behind (a store the processor does not log); the whole bank was sent.
};
class Queue {
public:
    // `storage` (8-byte aligned, `bytes` long, at least minimumStorage) holds
    // the two bank copies and the record ring; it stays with the queue. The
    // queue object itself is small: the executable's static size must not
    // grow (main.cpp, the log ring).
    static constexpr std::size_t minimumStorage=2*sizeof(gx::CommandRegisters)+65536*sizeof(std::uint32_t);
    bool start(Mode mode,void* storage,std::size_t bytes,const Adapter& adapter,const Platform& platform,bool verify);
    Mode mode()const{return mode_;}
    // Worker mode without a worker (its thread could not be created): the
    // game thread consumes the records. Only before the first shape.
    bool deferInstead(){
        if(mode_!=Mode::threaded||owned_.load()||head_!=tail.load())return false;
        mode_=Mode::deferred;return true;
    }
    bool verifying()const{return verify_;}
    // ---- game thread ----
    // Shapes are queued only while accepting: a started queue can be turned
    // off and on between pictures (the automatic test measures both). Turning
    // it off syncs first.
    void accept(bool on){if(!on)sync(SyncCause::other);accepting_=on&&mode_!=Mode::off;}
    bool accepting()const{return accepting_&&!consuming();}
    // Queues one shape under `bank`. `log` holds the processor's stores since
    // the last queued shape (the caller restarts it after a true result);
    // `arrays` are the array bindings of the processor right now (they are
    // sent only when the bank's vertex setup epoch moved since the last
    // shape: they cannot change without it).
    // `copied`: the bytes of a primitive run; they become the shape's list.
    // False as well when the shape with everything it carries is half the
    // ring or more (the stream's 64 KiB of immediate vertices fit; a longer
    // run is drawn by the caller): a record must fit in front of or behind
    // wherever an empty ring's indices stand.
    bool push(const gx::CommandRegisters& bank,const gx::BankLog& log,const gx::CommandProcessor::ArrayBindings& arrays,
              const gx::TargetExtent& extent,const QueuedShape& shape,const void* copied=nullptr,std::size_t copiedBytes=0){
        if(!accepting()||largestRecord(log.used,copiedBytes)>=capacity_/2)return false;
        pushShape(bank,log,!sentArraysValid_||sentSetupEpoch_!=bank.vertexSetupEpoch?&arrays:nullptr,extent,shape,copied,copiedBytes);
        return true;
    }
    // Native packets (runtime/native_packets.hpp): one record of `words`
    // body words. `prelude`: the bank delta, extent and bindings go first as
    // for a shape (the first native record after anything else); without it
    // the caller states that its bank did not change since the last record
    // (the worker's copy is ahead of it: it ran the earlier native records).
    // nativeFits: such a record with a log of `logWords` would be taken now.
    bool nativeFits(std::size_t logWords,std::size_t words)const{
        return accepting()&&mirrorProcessor_&&adapter_.native&&!resync_.load(std::memory_order_relaxed)&&
               largestRecord(logWords,words*4)<capacity_/2;
    }
    // `shapes`: the shapes the record draws; they count as queued, and as
    // drawn by whichever thread consumes the record (the worker's rest
    // heuristic and the log read those counters).
    bool pushNative(const gx::CommandRegisters& bank,const gx::BankLog& log,const gx::CommandProcessor::ArrayBindings& arrays,
                    const gx::TargetExtent& extent,const std::uint32_t* body,std::size_t words,bool prelude,unsigned shapes=0);
    // The next record sends the whole bank (verify mode after native
    // records: `sent` no longer describes the worker's copy).
    void invalidateSent(){sentValid_=false;}
    bool nativeAvailable()const{return mirrorProcessor_&&adapter_.native;}
    // Every queued shape has been drawn and the GPU is the caller's. A no-op
    // on the consumer itself (the worker, or the game thread inside sync()).
    void sync(SyncCause cause=SyncCause::other){
        if(const auto hook=captureHook)[[unlikely]]if(!consuming())hook();
        if(owned_.load(std::memory_order_relaxed))[[unlikely]]if(!consuming())drain(cause);
    }
    // Native packets (runtime/native_packets.hpp): GPU work of the game
    // thread first ends a packet capture in progress, so the packet's shapes
    // are queued before it as they were issued before it. Installed once,
    // before the worker starts; called on the game thread only.
    static inline void (*captureHook)()=nullptr;
    bool busy()const{return owned_.load(std::memory_order_relaxed);}
    // Alpha 15. A ring that is full no longer hands the whole queue to the
    // game thread: it draws only until the record fits, the worker goes on
    // with the rest (worker mode). Set before the first shape.
    void setPartialDrain(bool on){partialDrain_=on;}
    // Overlap (game thread, at the painter's end): the painter ended with
    // shapes the worker has not drawn. Instead of drawing them now (sync),
    // the game thread runs the next logic tick while the worker draws them;
    // the game code that frees, creates or rewrites what they may read syncs
    // first (overlapping() true), and the picture's close syncs in any case.
    // False: nothing is left to draw (the caller syncs as before, a cheap
    // hand-back). The worker calls Platform::drained once it drew the last.
    bool beginOverlap();
    // True on the game thread between beginOverlap and the sync that ends it.
    bool overlapping()const{return overlapping_.load(std::memory_order_relaxed);}
    // ---- worker thread ----
    void run();   // until stop()
    void stop();  // game thread; the worker leaves run()
    const Counters& counters()const{return counters_;}
    // True on the thread that is drawing queued shapes right now.
    static bool consuming(){return consumer_;}
private:
    enum Kind:std::uint32_t {wrap=1,full,delta,extentRecord,arraysRecord,shapeRecord,nativeRecord};
    static constexpr std::uint32_t header(Kind kind,std::size_t words){return std::uint32_t(kind)|std::uint32_t(words<<8);}
    enum Park:std::uint32_t {running,parkAsked,parked};
    // The most ring words one shape can take.
    static std::size_t largestRecord(std::size_t logWords,std::size_t copiedBytes);
    std::uint32_t* reserve(std::size_t words);
    void pushShape(const gx::CommandRegisters& bank,const gx::BankLog& log,const gx::CommandProcessor::ArrayBindings* arrays,
                   const gx::TargetExtent& extent,const QueuedShape& shape,const void* copied,std::size_t copiedBytes);
    // The bank delta (or whole bank), extent and bindings records in front of
    // a shape or native record at `at`; returns the words used.
    std::size_t writePrelude(std::uint32_t* at,const gx::CommandRegisters& bank,const gx::BankLog& log,
                             const gx::CommandProcessor::ArrayBindings* arrays,const gx::TargetExtent& extent);
    void publish();
    // The bindings the consumer draws with.
    const gx::CommandProcessor::ArrayBindings& mirrorArrays()const{return mirrorProcessor_?mirrorProcessor_->currentArrayBindings():mirrorArrays_;}
    // `room`: a ring-full sync that stops drawing once `room` words fit at the
    // head (0: every queued record is drawn).
    void drain(SyncCause cause,std::size_t room=0);
    // Game thread inside drain (the worker parked): `words` fit at the head.
    bool roomFor(std::size_t words)const;
    bool consumeOne(bool worker); // false: nothing left
    Mode mode_{Mode::off};bool verify_{},accepting_{},partialDrain_{};
    Adapter adapter_{};Platform platform_{};
    std::uint32_t* ring_{};std::uint32_t capacity_{};
    // Producer (game thread).
    std::uint32_t head_{};
    gx::CommandRegisters* sent_{};bool sentValid_{};
    gx::TargetExtent sentExtent_{};bool sentExtentValid_{};
    std::uint32_t sentSetupEpoch_{};bool sentArraysValid_{};
    // Shapes are queued and the GPU is the consumer's. Written by the game
    // thread only; the worker reads it where the draw code calls sync().
    std::atomic<bool> owned_{};
    // Consumer: the worker thread, or the game thread in deferred mode and
    // after it parked the worker. Never both at once.
    std::uint32_t tail_{};
    gx::CommandRegisters* mirror_{};
    gx::TargetExtent mirrorExtent_{};
    gx::CommandProcessor::ArrayBindings mirrorArrays_{};
    // Adapter::mirror: mirror_ is its bank and the bindings are its own.
    gx::CommandProcessor* mirrorProcessor_{};
    static thread_local bool consumer_;
    // Shared.
    alignas(32) std::atomic<std::uint32_t> head{};
    alignas(32) std::atomic<std::uint32_t> tail{};
    // park_: running -> parkAsked (game thread) -> parked (worker, between two
    // records) -> running (game thread, after it consumed what was left).
    // resync_: a record was unreadable, the producer sends the whole bank again.
    std::atomic<std::uint32_t> park_{running};
    std::atomic<bool> resync_{},stopping_{},workerSleeping_{},gameWaiting_{};
    // overlapping_: written by the game thread only (beginOverlap, drain);
    // atomic because the hooks of game code that other threads run too
    // (JKRHeap frees on the loader thread) look at it before they know their
    // thread. drainNotice_: game thread -> worker, call Platform::drained when
    // the queue is empty (beginOverlap); cleared by drain.
    std::atomic<bool> overlapping_{},drainNotice_{};
    // Worker: up while it takes the notice and runs Platform::drained.
    std::atomic<bool> drainedBusy_{};
    // Looks at the head before the worker sleeps (about 50 microseconds), and
    // at the park state before the game thread raises the worker's priority
    // and sleeps (workerUrgent). Longer looks were measured slower in the
    // emulator (Alpha 10, three zones, 63.3 -> 69.0 ms a picture): a game
    // thread that looked 15 times longer before it slept (w19), with or
    // without a worker that never slept while the painter paints (w18), left
    // the worker fewer shapes, and in Death Mountain the worker rested.
    static constexpr unsigned spinRounds=4000,parkSpinRounds=2000;
    static constexpr std::uint64_t stuckMicroseconds=20'000'000;
    Counters counters_{};
};
// The one queue of the process (off until game_startup.cpp starts it).
Queue& queue();
inline void sync(SyncCause cause=SyncCause::other){queue().sync(cause);}

}
