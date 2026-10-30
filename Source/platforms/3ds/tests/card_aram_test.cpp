#include <dolphin/card.h>
#include <dolphin/ar.h>
#include <cassert>
#include <cstdio>
#include <array>
#include <cstring>
#include <mutex>
#include <map>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
namespace {std::map<OSMutex*,std::recursive_mutex> locks;bool callbackRan;}
extern "C" {
void OSInitMutex(OSMutex* m){locks.try_emplace(m);}
void OSLockMutex(OSMutex* m){locks.at(m).lock();}
void OSUnlockMutex(OSMutex* m){locks.at(m).unlock();}
void OSPanic(const char*,int,const char*,...){throw std::runtime_error("Expected native contract rejection");}
}
void completed(uintptr_t p){auto* request=reinterpret_cast<ARQRequest*>(p);assert(request->length==32);callbackRan=true;}
namespace {
constexpr char containerPath[]="sdmc:/3ds/dusklight/saves/native-card0.bin";
std::vector<unsigned char> containerBytes(){
    auto* input=std::fopen(containerPath,"rb");assert(input);
    assert(std::fseek(input,0,SEEK_END)==0);const auto size=std::ftell(input);assert(size>0);
    assert(std::fseek(input,0,SEEK_SET)==0);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    assert(std::fread(bytes.data(),1,bytes.size(),input)==bytes.size());
    assert(std::fclose(input)==0);return bytes;
}
}
int main(){
    // Only this test's working directory is touched; no user/game save input.
    assert(mkdir("sdmc:",0777)==0);assert(mkdir("sdmc:/3ds",0777)==0);
    for(const char* game:{"GZ2E","GZ2P"}){
    CARDInit(game,"01");assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);assert(CARDCheck(0)==CARD_RESULT_READY);
    CARDFileInfo file{};assert(CARDOpen(0,"zeldaTp.dat",&file)==CARD_RESULT_NOFILE);
    assert(CARDCreate(0,"zeldaTp.dat",32768,&file)==CARD_RESULT_READY);
    std::array<unsigned char,8192> data{},read{};data.fill(0xa5);
    assert(CARDWrite(&file,data.data(),data.size(),16384)==CARD_RESULT_READY);
    assert(CARDRead(&file,read.data(),read.size(),16384)==CARD_RESULT_READY&&data==read);
    CARDStat stat{};assert(CARDGetStatus(0,0,&stat)==CARD_RESULT_READY);stat.iconAddr=123;stat.commentAddr=256;
    assert(!std::memcmp(stat.gameName,game,4)&&!std::memcmp(stat.company,"01",2));
    assert(CARDSetStatus(0,0,&stat)==CARD_RESULT_READY);assert(CARDClose(&file)==CARD_RESULT_READY);
    assert(CARDUnmount(0)==CARD_RESULT_READY);assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);
    assert(CARDOpen(0,"zeldaTp.dat",&file)==CARD_RESULT_READY);assert(CARDGetStatus(0,0,&stat)==CARD_RESULT_READY&&stat.iconAddr==123);
    assert(CARDRead(&file,read.data(),read.size(),16384)==CARD_RESULT_READY&&data==read);
    assert(CARDFormat(0)==CARD_RESULT_NOPERM);assert(CARDCreate(0,"other",8192,&file)==CARD_RESULT_EXIST);
    assert(CARDWrite(&file,data.data(),data.size(),32768)==CARD_RESULT_FATAL_ERROR);
    assert(CARDRead(&file,read.data(),read.size(),-1)==CARD_RESULT_FATAL_ERROR);
    assert(CARDUnmount(0)==CARD_RESULT_READY);
    const auto originalStat=stat;
    const auto originalBytes=containerBytes();
    assert(originalBytes.size()>=8&&!std::memcmp(originalBytes.data(),"DSVCARD1",8));
    const char* foreignGame=!std::strcmp(game,"GZ2E")?"GZ2P":"GZ2E";
    auto reopenOwned=[&]{
        CARDInit(game,"01");assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);
        assert(CARDCheck(0)==CARD_RESULT_READY);
        CARDFileInfo reopened{};assert(CARDOpen(0,"zeldaTp.dat",&reopened)==CARD_RESULT_READY);
        CARDStat restored{};assert(CARDGetStatus(0,0,&restored)==CARD_RESULT_READY);
        assert(!std::memcmp(restored.gameName,game,4)&&!std::memcmp(restored.company,"01",2));
        assert(!std::strcmp(restored.fileName,originalStat.fileName)&&restored.length==originalStat.length);
        assert(restored.iconAddr==originalStat.iconAddr&&restored.commentAddr==originalStat.commentAddr);
        read.fill(0);assert(CARDRead(&reopened,read.data(),read.size(),16384)==CARD_RESULT_READY&&read==data);
        assert(CARDClose(&reopened)==CARD_RESULT_READY);assert(CARDUnmount(0)==CARD_RESULT_READY);
        assert(containerBytes()==originalBytes);
    };
    for(bool foreignMaker:{false,true}){
        const char* attemptedGame=foreignMaker?game:foreignGame;
        const char* attemptedMaker=foreignMaker?"02":"01";
        CARDInit(attemptedGame,attemptedMaker);assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);
        CARDFileInfo denied{};
        assert(CARDOpen(0,"zeldaTp.dat",&denied)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        CARDStat foreignStat{};
        assert(CARDGetStatus(0,0,&foreignStat)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        assert(CARDCheck(0)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        assert(CARDCreate(0,"other",8192,&denied)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        // Retain the real original file info across reinit: ownership must be checked on load too.
        std::array<unsigned char,8192> untouched{};untouched.fill(0x5a);read=untouched;
        assert(CARDRead(&file,read.data(),read.size(),16384)==CARD_RESULT_NOPERM&&read==untouched);
        assert(containerBytes()==originalBytes);
        assert(CARDWrite(&file,untouched.data(),untouched.size(),16384)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        foreignStat=originalStat;foreignStat.iconAddr=456;foreignStat.commentAddr=512;
        std::memcpy(foreignStat.gameName,attemptedGame,4);std::memcpy(foreignStat.company,attemptedMaker,2);
        assert(CARDSetStatus(0,0,&foreignStat)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        assert(CARDFormat(0)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        assert(CARDUnmount(0)==CARD_RESULT_READY);
        reopenOwned();
    }
    for(bool foreignMaker:{false,true}){
        CARDInit(game,"01");assert(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY);
        auto changedOwner=originalStat;
        if(foreignMaker)std::memcpy(changedOwner.company,"02",2);
        else std::memcpy(changedOwner.gameName,foreignGame,4);
        assert(CARDSetStatus(0,0,&changedOwner)==CARD_RESULT_NOPERM);
        assert(containerBytes()==originalBytes);
        assert(CARDUnmount(0)==CARD_RESULT_READY);
        reopenOwned();
    }
    assert(std::remove(containerPath)==0);
    }
    std::array<u32,3> lengths{};assert(ARInit(lengths.data(),lengths.size())==0x4000&&ARGetSize()==16*1024*1024);
    auto at=ARAlloc(32);assert(at==0x4000);ARQRequest request{};alignas(32) std::array<u8,32> source{},output{};source.fill(0x3c);
    ARQPostRequest(&request,1,0,0,reinterpret_cast<uintptr_t>(source.data()),at,32,completed);assert(callbackRan);
    ARQPostRequest(&request,1,1,0,at,reinterpret_cast<uintptr_t>(output.data()),32,nullptr);assert(output==source);
    bool rejected=false;try{ARQPostRequest(&request,1,0,0,reinterpret_cast<uintptr_t>(source.data()),ARGetSize()-16,32,nullptr);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    u32 size{};assert(ARFree(&size)==0x4000&&size==32);
    std::puts("Actual private card create/write/read/reopen/metadata and ARAM transfer/bounds passed");
}
