#pragma once
#include <cstddef>
#include <cstdint>
class JKRHeap;
class JKRExpHeap;
class JKRSolidHeap;
namespace dusk::bg76 {
// EXPERIMENTAL. No production budget defaults. initialize only on game owner.
struct Config { std::uint32_t actor, aggregate, headroom; };
struct Ticket { std::uint32_t slot=32, generation=0; };
struct Snapshot { std::int32_t gameTotal,gameLargest,archiveTotal,archiveLargest,roomTotal,roomLargest; };
struct Context { void* actor; std::uint32_t id; int room,block; JKRExpHeap* roomHeap; JKRHeap* archiveOwner; JKRHeap* resourceOwner; };
struct Event {
    std::uint32_t frame,kind,generation,id,cap,live,request,charged,used,payload;
    std::int32_t result,room,block,callback,postTotal;
    std::uintptr_t actor,heap,roomHeap,archiveOwner,resourceOwner;
    Snapshot before,after;
};
bool parse(const char*,std::size_t,Config&);
bool initialize(const Config*,JKRExpHeap*,JKRExpHeap*);
bool enabled();
bool owner();
bool mayCreate(void*,std::uint32_t);
bool releasable(Ticket,void*,std::uint32_t,JKRSolidHeap*);
Ticket ticket(void*,std::uint32_t);
JKRSolidHeap* begin(Context,std::uint32_t reservation,bool exactMode,Ticket&);
void finish(Ticket,int callbackResult);
void outer(void*,std::uint32_t,int result);
bool release(Ticket,void*,std::uint32_t,JKRSolidHeap*&); // AFTER actual BG destructor.
Config configValues();
std::uintptr_t gameOwner();
std::uintptr_t archiveOwner();
std::uint32_t reservation();
std::uint32_t liveReserved();
std::uint32_t dropped();
bool take(Event&);
std::size_t ledgerBytes();
std::size_t fixedBytes();
void initializeFromSd();
void setFrame(std::uint32_t);
void flush();
std::uintptr_t threadToken(); // explicit OS adapter, no allocation
}
