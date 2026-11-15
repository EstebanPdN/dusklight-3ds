#include <dolphin/card.h>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
extern "C" void OSReport(const char*,...);
// Native private save container. This is not a raw GCN card or a GCI file.
// The original save payload, duplicate sectors and checksums remain game-owned.
//
// Replacing the file: the 3DS SD archive does not replace an existing file on
// rename (POSIX does). Alpha 0 wrote every change to .pending and then failed
// the rename, so the game found a created but never written file and called the
// card corrupt. A change is now written whole to .pending (one flushed write),
// the old file is removed, and .pending takes its name; a start that finds only
// a complete .pending finishes that rename. The container is mirrored in RAM:
// reads, status and checks never touch the SD, a change costs one write.
namespace {
constexpr char path[]="sdmc:/3ds/dusklight/saves/native-card0.bin";
constexpr char temporary[]="sdmc:/3ds/dusklight/saves/native-card0.pending";
constexpr char formatted[]="sdmc:/3ds/dusklight/saves/native-card0.formatted";
constexpr u32 capacity=1024*1024;
struct Header {char magic[8];u32 version;CARDStat stat;};
bool mounted{};
std::array<char,4> gameId{};
std::array<char,2> makerId{};
// RAM mirror of the container, read once per mount.
enum class Mirror {unknown,absent,present,broken,foreign,unreadable};
Mirror state=Mirror::unknown;Header header{};std::vector<unsigned char> body;
struct Mutex {OSMutex m;Mutex(){OSInitMutex(&m);}};
struct Guard {OSMutex* m;Guard(){static Mutex value;m=&value.m;OSLockMutex(m);}~Guard(){OSUnlockMutex(m);}};
bool directory(const char* name){return mkdir(name,0777)==0||errno==EEXIST;}
bool valid(const Header& h){return !std::memcmp(h.magic,"DSVCARD1",8)&&h.version==1&&h.stat.length&&h.stat.length<=capacity&&!(h.stat.length%8192)&&std::memchr(h.stat.fileName,0,CARD_FILENAME_MAX);}
bool sameOwner(const CARDStat& stat){return !std::memcmp(stat.gameName,gameId.data(),gameId.size())&&!std::memcmp(stat.company,makerId.data(),makerId.size());}
// A complete container: a valid header and exactly its length of data.
s32 readFile(const char* name,Header& h,std::vector<unsigned char>& out){
    std::FILE* file=std::fopen(name,"rb");if(!file)return errno==ENOENT?CARD_RESULT_NOFILE:CARD_RESULT_IOERROR;
    bool okay=std::fread(&h,1,sizeof(h),file)==sizeof(h)&&valid(h)&&std::fseek(file,0,SEEK_END)==0&&
        std::ftell(file)==long(sizeof(h)+h.stat.length)&&std::fseek(file,sizeof(h),SEEK_SET)==0;
    if(okay){out.resize(h.stat.length);okay=std::fread(out.data(),1,out.size(),file)==out.size();}
    std::fclose(file);
    return okay?CARD_RESULT_READY:CARD_RESULT_BROKEN;
}
bool writeFile(const Header& h,const std::vector<unsigned char>& data){
    std::vector<unsigned char> all(sizeof(h)+data.size());
    std::memcpy(all.data(),&h,sizeof(h));if(!data.empty())std::memcpy(all.data()+sizeof(h),data.data(),data.size());
    auto* output=std::fopen(temporary,"wb");if(!output)return false;
    // Unbuffered: the whole file in one SD write call (each costs ~80 ms on a console).
    std::setvbuf(output,nullptr,_IONBF,0);
    bool okay=std::fwrite(all.data(),1,all.size(),output)==all.size();
    if(okay&&fsync(fileno(output))!=0)okay=false;
    if(std::fclose(output)!=0)okay=false;
    // The complete new file exists before the old one goes.
    if(okay&&std::remove(path)!=0&&errno!=ENOENT)okay=false;
    if(okay)okay=std::rename(temporary,path)==0;
    if(!okay)OSReport("Native card: writing the save failed (errno %d)\n",errno);
    return okay;
}
s32 result(){
    switch(state){
    case Mirror::present:return CARD_RESULT_READY;
    case Mirror::absent:return CARD_RESULT_NOFILE;
    case Mirror::broken:return CARD_RESULT_BROKEN;
    case Mirror::foreign:return CARD_RESULT_NOPERM;
    default:return CARD_RESULT_IOERROR;
    }
}
s32 mirror(){
    if(state!=Mirror::unknown&&state!=Mirror::unreadable)return result();
    Header h{};std::vector<unsigned char> data;
    auto status=readFile(path,h,data);
    if(status==CARD_RESULT_NOFILE){
        // Stopped between removing the old file and the rename: finish it.
        Header p{};std::vector<unsigned char> pending;
        if(readFile(temporary,p,pending)==CARD_RESULT_READY&&std::rename(temporary,path)==0){
            OSReport("Native card: finished an interrupted save\n");h=p;data=std::move(pending);status=CARD_RESULT_READY;
        }
    }else if(status==CARD_RESULT_READY)std::remove(temporary); // an unfinished change the game never saw succeed
    if(status==CARD_RESULT_READY&&!h.stat.commentAddr&&!h.stat.bannerFormat&&!h.stat.iconAddr&&!h.stat.iconFormat){
        // Created but never written (every Alpha 0 save): nothing of the
        // player's is in it, and the game would call it corrupt forever.
        bool empty=true;for(const auto byte:data)if(byte){empty=false;break;}
        if(empty&&std::remove(path)==0){OSReport("Native card: removed a save file that was created but never written\n");status=CARD_RESULT_NOFILE;}
    }
    if(status==CARD_RESULT_READY&&!sameOwner(h.stat))state=Mirror::foreign;
    else if(status==CARD_RESULT_READY){state=Mirror::present;header=h;body=std::move(data);}
    else state=status==CARD_RESULT_NOFILE?Mirror::absent:status==CARD_RESULT_BROKEN?Mirror::broken:Mirror::unreadable;
    return result();
}
bool owner(const CARDFileInfo* f){return mounted&&f&&f->chan==0&&f->fileNo==0;}
}
extern "C" {
void CARDInit(const char* game,const char* maker){Guard guard;mounted=false;state=Mirror::unknown;body.clear();gameId.fill(0);makerId.fill(0);if(game)std::memcpy(gameId.data(),game,gameId.size());if(maker)std::memcpy(makerId.data(),maker,makerId.size());}
s32 CARDProbeEx(s32 channel,s32* size,s32* sector){if(channel!=0)return CARD_RESULT_NOCARD;if(size)*size=8;if(sector)*sector=8192;return CARD_RESULT_READY;}
s32 CARDMount(s32 channel,void*,CARDCallback){Guard guard;if(channel!=0)return CARD_RESULT_NOCARD;if(!directory("sdmc:/3ds/dusklight")||!directory("sdmc:/3ds/dusklight/saves"))return CARD_RESULT_IOERROR;mounted=true;state=Mirror::unknown;body.clear();return CARD_RESULT_READY;}
s32 CARDUnmount(s32 channel){Guard guard;if(channel!=0)return CARD_RESULT_NOCARD;mounted=false;state=Mirror::unknown;body.clear();return CARD_RESULT_READY;}
s32 CARDCheck(s32 channel){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;const auto status=mirror();return status==CARD_RESULT_NOFILE?CARD_RESULT_READY:status;}
// Formatting the private container keeps the old file aside (.formatted); a
// container of another game or region is never touched.
s32 CARDFormat(s32 channel){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;mirror();
    if(state==Mirror::foreign)return CARD_RESULT_NOPERM;
    if(state!=Mirror::absent){std::remove(formatted);if(std::rename(path,formatted)!=0&&errno!=ENOENT)return CARD_RESULT_IOERROR;std::remove(temporary);OSReport("Native card: formatted; the old save is kept as native-card0.formatted\n");}
    state=Mirror::absent;body.clear();return CARD_RESULT_READY;}
s32 CARDFreeBlocks(s32 channel,s32* bytes,s32* files){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;const auto status=mirror();if(status!=CARD_RESULT_READY&&status!=CARD_RESULT_NOFILE)return status;if(bytes)*bytes=capacity-(status==CARD_RESULT_READY?header.stat.length:0);if(files)*files=status==CARD_RESULT_READY?0:1;return CARD_RESULT_READY;}
s32 CARDOpen(s32 channel,const char* name,CARDFileInfo* info){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(!name||!info)return CARD_RESULT_FATAL_ERROR;const auto status=mirror();if(status!=CARD_RESULT_READY)return status;if(std::strncmp(name,header.stat.fileName,CARD_FILENAME_MAX))return CARD_RESULT_NOFILE;*info={0,0,0,static_cast<s32>(header.stat.length),0};return CARD_RESULT_READY;}
s32 CARDClose(CARDFileInfo* info){Guard guard;if(!owner(info))return CARD_RESULT_NOFILE;*info={-1,-1,0,0,0};return CARD_RESULT_READY;}
s32 CARDCreate(s32 channel,const char* name,u32 bytes,CARDFileInfo* info){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(!name||!info||!bytes||bytes>capacity||bytes%8192)return CARD_RESULT_FATAL_ERROR;if(std::strlen(name)>=CARD_FILENAME_MAX)return CARD_RESULT_NAMETOOLONG;
    const auto status=mirror();if(status!=CARD_RESULT_NOFILE)return status==CARD_RESULT_READY?CARD_RESULT_EXIST:status;
    Header h{};std::memcpy(h.magic,"DSVCARD1",8);h.version=1;std::strcpy(h.stat.fileName,name);h.stat.length=bytes;std::memcpy(h.stat.gameName,gameId.data(),gameId.size());std::memcpy(h.stat.company,makerId.data(),makerId.size());
    std::vector<unsigned char> data(bytes,0);if(!writeFile(h,data))return CARD_RESULT_IOERROR;
    header=h;body=std::move(data);state=Mirror::present;*info={0,0,0,static_cast<s32>(bytes),0};return CARD_RESULT_READY;}
s32 CARDRead(const CARDFileInfo* info,void* data,s32 bytes,s32 offset){Guard guard;if(!owner(info)||!data||bytes<0||offset<0)return CARD_RESULT_FATAL_ERROR;const auto status=mirror();if(status!=CARD_RESULT_READY)return status;
    if(u32(offset)>header.stat.length||u32(bytes)>header.stat.length-u32(offset))return CARD_RESULT_IOERROR;std::memcpy(data,body.data()+offset,bytes);return CARD_RESULT_READY;}
s32 CARDWrite(const CARDFileInfo* info,const void* data,s32 bytes,s32 offset){Guard guard;if(!owner(info)||!data||bytes<0||offset<0)return CARD_RESULT_FATAL_ERROR;const auto status=mirror();if(status!=CARD_RESULT_READY)return status;
    if(u32(offset)>header.stat.length||u32(bytes)>header.stat.length-u32(offset))return CARD_RESULT_FATAL_ERROR;
    auto changed=body;std::memcpy(changed.data()+offset,data,bytes);if(!writeFile(header,changed))return CARD_RESULT_IOERROR;body=std::move(changed);return CARD_RESULT_READY;}
s32 CARDGetStatus(s32 channel,s32 number,CARDStat* stat){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(number!=0)return CARD_RESULT_NOFILE;if(!stat)return CARD_RESULT_FATAL_ERROR;const auto status=mirror();if(status==CARD_RESULT_READY)*stat=header.stat;return status;}
s32 CARDSetStatus(s32 channel,s32 number,const CARDStat* stat){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(number!=0||!stat)return CARD_RESULT_NOFILE;const auto status=mirror();if(status!=CARD_RESULT_READY)return status;if(!sameOwner(*stat))return CARD_RESULT_NOPERM;
    if(stat->length!=header.stat.length||std::memcmp(stat->fileName,header.stat.fileName,CARD_FILENAME_MAX))return CARD_RESULT_FATAL_ERROR;
    Header h=header;h.stat=*stat;if(!valid(h))return CARD_RESULT_FATAL_ERROR;if(!writeFile(h,body))return CARD_RESULT_IOERROR;header=h;return CARD_RESULT_READY;}
s32 CARDGetSerialNo(s32 channel,u64* serial){if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(!serial)return CARD_RESULT_FATAL_ERROR;*serial=0x4455534b43415244ULL;return CARD_RESULT_READY;}
}
