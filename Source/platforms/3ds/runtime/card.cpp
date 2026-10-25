#include <dolphin/card.h>
#include <cstdio>
#include <cstring>
#include <array>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
// Native private save container. This is not a raw GCN card or a GCI file.
// The original save payload, duplicate sectors and checksums remain game-owned.
namespace {
constexpr char path[]="sdmc:/3ds/dusklight/saves/native-card0.bin";
constexpr char temporary[]="sdmc:/3ds/dusklight/saves/native-card0.pending";
constexpr u32 capacity=1024*1024;
struct Header {char magic[8];u32 version;CARDStat stat;};
bool mounted{};
struct Mutex {OSMutex m;Mutex(){OSInitMutex(&m);}};
struct Guard {OSMutex* m;Guard(){static Mutex value;m=&value.m;OSLockMutex(m);}~Guard(){OSUnlockMutex(m);}};
bool directory(const char* name){return mkdir(name,0777)==0||errno==EEXIST;}
bool valid(const Header& h){return !std::memcmp(h.magic,"DSVCARD1",8)&&h.version==1&&h.stat.length&&h.stat.length<=capacity&&!(h.stat.length%8192)&&std::memchr(h.stat.fileName,0,CARD_FILENAME_MAX);}
s32 load(Header& h,std::FILE*& file){
    file=std::fopen(path,"rb");if(!file)return errno==ENOENT?CARD_RESULT_NOFILE:CARD_RESULT_IOERROR;
    bool okay=std::fread(&h,1,sizeof(h),file)==sizeof(h)&&valid(h);
    if(okay){okay=std::fseek(file,0,SEEK_END)==0&&std::ftell(file)==long(sizeof(h)+h.stat.length)&&std::fseek(file,sizeof(h),SEEK_SET)==0;}
    if(!okay){std::fclose(file);file=nullptr;return CARD_RESULT_BROKEN;}return CARD_RESULT_READY;
}
bool commit(const Header& h,const void* patch,u32 length,u32 offset,bool create){
    std::FILE* input=nullptr;Header old{};
    if(!create&&load(old,input)!=CARD_RESULT_READY)return false;
    auto* output=std::fopen(temporary,"wb");if(!output){if(input)std::fclose(input);return false;}
    bool okay=std::fwrite(&h,1,sizeof(h),output)==sizeof(h);
    std::array<unsigned char,8192> block{};
    for(u32 at=0;okay&&at<h.stat.length;at+=block.size()){
        if(input)okay=std::fread(block.data(),1,block.size(),input)==block.size();else block.fill(0);
        const u32 first=at>offset?at:offset,last=at+block.size()<offset+length?at+block.size():offset+length;
        if(last>first)std::memcpy(block.data()+first-at,static_cast<const unsigned char*>(patch)+first-offset,last-first);
        okay=okay&&std::fwrite(block.data(),1,block.size(),output)==block.size();
    }
    if(input)std::fclose(input);
    if(std::fflush(output)!=0)okay=false;
    if(okay&&fsync(fileno(output))!=0)okay=false;
    if(std::fclose(output)!=0)okay=false;
    // Never remove the valid save before rename. A failed write retains it.
    if(okay)okay=std::rename(temporary,path)==0;
    return okay;
}
bool owner(const CARDFileInfo* f){return mounted&&f&&f->chan==0&&f->fileNo==0;}
}
extern "C" {
void CARDInit(const char*,const char*){mounted=false;}
s32 CARDProbeEx(s32 channel,s32* size,s32* sector){if(channel!=0)return CARD_RESULT_NOCARD;if(size)*size=8;if(sector)*sector=8192;return CARD_RESULT_READY;}
s32 CARDMount(s32 channel,void*,CARDCallback){Guard guard;if(channel!=0)return CARD_RESULT_NOCARD;if(!directory("sdmc:/3ds/dusklight")||!directory("sdmc:/3ds/dusklight/saves"))return CARD_RESULT_IOERROR;mounted=true;return CARD_RESULT_READY;}
s32 CARDUnmount(s32 channel){Guard guard;if(channel!=0)return CARD_RESULT_NOCARD;mounted=false;return CARD_RESULT_READY;}
s32 CARDCheck(s32 channel){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);return result==CARD_RESULT_NOFILE?CARD_RESULT_READY:result;}
s32 CARDFormat(s32 channel){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;Header h{};std::FILE* f{};const auto status=load(h,f);if(f)std::fclose(f);return status==CARD_RESULT_NOFILE?CARD_RESULT_READY:CARD_RESULT_NOPERM;}
s32 CARDFreeBlocks(s32 channel,s32* bytes,s32* files){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);if(result!=CARD_RESULT_READY&&result!=CARD_RESULT_NOFILE)return result;if(bytes)*bytes=capacity-(result==CARD_RESULT_READY?h.stat.length:0);if(files)*files=result==CARD_RESULT_READY?0:1;return CARD_RESULT_READY;}
s32 CARDOpen(s32 channel,const char* name,CARDFileInfo* info){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(!name||!info)return CARD_RESULT_FATAL_ERROR;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);if(result!=CARD_RESULT_READY)return result;if(std::strncmp(name,h.stat.fileName,CARD_FILENAME_MAX))return CARD_RESULT_NOFILE;*info={0,0,0,static_cast<s32>(h.stat.length),0};return CARD_RESULT_READY;}
s32 CARDClose(CARDFileInfo* info){Guard guard;if(!owner(info))return CARD_RESULT_NOFILE;*info={-1,-1,0,0,0};return CARD_RESULT_READY;}
s32 CARDCreate(s32 channel,const char* name,u32 bytes,CARDFileInfo* info){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(!name||!info||!bytes||bytes>capacity||bytes%8192)return CARD_RESULT_FATAL_ERROR;if(std::strlen(name)>=CARD_FILENAME_MAX)return CARD_RESULT_NAMETOOLONG;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);if(result!=CARD_RESULT_NOFILE)return result==CARD_RESULT_READY?CARD_RESULT_EXIST:result;std::memcpy(h.magic,"DSVCARD1",8);h.version=1;std::strcpy(h.stat.fileName,name);h.stat.length=bytes;std::memcpy(h.stat.gameName,"GZ2E",4);std::memcpy(h.stat.company,"01",2);if(!commit(h,nullptr,0,0,true))return CARD_RESULT_IOERROR;*info={0,0,0,static_cast<s32>(bytes),0};return CARD_RESULT_READY;}
s32 CARDRead(const CARDFileInfo* info,void* data,s32 bytes,s32 offset){Guard guard;if(!owner(info)||!data||bytes<0||offset<0)return CARD_RESULT_FATAL_ERROR;Header h{};std::FILE* f{};auto result=load(h,f);if(result!=CARD_RESULT_READY)return result;bool okay=u32(offset)<=h.stat.length&&u32(bytes)<=h.stat.length-u32(offset)&&std::fseek(f,sizeof(h)+offset,SEEK_SET)==0&&std::fread(data,1,bytes,f)==u32(bytes);std::fclose(f);return okay?CARD_RESULT_READY:CARD_RESULT_IOERROR;}
s32 CARDWrite(const CARDFileInfo* info,const void* data,s32 bytes,s32 offset){Guard guard;if(!owner(info)||!data||bytes<0||offset<0)return CARD_RESULT_FATAL_ERROR;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);if(result!=CARD_RESULT_READY)return result;if(u32(offset)>h.stat.length||u32(bytes)>h.stat.length-u32(offset))return CARD_RESULT_FATAL_ERROR;if(!commit(h,data,bytes,offset,false))return CARD_RESULT_IOERROR;return CARD_RESULT_READY;}
s32 CARDGetStatus(s32 channel,s32 number,CARDStat* stat){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(number!=0)return CARD_RESULT_NOFILE;if(!stat)return CARD_RESULT_FATAL_ERROR;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);if(result==CARD_RESULT_READY)*stat=h.stat;return result;}
s32 CARDSetStatus(s32 channel,s32 number,const CARDStat* stat){Guard guard;if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(number!=0||!stat)return CARD_RESULT_NOFILE;Header h{};std::FILE* f{};auto result=load(h,f);if(f)std::fclose(f);if(result!=CARD_RESULT_READY)return result;if(stat->length!=h.stat.length||std::memcmp(stat->fileName,h.stat.fileName,CARD_FILENAME_MAX))return CARD_RESULT_FATAL_ERROR;h.stat=*stat;if(!valid(h))return CARD_RESULT_FATAL_ERROR;return commit(h,nullptr,0,0,false)?CARD_RESULT_READY:CARD_RESULT_IOERROR;}
s32 CARDGetSerialNo(s32 channel,u64* serial){if(channel!=0||!mounted)return CARD_RESULT_NOCARD;if(!serial)return CARD_RESULT_FATAL_ERROR;*serial=0x4455534b43415244ULL;return CARD_RESULT_READY;}
}
