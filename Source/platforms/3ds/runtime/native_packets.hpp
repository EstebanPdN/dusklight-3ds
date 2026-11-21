#pragma once
// Native packets: the register work of J3D material packets leaves the game
// thread (Alpha 12, stage 1: room models).
//
// A J3D material packet (J3DMatPacket::draw) loads its textures, calls its
// material display list and, for each shape, its differential list (lights,
// fog, TEV colours: rewritten every picture), its vertex setup, the current
// texture matrix indices, the matrix arrays and two indexed matrix loads,
// then queues the shape for the second-core worker with the bank words it
// changed (gx_worker.hpp). All of that is register work on the game thread's
// command processor: parsing lists, comparing, hashing and logging every word
// (Alpha 11 console profile: j3d.matPacket 13.6 ms a picture, 61 packets).
//
// With a packet captured (begin/end around the original J3D code, nothing of
// it changed) the command stream does not run those operations: it writes
// them, in their order, into one record (gx_stream.hpp PacketCapture: display
// lists by reference, matrices read at capture). The record goes to the queue
// as one entry. The worker runs it on its copy of the bank with the same
// processor functions the game thread would have used, and draws each shape
// at its place in the sequence: every shape is drawn under exactly the bank
// and bindings the original path would have queued it with.
//
// The game thread keeps the records of consecutive shipped packets (a run)
// and, the first time anything else uses the stream, builds from them an
// overlay (the last value of each register they wrote, the TEV colour
// latches, the matrix-index family written last, the array bindings) and
// writes that into its bank once (seal): the bank then holds the values the
// original path would have left, word for word. Its epochs move past every value the worker's copy can
// have reached (epochs are compared for equality only, so a higher value is
// safe; a lower one could make a cache believe nothing changed).
//
// Anything a capture cannot express ends it: the operations captured so far
// run on the game thread's processor in their order, exactly as the original
// path would have run them (the shapes are queued or drawn as
// duskNativeDrawShape does), and the packet goes on uncaptured. So does a
// packet whose lists hold anything but register loads, or that the queue
// cannot take whole. A fallback costs time, never a different picture.
//
// While a packet is captured the bank lags behind it. Reading registers() is
// allowed and counted (PacketCapture::staleReads): in the J3D path only the
// vertex setup memo does it (gx_command_api.cpp duskNativeCallVertexSetup:
// "the same list bytes as the last one run, and the setup epoch unchanged").
// Every captured operation that can change the vertex setup (a list, an
// array binding, a CP write, XF 0x1008, a written CP/XF command) moves the
// game bank's setup epoch, so the memo never skips a list the original path
// would run (it then runs a few the original path skips: running a list
// equal to what the registers hold changes no word). The epoch only moves
// up, which is all its users compare.
//
// Console (what Azahar does not show):
// - CPU caches and the GPU: nothing here is read by the GPU. The records,
//   the overlay and the mirror processor are CPU memory; every PICA command
//   is still made by the unchanged draw code (drawQueuedShape) from the bank
//   it is given, so no new buffer needs a flush and no new command reaches
//   the PICA (the PICA output rule and the shaders are untouched).
// - Cross-core order: a record's words, and everything it points at (display
//   lists written by the game before the painter, the matrix bytes copied
//   into it), are written before Queue::publish's store of the head and read
//   after the consumer's load of it (both sequentially consistent: release
//   and acquire on the ARM11's cores, whose data caches the SCU keeps
//   coherent). The mirror processor has one owner at a time, handed over by
//   the queue's park handshake (acquire/release). The two hooks
//   (CommandStream::nativeSettle, Queue::captureHook) are written once,
//   before the worker thread is created, and only read afterwards. The
//   display lists a record points at stay as they are until the painter's
//   final sync: material lists of room models change only when patched
//   outside the painter, differential lists are double-buffered by
//   J3DDisplayListObj (the next diff() writes the other buffer), vertex
//   setup lists are built at load.
//
// SD switch native-packets.txt (read once, when the first room is created):
// `0` off, `b` room models, `a` every rigid material packet (default), `d`
// direct records (default since Alpha 14), `c` captured records instead of
// direct ones, `f` also where the worker shares the game thread's core (see
// noteWorkerCore), `v` (with `b` or `a`, or alone for all rigid packets) verify:
// each captured packet is also replayed on scratch processors and then runs
// on the original path; the periodic log line counts shapes whose bank
// differs from the worker's replay (mirror) and packets whose final bank
// differs from the seal's. Both must stay zero.
//
// Alpha 14: the seal. Alpha 12 folded every shipped packet into the overlay
// as it ended (a checksum and a parse of every list, every register word
// stored): as much work on the game thread as running the lists. Now a
// shipped packet's record stays where it was captured, its lists are only
// compared with their templates (the bytes that decide the parse: see
// native_packets.cpp "list templates"), and the bank is built once for the
// whole run when something needs it (the seal), backwards: the last write of
// each register decides it, a list whose registers are all decided is
// skipped. The values are read from the lists then, so the painter's end
// seals what is still pending (game_startup.cpp duskNativePainterDone)
// before actors can rewrite them.
#include "gx_stream.hpp"
#include "gx_worker.hpp"
#include <array>
#include <cstdint>

