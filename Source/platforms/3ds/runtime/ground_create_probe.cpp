#include "ground_create_probe.hpp"
#if defined(DUSK_TARGET_3DS) && DUSK_GROUND_CREATE_PROBE
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
extern "C" void OSReport(const char*, ...);
namespace dusk::native {
namespace {
using U = std::uint32_t;
struct Bg {
    U id,parent,actor,roomHeap,solidHeap,branch;
    int room;
    U attempts,callbacks,callbackFailures,present,created,failure,failPresent,failCreated,failHeap;
    int model,callback;
    U lookup,res[3],collider[2],regCalls[2],generation[2],used[2],releases[2],releaseFrame[2];
    int regResult[2],slot[2],outer;
    U outerCalls;
};
struct Collider { U token,actor,generation,type,before,after,hit; int slot; };
struct Query {
    GroundLinkInput in;
    GroundPosition after,oldAfter,point;
    U queryToken,frame,flagsAfter,count,dispatchCalls,counts[5],summaries,summaryDrops;
    U flags,f90,f94,offset,sameActor,queryFlags,passBits,height;
    int poly,bg;
    bool pending,finished,awaitGate,gateKnown,noGround,queryActive;
    Collider collider[8];
};
struct State {
    // Lock-free identity publication: rejected workers never read/write the remaining State.
    std::atomic<std::uintptr_t> owner{0};
    Bg bg[8]{};
    Query q{};
    std::uintptr_t scope=0;
    U frame=0,rows=0,droppedSlots=0,missing=0,protocol=0,generation=0,reports=0,outputDrops=0,summaryDrops=0;
    bool initialized=false,enabled=false,inFrame=false,firstSeen=false,boundaryTaken=false,failure=false,retainSuccessDetails=false;
};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free, "owner guard must not use locks");
static_assert(sizeof(State) + sizeof(bool) <= 4096, "ground probe plus SD once flag exceeds 4KiB");
State state;
void inc(U& n) { if(n != std::numeric_limits<U>::max()) ++n; }
U token(const void* p) { return static_cast<U>(reinterpret_cast<std::uintptr_t>(p)); }
bool owner() { const auto p=state.owner.load(std::memory_order_acquire); return p && p==groundProbeThreadToken(); }
Bg* row(U id) {
    for(U i=0;i<state.rows;++i) if(state.bg[i].id==id) return &state.bg[i];
    inc(state.missing); return nullptr;
}
bool query(const void* p) {
    return groundProbeEnabled() && state.scope==reinterpret_cast<std::uintptr_t>(p) && state.scope && state.q.pending && !state.q.finished;
}
bool dispatch(const void* p) { return query(p) && state.q.queryActive; }
// Publication-only stack state; no hook formats or emits. OSReport's native formatter is 256 bytes.
struct Writer {
    U lines=0,bytes=0;
    void line(const char* fmt, ...) {
        char text[256]; va_list ap; va_start(ap,fmt);
        int n=std::vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
        if(n<0 || static_cast<unsigned>(n)>=sizeof(text) || lines>=48 || bytes+static_cast<U>(n)>8192) { inc(state.outputDrops); return; }
        ++lines; bytes+=static_cast<U>(n); OSReport("%s",text);
    }
};
void emit(bool boundary) {
    if(state.reports>=3) { inc(state.protocol); return; }
    ++state.reports; Writer w;
    const Query& q=state.q;
    const bool details=state.retainSuccessDetails || state.failure || (q.pending && q.gateKnown && q.noGround);
    w.line("GCP1 BEGIN n=%u frame=%u sample=%u boundary=%u details=%u state=%u\n",state.reports,state.frame,q.pending?q.frame:0,boundary,details,static_cast<U>(sizeof(State)));
    if(details) {
        for(U i=0;i<state.rows;++i) {
            const Bg& b=state.bg[i];
            w.line("GCP1 BG id=%u room=%d parent=%u actor=%08x branch=%u roomheap=%08x solid=%08x attempts=%u cb=%u rt=%d cbfail=%u outer=%d outercalls=%u\n",b.id,b.room,b.parent,b.actor,b.branch,b.roomHeap,b.solidHeap,b.attempts,b.callbacks,b.callback,b.callbackFailures,b.outer,b.outerCalls);
            w.line("GCP1 SETUP id=%u present=%x created=%x fail=%u model=%d failpresent=%x failcreated=%x failheap=%08x lookup=%x dzb=%08x kcl=%08x plc=%08x\n",b.id,b.present,b.created,b.failure,b.model,b.failPresent,b.failCreated,b.failHeap,b.lookup,b.res[0],b.res[1],b.res[2]);
            for(U k=0;k<2;++k) w.line("GCP1 COLL id=%u type=%u ptr=%08x regcalls=%u rt=%d slot=%d used=%u gen=%u release=%u releaseframe=%u\n",b.id,k+1,b.collider[k],b.regCalls[k],b.regResult[k],b.slot[k],b.used[k],b.generation[k],b.releases[k],b.releaseFrame[k]);
        }
        if(q.pending) {
            w.line("GCP1 LINK id=%u param=%08x mode=%d point=%d query=%08x gateknown=%u noground=%u finished=%u calls=%u dispatchcalls=%u\n",q.in.id,q.in.param,q.in.mode,q.in.point,q.queryToken,q.gateKnown,q.noGround,q.finished,q.count,q.dispatchCalls);
            w.line("GCP1 POS current=%08x,%08x,%08x old=%08x,%08x,%08x\n",q.in.pos.x,q.in.pos.y,q.in.pos.z,q.in.old.x,q.in.old.y,q.in.old.z);
            w.line("GCP1 AFTER current=%08x,%08x,%08x old=%08x,%08x,%08x\n",q.after.x,q.after.y,q.after.z,q.oldAfter.x,q.oldAfter.y,q.oldAfter.z);
            w.line("GCP1 ACCH flags=%08x after=%08x f90=%08x f94=%08x offset=%08x\n",q.in.flags,q.flagsAfter,q.in.f90,q.in.f94,q.in.offset);
            w.line("GCP1 QUERY xyz=%08x,%08x,%08x height=%08x poly=%d bg=%d flags=%08x f90=%08x f94=%08x offset=%08x same=%u qflags=%08x pass=%x\n",q.point.x,q.point.y,q.point.z,q.height,q.poly,q.bg,q.flags,q.f90,q.f94,q.offset,q.sameActor,q.queryFlags,q.passBits);
            w.line("GCP1 DISPATCH used=%u notready=%u sameactor=%u dispatched=%u hits=%u kept=%u dropped=%u\n",q.counts[0],q.counts[1],q.counts[2],q.counts[3],q.counts[4],q.summaries,q.summaryDrops);
            for(U i=0;i<q.summaries;++i) { const Collider& c=q.collider[i]; w.line("GCP1 HIT slot=%d actor=%u ptr=%08x gen=%u type=%u before=%08x after=%08x hit=%u\n",c.slot,c.actor,c.token,c.generation,c.type,c.before,c.after,c.hit); }
        }
    }
    bool complete=!state.droppedSlots && !state.missing && !state.protocol && !state.outputDrops && !state.summaryDrops && (!q.pending || (q.finished && q.gateKnown));
    w.line("GCP1 END n=%u rows=%u dropslots=%u missing=%u protocol=%u outdrops=%u complete=%u window=%u querypresent=%u calls=%u gate=%u noground=%u sumdrops=%u\n",state.reports,state.rows,state.droppedSlots,state.missing,state.protocol,state.outputDrops,complete,state.frame==480,q.pending,q.count,q.gateKnown,q.noGround,state.summaryDrops);
}
}
U groundBits(float f) { U v; static_assert(sizeof(f)==sizeof(v)); std::memcpy(&v,&f,sizeof(v)); return v; }
bool parseGroundProbeConfig(const char* p,std::size_t n) { constexpr char text[]="ground-create-probe=1\n"; return p && n==sizeof(text)-1 && !std::memcmp(p,text,n); }
bool parseGroundProbeSuccessConfig(const char* p,std::size_t n) { constexpr char text[]="ground-create-probe=2\n"; return p && n==sizeof(text)-1 && !std::memcmp(p,text,n); }
void initializeGroundProbe(bool enabled) { initializeGroundProbe(enabled,false); }
void initializeGroundProbe(bool enabled,bool retainSuccessDetails) {
    // Sole-owner initialization contract; immutable owner publication follows all initial writes.
    if(state.owner.load(std::memory_order_acquire)) return;
    if(state.initialized) return;
    state.initialized=true; state.enabled=enabled; state.retainSuccessDetails=enabled && retainSuccessDetails;
    state.owner.store(groundProbeThreadToken(),std::memory_order_release);
}
bool groundProbeEnabled() { return owner() && state.enabled; }
void groundProbeBegin(U frame) {
    if(!groundProbeEnabled()) return;
    if(state.inFrame || frame!=state.frame+1 || frame>480) { inc(state.protocol); state.enabled=false; return; }
    state.frame=frame; state.inFrame=true; state.boundaryTaken=false;
}
void groundProbeEnd() {
    if(!groundProbeEnabled()) return;
    if(!state.inFrame || state.scope) { inc(state.protocol); state.enabled=false; return; }
    bool boundary=state.frame==300 || state.frame==480;
    if(state.q.pending || boundary) {
        emit(boundary);
        // First selected CrrPos is immutable until publication, even across startup/frame begin.
        state.q={};
    }
    state.inFrame=false;
    if(state.frame==480) state.enabled=false;
}
void groundProbeClose() { if(owner()) { state.enabled=false; state.scope=0; } }
void groundBgBegin(U id,int room,U parent,const void* actor,const void* heap) {
    if(!groundProbeEnabled()) return;
    for(U i=0;i<state.rows;++i) if(state.bg[i].id==id) return;
    if(state.rows==8) { inc(state.droppedSlots); return; }
    Bg& b=state.bg[state.rows++]; b.id=id;b.room=room;b.parent=parent;b.actor=token(actor);b.roomHeap=token(heap);b.branch=heap?1:2;
    b.model=-1;b.callback=-1;b.outer=-1;for(unsigned k=0;k<2;++k){b.regResult[k]=-1;b.slot[k]=-1;}
}
void groundBgHeapBegin(U id,const void* heap) { if(!groundProbeEnabled())return; if(auto* b=row(id)){inc(b->attempts);b->solidHeap=token(heap);b->present=0;b->created=0;} }
void groundBgHeapResult(U id,int rt,const void* heap) { if(!groundProbeEnabled())return;if(auto* b=row(id)){inc(b->callbacks);b->callback=rt;b->solidHeap=token(heap);if(rt!=1){inc(b->callbackFailures);if(!b->failure)inc(state.missing);state.failure=true;}} }
void groundBgModel(U id,int i,bool present,bool created) { if(!groundProbeEnabled())return;if(i<0||i>=6){inc(state.protocol);return;}if(auto* b=row(id)){if(present)b->present|=1u<<i;if(created)b->created|=1u<<i;} }
void groundBgFailure(U id,GroundFailure why,int model) { if(!groundProbeEnabled())return;state.failure=true;if(auto* b=row(id))if(!b->failure){b->failure=static_cast<U>(why);b->model=model;b->failPresent=b->present;b->failCreated=b->created;b->failHeap=b->solidHeap;} }
void groundBgResource(U id,unsigned which,const void* p) { if(!groundProbeEnabled())return;if(which>=3){inc(state.protocol);return;}if(auto* b=row(id)){b->lookup|=1u<<which;b->res[which]=token(p);} }
void groundBgCollision(U id,unsigned kind,const void* p) { if(!groundProbeEnabled())return;if(kind<1||kind>2){inc(state.protocol);return;}if(auto* b=row(id))b->collider[kind-1]=token(p); }
void groundBgRegist(U id,unsigned kind,const void* p,bool rt,int slot,bool used) {
    if(!groundProbeEnabled())return;if(kind<1||kind>2){inc(state.protocol);return;}
    if(auto* b=row(id)){unsigned k=kind-1;inc(b->regCalls[k]);b->collider[k]=token(p);b->regResult[k]=rt;b->slot[k]=slot;b->used[k]=used;
        if(!rt){if(state.generation==std::numeric_limits<U>::max())inc(state.protocol);inc(state.generation);b->generation[k]=state.generation;}else state.failure=true;}
}
void groundBgOuter(U id,int rt) { if(!groundProbeEnabled())return;if(auto* b=row(id)){b->outer=rt;inc(b->outerCalls);if(rt==5)state.failure=true;} }
void groundReleased(const void* p,int slot,U actor) {
    if(!groundProbeEnabled())return;
    for(U i=0;i<state.rows;++i)for(unsigned k=0;k<2;++k){auto& b=state.bg[i];if(b.id==actor && b.used[k] && b.collider[k] && b.collider[k]==token(p) && b.slot[k]==slot){inc(b.releases[k]);b.releaseFrame[k]=state.frame;b.used[k]=0;}}
}
GroundScope groundLinkOpen(const void* p,const GroundLinkInput& in) {
    if(!groundProbeEnabled())return {};
    GroundScope cookie{state.scope,false};
    if(!state.scope && !state.q.pending && (!state.firstSeen || ((state.frame==300||state.frame==480)&&!state.boundaryTaken))) {
        state.q={};auto& q=state.q;q.in=in;q.frame=state.frame;q.queryToken=token(p);q.pending=true;q.poly=-1;q.bg=-1;
        state.scope=reinterpret_cast<std::uintptr_t>(p);state.firstSeen=true;state.boundaryTaken=true;cookie.selected=true;
    } else state.scope=0; // Nested/repeated calls cannot mutate the frozen first selection.
    return cookie;
}
void groundLinkFinish(GroundScope cookie,GroundPosition p,GroundPosition old,U flags) {
    if(!groundProbeEnabled())return;
    if(cookie.selected){auto& q=state.q;q.after=p;q.oldAfter=old;q.flagsAfter=flags;q.finished=true;q.awaitGate=true;q.queryActive=false;}
    state.scope=cookie.previous;
}
bool groundFirstGate(bool value) {
    if(groundProbeEnabled() && state.q.awaitGate){state.q.gateKnown=true;state.q.noGround=value;state.q.awaitGate=false;}
    return value;
}
bool groundQuerySelected(const void* p) { return query(p); }
void groundQueryBefore(const void* p,float x,float y,float z,U flags,float f90,float f94,float offset,bool same,U qflags,U pass) {
    if(!query(p))return;auto& q=state.q;inc(q.count);q.queryActive=q.count==1;
    if(!q.queryActive)return;
    q.point={groundBits(x),groundBits(y),groundBits(z)};q.flags=flags;q.f90=groundBits(f90);q.f94=groundBits(f94);q.offset=groundBits(offset);q.sameActor=same;q.queryFlags=qflags;q.passBits=pass;
}
void groundQueryAfter(const void* p,float height,int poly,int bg) { if(!dispatch(p))return;auto& q=state.q;q.height=groundBits(height);q.poly=poly;q.bg=bg;q.queryActive=false; }
bool groundDispatchBegin(const void* p) { if(!dispatch(p))return false;inc(state.q.dispatchCalls);return true; }
void groundDispatchCount(const void* p,unsigned reason) { if(!dispatch(p))return;if(reason>=3){inc(state.protocol);return;}inc(state.q.counts[reason]); }
void groundDispatch(const void* p,int slot,U actor,const void* collider,float before,float after,bool hit) {
    if(!dispatch(p))return;auto& q=state.q;inc(q.counts[3]);if(hit)inc(q.counts[4]);
    if(q.summaries==8){inc(q.summaryDrops);inc(state.summaryDrops);return;}
    auto& c=q.collider[q.summaries++];c.token=token(collider);c.actor=actor;c.slot=slot;c.before=groundBits(before);c.after=groundBits(after);c.hit=hit;
    for(U i=0;i<state.rows;++i)for(unsigned k=0;k<2;++k){const auto& b=state.bg[i];if(b.id==actor&&b.collider[k]==c.token&&b.slot[k]==slot&&b.used[k]){c.generation=b.generation[k];c.type=k+1;}}
}
}
#endif
