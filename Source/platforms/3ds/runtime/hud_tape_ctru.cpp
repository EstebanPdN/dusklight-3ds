#include "gx_worker_sync.hpp"
#include "hud_tape.hpp"
#include "gpu_canary.hpp"
#include "gpu_sync.hpp"
#include "gx_compact_cache.hpp"
#include "gx_framebuffer.hpp"
#include "gx_profile.hpp"
#include "gx_state_serial.hpp"
#include "gx_stream.hpp"
#include "gx_target_extent.hpp"
#include "tape_context.hpp"
#include "../gx_geometry_pica.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <dolphin/os.h>
#include <atomic>
#include <cstring>
#include <new>

namespace dusk::native {
namespace {
// Words kept from one pass. The in-game HUD is about 20,000; file select and
// the collection screens are several times that and stay out.
constexpr u32 storeWords=40*1024;
// Command words left for the rest of the frame after a replay
// (platformPrepareGpuDraw keeps 8192 before every draw).
constexpr u32 reserveWords=16*1024;

struct Tape {
    u32* words{};
    u32 count{};
    bool valid{};
    std::uint64_t picture{};           // the picture it was recorded in
    std::uint32_t storageSerial{};     // gxTextureStorageSerial at the recording
    std::uint32_t indexEpoch{},cacheEpoch{};
    gx::TargetExtent extent{};
    FramebufferHandle target{};
} tape;
unsigned picturesPerRecording=2;
std::uint64_t picture{};
// The recording in progress.
struct Recording {
    bool active{};
    std::uint64_t picture{};           // the picture it records
    u32* buffer{};u32 offset{};
    std::uint32_t spanSerial{},copySerial{},storageSerial{};
    FramebufferHandle target{};
} recording;
HudTapeStatistics stats{};
// Alpha 15 (gx-overlap.txt 2): the HUD pass while the painter's draws go to
// the second-core worker's queue (runs deferred through the 2D passes). The
// tape, the recording and `stats` are then the drawing side's state, moved
// with the queue like the copy of the register bank: the replay and both ends
// of a recording are jobs that the thread that draws runs in their place
// (runHudTapeJob). The game thread decides from its own picture count and
// from what the drawing side published when a tape became valid or was
// dropped; it never reads `tape` or `recording` without a sync.
std::atomic<bool> publishedValid{};
std::atomic<std::uint32_t> publishedPicture{};
// Game side: a recording was queued and its end is still to be queued.
bool queuedRecording{};
std::uint64_t queuedReplays{},queuedRecordings{};
// The words of the kept pass: allocated once on the game thread; a queued
// recording carries the pointer in its job.
u32* store{};
u32* gameStore(){if(!store)store=new(std::nothrow)u32[storeWords];return store;}
struct TapeJob {std::uint32_t kind{};std::uint32_t picture{};u32* words{};};
enum : std::uint32_t {jobReplay=1,jobRecordBegin,jobRecordEnd};

bool contextUsable(){return tapeContextUsable();}
void markContextChanged(){tapeMarkContextChanged();}
std::uint32_t cacheEpoch(){const auto* cache=gx::staticCompactCache();return cache?cache->epoch:0;}
// Drawing side from here to runHudTapeJob.
void setValid(bool valid){
    tape.valid=valid;
    if(valid)publishedPicture.store(std::uint32_t(tape.picture),std::memory_order_relaxed);
    publishedValid.store(valid,std::memory_order_release);
}
bool sameSurroundings(){
    return tape.storageSerial==gxTextureStorageSerial&&tape.indexEpoch==residentIndexEpoch()&&tape.cacheEpoch==cacheEpoch()&&
        tape.target==activeFramebuffer()&&!std::memcmp(&tape.extent,&gx::currentTargetExtent,sizeof(tape.extent));
}
void drop(){if(tape.valid){setValid(false);++stats.dropped;}}
// The words of the tape into the frame's command list. False: no room.
bool runTape(){
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    if(!buffer||size<offset||size-offset<tape.count+reserveWords)return false;
    GPUCMD_AddRawCommands(tape.words,tape.count);
    gpuCanaryHudTapeList();
    markContextChanged();
    // Other draws ran on the PICA: shape replays must not continue across it.
    ++gxPicaStateSerial;
    ++stats.replayed;
    return true;
}
// 2: the tape ran. 0: it cannot run here (dropped when it never can again).
int replay(){
    if(!sameSurroundings()){drop();return 0;}
    auto* stream=gx::commandStream();
    if(!stream||!stream->finish())return 0;
    return runTape()?2:0;
}
// A recording of the pass starts at the command list's current place.
bool startRecording(std::uint64_t forPicture){
    if(!beginGxTapeGeometry())return false;
    setValid(false);
    platformHoldGpuChunks(true);
    markContextChanged();
    recording.active=true;recording.picture=forPicture;
    u32 size{};GPUCMD_GetBuffer(&recording.buffer,&size,&recording.offset);
    recording.spanSerial=platformGpuSpanSerial();
    recording.copySerial=gxCopySerial;recording.storageSerial=gxTextureStorageSerial;recording.target=activeFramebuffer();
    return true;
}
// After the pass's last draw (flushed: the game thread's command stream had
// nothing pending; a job always comes after every draw issued before it).
void finishRecording(bool flushed){
    recording.active=false;
    // The list that carries this pass, kept or not: its state was sent whole.
    gpuCanaryHudTapeList();
    platformHoldGpuChunks(false);
    const bool geometry=endGxTapeGeometry();
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    // The words of this pass are one unbroken run of the frame's span.
    if(!flushed||buffer!=recording.buffer||offset<=recording.offset||platformGpuSpanSerial()!=recording.spanSerial){++stats.refusedSpan;return;}
    // A texture retired or re-housed, a copy or a target change in the pass.
    if(gxCopySerial!=recording.copySerial||gxTextureStorageSerial!=recording.storageSerial||activeFramebuffer()!=recording.target){++stats.refusedState;return;}
    if(!geometry){++stats.refusedGeometry;return;}
    const u32 count=offset-recording.offset;
    if(count>stats.largestWords)stats.largestWords=count;
    if(count>storeWords){++stats.refusedSize;return;}
    std::memcpy(tape.words,buffer+recording.offset,count*sizeof(u32));
    tape.count=count;tape.picture=recording.picture;tape.storageSerial=gxTextureStorageSerial;
    tape.indexEpoch=residentIndexEpoch();tape.cacheEpoch=cacheEpoch();
    tape.extent=gx::currentTargetExtent;tape.target=activeFramebuffer();
    setValid(true);++stats.recorded;
}
// Game side of the queued pass (duskHudTapeBegin while runs are deferred):
// 2 the replay is queued (skip the pass), 1 a recording is queued (draw the
// pass, then duskHudTapeEnd), 0 draw the pass (its draws are queued).
int queuedBegin(){
    ++picture;
    if(picturesPerRecording<2||gpuCanaryHudTapeRefused())return 0;
    if(publishedValid.load(std::memory_order_acquire)&&
       std::uint32_t(std::uint32_t(picture)-publishedPicture.load(std::memory_order_relaxed))<picturesPerRecording){
        const TapeJob job{jobReplay,std::uint32_t(picture),nullptr};
        if(!queueGxJob(gxJobHudTape,&job,sizeof(job)))return 0;
        ++queuedReplays;return 2;
    }
    const TapeJob job{jobRecordBegin,std::uint32_t(picture),gameStore()};
    if(!job.words||!queueGxJob(gxJobHudTape,&job,sizeof(job)))return 0;
    queuedRecording=true;++queuedRecordings;
    return 1;
}
}
void setHudTapePictures(unsigned pictures){
    picturesPerRecording=pictures<1?1:pictures>8?8:pictures;
    if(picturesPerRecording==1)setValid(false);
}
unsigned hudTapePictures(){return picturesPerRecording;}
void dropHudTape(){drop();}
HudTapeStatistics hudTapeStatistics(){auto out=stats;out.words=tape.valid?tape.count:0;out.queuedReplays=queuedReplays;out.queuedRecordings=queuedRecordings;return out;}
void reportHudTape(){
    static std::uint64_t reported=~std::uint64_t(0);
    const std::uint64_t sum=stats.recorded+stats.replayed+stats.refusedSpan+stats.refusedState+stats.refusedGeometry+stats.refusedSize+stats.dropped+
        stats.queuedReplayFailures+queuedReplays+queuedRecordings;
    if(sum==reported)return;
    reported=sum;
    OSReport("Native HUD tape (%u pictures per recording): recorded=%llu replayed=%llu refused span=%llu state=%llu geometry=%llu size=%llu dropped=%llu words=%lu largest=%lu; "
        "queued (overlap) replays=%llu recordings=%llu replays refused on the drawing side=%llu\n",
        picturesPerRecording,stats.recorded,stats.replayed,stats.refusedSpan,stats.refusedState,stats.refusedGeometry,stats.refusedSize,stats.dropped,
        static_cast<unsigned long>(tape.valid?tape.count:0),static_cast<unsigned long>(stats.largestWords),
        static_cast<unsigned long long>(queuedReplays),static_cast<unsigned long long>(queuedRecordings),
        static_cast<unsigned long long>(stats.queuedReplayFailures));
}
// gxJobHudTape (gx_worker_sync.hpp): run by the thread that draws, in the
// job's place in the queue (every draw issued before it is drawn).
void runHudTapeJob(const void* arguments,std::size_t bytes){
    TapeJob job;
    if(bytes!=sizeof(job))return;
    std::memcpy(&job,arguments,sizeof(job));
    if(job.kind==jobReplay){
        // The game thread skipped the pass: the tape is this picture's HUD.
        // Refused here (the surroundings changed after the game thread
        // looked, or no room): this picture has no HUD, the tape is dropped
        // and the next picture records. Counted in the report.
        if(!tape.valid||!contextUsable()||!sameSurroundings()){drop();++stats.queuedReplayFailures;return;}
        if(!runTape())++stats.queuedReplayFailures;
    }else if(job.kind==jobRecordBegin){
        if(recording.active||!contextUsable())return;
        if(!tape.words)tape.words=job.words;
        if(tape.words)(void)startRecording(job.picture);
    }else if(job.kind==jobRecordEnd){
        if(recording.active)finishRecording(true);
    }
}
}

