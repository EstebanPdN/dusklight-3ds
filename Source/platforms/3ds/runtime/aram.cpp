#include <dolphin/ar.h>
#include <dolphin/os.h>
#include <cstdlib>
#include <cstring>
#include <limits>
namespace {
constexpr u32 bytes=16*1024*1024,base=0x4000;
u8* storage{};u32 cursor=base,*lengths{},entries{},used{};
struct Mutex {OSMutex m;Mutex(){OSInitMutex(&m);}};
struct Guard {OSMutex* m;Guard(){static Mutex mutex;m=&mutex.m;OSLockMutex(m);}~Guard(){OSUnlockMutex(m);}};
void require(bool ok,const char* text){if(!ok)OSPanic(__FILE__,__LINE__,"%s",text);}
}
extern "C" {
u32 ARInit(u32* stack,u32 count){Guard guard;if(storage)return base;require(stack&&count,"Invalid ARAM allocation stack");storage=static_cast<u8*>(std::calloc(1,bytes));require(storage,"Native ARAM allocation failed");lengths=stack;entries=count;return base;}
BOOL ARCheckInit(){return storage!=nullptr;}
u32 ARGetSize(){return storage?bytes:0;}
void* ARGetStorageAddress(){return storage;}
u32 ARAlloc(u32 length){Guard guard;require(storage&&length&&!(length&31)&&used<entries&&length<=bytes-cursor,"Native ARAM exhausted or invalid allocation");u32 result=cursor;cursor+=length;lengths[used++]=length;return result;}
u32 ARFree(u32* length){Guard guard;require(storage&&used,"Native ARAM stack underflow");auto size=lengths[--used];cursor-=size;if(length)*length=size;return cursor;}
void ARQInit(){require(storage,"ARAM queue requires storage");}
void ARQPostRequest(ARQRequest* request,u32 owner,u32 type,u32 priority,uintptr_t source,uintptr_t dest,u32 length,ARQCallback callback){
    {
        Guard guard;const auto offset=type==ARAM_DIR_MRAM_TO_ARAM?dest:source;
        const auto host=type==ARAM_DIR_MRAM_TO_ARAM?source:dest;
        require(storage&&request&&type<=1&&priority<=1&&host&&offset<=bytes&&length<=bytes-offset&&length<=UINTPTR_MAX-host,"Invalid native ARAM transfer");
        *request={nullptr,owner,type,priority,static_cast<u32>(source),static_cast<u32>(dest),length,callback};
        if(type==ARAM_DIR_MRAM_TO_ARAM)std::memcpy(storage+offset,reinterpret_cast<void*>(host),length);
        else std::memcpy(reinterpret_cast<void*>(host),storage+offset,length);
    }
    // The CPU copy has completed before the original callback sees its request.
    if(callback)callback(reinterpret_cast<uintptr_t>(request));
}
}
