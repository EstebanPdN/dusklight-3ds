#include "gx_worker_sync.hpp"
#include "screen_tape.hpp"
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
#include "../gx_pica.hpp"
#include "../gx_texture_pica.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <dolphin/os.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <new>
#include <span>

namespace dusk::native {
namespace {
// Kept recordings. One owner (a screen, or a child subtree of a split pane)
// keeps at most two: a blinking cursor alternates between two fingerprints.
// A third state is recorded over the older of the two, in the same command
// words and vertex bytes when it fits, so a pane that changes in every
// picture does not fill the store. A split file select holds about 100.
constexpr unsigned tapeSlots=256,tapesPerOwner=2;
// Command words of every kept recording (file select: about 70,000 in five
// screens) and the bank words they changed. Both are filled front to back and
// start again together with the geometry buffer (recycle).
constexpr u32 poolWords=224*1024;
constexpr unsigned bankPoolWords=24*1024,bankWordsPerTape=1536;
// Command words left for the rest of the frame after a replay
// (platformPrepareGpuDraw keeps 8192 before every draw).
constexpr u32 reserveWords=16*1024;
// An owner recorded this many times in a row without one replay changes in
// every picture: the caller splits it or lets it rest (duskScreenTapeEnd).
constexpr unsigned missesBeforeRest=4,restDraws=24;

struct Tape {
    const void* owner{};
    std::uint32_t a{},b{};              // fingerprint
    u32 first{},count{},capacity{};     // command words in the pool
    u32 bankFirst{},bankCount{},bankCapacity{}; // changed bank words in the bank pool
    std::size_t geometryAt{},geometryCapacity{}; // its vertices in the geometry buffer
    bool valid{},volatileScreen{};
    std::uint32_t storageSerial{},indexEpoch{},cacheEpoch{};
    gx::TargetExtent extent{};
    FramebufferHandle target{};
    std::uint64_t used{};               // order of use (least recently used gives way)
    std::uint32_t usedFrame{};          // the frame it was last replayed or recorded in
};
// The slots live in store memory too (allocated with the pools).
std::span<Tape> tapes{};
u32* words{};u32 wordsUsed{};
gx::BankWord* bankWords{};unsigned bankUsed{};
// The bank before the recording in progress and the list of what it changed
// (store memory, not static: the executable's static size stays as it is,
// see main.cpp).
gx::CommandRegisters* bankBefore{};
gx::BankWord* bankChanged{};
bool storageFailed{},recyclePending{};
ScreenTapeMode mode=ScreenTapeMode::on;
std::uint64_t useClock{};
std::uint32_t frameSerial{1};
// Owners that keep changing (see missesBeforeRest).
struct Heat {const void* owner{};std::uint8_t misses{},rest{},level{};};
std::array<Heat,512> heats{};
// The recording in progress.
struct Recording {
    bool active{},volatileTexture{};
    const void* owner{};std::uint32_t a{},b{};
    u32* buffer{};u32 offset{};
    std::uint32_t spanSerial{},copySerial{},storageSerial{};
    FramebufferHandle target{};
    Tape* slot{};                       // the slot this recording goes to
    bool inPlace{};                     // its vertices go over the slot's old ones
    const Tape* verify{};               // verify mode: the recording this draw is compared with
} recording;
ScreenTapeStatistics stats{};
// Screens drawn as plain runs while the painter's draws were queued for the
// second-core worker (duskScreenTapeBegin): the game thread's count.
std::uint64_t plainWhileQueued{};
bool queuedPlain{}; // setScreenTapeQueuedPlain (gx-overlap.txt 1 or 2)
// Alpha 15, while the painter's draws go to the second-core worker's queue
// (gx-overlap.txt 2): a screen with a valid recording is a queued replay. The
// game thread decides from its slot table (which only it writes: recordings
// stay on this thread, after a sync, with the queue's runs suspended for that
// screen) and applies the recording's bank words to its own bank; the thread
// that draws appends the command words in the job's place (runScreenTapeJob)
// after checking the epochs it owns. A refused job leaves that screen out of
// one picture; the game thread then drops every recording (re-recorded).
struct ScreenJob {const u32* words{};u32 first{},count{};std::uint32_t storageSerial{},indexEpoch{},cacheEpoch{};};
std::atomic<std::uint32_t> jobRefusals{},jobReplays{};
std::uint32_t seenRefusals{};
std::uint64_t queuedReplays{},suspendedRecordings{};
// tapeContextUsable() answered true on a synced begin (its first answer reads
// Citro3D's context, which belongs to the thread that draws).
bool contextKnownUsable{};
// A recording made while the queue's runs were suspended (duskScreenTapeEnd
// resumes them).
bool screenSuspended{};
// Images the game can rewrite whose content is in a fingerprint taken in this
// picture (screenTapeTextureSum).
struct MeasuredImage {gx::TextureHandle handle;std::uint32_t picture;};
std::array<MeasuredImage,96> measuredImages{};

std::uint32_t cacheEpoch(){const auto* cache=gx::staticCompactCache();return cache?cache->epoch:0;}
bool sameSurroundings(const Tape& t){
    return t.storageSerial==gxTextureStorageSerial&&t.indexEpoch==residentIndexEpoch()&&t.cacheEpoch==cacheEpoch()&&
        t.target==activeFramebuffer()&&!std::memcmp(&t.extent,&gx::currentTargetExtent,sizeof(t.extent));
}
bool storage(){
    if(words&&bankWords&&bankBefore&&bankChanged&&!tapes.empty())return true;
    if(storageFailed)return false;
    if(tapes.empty())if(auto* slots=new(std::nothrow)Tape[tapeSlots])tapes={slots,tapeSlots};
    if(!words)words=new(std::nothrow)u32[poolWords];
    if(!bankWords)bankWords=new(std::nothrow)gx::BankWord[bankPoolWords];
    if(!bankBefore)bankBefore=new(std::nothrow)gx::CommandRegisters;
    if(!bankChanged)bankChanged=new(std::nothrow)gx::BankWord[bankWordsPerTape];
    if(!words||!bankWords||!bankBefore||!bankChanged||tapes.empty()){
        storageFailed=true;
        OSReport("Native retained screens: no memory for the store, screens are drawn every picture\n");
        return false;
    }
    return true;
}
Heat& heatOf(const void* owner){
    const auto at=reinterpret_cast<std::uintptr_t>(owner);
    auto& h=heats[((at>>4)^(at>>11))%heats.size()];
    if(h.owner!=owner)h={owner,0,0,0};
    return h;
}
Tape* find(const void* owner,std::uint32_t a,std::uint32_t b){
    for(auto& t:tapes)if(t.owner==owner&&t.a==a&&t.b==b)return &t;
    return nullptr;
}
// The slot a new recording of `owner` takes: its own older recording once it
// has tapesPerOwner (never one used in this frame: its commands are queued),
// else a free slot, else the least recently used one of all. Null: none now.
Tape* claim(const void* owner){
    Tape* own=nullptr;unsigned owned=0;
    Tape* free=nullptr;Tape* oldest=nullptr;
    for(auto& t:tapes){
        if(!t.owner){if(!free)free=&t;continue;}
        if(t.owner==owner){
            ++owned;
            if(t.usedFrame!=frameSerial&&(!own||t.used<own->used))own=&t;
        }else if(t.usedFrame!=frameSerial&&(!oldest||t.used<oldest->used))oldest=&t;
    }
    if(owned>=tapesPerOwner)return own;
    return free?free:oldest;
}
// 2: the recording ran. 0: it cannot run here.
int replay(Tape& t){
    auto* stream=gx::commandStream();
    if(!stream||!stream->finish())return 0;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    if(!buffer||size<offset||size-offset<t.count+reserveWords)return 0;
    if(!stream->applyChanges({bankWords+t.bankFirst,t.bankCount}))return 0;
    if(t.count){
        GPUCMD_AddRawCommands(words+t.first,t.count);
        gpuCanaryHudTapeList();
        tapeMarkContextChanged();
        // Other draws ran on the PICA: shape replays must not continue across it.
        ++gxPicaStateSerial;
    }
    t.used=++useClock;t.usedFrame=frameSerial;
    ++stats.replayed;stats.bankWords+=t.bankCount;
    return 2;
}
// Verify mode: state a recording sends whole but no pane sets (uniform rows of
// other draws, unused combiner stages) starts equal in every recording, so
// that two recordings of an unchanged screen are equal word for word. The
// renderer's own copies are dropped with it. (Texture units stay: Citro3D
// reads the texture of units 1 and 2 when it binds them, so they cannot be
// emptied; a unit no pane uses keeps what the 3D scene bound there.)
void normalizeContext(){
    std::memset(C3D_FVUnif[GPU_VERTEX_SHADER],0,sizeof(C3D_FVUnif[GPU_VERTEX_SHADER]));
    for(int stage=0;stage<6;++stage)C3D_TexEnvInit(C3D_GetTexEnv(stage));
    // One texture of its own on the three units (Citro3D reads the texture it
    // binds, so a unit cannot be emptied): a unit no pane uses then sends the
    // same words in every recording.
    static C3D_Tex blank;static bool made;
    if(!made&&C3D_TexInit(&blank,8,8,GPU_RGBA8)){std::memset(blank.data,0,blank.size);made=true;}
    if(made)for(int unit=0;unit<3;++unit)C3D_TexBind(unit,&blank);
    invalidatePicaCommandState();
}
// Verify mode: the PICA register a word of a command run belongs to, and for
// float uniform data the uniform row (-1 otherwise). A command is a parameter,
// a header (register, byte mask, count of further parameters, consecutive
// flag) and those parameters, padded to an even count of words.
struct WordPlace {std::uint32_t reg{};std::int32_t row{-1};bool header{};std::uint32_t key{};};
WordPlace describeWord(const u32* run,u32 count,u32 where){
    WordPlace place;
    std::uint32_t uniformRow=0,uniformWords=0;bool wide=false;
    for(u32 i=0;i+1<count;){
        const u32 header=run[i+1];
        const std::uint32_t reg=header&0xffff,extra=(header>>20)&0xff;const bool consecutive=header>>31;
        const u32 span=(2+extra+1)&~1u;
        if(reg==0x2c0){uniformRow=run[i]&0xff;wide=run[i]>>31;uniformWords=0;}
        const bool data=reg>=0x2c1&&reg<=0x2c8&&!consecutive;
        if(where>=i&&where<i+span){
            const u32 at=where-i;
            place.header=at==1;
            const u32 parameter=at==0?0:at-1; // 0: the first, then the ones after the header
            place.reg=reg+(consecutive&&at>1?parameter:0);
            if(data&&at!=1)place.row=std::int32_t(uniformRow+(uniformWords+parameter)/(wide?4u:3u));
            break;
        }
        if(data){uniformWords+=1+extra;}
        i+=span;
    }
    place.key=(place.reg<<16)^std::uint32_t(place.row&0xffff)^(place.header?0x80000000u:0);
    return place;
}
// Two recordings of one screen differ only in where their vertices are: every
// other word must be equal, and those differ by the distance of the two
// places in the geometry buffer.
bool sameWords(const u32* kept,const u32* now,u32 count,std::size_t keptAt,std::size_t nowAt,u32& where){
    const u32 delta=u32(nowAt-keptAt);
    for(u32 i=0;i<count;++i)if(kept[i]!=now[i]&&now[i]-kept[i]!=delta){where=i;return false;}
    return true;
}
}
bool screenTapeTextureSum(const gx::Texture& texture,std::uint64_t& sum){
    // The sum reads the texture cache's record of the image, which belongs to
    // the thread that draws (gx_worker_sync.hpp).
    syncGxWorker();
    // Hashed again for every pane that shows it: the game may write between
    // two screens of one picture.
    if(!gxTextureSourceSum(texture,sum))return false;
    MeasuredImage* place=nullptr;
    for(auto& image:measuredImages){
        if(image.picture==frameSerial&&image.handle==texture.handle)return true;
        if(!place&&image.picture!=frameSerial)place=&image;
    }
    if(!place)return false;
    *place={texture.handle,frameSerial};return true;
}
void setScreenTapeMode(ScreenTapeMode value){
    mode=value;
    if(mode==ScreenTapeMode::off)for(auto& t:tapes)t.valid=false;
}
ScreenTapeMode screenTapeMode(){return mode;}
ScreenTapeStatistics screenTapeStatistics(){
    auto out=stats;
    for(const auto& t:tapes)if(t.valid){++out.tapes;out.words+=t.count;}
    out.geometryBytes=std::uint32_t(gxScreenGeometryUsed());
    return out;
}
void reportScreenTape(){
    static std::uint64_t reported=~std::uint64_t(0);
    const auto s=screenTapeStatistics();
    const std::uint64_t sum=s.begins+s.replayed+s.recorded+s.dropped+s.recycled+s.verified+s.verifyMismatches+s.notes[duskScreenNoteChildren]+plainWhileQueued+queuedReplays;
    if(sum==reported)return;
    reported=sum;
    OSReport("Native retained screens (%s): draws=%llu replayed=%llu recorded=%llu in-place=%llu plain=%llu volatile=%llu refused span=%llu "
             "state=%llu geometry=%llu size=%llu dropped=%llu recycled=%llu\n",
        mode==ScreenTapeMode::off?"off":mode==ScreenTapeMode::verify?"verify":"on",s.begins,s.replayed,s.recorded,s.inPlace,s.plain,
        s.volatileScreens,s.refusedSpan,s.refusedState,s.refusedGeometry,s.refusedSize,s.dropped,s.recycled);
    OSReport("Native retained screens store: kept=%lu words=%lu (pool %lu of %lu) largest=%lu geometry=%lu of %lu bytes bank words replayed=%llu "
             "verified=%llu mismatches=%llu\n",
        static_cast<unsigned long>(s.tapes),static_cast<unsigned long>(s.words),static_cast<unsigned long>(wordsUsed),
        static_cast<unsigned long>(poolWords),static_cast<unsigned long>(s.largestWords),static_cast<unsigned long>(s.geometryBytes),
        static_cast<unsigned long>(gxScreenGeometryCapacity()),s.bankWords,s.verified,s.verifyMismatches);
    if(plainWhileQueued||queuedReplays||suspendedRecordings)
        OSReport("Native retained screens while runs were queued (gx-overlap): replays queued %llu (drawn %lu, refused on the drawing side %lu), "
            "recorded after a sync %llu, drawn as queued runs %llu\n",
            static_cast<unsigned long long>(queuedReplays),static_cast<unsigned long>(jobReplays.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(jobRefusals.load(std::memory_order_relaxed)),static_cast<unsigned long long>(suspendedRecordings),
            static_cast<unsigned long long>(plainWhileQueued));
    OSReport("Native retained screens panes: split screens=%llu children=%llu in runs=%llu changed lately=%llu not recordable=%llu "
             "base changes=%llu left out=%llu\n",
        s.notes[duskScreenNoteSplitScreens],s.notes[duskScreenNoteChildren],s.notes[duskScreenNoteInRun],s.notes[duskScreenNoteUnsettled],
        s.notes[duskScreenNoteUnrecordable],s.notes[duskScreenNoteBaseChanged],s.notes[duskScreenNoteOverflow]);
}
void setScreenTapeQueuedPlain(bool on){queuedPlain=on;}
void beginScreenTapeFrame(){
    ++frameSerial;
    if(!recyclePending||recording.active)return;
    recyclePending=false;
    for(auto& t:tapes)t={};
    wordsUsed=0;bankUsed=0;
    recycleGxScreenGeometry();
    // A HUD tape of a pass that replayed screens points at these vertices.
    dropHudTape();
    ++stats.recycled;
}
}

using namespace dusk::native;
extern "C" unsigned duskScreenTapePicture(void){return frameSerial;}
extern "C" void duskScreenTapeNote(unsigned which){if(which<duskScreenNoteCount)++stats.notes[which];}
namespace dusk::native {
namespace {
int beginSynced(const void* screen,unsigned long fingerprintA,unsigned long fingerprintB,int recordable);
int endSynced();
// Game side of a queued replay: the job first (it comes after every draw
// issued so far), then the recording's bank words into this thread's bank
// (the next record carries them to the worker's copy), as replay() does.
bool queuedReplay(Tape& t){
    auto* stream=gx::commandStream();
    if(!stream||!stream->finish())return false;
    const ScreenJob job{words,t.first,t.count,t.storageSerial,t.indexEpoch,t.cacheEpoch};
    if(!queueGxJob(gxJobScreenTape,&job,sizeof(job)))return false;
    if(!stream->applyChanges({bankWords+t.bankFirst,t.bankCount}))
        OSPanic(__FILE__,__LINE__,"Native retained screen: bank words of a queued replay refused");
    t.used=++useClock;t.usedFrame=frameSerial;
    ++stats.replayed;++queuedReplays;stats.bankWords+=t.bankCount;
    return true;
}
// duskScreenTapeBegin while runs are queued: 2 replay queued, 0 draw the
// screen as plain queued runs, 1 recorded here (after a sync, runs suspended).
int queuedBegin(const void* screen,unsigned long fingerprintA,unsigned long fingerprintB,int recordable){
    // A job the drawing side refused: every recording is made again.
    if(const auto refusals=jobRefusals.load(std::memory_order_acquire);refusals!=seenRefusals){
        seenRefusals=refusals;
        for(auto& t:tapes)if(t.valid){t.valid=false;++stats.dropped;}
    }
    if(mode==ScreenTapeMode::off||!screen||recording.active||gpuCanaryHudTapeRefused()||!storage()){
        ++stats.begins;++stats.plain;++plainWhileQueued;return 0;
    }
    // Verify mode, or Citro3D's context not checked yet (the first screen):
    // the synced path below decides.
    if(mode==ScreenTapeMode::on&&contextKnownUsable){
        ++stats.begins;
        if(!recordable){++stats.volatileScreens;++plainWhileQueued;return 0;}
        Tape* same=find(screen,std::uint32_t(fingerprintA),std::uint32_t(fingerprintB));
        if(same&&same->volatileScreen){++stats.volatileScreens;++plainWhileQueued;return 0;}
        if(same&&same->valid){
            // The target and the extent are this thread's; the drawing side
            // checks the texture storage and the resident pools in the job.
            if(same->target!=activeFramebuffer()||std::memcmp(&same->extent,&gx::currentTargetExtent,sizeof(same->extent))){
                same->valid=false;++stats.dropped;
            }else if(queuedReplay(*same)){auto& h=heatOf(screen);h.misses=0;h.level=0;return 2;}
            else{++stats.plain;++plainWhileQueued;return 0;}
        }
        auto& heat=heatOf(screen);
        if(heat.rest){--heat.rest;++stats.plain;++plainWhileQueued;return 0;}
        if(recyclePending){++stats.plain;++plainWhileQueued;return 0;}
        --stats.begins; // counted again by the synced begin
    }
    // Recorded here: the queue is drawn, this screen's runs are drawn on this
    // thread (into the recording), the queue takes them again after it.
    syncGxWorker();
    suspendNativeRuns(true);
    const int result=beginSynced(screen,fingerprintA,fingerprintB,recordable);
    if(result==1){screenSuspended=true;++suspendedRecordings;}
    else suspendNativeRuns(false);
    return result;
}
}
}
extern "C" int duskScreenTapeBegin(const void* screen,unsigned long fingerprintA,unsigned long fingerprintB,int recordable){
    // The painter's draws go to the second-core worker's queue right now
    // (late runs, or the 2D passes with gx-overlap.txt 2): the screen is
    // drawn as plain runs that follow the queue, with no sync (a sync here
    // handed every queued shape to the game thread and the result was the
    // same plain draw). Its counts are the game thread's.
    if(dusk::native::queuedPlain&&dusk::native::nativeRunsDeferred())return dusk::native::queuedBegin(screen,fingerprintA,fingerprintB,recordable);
    return dusk::native::beginSynced(screen,fingerprintA,fingerprintB,recordable);
}
namespace dusk::native {
namespace {
// The shared-state audit takes this one as a barrier: syncGxWorker() first.
[[gnu::noinline]] int beginSynced(const void* screen,unsigned long fingerprintA,unsigned long fingerprintB,int recordable){
    // As the HUD tape: a screen starts behind everything queued for the
    // second-core worker and ends with its own shapes drawn.
    syncGxWorker();
    ++stats.begins;
    // Inside a HUD tape recording too: the words of a replay or of a new
    // recording are then part of that pass.
    if(mode==ScreenTapeMode::off||!screen||recording.active||platformGpuOwner()!=OSGetCurrentThread()||
       gpuCanaryHudTapeRefused()||!tapeContextUsable()||!storage()||nativeRunsDeferred()){++stats.plain;return 0;}
    contextKnownUsable=true;
    if(!recordable){++stats.volatileScreens;return 0;}
    const auto a=std::uint32_t(fingerprintA),b=std::uint32_t(fingerprintB);
    recording.verify=nullptr;
    Tape* same=find(screen,a,b);
    if(same){
        if(same->volatileScreen){++stats.volatileScreens;return 0;}
        if(same->valid){
            if(!sameSurroundings(*same)){same->valid=false;++stats.dropped;}
            else if(mode==ScreenTapeMode::verify)recording.verify=same;
            else{
                if(const int ran=replay(*same)){auto& h=heatOf(screen);h.misses=0;h.level=0;return ran;}
                // No room in the command buffer for it here: drawn, and kept.
                ++stats.plain;return 0;
            }
        }
    }
    auto& heat=heatOf(screen);
    if(heat.rest){--heat.rest;++stats.plain;return 0;}
    auto* stream=gx::commandStream();
    if(!stream||!stream->finish()||recyclePending){++stats.plain;return 0;}
    // Where the recording goes. A verification keeps the recording it checks
    // and uses no slot; a dropped recording of the same fingerprint gives its
    // own slot; otherwise the owner's older recording or a free slot.
    Tape* slot=recording.verify?nullptr:same&&same->usedFrame!=frameSerial?same:claim(screen);
    if(!recording.verify&&!slot){++stats.plain;return 0;}
    // Its old vertices are written over when they are not in this frame's
    // command lists and the region is a real one.
    const bool inPlace=slot&&slot->owner==screen&&slot->usedFrame!=frameSerial&&slot->geometryCapacity>=256;
    if(!(inPlace?beginGxScreenGeometry(slot->geometryAt,slot->geometryCapacity):beginGxScreenGeometry())){
        if(!inPlace)recyclePending=true;
        ++stats.plain;return 0;
    }
    // From here the slot's old recording is gone: its vertices may be written over.
    if(slot)slot->valid=false;
    if(mode==ScreenTapeMode::verify)normalizeContext();
    platformHoldGpuChunks(true);
    tapeMarkContextChanged();
    recording.active=true;recording.volatileTexture=false;
    recording.owner=screen;recording.a=a;recording.b=b;recording.slot=slot;recording.inPlace=inPlace;
    u32 size{};GPUCMD_GetBuffer(&recording.buffer,&size,&recording.offset);
    recording.spanSerial=platformGpuSpanSerial();
    recording.copySerial=gxCopySerial;recording.storageSerial=gxTextureStorageSerial;recording.target=activeFramebuffer();
    *bankBefore=stream->registers();
    return 1;
}
}
}
extern "C" int duskScreenTapeEnd(void){
    const int result=dusk::native::endSynced();
    // A recording made while the painter's runs went to the queue: they go
    // there again from here.
    if(dusk::native::screenSuspended){dusk::native::screenSuspended=false;dusk::native::suspendNativeRuns(false);}
    return result;
}
namespace dusk::native {
namespace {
// The shared-state audit takes this one as a barrier: syncGxWorker() first.
[[gnu::noinline]] int endSynced(){
    syncGxWorker();
    if(!recording.active)return 0;
    recording.active=false;
    auto* stream=gx::commandStream();
    const bool flushed=stream&&stream->finish();
    // The list that carries this screen, kept or not: its state was sent whole.
    gpuCanaryHudTapeList();
    platformHoldGpuChunks(false);
    std::size_t geometryAt{},geometryBytes{};
    const bool geometry=endGxScreenGeometry(geometryAt,geometryBytes);
    auto& heat=heatOf(recording.owner);
    // A recording that was not replayed since the last one: after a few in a
    // row (kept or refused) the caller is told the owner keeps changing.
    auto missed=[&]{if(++heat.misses>=missesBeforeRest){heat.misses=0;return 1;}return 0;};
    Tape* const slot=recording.slot;
    // A slot whose old vertices were written over holds nothing any more; the
    // region stays its own for the next recording of this owner.
    auto abandon=[&]{if(slot&&recording.inPlace){slot->valid=false;slot->volatileScreen=false;slot->a=slot->b=0;}};
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    // The words of this screen are one unbroken run of the frame's span.
    if(!flushed||buffer!=recording.buffer||offset<recording.offset||platformGpuSpanSerial()!=recording.spanSerial){abandon();++stats.refusedSpan;return missed();}
    // A texture retired or re-housed, a copy or a target change in the screen.
    if(gxCopySerial!=recording.copySerial||gxTextureStorageSerial!=recording.storageSerial||activeFramebuffer()!=recording.target){abandon();++stats.refusedState;return missed();}
    if(!geometry){
        // Over the end of the buffer (ask for a recycle) or of the reused
        // region (the next recording of this owner takes a new one).
        abandon();++stats.refusedGeometry;
        if(recording.inPlace)slot->geometryCapacity=0;else recyclePending=true;
        return missed();
    }
    const u32 count=offset-recording.offset;
    if(count>stats.largestWords)stats.largestWords=count;
    if(const Tape* kept=recording.verify){
        recording.verify=nullptr;
        ++stats.verified;
        u32 where{};
        const bool same=sameWords(words+kept->first,buffer+recording.offset,std::min(count,kept->count),kept->geometryAt,geometryAt,where);
        if(recording.volatileTexture||count!=kept->count||!same){
            if(same)where=std::min(count,kept->count);
            // One line per place (register and uniform row), 16 places.
            const auto place=describeWord(buffer+recording.offset,count,where);
            static std::array<std::uint32_t,16> places;static unsigned reported;
            bool known=false;
            for(unsigned n=0;n<reported;++n)known|=places[n]==place.key;
            if(++stats.verifyMismatches&&!known&&reported<places.size()){
                places[reported++]=place.key;
                OSReport("Native retained screens verify mismatch: owner=%p words kept=%lu now=%lu first difference at %lu (%08lx/%08lx) "
                         "register %03lx%s uniform row %ld\n",
                    recording.owner,static_cast<unsigned long>(kept->count),static_cast<unsigned long>(count),static_cast<unsigned long>(where),
                    static_cast<unsigned long>(where<kept->count?words[kept->first+where]:0),
                    static_cast<unsigned long>(where<count?buffer[recording.offset+where]:0),
                    static_cast<unsigned long>(place.reg),place.header?" (header)":"",static_cast<long>(place.row));
            }
        }
        return 0;
    }
    auto& t=*slot;
    // What the slot keeps for itself across recordings: its regions.
    const bool own=t.owner==recording.owner;
    const u32 oldFirst=own?t.first:0,oldCapacity=own?t.capacity:0,oldBankFirst=own?t.bankFirst:0,oldBankCapacity=own?t.bankCapacity:0;
    const std::size_t oldGeometryAt=own?t.geometryAt:0,oldGeometryCapacity=own?t.geometryCapacity:0;
    t={};
    t.owner=recording.owner;t.a=recording.a;t.b=recording.b;t.used=++useClock;t.usedFrame=frameSerial;
    t.first=oldFirst;t.capacity=oldCapacity;t.bankFirst=oldBankFirst;t.bankCapacity=oldBankCapacity;
    if(recording.inPlace){t.geometryAt=oldGeometryAt;t.geometryCapacity=oldGeometryCapacity;}
    else{t.geometryAt=geometryAt;t.geometryCapacity=geometryBytes;}
    if(recording.volatileTexture){
        // Known for this fingerprint: not recorded again until it changes.
        t.volatileScreen=true;++stats.volatileScreens;return 0;
    }
    // The changed bank words: into the slot's own run when they fit, else a new one.
    bool bankOkay=false;
    const unsigned bankCount=stream->collectChanges(*bankBefore,{bankChanged,bankWordsPerTape},bankOkay);
    if(!bankOkay){++stats.refusedState;return missed();}
    if(bankCount>t.bankCapacity){
        if(bankCount>bankPoolWords-bankUsed){++stats.refusedSize;recyclePending=true;return 0;}
        t.bankFirst=bankUsed;t.bankCapacity=bankCount;bankUsed+=bankCount;
    }
    if(count>t.capacity){
        if(count>poolWords-wordsUsed){++stats.refusedSize;recyclePending=true;return 0;}
        t.first=wordsUsed;t.capacity=count;wordsUsed+=count;
    }else if(own)++stats.inPlace;
    // An empty screen (every pane hidden) is a valid, empty recording.
    std::memcpy(words+t.first,buffer+recording.offset,count*sizeof(u32));
    std::memcpy(bankWords+t.bankFirst,bankChanged,bankCount*sizeof(gx::BankWord));
    t.count=count;t.bankCount=bankCount;
    t.storageSerial=gxTextureStorageSerial;t.indexEpoch=residentIndexEpoch();t.cacheEpoch=cacheEpoch();
    t.extent=gx::currentTargetExtent;t.target=activeFramebuffer();
    t.valid=true;
    ++stats.recorded;
    return missed();
}
}
// gxJobScreenTape (gx_worker_sync.hpp): a queued replay, on the thread that
// draws, in the job's place in the queue.
void runScreenTapeJob(const void* arguments,std::size_t bytes){
    ScreenJob job;
    if(bytes!=sizeof(job))return;
    std::memcpy(&job,arguments,sizeof(job));
    if(job.storageSerial!=gxTextureStorageSerial||job.indexEpoch!=residentIndexEpoch()||job.cacheEpoch!=cacheEpoch()||!job.words){
        jobRefusals.fetch_add(1,std::memory_order_release);return;
    }
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    if(!buffer||size<offset||size-offset<job.count+reserveWords){jobRefusals.fetch_add(1,std::memory_order_release);return;}
    if(job.count){
        GPUCMD_AddRawCommands(job.words+job.first,job.count);
        gpuCanaryHudTapeList();
        tapeMarkContextChanged();
        // Other draws ran on the PICA: shape replays must not continue across it.
        ++gxPicaStateSerial;
    }
    jobReplays.fetch_add(1,std::memory_order_relaxed);
}
}
extern "C" void duskScreenTapeRest(const void* screen){
    if(!screen)return;
    // Each rest in a row without a replay is twice as long (up to 192 draws).
    auto& heat=heatOf(screen);
    heat.misses=0;heat.rest=std::uint8_t(restDraws<<heat.level);
    if(heat.level<3)++heat.level;
}
extern "C" void duskScreenTapeForget(const void* screen){
    if(!screen)return;
    // The slots stay the owner's (their regions are reused by a screen at the
    // same address); what they hold is no longer replayed.
    for(auto& t:tapes)if(t.owner==screen){t.valid=false;t.volatileScreen=false;t.a=t.b=0;}
}
extern "C" void duskScreenTapeVolatileTexture(unsigned long long handle){
    if(!recording.active)return;
    for(const auto& image:measuredImages)if(image.picture==frameSerial&&image.handle==handle)return;
    recording.volatileTexture=true;
}