namespace dusk::native {
namespace {
// The pass with the GPU on this thread (the shared-state audit takes these
// two as barriers: syncGxWorker() is their first statement).
[[gnu::noinline]] int beginSynced(){
    // The pass starts behind everything queued for the second-core worker,
    // and ends (below) with every shape issued inside it drawn: a recording
    // holds the whole pass (gx_worker_sync.hpp).
    syncGxWorker();
    ++picture;
    if(picturesPerRecording<2||recording.active||platformGpuOwner()!=OSGetCurrentThread()||gpuCanaryHudTapeRefused()||!contextUsable()||
       nativeRunsDeferred())return 0;
    if(tape.valid&&picture-tape.picture<picturesPerRecording){
        if(const int ran=replay())return ran;
    }
    // Record this pass.
    auto* stream=gx::commandStream();
    if(!stream||!stream->finish())return 0;
    if(!tape.words&&!(tape.words=gameStore()))return 0;
    return startRecording(picture)?1:0;
}
[[gnu::noinline]] void endSynced(){
    syncGxWorker();
    if(!recording.active)return;
    auto* stream=gx::commandStream();
    finishRecording(stream&&stream->finish());
}
}
}
using namespace dusk::native;
extern "C" int duskHudTapeBegin(void){
    // Alpha 15: the painter's draws go on to the worker's queue through the
    // 2D passes (gx-overlap.txt 2): the replay and the recording are jobs.
    if(nativeRunsDeferred()&&!queuedRecording)return queuedBegin();
    return beginSynced();
}
extern "C" void duskHudTapeEnd(void){
    if(queuedRecording){
        queuedRecording=false;
        const TapeJob job{jobRecordEnd,std::uint32_t(picture),nullptr};
        if(queueGxJob(gxJobHudTape,&job,sizeof(job)))return;
        // Not queued: after the sync in endSynced this thread is the
        // drawing side and ends the recording itself.
    }
    endSynced();
}
