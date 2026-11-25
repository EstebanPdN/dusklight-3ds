// Second-core worker: the register bank deltas and the shape queue.
//  1. A processor driven through its entry points logs every word it changes
//     in the bank's arrays and says when it made one valid: after each step
//     its log and the difference of the words behind the arrays (with the
//     valid bits when it said so) bring a copy to the bank bit for bit; a
//     log that filled up says so.
//  2. The queue in deferred mode (records consumed by the game thread):
//     every shape is drawn once, in order, under the bank, extent and
//     bindings it was queued with.
//  3. The same with a consumer on a second thread, the game thread syncing
//     at random places and taking over what the worker has not drawn yet:
//     each shape is drawn by the thread that owns the GPU at that moment
//     (built a second time with ThreadSanitizer).
#include "../runtime/gx_worker.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include <dolphin/gx/GXCommandList.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace dusk::native;
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
thread_local dusk::native::gx::StaticGeometryHint dusk::native::gx::staticGeometryHint{};

namespace {
std::uint32_t seed=0x2545F491u;
std::uint32_t random32(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
unsigned pick(unsigned n){return random32()%n;}
#define CHECK(condition) do{if(!(condition)){std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#condition);std::exit(1);}}while(0)

// One random step through a processor entry point that writes the bank.
alignas(4) float matrices[64][12],matricesB[64][12];
void step(CommandProcessor& p){
    switch(pick(12)){
    case 0:{const u8 regs[]{0x30,0x40,0x50,0x60,0x70,0x80,0x90,0x77,0xb0,0xb1};CHECK(p.writeCp(regs[pick(10)],random32()&0x1fffff));break;}
    case 1:{u8 reg=u8(pick(256));if(reg==0x52||reg==0x65)reg=0x40;CHECK(p.writeBp(reg,random32()&0xffffff));break;}
    case 2:{u8 reg=u8(pick(256));if(reg==0x52||reg==0x65||reg==0xfe)reg=0xc0;CHECK(p.writeBpMasked(reg,random32()&0xffffff,random32()&0xffffff));break;}
    case 3:{ // matrix memory
        std::uint32_t values[12];for(auto& v:values)v=random32();
        const u16 at=u16(pick(2)?pick(10)*12:0x400+pick(10)*9);
        CHECK(p.writeXf(at,{values,at<0x400?12u:9u}));break;}
    case 4:{ // texture matrices, post matrices, lights, registers
        const u16 bases[]{0x78,0x500,0x600,0x1005,0x100e,0x1018,0x101a,0x1040,0x104c};
        std::uint32_t values[8];for(auto& v:values)v=random32()&0xffffff;
        CHECK(p.writeXf(u16(bases[pick(9)]+pick(4)),{values,1+pick(6)}));break;}
    case 5:CHECK(p.loadIndexed(GX_LOAD_INDX_A,u16(pick(64)),u16(0xb000|(pick(10)*12))));break;
    case 6:CHECK(p.loadIndexed(GX_LOAD_INDX_B,u16(pick(64)),u16(0x8000|(0x400+pick(10)*9))));break;
    case 7:{ // a display list of register loads (interpreter)
        std::vector<u8> list;
        auto word=[&](std::uint32_t v){for(int s=24;s>=0;s-=8)list.push_back(u8(v>>s));};
        for(unsigned n=1+pick(6);n;--n)switch(pick(3)){
        case 0:list.push_back(GX_LOAD_BP_REG);word((std::uint32_t(0x20+pick(0x20))<<24)|(random32()&0xffffff));break;
        case 1:list.push_back(GX_LOAD_CP_REG);list.push_back(u8(0x70+pick(8)));word(random32());break;
        default:{const unsigned count=1+pick(4);list.push_back(GX_LOAD_XF_REG);list.push_back(0);list.push_back(u8(count-1));
            const unsigned at=0x1008+pick(0x30);list.push_back(u8(at>>8));list.push_back(u8(at));
            for(unsigned i=0;i<count;++i)word(random32()&0xffff);break;}
        }
        CHECK(p.execute(list));break;}
    case 8:{ // a compiled list: the same bytes twice
        static std::vector<u8> list;
        if(list.empty()){
            auto word=[&](std::uint32_t v){for(int s=24;s>=0;s-=8)list.push_back(u8(v>>s));};
            for(unsigned n=0;n<8;++n){list.push_back(GX_LOAD_BP_REG);word((std::uint32_t(0xc0+n)<<24)|0x123456);}
            list.push_back(GX_LOAD_XF_REG);list.push_back(0);list.push_back(2);list.push_back(0x10);list.push_back(0x0a);
            word(0x11);word(0x22);word(0x33);
            while(list.size()%32)list.push_back(0);
        }
        list[4]=u8(pick(256)); // a changed list is compiled again
        CHECK(p.executeList(list));CHECK(p.executeList(list));break;}
    case 9:{ // retained 2D screens: changes collected and applied
        const auto before=p.registers();
        std::uint32_t value=random32()&0xffffff;CHECK(p.writeBp(0x41,value));CHECK(p.writeXf(0x1009,{&value,1}));
        std::array<BankWord,64> words;bool ok=false;const unsigned count=p.collectChanges(before,words,ok);
        CHECK(ok);CHECK(p.applyChanges({words.data(),count}));break;}
    case 10:if(!pick(40)){ // a reset empties the bank and the bindings
        p.reset();CHECK(p.bindArray(12,matrices,sizeof(matrices),48,true));CHECK(p.bindArray(13,matrices,sizeof(matrices),48,true));}
        break;
    default:{ // palette loads and TEV colour banks
        CHECK(p.writeBp(0x64,random32()&0xffffff));CHECK(p.writeBp(0x65,(pick(16)<<10)|pick(8)));
        CHECK(p.writeBp(u8(0xe0+pick(8)),random32()&0xffffff));break;}
    }
}
void marks(){
    for(auto& m:matrices)for(auto& v:m)v=float(random32()%1000)/7.f;
    for(auto& m:matricesB)for(auto& v:m)v=float(random32()%1000)/3.f;
    static CommandMemory memory;static CommandProcessor p(memory);
    MemoryHandle handle{};CHECK(memory.add({reinterpret_cast<const u8*>(matrices),sizeof(matrices)},handle));
    CHECK(p.bindArray(12,matrices,sizeof(matrices),48,true));CHECK(p.bindArray(13,matrices,sizeof(matrices),48,true));
    static CommandRegisters sent,mirror;
    static std::uint32_t logWords[512],delta[bankTailRunCapacity+bank_layout::words];
    p.attachBankLog(logWords);
    sent=p.registers();mirror=sent;p.restartBankLog();
    std::size_t largest=0,total=0,lost=0;unsigned steps=0;
    for(unsigned round=0;round<60000;++round){
        // Mostly short bursts; now and then one that overflows the log.
        const unsigned burst=pick(60)?1+pick(4):40+pick(200);
        for(unsigned n=0;n<burst;++n)step(p);
        steps+=burst;
        const auto& log=p.bankStores();
        if(log.lost){++lost;mirror=p.registers();sent=mirror;}
        else{
            CHECK(log.used<=std::size(logWords));
            CHECK(applyBankDelta(mirror,log.words,log.used));
            const std::size_t words=copyBankTail(p.registers(),log.grewValid,delta);
            CHECK(words<=bankTailRunCapacity);
            CHECK(applyBankDelta(mirror,delta,words));
            largest=std::max<std::size_t>(largest,log.used+words);total+=log.used+words;
        }
        p.restartBankLog();
        const std::size_t differs=firstBankDifference(p.registers(),mirror);
        if(differs!=bank_layout::words){
            std::printf("FAIL log: word %zu of the bank differs after round %u (cp at %zu, bp %zu, xf %zu, valid %zu, tail %zu)\n",
                differs,round,bank_layout::cpAt,bank_layout::bpAt,bank_layout::xfAt,bank_layout::cpValidAt,bank_layout::tailAt);
            std::exit(1);
        }
    }
    CHECK(lost>0&&lost<3000);
    // Damaged deltas are refused whole.
    delta[0]=std::uint32_t((bank_layout::words-1)<<16|2);CHECK(!applyBankDelta(mirror,delta,3));
    delta[0]=std::uint32_t(4<<16|5);CHECK(!applyBankDelta(mirror,delta,3));
    delta[0]=std::uint32_t(4<<16);CHECK(!applyBankDelta(mirror,delta,1));
    // The adopted bank and bindings are the donor's.
    static CommandProcessor other(memory);
    CHECK(other.adopt(p.registers(),p.arrayBindings()));
    CHECK(firstBankDifference(other.registers(),p.registers())==bank_layout::words);
    CHECK(!std::memcmp(other.arrayBindings().bytes.data(),p.arrayBindings().bytes.data(),sizeof(CommandProcessor::ArrayBindings::bytes)));
    CHECK(other.loadIndexed(GX_LOAD_INDX_A,3,0xb000)&&p.loadIndexed(GX_LOAD_INDX_A,3,0xb000));
    CHECK(firstBankDifference(other.registers(),p.registers())==bank_layout::words);
    std::printf("PASS bank deltas: %u processor steps in 60000 rounds, the log of stores and the tail bring a copy to the bank every time "
        "(bank %zu words, mean delta %.1f words, largest %zu); the log filled up %zu times and said so (whole bank sent); "
        "damaged deltas refused; an adopted bank equals its donor\n",
        steps,bank_layout::words,double(total)/60000.0,largest,lost);
}

// Alpha 15 owner bug (2D and water blinking; dump alpha15/014, 7272 queued
// draws not drawn): the second-core worker's list processor refused to adopt
// a queued shape's bank while the game thread recorded a J2D material
// (materialRecording), and drawQueuedShape then drew nothing. Only the
// recording's own processor may refuse; any other one must adopt.
void adoptWhileRecording(){
    static CommandMemory memory;
    auto* game=new CommandProcessor(memory);auto* list=new CommandProcessor(memory);
    setMaterialMemo(true,false);
    static int material;MaterialMemoSum sum;materialMemoChecksum(sum,&material,sizeof(material));
    const auto ticket=materialMemoLookup(game->registers(),&material,sum,1,1);
    CHECK(ticket.action==MaterialMemoAction::record&&materialMemoRecording());
    // The worker's processor draws the queued shape under its own bank copy.
    CHECK(list->adopt(game->registers(),game->arrayBindings()));
    // The recorded processor itself still refuses another bank mid-recording.
    CHECK(!game->adopt(game->registers(),game->arrayBindings()));
    (void)materialMemoFinish(ticket);
    CHECK(!materialMemoRecording());
    CHECK(list->adopt(game->registers(),game->arrayBindings())&&game->adopt(game->registers(),game->arrayBindings()));
    setMaterialMemo(false,false);
    delete game;delete list;
    std::printf("PASS a queued shape is adopted while the game thread records a J2D material (only the recorded processor refuses)\n");
}

// ---- the queue ----
struct Expected {std::uint32_t sum;TargetExtent extent;std::uint32_t arrays;std::uint32_t runBytes,runSum,job;};
std::vector<Expected> expected;
std::atomic<std::size_t> drawnCount{},byWorker{},byGame{};
std::atomic<bool> orderBroken{},bankWrong{},extentWrong{},arraysWrong{},ownerWrong{},runWrong{};
std::atomic<bool> slowConsumer{}; // the consumer falls behind: the producer finds the ring full or takes shapes over
std::atomic<int> gpuOwner{0}; // 0 game, 1 worker
// Alpha 15: the worker's submission after the last shape of an overlap
// (Platform::drained) must find the GPU its own from start to end, however
// the game thread's sync falls around it.
std::atomic<std::size_t> drainedCalls{};
std::thread::id gameThread;
gxworker::Queue* activeQueue{};
std::uint32_t sumOf(const CommandRegisters& bank){
    const auto* w=bankWordsOf(bank);std::uint32_t s=7;for(std::size_t i=0;i<bank_layout::words;++i)s=(s^w[i])*16777619u;return s;
}
// Shapes are numbered through `generation`.
void draw(const gxworker::QueuedShape& shape,const CommandRegisters& bank,const CommandProcessor::ArrayBindings& arrays,const TargetExtent& extent){
    const std::size_t index=shape.generation;
    if(index!=drawnCount.load()||index>=expected.size()){orderBroken=true;return;}
    const auto& want=expected[index];
    std::uint32_t tag;std::memcpy(&tag,arrays.bytes.data(),4);
    if(sumOf(bank)!=want.sum)bankWrong=true;
    if(std::memcmp(&extent,&want.extent,sizeof(extent)))extentWrong=true;
    if(tag!=want.arrays)arraysWrong=true;
    // A copied run arrives byte for byte; a shape without one has no list here.
    if(want.runBytes){
        std::uint32_t sum=5;const auto* bytes=static_cast<const std::uint8_t*>(shape.list);
        if(!bytes||shape.listBytes!=want.runBytes)runWrong=true;
        else{for(std::uint32_t i=0;i<shape.listBytes;++i)sum=(sum^bytes[i])*16777619u;if(sum!=want.runSum)runWrong=true;}
    }else if(shape.list||shape.listBytes)runWrong=true;
    // A job keeps its number and arrives in its place among the shapes.
    if(shape.job!=want.job)runWrong=true;
    // The thread that draws has the GPU.
    const bool game=std::this_thread::get_id()==gameThread;
    if(gpuOwner.load()!=(game?0:1))ownerWrong=true;
    ++(game?byGame:byWorker);
    if(slowConsumer.load()&&!game&&index%8==0)std::this_thread::sleep_for(std::chrono::microseconds(300));
    // The draw code calls sync() (the draw sink does): a no-op on the consumer.
    activeQueue->sync();
    ++drawnCount;
}
std::mutex gate;std::condition_variable workerWake,gameWake;bool workerSignal{},gameSignal{};
gxworker::Platform threadedPlatform(){
    gxworker::Platform p;
    p.wakeWorker=+[]{{std::lock_guard lock(gate);workerSignal=true;}workerWake.notify_one();};
    p.waitForWorker=+[](unsigned microseconds){std::unique_lock lock(gate);
        gameWake.wait_for(lock,std::chrono::microseconds(microseconds),[]{return gameSignal;});gameSignal=false;};
    p.workerProgress=+[]{{std::lock_guard lock(gate);gameSignal=true;}gameWake.notify_one();};
    p.workerIdle=+[](unsigned microseconds){std::unique_lock lock(gate);
        workerWake.wait_for(lock,std::chrono::microseconds(microseconds),[]{return workerSignal;});workerSignal=false;};
    p.gpuToWorker=+[]{gpuOwner=1;};p.gpuToGame=+[]{gpuOwner=0;};
    p.drained=+[]{
        if(gpuOwner.load()!=1||std::this_thread::get_id()==gameThread)ownerWrong=true;
        static thread_local unsigned calls;
        std::this_thread::sleep_for(std::chrono::microseconds(++calls%3?20:200));
        if(gpuOwner.load()!=1)ownerWrong=true;
        ++drainedCalls;
    };
    // A queue that never hands the GPU back fails the test (after the
    // queue's own 20 s) instead of hanging it.
    p.now=+[]{return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    p.workerStuck=+[]{std::printf("FAIL the worker did not stop between two shapes within 20 s\n");std::fflush(stdout);std::_Exit(1);};
    return p;
}
// syncEvery: the game thread syncs after one shape in so many (a large value
// lets the ring fill: the game thread then draws what is queued itself).
// partial (Alpha 15): a full ring draws on the game thread only until the
// record fits. overlapEvery: after one shape in so many the painter "ends"
// (beginOverlap), the game thread works on while the worker draws, then a
// targeted sync (heap, deletion, creation, vertex) or the picture's close.
void queueRun(gxworker::Mode mode,bool verify,unsigned shapes,const char* name,unsigned syncEvery=9,bool mustFill=false,
              bool partial=false,unsigned overlapEvery=0){
    static CommandMemory memory;
    auto* p=new CommandProcessor(memory);
    MemoryHandle handle{};(void)memory.add({reinterpret_cast<const u8*>(matrices),sizeof(matrices)},handle);
    (void)memory.add({reinterpret_cast<const u8*>(matricesB),sizeof(matricesB)},handle);
    CHECK(p->bindArray(12,matrices,sizeof(matrices),48,true));CHECK(p->bindArray(13,matrices,sizeof(matrices),48,true));
    static std::uint32_t logWords[1024];p->attachBankLog(logWords);
    const std::size_t bytes=gxworker::Queue::minimumStorage;
    void* storage=std::aligned_alloc(8,bytes);
    auto& q=*new gxworker::Queue;activeQueue=&q;gameThread=std::this_thread::get_id();
    CHECK(q.start(mode,storage,bytes,{draw},mode==gxworker::Mode::threaded?threadedPlatform():gxworker::Platform{},verify));
    q.setPartialDrain(partial);
    std::size_t overlapsBegun=0,overlapsEmpty=0;drainedCalls=0;
    expected.assign(shapes,{});drawnCount=byWorker=byGame=0;orderBroken=bankWrong=extentWrong=arraysWrong=ownerWrong=runWrong=false;gpuOwner=0;
    std::thread worker;
    if(mode==gxworker::Mode::threaded)worker=std::thread([&]{q.run();});
    TargetExtent extent{};std::uint32_t arraysTag=0;std::size_t syncs=0,refusedWhileOff=0;
    // The bindings a shape is drawn with are the ones captured when the
    // vertex setup epoch last moved (they cannot change without it).
    std::uint32_t capturedTag=0,capturedEpoch=0;bool captured=false;
    for(unsigned n=0;n<shapes;++n){
        for(unsigned s=pick(4);s;--s)step(*p);
        if(!pick(50)){extent.logicalWidth=64+pick(400);extent.pixelWidth=extent.logicalWidth;}
        if(!pick(30)){ // new arrays: the setup epoch moves
            ++arraysTag;CHECK(p->bindArray(12,arraysTag&1?matricesB:matrices,sizeof(matrices),48,true));
        }
        if(!pick(700)){
            // Turned off and on again between two shapes (the automatic test
            // does): nothing is accepted while off, and the stores made
            // meanwhile still reach the copy with the next shape.
            q.accept(false);CHECK(!q.busy()&&gpuOwner.load()==0&&drawnCount.load()==n);
            gxworker::QueuedShape none;
            CHECK(!q.push(p->registers(),p->bankStores(),p->currentArrayBindings(),extent,none));++refusedWhileOff;
            for(unsigned s=pick(40);s;--s)step(*p);
            q.accept(true);
        }
        gxworker::QueuedShape shape;shape.generation=n;
        CommandProcessor::ArrayBindings current=p->arrayBindings();
        CHECK(!std::memcmp(&current,&p->currentArrayBindings(),sizeof(current)));
        // The test's tag travels in the first bytes of the bindings.
        const std::uint32_t tag=arraysTag;std::memcpy(current.bytes.data(),&tag,4);
        if(!captured||capturedEpoch!=p->registers().vertexSetupEpoch){captured=true;capturedEpoch=p->registers().vertexSetupEpoch;capturedTag=tag;}
        // One shape in five is a copied primitive run of 1 to 3,000 bytes
        // (now and then the largest one the queue takes); the buffer it
        // came from is overwritten right after the push.
        static std::vector<std::uint8_t> run(64*1024);
        std::uint32_t runBytes=0,runSum=5;
        if(!pick(5)){
            runBytes=!pick(400)?std::uint32_t(run.size()):1+pick(3000);
            for(std::uint32_t i=0;i<runBytes;++i){run[i]=std::uint8_t(random32());runSum=(runSum^run[i])*16777619u;}
        }
        // One copied record in three is a job (its arguments are the bytes).
        if(runBytes&&!pick(3))shape.job=1+pick(7);
        expected[n]={sumOf(p->registers()),extent,capturedTag,runBytes,runSum,shape.job};
        const bool queued=q.push(p->registers(),p->bankStores(),current,extent,shape,runBytes?run.data():nullptr,runBytes);
        if(runBytes)std::memset(run.data(),0xee,runBytes);
        CHECK(queued);p->restartBankLog();
        if(overlapEvery&&!pick(overlapEvery)){
            // The painter's end: the worker goes on, the game thread runs
            // logic that touches nothing queued, then a sync ends it.
            if(q.beginOverlap()){
                ++overlapsBegun;
                CHECK(q.overlapping()&&q.busy());
                for(unsigned s=pick(40);s;--s)step(*p); // stores of the next tick go to the game bank only
                std::this_thread::sleep_for(std::chrono::microseconds(pick(400)));
                static const gxworker::SyncCause ends[]{gxworker::SyncCause::frameEnd,gxworker::SyncCause::frameEnd,gxworker::SyncCause::heap,
                    gxworker::SyncCause::deletion,gxworker::SyncCause::creation,gxworker::SyncCause::vertex,gxworker::SyncCause::gpu};
                q.sync(ends[pick(7)]);
            }else{++overlapsEmpty;q.sync(gxworker::SyncCause::painterEnd);}
            ++syncs;
            CHECK(!q.overlapping()&&!q.busy()&&gpuOwner.load()==0);
            CHECK(drawnCount.load()==n+1);
        }else if(!pick(syncEvery)){
            q.sync();++syncs;
            CHECK(!q.busy()&&gpuOwner.load()==0);
            CHECK(drawnCount.load()==n+1);
        }
    }
    q.sync();
    CHECK(!q.busy()&&gpuOwner.load()==0);
    if(mode==gxworker::Mode::threaded){q.stop();worker.join();}
    CHECK(!orderBroken.load());CHECK(!bankWrong.load());CHECK(!extentWrong.load());CHECK(!arraysWrong.load());CHECK(!ownerWrong.load());CHECK(!runWrong.load());
    CHECK(drawnCount.load()==shapes);
    const auto& c=q.counters();
    CHECK(c.queued==shapes&&c.drawnByWorker+c.drawnByGame==shapes&&c.unmarkedBlocks==0&&c.sumMismatches==0&&c.damaged==0);
    CHECK(c.drawnByWorker==byWorker.load()&&c.drawnByGame==byGame.load());
    CHECK(c.wraps>0&&c.runs>0);
    // A run of half the ring is refused (the caller draws it itself).
    {gxworker::QueuedShape none;static std::uint8_t tooLarge[128*1024];
     CHECK(!q.push(p->registers(),p->bankStores(),p->currentArrayBindings(),extent,none,tooLarge,sizeof(tooLarge)));
     CHECK(q.counters().queued==shapes);}
    if(mode==gxworker::Mode::threaded)CHECK(c.drawnByWorker>0);else CHECK(c.drawnByWorker==0);
    // A consumer that falls behind is relieved by the game thread.
    if(slowConsumer.load())CHECK(c.takeovers>0&&c.drawnByGame>0);
    if(mustFill)CHECK(c.ringFull>0);
    // A full ring with the partial rule hands back only part of the queue.
    if(partial&&mustFill&&mode==gxworker::Mode::threaded)CHECK(c.partialDrains>0);
    if(!partial)CHECK(c.partialDrains==0);
    if(overlapEvery){
        CHECK(c.overlaps==overlapsBegun);
        std::uint64_t ended=0;for(const auto value:c.overlapEndsBy)ended+=value;
        CHECK(ended==overlapsBegun&&c.overlapsFinished<=c.overlapEndsBy[unsigned(gxworker::SyncCause::frameEnd)]);
        // Each overlap whose last shape the worker drew before the sync
        // let it submit at most once.
        CHECK(drainedCalls.load()<=overlapsBegun);
        if(mode==gxworker::Mode::threaded&&overlapsBegun>20)CHECK(drainedCalls.load()>0&&c.overlapsFinished>0);
        std::printf("  overlaps %zu (painter ended with nothing left %zu), finished by the worker before the close %llu, worker submissions %zu, "
            "game drew %llu shapes ending them\n",overlapsBegun,overlapsEmpty,static_cast<unsigned long long>(c.overlapsFinished),drainedCalls.load(),
            static_cast<unsigned long long>(c.overlapShapesByGame));
    }else CHECK(c.overlaps==0);
    if(partial)std::printf("  partial ring drains %llu\n",static_cast<unsigned long long>(c.partialDrains));
    std::printf("PASS %s: %u shapes in order under their own bank, extent and bindings (%llu by the worker, %llu by the game thread); "
        "%llu copied runs (%llu bytes) arrived byte for byte; "
        "%zu syncs with %llu takeovers, ring wrapped %llu times and filled %llu times, %llu delta words, %llu whole banks, turned off %zu times\n",
        name,shapes,static_cast<unsigned long long>(c.drawnByWorker),static_cast<unsigned long long>(c.drawnByGame),
        static_cast<unsigned long long>(c.runs),static_cast<unsigned long long>(c.runBytes),syncs,
        static_cast<unsigned long long>(c.takeovers),static_cast<unsigned long long>(c.wraps),static_cast<unsigned long long>(c.ringFull),
        static_cast<unsigned long long>(c.deltaWords),static_cast<unsigned long long>(c.fullBanks),refusedWhileOff);
    std::free(storage);delete p;
}
}
int main(int argc,char** argv){
    const bool threadsOnly=argc>1&&!std::strcmp(argv[1],"threads");
    if(!threadsOnly){
        marks();
        adoptWhileRecording();
        queueRun(gxworker::Mode::deferred,false,20000,"deferred queue");
        queueRun(gxworker::Mode::deferred,true,6000,"deferred queue with verify");
        queueRun(gxworker::Mode::deferred,false,20000,"deferred queue, ring filled",4000,true);
    }
    queueRun(gxworker::Mode::threaded,false,threadsOnly?6000:20000,"queue with a consumer thread");
    queueRun(gxworker::Mode::threaded,true,threadsOnly?3000:6000,"queue with a consumer thread and verify");
    // A sync after every shape: the game thread hands the queue back and
    // asks for it again while the worker still looks for the hand-back (a
    // lost hand-back hung a worker that did not sleep in an Alpha 10 draft).
    queueRun(gxworker::Mode::threaded,false,threadsOnly?20000:40000,"queue with a consumer thread, a sync after every shape",1);
    slowConsumer=true;
    queueRun(gxworker::Mode::threaded,false,threadsOnly?4000:12000,"queue with a slow consumer thread",9);
    queueRun(gxworker::Mode::threaded,false,threadsOnly?4000:12000,"queue with a slow consumer thread, ring filled",4000,true);
    // Alpha 15: a full ring hands back only part of the queue; the painter's
    // end leaves the worker drawing during the next logic tick.
    queueRun(gxworker::Mode::threaded,false,threadsOnly?4000:12000,"queue with a slow consumer thread, ring filled, partial drains",4000,true,true);
    queueRun(gxworker::Mode::threaded,false,threadsOnly?4000:12000,"queue with a slow consumer thread, overlaps",4000,false,true,60);
    slowConsumer=false;
    queueRun(gxworker::Mode::threaded,false,threadsOnly?6000:20000,"queue with a consumer thread, overlaps",4000,false,true,25);
    queueRun(gxworker::Mode::threaded,true,threadsOnly?3000:6000,"queue with a consumer thread, overlaps and verify",4000,false,true,25);
    if(!threadsOnly)queueRun(gxworker::Mode::deferred,false,6000,"deferred queue: no overlap without a worker",4000,false,true,25);
    std::printf("gx_worker_test: all passed\n");
    return 0;
}
