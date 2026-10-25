#include <dolphin/card.h>
#include <dolphin/ar.h>
#include <cassert>
#include <cstdio>
#include <array>
#include <cstring>
#include <mutex>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
namespace {std::map<OSMutex*,std::recursive_mutex> locks;bool callbackRan;}
extern "C" {
void OSInitMutex(OSMutex* m){locks.try_emplace(m);}
void OSLockMutex(OSMutex* m){locks.at(m).lock();}
void OSUnlockMutex(OSMutex* m){locks.at(m).unlock();}
void OSPanic(const char*,int,const char*,...){throw std::runtime_error("Expected native contract rejection");}
}
void completed(uintptr_t p){auto* request=reinterpret_cast<ARQRequest*>(p);assert(request->length==32);callbackRan=true;}
int main(){
    // Only this test's working directory is touched; no user/game save input.
    assert(mkdir("sdmc:",0777)==0);assert(mkdir("sdmc:/3ds",0777)==0);
    CARDInit("GZ2E","01");assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);assert(CARDCheck(0)==CARD_RESULT_READY);
    CARDFileInfo file{};assert(CARDOpen(0,"zeldaTp.dat",&file)==CARD_RESULT_NOFILE);
    assert(CARDCreate(0,"zeldaTp.dat",32768,&file)==CARD_RESULT_READY);
    std::array<unsigned char,8192> data{},read{};data.fill(0xa5);
    assert(CARDWrite(&file,data.data(),data.size(),16384)==CARD_RESULT_READY);
    assert(CARDRead(&file,read.data(),read.size(),16384)==CARD_RESULT_READY&&data==read);
    CARDStat stat{};assert(CARDGetStatus(0,0,&stat)==CARD_RESULT_READY);stat.iconAddr=123;stat.commentAddr=256;
    assert(CARDSetStatus(0,0,&stat)==CARD_RESULT_READY);assert(CARDClose(&file)==CARD_RESULT_READY);
    assert(CARDUnmount(0)==CARD_RESULT_READY);assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);
    assert(CARDOpen(0,"zeldaTp.dat",&file)==CARD_RESULT_READY);assert(CARDGetStatus(0,0,&stat)==CARD_RESULT_READY&&stat.iconAddr==123);
    assert(CARDRead(&file,read.data(),read.size(),16384)==CARD_RESULT_READY&&data==read);
    assert(CARDFormat(0)==CARD_RESULT_NOPERM);assert(CARDCreate(0,"other",8192,&file)==CARD_RESULT_EXIST);
    assert(CARDWrite(&file,data.data(),data.size(),32768)==CARD_RESULT_FATAL_ERROR);
    assert(CARDRead(&file,read.data(),read.size(),-1)==CARD_RESULT_FATAL_ERROR);
    std::array<u32,3> lengths{};assert(ARInit(lengths.data(),lengths.size())==0x4000&&ARGetSize()==16*1024*1024);
    auto at=ARAlloc(32);assert(at==0x4000);ARQRequest request{};alignas(32) std::array<u8,32> source{},output{};source.fill(0x3c);
    ARQPostRequest(&request,1,0,0,reinterpret_cast<uintptr_t>(source.data()),at,32,completed);assert(callbackRan);
    ARQPostRequest(&request,1,1,0,at,reinterpret_cast<uintptr_t>(output.data()),32,nullptr);assert(output==source);
    bool rejected=false;try{ARQPostRequest(&request,1,0,0,reinterpret_cast<uintptr_t>(source.data()),ARGetSize()-16,32,nullptr);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    u32 size{};assert(ARFree(&size)==0x4000&&size==32);
    std::puts("Actual private card create/write/read/reopen/metadata and ARAM transfer/bounds passed");
}
