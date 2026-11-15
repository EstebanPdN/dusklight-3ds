#include "durable_file.hpp"
#include <3ds.h>
#include <cstdio>
#include <cstring>
namespace dusk::native {
namespace {
bool openFile(const char* path,Handle& handle,bool create){
    return R_SUCCEEDED(FSUSER_OpenFileDirectly(&handle,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,""),fsMakePath(PATH_ASCII,path),
        FS_OPEN_READ|FS_OPEN_WRITE|(create?FS_OPEN_CREATE:0),0));
}
bool writeAll(Handle handle,std::uint64_t offset,const void* data,std::size_t bytes){
    const auto* at=static_cast<const std::uint8_t*>(data);
    while(bytes){
        const u32 piece=bytes>256*1024?256*1024:u32(bytes);u32 written=0;
        if(R_FAILED(FSFILE_Write(handle,&written,offset,at,piece,FS_WRITE_FLUSH))||written!=piece)return false;
        at+=piece;offset+=piece;bytes-=piece;
    }
    return true;
}
}
bool durableReplace(const char* path,const void* data,std::size_t bytes){
    Handle handle{};
    if(!openFile(path,handle,true))return false;
    const bool okay=R_SUCCEEDED(FSFILE_SetSize(handle,0))&&writeAll(handle,0,data,bytes);
    FSFILE_Close(handle);return okay;
}
bool durableAppend(const char* path,const void* data,std::size_t bytes){
    Handle handle{};u64 size=0;
    if(!openFile(path,handle,true))return false;
    const bool okay=R_SUCCEEDED(FSFILE_GetSize(handle,&size))&&writeAll(handle,size,data,bytes);
    FSFILE_Close(handle);return okay;
}
bool durableReplaceText(const char* path,const char* text){return durableReplace(path,text,std::strlen(text));}
bool durableAppendText(const char* path,const char* text){return durableAppend(path,text,std::strlen(text));}
bool durableReadText(const char* path,char* out,std::size_t capacity){
    if(!capacity)return false;
    out[0]=0;Handle handle{};u64 size=0;u32 read=0;
    if(R_FAILED(FSUSER_OpenFileDirectly(&handle,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,""),fsMakePath(PATH_ASCII,path),FS_OPEN_READ,0)))return false;
    const bool okay=R_SUCCEEDED(FSFILE_GetSize(handle,&size))&&
        R_SUCCEEDED(FSFILE_Read(handle,&read,0,out,u32(size<capacity-1?size:capacity-1)));
    FSFILE_Close(handle);
    out[okay?read:0]=0;return okay;
}
bool durableLogOpen(DurableLog& log,const char* path){
    Handle handle{};
    if(!openFile(path,handle,true))return false;
    if(R_FAILED(FSFILE_SetSize(handle,0))){FSFILE_Close(handle);return false;}
    log={handle,0,true};return true;
}
bool durableLogAppend(DurableLog& log,const void* data,std::size_t bytes){
    if(!log.open)return false;
    if(!writeAll(log.handle,log.offset,data,bytes))return false;
    log.offset+=bytes;return true;
}
bool durableSlotOpen(DurableSlot& slot,const char* path,std::uint32_t bytes){
    slot={};
    if(!bytes||bytes>1024)return false;
    Handle handle{};u64 size=0;
    if(!openFile(path,handle,true))return false;
    if(R_FAILED(FSFILE_GetSize(handle,&size))||(size!=bytes&&R_FAILED(FSFILE_SetSize(handle,bytes)))){FSFILE_Close(handle);return false;}
    slot={handle,bytes,true};return true;
}
bool durableSlotWrite(DurableSlot& slot,const char* text){
    if(!slot.open)return false;
    char record[1024];
    std::memset(record,'\n',slot.bytes);
    const auto length=std::strlen(text);
    std::memcpy(record,text,length<slot.bytes-1?length:slot.bytes-1);
    u32 written=0;
    return R_SUCCEEDED(FSFILE_Write(slot.handle,&written,0,record,slot.bytes,FS_WRITE_FLUSH))&&written==slot.bytes;
}
void durableRotate(const char* stem,const char* extension,unsigned keep){
    char from[96],to[96];
    for(unsigned n=keep;n>=1;--n){
        if(n==1)std::snprintf(from,sizeof(from),"sdmc:%s.%s",stem,extension);
        else std::snprintf(from,sizeof(from),"sdmc:%s-%u.%s",stem,n-1,extension);
        std::snprintf(to,sizeof(to),"sdmc:%s-%u.%s",stem,n,extension);
        std::remove(to);std::rename(from,to);
    }
}
}
