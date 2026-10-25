#include "bg_budget_experiment.hpp"
#include "native_heap_partition.hpp"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include <climits>
namespace dusk::bg76 {
namespace {
// Admission/charge limits stay fixed; owner capacity has a separate ceiling.
constexpr std::uint32_t maxGame=4513792,slots=32,ring=64;
struct Row { Context context{}; JKRSolidHeap* heap=nullptr; Snapshot before{}; std::uint32_t gen=0,cap=0,request=0,charged=0,used=0,payload=0; int callback=-1,postTotal=-1; bool finished=false,outerFailed=false; };
Row rows[slots]{};
Event events[ring]{};
Config config{};
JKRExpHeap *game=nullptr,*archive=nullptr;
std::uintptr_t gameThread=0;
std::uint32_t generation=0,live=0,readIndex=0,count=0,drops=0,emitted=0,frame=0;
bool initialized=false,on=false,exhausted=false;
void drop(){if(drops!=UINT32_MAX)++drops;}
Snapshot snapshot(JKRExpHeap* room) {
    return {game->getTotalFreeSize(),game->getFreeSize(),archive?archive->getTotalFreeSize():-1,
        archive?archive->getFreeSize():-1,room?room->getTotalFreeSize():-1,room?room->getFreeSize():-1};
}
void event(Row& r,unsigned kind,int result) {
    if(count==ring || emitted+count>=256){drop();return;}
    auto& e=events[(readIndex+count)%ring]; ++count;
    e={frame,kind,r.gen,r.context.id,r.cap,live,r.request,r.charged,r.used,r.payload,result,r.context.room,r.context.block,r.callback,r.postTotal,
        reinterpret_cast<std::uintptr_t>(r.context.actor),reinterpret_cast<std::uintptr_t>(r.heap),
        reinterpret_cast<std::uintptr_t>(r.context.roomHeap),reinterpret_cast<std::uintptr_t>(r.context.archiveOwner),
        reinterpret_cast<std::uintptr_t>(r.context.resourceOwner),r.before,snapshot(r.context.roomHeap)};
}
Row* find(Ticket t){return t.slot<slots&&t.generation&&rows[t.slot].gen==t.generation?&rows[t.slot]:nullptr;}
std::uint32_t overhead(){return 2*sizeof(JKRExpHeap::CMemBlock)+31;}
std::uint32_t charge(JKRSolidHeap* h){
    auto size=game->getSize(h);
    if(size<0 || std::uint32_t(size)>maxGame-overhead())return UINT32_MAX;
    // Includes actual parent allocation size, block header, alignment and
    // a second header allowance. Never infer costs from racing free deltas.
    return std::uint32_t(size)+sizeof(JKRExpHeap::CMemBlock)+15;
}
bool valid(Config c){return c.actor%32==0 && c.aggregate%32==0 && c.headroom%32==0 &&
    c.actor>=((sizeof(JKRSolidHeap)+15)/16*16)+overhead()+32 &&
    c.actor<=c.aggregate && c.aggregate<=maxGame && c.headroom>0 && c.headroom<=maxGame;}
}
bool parse(const char* p,std::size_t n,Config& out){
    out={}; if(!p||n>96)return false;
    Config c{};std::size_t pos=0;
    auto token=[&](const char* s){while(*s){if(pos==n||p[pos++]!=*s++)return false;}return true;};
    auto number=[&](std::uint32_t& v){
        if(pos==n||p[pos]<'1'||p[pos]>'9')return false;
        v=0; do{auto d=unsigned(p[pos++]-'0');if(v>(UINT32_MAX-d)/10)return false;v=v*10+d;}
        while(pos<n&&p[pos]>='0'&&p[pos]<='9');return true;
    };
    if(!token("BG76 actor=")||!number(c.actor)||!token(" aggregate=")||!number(c.aggregate)||
       !token(" headroom=")||!number(c.headroom)||!token("\n")||pos!=n||!valid(c))return false;
    out=c;return true;
}
bool initialize(const Config* c,JKRExpHeap* g,JKRExpHeap* a){
    if(initialized)return false;
    initialized=true;gameThread=threadToken();
#if DEBUG || VERSION != 0
    (void)c;(void)g;(void)a; return false;
#else
    if(!gameThread||!c||!valid(*c)||!g||g->getHeapType()!='EXPH'||g->getHeapSize()>dusk::native::heap64::maxGame||
        c->aggregate>g->getHeapSize()||c->headroom>g->getHeapSize())return false;
    config=*c;game=g;archive=a;on=true;return true;
#endif
}
void setFrame(std::uint32_t value){if(on&&owner())frame=value;}
bool enabled(){return on;}
bool owner(){return !on || (gameThread && gameThread==threadToken());}
bool mayCreate(void* a,std::uint32_t id){
    if(!on)return true;if(!owner())return false;auto* r=find(ticket(a,id));
    return !r || (r->heap && r->finished && r->callback==1 && !r->outerFailed);
}
bool releasable(Ticket t,void* a,std::uint32_t id,JKRSolidHeap* h){
    if(!on||!owner())return false;auto* r=find(t);
    return r&&r->context.actor==a&&r->context.id==id&&r->heap==h&&JKRHeap::getCurrentHeap()!=h;
}
Config configValues(){return config;}
std::uintptr_t gameOwner(){return reinterpret_cast<std::uintptr_t>(game);}
std::uintptr_t archiveOwner(){return reinterpret_cast<std::uintptr_t>(archive);}
std::uint32_t reservation(){return on?config.actor:0;}
std::uint32_t liveReserved(){return owner()?live:UINT32_MAX;}
std::uint32_t dropped(){return owner()?drops:UINT32_MAX;}
std::size_t ledgerBytes(){return sizeof(rows);}
std::size_t fixedBytes(){return sizeof(rows)+sizeof(events)+sizeof(config)+sizeof(game)+sizeof(archive)+sizeof(gameThread)+7*sizeof(std::uint32_t)+4*sizeof(bool);}
Ticket ticket(void* actor,std::uint32_t id){
    if(!on||!owner())return {};
    for(unsigned i=0;i<slots;++i)if(rows[i].gen&&rows[i].context.actor==actor&&rows[i].context.id==id)return {i,rows[i].gen};
    return {};
}
JKRSolidHeap* begin(Context c,std::uint32_t cap,bool exactMode,Ticket& t){
    t={}; if(!on||!owner()||!c.actor||!c.roomHeap||exhausted)return nullptr;
    if(ticket(c.actor,c.id).generation)return nullptr; // one attempt, including rejected/failed callbacks
    unsigned i=0;for(;i<slots&&rows[i].gen;++i){}
    if(i==slots||generation==UINT32_MAX){exhausted=true;drop();return nullptr;}
    Row& r=rows[i]; r={};r.context=c;r.gen=++generation;r.before=snapshot(c.roomHeap);t={i,r.gen};
    if(exactMode||!cap||cap%32||cap>config.actor||cap<((sizeof(JKRSolidHeap)+15)/16*16)+overhead()+32||
       cap>config.aggregate-live){event(r,1,-2);return nullptr;}
    r.cap=cap;r.request=(cap-overhead())/32*32;
    if(r.before.gameTotal<0 || std::uint32_t(r.before.gameTotal)<config.headroom ||
       cap>std::uint32_t(r.before.gameTotal)-config.headroom ||
       game->getMaxAllocatableSize(16)<r.request){r.cap=0;event(r,1,-3);return nullptr;}
    live+=cap; // reserve before allocation; do not change process current heap on rejects
    auto* old=JKRHeap::getCurrentHeap();
    auto* h=JKRSolidHeap::create(r.request,game,false);
    JKRHeap::setCurrentHeap(old); // JKR constructor may change root-current heap
    if(!h){live-=cap;r.cap=0;event(r,1,-4);return nullptr;}
    auto charged=charge(h); auto total=game->getTotalFreeSize();r.charged=charged;r.postTotal=total;
    if(charged>cap || total<0 || std::uint32_t(total)<config.headroom){
        h->destroy();live-=cap;r.cap=0;event(r,1,-5);return nullptr;
    }
    r.heap=h;r.charged=charged;event(r,2,1);return h; // publication only after post-allocation check
}
void finish(Ticket t,int result){
    if(!on||!owner())return;auto* r=find(t);if(!r||!r->heap||r->finished)return;
    r->finished=true;r->callback=result;
    r->used=r->heap->getHeapSize()-r->heap->getFreeSize();
    if(result==1)r->heap->adjustSize(); // existing allocator, single callback, no trial/retry
    r->payload=r->heap->getHeapSize();r->charged=charge(r->heap);
    event(*r,3,result); // failed callback retained for ordinary BG cleanup, never COMPLETE
}
void outer(void* a,std::uint32_t id,int result){if(!on||!owner())return;auto* r=find(ticket(a,id));if(r){if(result==5)r->outerFailed=true;event(*r,4,result);}}
bool release(Ticket t,void* a,std::uint32_t id,JKRSolidHeap*& h){
    if(!on||!owner())return false; auto* r=find(t);
    if(!r||r->context.actor!=a||r->context.id!=id||r->heap!=h)return false;
    if(h){if(JKRHeap::getCurrentHeap()==h)return false;h->destroy();h=nullptr;}
    live-=r->cap; event(*r,5,1); // capacity reusable only AFTER correct owner storage release
    *r={};return true;
}
bool take(Event& e){if(!on||!owner()||!count)return false;e=events[readIndex];readIndex=(readIndex+1)%ring;--count;++emitted;return true;}
}
