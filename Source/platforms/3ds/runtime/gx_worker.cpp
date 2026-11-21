#include "gx_worker.hpp"
#include "gx_profile.hpp"
#include <cstring>
#include <new>

namespace dusk::native::gxworker {
namespace {
Queue theQueue;
static_assert(std::is_trivially_copyable_v<QueuedShape>&&std::is_trivially_copyable_v<gx::TargetExtent>&&
              std::is_trivially_copyable_v<gx::CommandProcessor::ArrayBindings>);
constexpr std::size_t shapeWords=(sizeof(QueuedShape)+3)/4,extentWords=(sizeof(gx::TargetExtent)+3)/4,
    arraysWords=(sizeof(gx::CommandProcessor::ArrayBindings)+3)/4;
// Verify mode: a sum that depends on every word and on its place.
std::uint32_t bankSum(const gx::CommandRegisters& bank){
    const auto* words=gx::bankWordsOf(bank);std::uint32_t sum=0x811C9DC5u;
    for(std::size_t i=0;i<gx::bank_layout::words;++i)sum=(sum^words[i])*16777619u;
    return sum;
}
struct Consuming {
    bool& flag;const bool before;
    explicit Consuming(bool& value):flag(value),before(value){flag=true;}
    ~Consuming(){flag=before;}
};
}
thread_local bool Queue::consumer_=false;
Queue& queue(){return theQueue;}

bool Queue::start(Mode mode,void* storage,std::size_t bytes,const Adapter& adapter,const Platform& platform,bool verify){
    if(mode_!=Mode::off||mode==Mode::off||!storage||bytes<minimumStorage||reinterpret_cast<std::uintptr_t>(storage)%8||!adapter.draw)return false;
    if(mode==Mode::threaded&&(!platform.wakeWorker||!platform.waitForWorker||!platform.workerProgress||!platform.workerIdle||
                              !platform.gpuToWorker||!platform.gpuToGame))return false;
    auto* at=static_cast<std::byte*>(storage);
    sent_=new(at)gx::CommandRegisters{};mirror_=new(at+sizeof(gx::CommandRegisters))gx::CommandRegisters{};
    // Native packets: the worker's copy of the bank is the mirror
    // processor's own (the first record always sends the whole bank).
    mirrorProcessor_=adapter.native?adapter.mirror:nullptr;
    if(mirrorProcessor_){mirror_=&mirrorProcessor_->mirrorRegisters();mirrorProcessor_->adoptArrays({});}
    ring_=reinterpret_cast<std::uint32_t*>(at+2*sizeof(gx::CommandRegisters));
    capacity_=std::uint32_t((bytes-2*sizeof(gx::CommandRegisters))/4);
    adapter_=adapter;platform_=platform;verify_=verify;
    head_=tail_=0;head.store(0);tail.store(0);
    sentValid_=sentExtentValid_=sentArraysValid_=false;owned_.store(false);
    park_.store(running);resync_.store(false);stopping_.store(false);
    overlapping_.store(false);drainNotice_.store(false);drainedBusy_.store(false);
    mode_=mode;accepting_=true;
    return true;
}
std::size_t Queue::largestRecord(std::size_t logWords,std::size_t copiedBytes){
    return (1+gx::bank_layout::words)+(1+logWords+gx::bankTailRunCapacity)+(1+extentWords)+(1+arraysWords)+(1+shapeWords+(copiedBytes+3)/4);
}
// Room for `words` contiguous words at the head. One word always stays free
// behind a record (the wrap marker's) and the head never reaches the tail
// from behind, so equal indices only ever mean "empty".
std::uint32_t* Queue::reserve(std::size_t words){
    for(;;){
        const std::uint32_t t=tail.load(std::memory_order_acquire);
        if(head_>=t){
            if(capacity_-head_>words)return ring_+head_;
            if(t>words){
                // Not enough to the end: the records go on at the start.
                ring_[head_]=header(wrap,1);head_=0;head.store(0);++counters_.wraps;
                continue;
            }
        }else if(t-head_>words)return ring_+head_;
        // The consumer is that far behind: the game thread draws what is
        // queued (Alpha 15: only until the record fits, in worker mode).
        ++counters_.ringFull;
        if(owned_.load())drain(SyncCause::ringFull,partialDrain_&&mode_==Mode::threaded?words:0);
        else if(tail.load()==t)return nullptr; // nothing queued and still no room: the record is larger than the ring
    }
}
// reserve()'s test, with the consumer's place as the game thread holds it
// inside drain (the worker parked).
bool Queue::roomFor(std::size_t words)const{
    const std::uint32_t t=tail_;
    if(head_>=t)return capacity_-head_>words||t>words;
    return t-head_>words;
}
bool Queue::beginOverlap(){
    if(mode_!=Mode::threaded||!owned_.load(std::memory_order_relaxed))return false;
    // Nothing left: the caller's sync is a hand-back without a takeover.
    if(tail.load(std::memory_order_acquire)==head_)return false;
    overlapping_.store(true,std::memory_order_relaxed);++counters_.overlaps;
    // The worker submits what it drew once the queue is empty (Platform::
    // drained); the look before it sleeps sees the notice too.
    drainNotice_.store(true,std::memory_order_release);
    if(workerSleeping_.load())platform_.wakeWorker();
    return true;
}
void Queue::pushShape(const gx::CommandRegisters& bank,const gx::BankLog& log,const gx::CommandProcessor::ArrayBindings* arrays,
                      const gx::TargetExtent& extent,const QueuedShape& queued,const void* copied,std::size_t copiedBytes){
    if(resync_.exchange(false))sentValid_=sentExtentValid_=sentArraysValid_=false;
    const std::size_t copiedWords=(copiedBytes+3)/4;
    const std::size_t worst=largestRecord(log.used,copiedBytes);
    // Profiled pictures: where a queued shape's time on the game thread goes.
    static const auto reserveIndex=registerGxNamedProfile("queue.reserve");GxNamedProfileSpan reserveSpan(reserveIndex);
    std::uint32_t* const at=reserve(worst);
    reserveSpan.stop();
    if(!at){
        // Cannot happen with minimumStorage; the shape is drawn right here.
        ++counters_.damaged;
        Consuming scope(consumer_);
        QueuedShape direct=queued;
        if(copiedBytes){direct.list=copied;direct.listBytes=std::uint32_t(copiedBytes);}
        adapter_.draw(direct,bank,arrays?*arrays:mirrorArrays(),extent);
        sentValid_=sentExtentValid_=sentArraysValid_=false;
        return;
    }
    std::size_t used=writePrelude(at,bank,log,arrays,extent);
    static const auto restIndex=registerGxNamedProfile("queue.records");GxNamedProfileSpan restSpan(restIndex);
    QueuedShape shape=queued;
    if(verify_)shape.bankSum=bankSum(bank);
    if(copiedBytes){
        // The run's bytes right behind the record; the consumer points the
        // shape's list at them.
        shape.list=nullptr;shape.listBytes=std::uint32_t(copiedBytes);
        at[used+shapeWords+copiedWords]=0;
        std::memcpy(at+used+1+shapeWords,copied,copiedBytes);
        ++counters_.runs;counters_.runBytes+=copiedBytes;
    }
    at[used]=header(shapeRecord,1+shapeWords+copiedWords);std::memcpy(at+used+1,&shape,sizeof(shape));used+=1+shapeWords+copiedWords;
    head_+=std::uint32_t(used);
    restSpan.stop();
    ++counters_.queued;
    publish();
}
std::size_t Queue::writePrelude(std::uint32_t* at,const gx::CommandRegisters& bank,const gx::BankLog& log,
                                const gx::CommandProcessor::ArrayBindings* arrays,const gx::TargetExtent& extent){
    constexpr std::size_t wholeBank=1+gx::bank_layout::words;
    std::size_t used=0;
    // `sent` (what the mirror holds) is kept in verify mode only.
    auto whole=[&]{
        at[used]=header(full,wholeBank);std::memcpy(at+used+1,&bank,sizeof(bank));
        if(verify_)*sent_=bank;
        sentValid_=true;used+=wholeBank;++counters_.fullBanks;
    };
    static const auto bankIndex=registerGxNamedProfile("queue.bank");GxNamedProfileSpan bankSpan(bankIndex);
    if(!sentValid_||log.lost)whole();
    else{
        // One record: the processor's stores as they are, then the words
        // behind the arrays.
        std::uint32_t* const body=at+used+1;
        if(log.used)std::memcpy(body,log.words,log.used*sizeof(std::uint32_t));
        const std::size_t words=log.used+gx::copyBankTail(bank,log.grewValid,body+log.used);
        at[used]=header(delta,1+words);
        if(verify_&&(!gx::applyBankDelta(*sent_,body,words)||gx::firstBankDifference(bank,*sent_)!=gx::bank_layout::words)){
            // A word of the arrays the log left behind, or a valid bit it did
            // not announce: counted, and the whole bank sent in place of the
            // delta so the pictures stay right.
            ++counters_.unmarkedBlocks;whole();
        }else{used+=1+words;counters_.deltaWords+=words;}
    }
    bankSpan.stop();
    if(!sentExtentValid_||std::memcmp(&sentExtent_,&extent,sizeof(extent))){
        at[used]=header(extentRecord,1+extentWords);std::memcpy(at+used+1,&extent,sizeof(extent));used+=1+extentWords;
        sentExtent_=extent;sentExtentValid_=true;
    }
    if(arrays||!sentArraysValid_){
        // The caller passes them whenever the setup epoch moved (push()).
        if(arrays){
            ++counters_.arrayRecords;
            at[used]=header(arraysRecord,1+arraysWords);std::memcpy(at+used+1,arrays,sizeof(*arrays));used+=1+arraysWords;
            sentArraysValid_=true;
        }
        sentSetupEpoch_=bank.vertexSetupEpoch;
    }
    return used;
}
// The records written at head_ become the consumer's: the release store of
// `head` makes every word of them (and whatever the game thread wrote before:
// display lists, the bytes a native record points at) visible to the
// consumer's acquire load (both default to sequentially consistent).
void Queue::publish(){
    static const auto publishIndex=registerGxNamedProfile("queue.publish");GxNamedProfileSpan publishSpan(publishIndex);
    if(!owned_.load(std::memory_order_relaxed)){owned_.store(true);if(platform_.gpuToWorker)platform_.gpuToWorker();}
    head.store(head_);
    if(mode_==Mode::threaded&&workerSleeping_.load())platform_.wakeWorker();
}
bool Queue::pushNative(const gx::CommandRegisters& bank,const gx::BankLog& log,const gx::CommandProcessor::ArrayBindings& arrays,
                       const gx::TargetExtent& extent,const std::uint32_t* body,std::size_t words,bool prelude,unsigned shapes){
    if(!nativeFits(log.used,words))return false;
    // A resync asked for by the consumer needs the whole bank, which is
    // only right with a prelude (nativeFits refuses the other case).
    if(prelude&&resync_.exchange(false))sentValid_=sentExtentValid_=sentArraysValid_=false;
    const std::size_t worst=largestRecord(log.used,words*4);
    static const auto reserveIndex=registerGxNamedProfile("queue.reserve");GxNamedProfileSpan reserveSpan(reserveIndex);
    std::uint32_t* const at=reserve(worst);
    reserveSpan.stop();
    if(!at){++counters_.damaged;return false;}
    std::size_t used=0;
    if(prelude)used=writePrelude(at,bank,log,!sentArraysValid_||sentSetupEpoch_!=bank.vertexSetupEpoch?&arrays:nullptr,extent);
    else if(!sentExtentValid_||std::memcmp(&sentExtent_,&extent,sizeof(extent))){
        at[used]=header(extentRecord,1+extentWords);std::memcpy(at+used+1,&extent,sizeof(extent));used+=1+extentWords;
        sentExtent_=extent;sentExtentValid_=true;
    }
    // The record: its shape count, then the body.
    at[used]=header(nativeRecord,2+words);at[used+1]=shapes;std::memcpy(at+used+2,body,words*sizeof(std::uint32_t));used+=2+words;
    head_+=std::uint32_t(used);
    ++counters_.natives;counters_.nativeWords+=words;counters_.queued+=shapes;
    publish();
    return true;
}
// Consumer: one record. False when there is none.
bool Queue::consumeOne(bool worker){
    const std::uint32_t h=head.load();
    if(tail_==h)return false;
    const std::uint32_t first=ring_[tail_],words=first>>8;
    const std::uint32_t* body=ring_+tail_+1;
    static const auto applyIndex=registerGxNamedProfile("queue.apply");GxNamedProfileSpan applySpan(applyIndex);
    bool sound=words>=1&&words<=capacity_-tail_;
    if(sound)switch(Kind(first&0xff)){
    case wrap:tail_=0;tail.store(0);return true;
    case full:
        sound=words==1+gx::bank_layout::words;
        if(sound)std::memcpy(static_cast<void*>(mirror_),body,sizeof(*mirror_));
        break;
    case delta:sound=gx::applyBankDelta(*mirror_,body,words-1);break;
    case extentRecord:
        sound=words==1+extentWords;
        if(sound)std::memcpy(static_cast<void*>(&mirrorExtent_),body,sizeof(mirrorExtent_));
        break;
    case arraysRecord:
        sound=words==1+arraysWords;
        if(sound){
            if(mirrorProcessor_){gx::CommandProcessor::ArrayBindings arrays;std::memcpy(arrays.bytes.data(),body,sizeof(arrays.bytes));mirrorProcessor_->adoptArrays(arrays);}
            else std::memcpy(static_cast<void*>(&mirrorArrays_),body,sizeof(mirrorArrays_));
        }
        break;
    case nativeRecord:
        sound=mirrorProcessor_&&adapter_.native&&words>=2;
        if(!sound)break;
        applySpan.stop();
        // The packet's operations on the copy of the bank, its shapes drawn
        // in between (Adapter::native).
        adapter_.native(body+1,words-2,*mirrorProcessor_,mirrorExtent_);
        (worker?counters_.drawnByWorker:counters_.drawnByGame)+=body[0];
        break;
    case shapeRecord:{
        sound=words>=1+shapeWords;
        if(!sound)break;
        QueuedShape shape;std::memcpy(static_cast<void*>(&shape),body,sizeof(shape));
        // A copied run lies behind the record.
        const std::size_t copiedWords=words-1-shapeWords;
        sound=copiedWords?!shape.list&&(shape.listBytes+3u)/4==copiedWords:true;
        if(!sound)break;
        if(copiedWords)shape.list=body+shapeWords;
        if(verify_&&shape.bankSum!=bankSum(*mirror_))++counters_.sumMismatches;
        applySpan.stop();
        adapter_.draw(shape,*mirror_,mirrorArrays(),mirrorExtent_);
        ++(worker?counters_.drawnByWorker:counters_.drawnByGame);
        break;}
    default:sound=false;break;
    }
    if(!sound){
        // Never seen; if a record is ever unreadable, what is queued is given
        // up (those shapes are missing from one picture) and the producer
        // sends the whole bank again.
        ++counters_.damaged;resync_.store(true);tail_=h;tail.store(h);return false;
    }
    tail_+=words;tail.store(tail_);++counters_.records;
    return true;
}
// Game thread: every queued shape drawn, the GPU back with the game thread.
void Queue::drain(SyncCause cause,std::size_t room){
    ++counters_.syncs;++counters_.syncsBy[unsigned(cause)];
    const std::uint64_t began=platform_.now?platform_.now():0;
    const bool ending=overlapping_.load(std::memory_order_relaxed)&&!room;
    const auto byGameBefore=counters_.drawnByGame;
    if(ending){
        ++counters_.overlapEndsBy[unsigned(cause)];
        if(cause==SyncCause::frameEnd&&tail.load(std::memory_order_acquire)==head_)++counters_.overlapsFinished;
    }
    if(mode_==Mode::threaded){
        // A worker that drew the last shape of an overlap may be submitting
        // (Platform::drained) with the GPU: no new submission once the notice
        // is down, and the one under way ends first (see run()).
        drainNotice_.store(false);
        while(drainedBusy_.load())platform_.waitForWorker(200);
        if(tail.load(std::memory_order_acquire)!=head_){
            // The worker is not through: it stops between two records and the
            // game thread draws the rest (see the rules in gx_worker.hpp).
            park_.store(parkAsked);
            if(workerSleeping_.load())platform_.wakeWorker();
            bool slept=false,urgent=false;std::uint64_t began=0;
            // A cheap look first, without a barrier per round: a worker that
            // is drawing parks within a shape.
            for(unsigned spins=0;spins<parkSpinRounds&&park_.load(std::memory_order_relaxed)!=parked;++spins){}
            // The acquire load that ends the wait makes what the worker wrote
            // before it parked (its place in the ring, the mirror, the
            // counters) visible here; a relaxed one does not (on the console
            // either: the ARM11 orders nothing on its own).
            while(park_.load(std::memory_order_acquire)!=parked){
                if(!urgent&&platform_.workerUrgent){platform_.workerUrgent(true);urgent=true;}
                gameWaiting_.store(true);
                if(park_.load()!=parked){platform_.waitForWorker(1000);slept=true;}
                gameWaiting_.store(false);
                // A shape takes microseconds to a few milliseconds.
                if(platform_.now&&platform_.workerStuck){
                    const auto now=platform_.now();
                    if(!began)began=now;
                    else if(now-began>stuckMicroseconds)platform_.workerStuck();
                }
            }
            if(slept)++counters_.parkWaits;
            if(urgent)platform_.workerUrgent(false);
        }
    }
    if(platform_.gpuToGame)platform_.gpuToGame();
    const std::uint64_t parkedAt=platform_.now?platform_.now():0;
    if(tail.load()!=head_){
        ++counters_.takeovers;++counters_.takeoversBy[unsigned(cause)];
        Consuming scope(consumer_);
        // The worker's place in the ring, its copy of the bank and of the
        // bindings: it wrote them before it parked.
        if(room){
            // Only until the record fits (the worker is behind by a whole
            // ring): the rest stays the worker's.
            ++counters_.partialDrains;
            while(!roomFor(room)&&consumeOne(false)){}
        }else while(consumeOne(false)){}
    }
    if(platform_.now){
        counters_.waitMicrosBy[unsigned(cause)]+=parkedAt-began;
        counters_.takeoverMicrosBy[unsigned(cause)]+=platform_.now()-parkedAt;
    }
    if(room&&tail.load()!=head_){
        // Records are left: the worker draws them, with the GPU. The release
        // store of park_ hands it the place in the ring, the copy of the bank
        // and the bindings this thread advanced (as at the end of a sync).
        if(platform_.gpuToWorker)platform_.gpuToWorker();
        park_.store(running);
        return;
    }
    if(ending)counters_.overlapShapesByGame+=counters_.drawnByGame-byGameBefore;
    overlapping_.store(false,std::memory_order_relaxed);drainNotice_.store(false,std::memory_order_relaxed);
    owned_.store(false);
    // The worker goes on with the next shape queued (it may sleep until then).
    if(mode_==Mode::threaded)park_.store(running);
}
void Queue::run(){
    consumer_=true;
    while(!stopping_.load(std::memory_order_acquire)){
        std::uint32_t asked=parkAsked;
        if(park_.load()!=running){
            // Between two records: the game thread takes the queue from here
            // and gives it back by setting the state to running.
            if(park_.compare_exchange_strong(asked,parked)&&gameWaiting_.load())platform_.workerProgress();
            // The game thread hands the queue back when it has drawn the
            // rest. Any state but parked ends the look: the game thread may
            // hand the queue back and ask for it again before this loop sees
            // `running` (it then waits for `parked`, set at the top).
            bool back=false;
            for(unsigned n=0;!back&&n<spinRounds;++n)
                back=park_.load(std::memory_order_relaxed)!=parked||stopping_.load(std::memory_order_relaxed);
            if(back)continue;
            workerSleeping_.store(true);
            if(park_.load()!=running&&!stopping_.load())platform_.workerIdle(2000);
            workerSleeping_.store(false);
            continue;
        }
        if(consumeOne(true))continue;
        // The painter ended and this was its last shape (beginOverlap): the
        // GPU is still this thread's, let it start on what was drawn.
        // drainedBusy_ goes up before the notice is taken (both sequentially
        // consistent): a drain() that cleared the notice and then saw the
        // flag down knows this thread will not touch the GPU (drain waits
        // while it is up).
        if(drainNotice_.load(std::memory_order_relaxed)){
            drainedBusy_.store(true);
            if(drainNotice_.exchange(false)&&platform_.drained)platform_.drained();
            drainedBusy_.store(false);
            continue;
        }
        // Nothing queued. The game thread queues a shape every 100 to 300
        // microseconds while it walks a list: look again for a moment before
        // sleeping (a sleep and its wake-up are two system calls).
        bool more=false;
        for(unsigned n=0;!more&&n<spinRounds;++n)
            more=head.load(std::memory_order_relaxed)!=tail_||park_.load(std::memory_order_relaxed)!=running||stopping_.load(std::memory_order_relaxed)||
                 drainNotice_.load(std::memory_order_relaxed);
        if(more)continue;
        workerSleeping_.store(true);
        if(head.load()==tail_&&park_.load()==running&&!stopping_.load()&&!drainNotice_.load())platform_.workerIdle(2000);
        workerSleeping_.store(false);
    }
}
void Queue::stop(){
    stopping_.store(true);
    if(mode_==Mode::threaded)platform_.wakeWorker();
}
}