namespace dusk::native::packets {
enum class Mode:std::uint8_t {off,bg,all};
// What the draw adapter (gx_command_draw.cpp) provides.
struct Hooks {
    // Shapes would be queued right now (painter open, queue accepting, a
    // draw context, the queue's native records available).
    bool (*canQueue)(){};
    // duskNativeDrawShape's own path for one shape (queue it, or draw it
    // from its records). False: the caller runs the shape's display list.
    bool (*drawShape)(std::uint32_t generation,const void* list,std::uint32_t size){};
    unsigned (*casterSlot)(){};
    void (*setCasterSlot)(unsigned){};
    void (*staticGeometry)(std::uint32_t generation,const void* list){};
    std::uint64_t (*now)(){};          // a clock in ticks (optional)
    std::uint64_t ticksPerMillisecond{1};
    void (*report)(const char* line){}; // the log (optional)
};
// Game thread, once, before the first picture (gx_command_draw.cpp).
bool install(const Hooks& hooks);
// The GX worker, once it runs (gx_worker_ctru.cpp): whether it has a core of
// its own. Shipping a packet moves its register work to the worker and adds a
// check and a seal on the game thread: a gain only where the two threads run
// at the same time. The built-in Azahar runs every application thread on
// core 0 (its log: "Native GX worker: running on core 0"), the console runs
// the worker on core 2. Packets are shipped only on a core of its own (or
// with the SD switch letter `f`); true until the worker says otherwise.
void noteWorkerCore(bool separate);
// The mode, from the SD switch or a test. Outside a picture.
void configure(Mode mode,bool verify,bool direct=false);
void configureFromSd(); // native-packets.txt, once (d_a_bg.cpp)
Mode mode();
bool verifying();

// ---- J3D (J3DMatPacket::draw) ----
// True: the packet is captured until end(). `kind` (J3DPacket.cpp): 0 the
// packet is not rigid (its model's vertex arrays are rewritten by the CPU, a
// shape without matrices draws its list directly: the capture would end at
// once), 1 a room model's packet (d_a_bg.cpp flags them), 2 another rigid
// packet. Room mode takes kind 1, all mode kinds 1 and 2.
enum PacketKind:int {notRigid=0,roomModel=1,rigid=2};
bool begin(int kind);
void end();
bool capturing();
// GPU work or a queued job is about to be issued on the game thread: a
// capture in progress ends first (its shapes go to the queue the original
// way, before the work, as they were issued before it).
void endCapture();
// duskNativeDrawShape while capturing: the shape is part of the record.
// False: the capture ended here, draw the shape the usual way.
bool captureShape(std::uint32_t generation,const void* list,std::uint32_t size,unsigned caster);

// ---- direct records (J3DPacket.cpp, J3DShape.cpp; SD switch letter `d`) ----
// On every picture J3D builds the record of an eligible packet itself (base
// matrix shapes, matrix pipeline 0, original vertex arrays): the material
// still loads its textures through GX (captured), then the material list,
// vertex setup lists, differential lists and shape lists are entered by
// reference, the current matrix indices and the two matrix arrays as the
// stream would capture them, and the draw matrices straight from the
// model's buffers. No GX call and no command stream entry per shape. The
// record has the capture's exact format; verify mode builds it, puts the J3D
// state back, runs the original code captured and compares both records
// word for word and both J3D states (Statistics::direct*).
bool directEnabled();
bool directVerifying();
bool directBegin(int kind);          // as begin(); counts a try
bool directOpen();                   // the capture is still open (the material load did not end it)
bool directList(const void* list,std::uint32_t size);
bool directCurrentMatrix(std::uint32_t a,std::uint32_t b);
bool directBind(unsigned slot,const void* data,std::uint32_t size,std::uint32_t stride);
bool directIndexed(unsigned code,unsigned index,unsigned packed);
bool directShape(std::uint32_t generation,const void* list,std::uint32_t size);
void directEnd(std::uint32_t stateHash); // shipped (or, verify, kept for the comparison)
void directAbort(const char* why);       // dropped: nothing of it ran; J3D runs the original code
void directFallback(const char* why);    // not tried (counted)
void directRealState(std::uint32_t stateHash); // verify: the J3D state the original code left

// ---- consumer (Adapter::native) ----
using DrawShape=void(*)(const gxworker::QueuedShape&,const gx::CommandRegisters&,const gx::CommandProcessor::ArrayBindings&,
                        const gx::TargetExtent&);
// Runs one record on `processor` (the worker's copy of the bank); `failures`
// counts operations the processor refused (the game thread had taken them:
// must stay zero).
void replay(const std::uint32_t* body,std::size_t words,gx::CommandProcessor& processor,const gx::TargetExtent& extent,
            DrawShape draw,std::uint64_t* failures);

// ---- diagnostics ----
struct Statistics {
    std::uint64_t pictures{},captured{},packets{},shapes{},recordWords{},runs{},sealedWords{},sealTicks{},foldTicks{};
    std::uint64_t notEligible{},queueRefused{},streamRefused{},storageRefused{};
    std::uint64_t ended{},foldRefused{},staleReads{},workerFailures{};
    std::uint64_t verifiedPackets{},verifiedShapes{},mirrorMismatches{},sealMismatches{};
    std::uint64_t listsCached{},listsParsed{},listsChanging{}; // list templates: used as they were, built, built again (the bytes moved)
    // Direct records: tried, built, fallbacks (by cause in `causes`); verify:
    // compared with the capture, records that differ, J3D states that differ,
    // comparisons the capture did not complete.
    std::uint64_t directTried{},directRecords{},directFallbacks{};
    std::uint64_t directCompared{},directMismatches{},directStateMismatches{},directIncomplete{};
    char firstDirect[40]{};
    const char* firstEnd{};const char* firstFold{};
    // Packets kept on the original path by cause (static strings: ended
    // early, fold refused), counted since the last report.
    struct Cause {const char* why{};std::uint64_t count{};};
    std::array<Cause,12> causes{};
    char firstMismatch[64]{};
};
const Statistics& statistics();
// The periodic report (gx_worker_ctru.cpp reportGxWorker, with the pictures
// counted since its last report): a few log lines, each under the log's 255
// characters. In verify mode nothing is shipped by design: the captured
// packets are counted as verified.
void report(unsigned pictures);
}
