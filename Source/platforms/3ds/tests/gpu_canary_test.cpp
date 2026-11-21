// Host test of the console GPU guard (runtime/gpu_canary_ctru.cpp), built by
// Tools/3ds/test_gpu_canary.py with stub <3ds.h>/<citro3d.h>. The guard file is
// included whole so its internal state can be compared:
//   parse-equivalence  the parse of Alpha 6 (copied below, word by word) and
//                      the current one leave the same shadow registers,
//                      uniforms, program hash and signatures on Citro3D-like
//                      and random command streams (and on raw lists given as
//                      extra arguments);
//   signature-groups   partial signature mixes equal a full mix;
//   verified-store     gpu-verified.bin keeps the most recent signatures, is
//                      written again only when needed and in one FS call, never
//                      cleared in a session;
//   canary-flow        drains per canary, journal before the PICA sees the draw
//                      and "idle" before any other list, a draw first in its
//                      list waits for earlier lists;
//   coarse             SD switch gpu-canary-coarse.txt (fragment-only differences);
//   hang-attributed / hang-other-list / journal-learn  the hang protocol;
//   benchmark          host timing of both parses (information only).
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "../runtime/gpu_canary_ctru.cpp"

namespace {
int failures=0;
#define CHECK(condition) do{if(!(condition)){std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#condition);++failures;}}while(0)

// ---- Host stand-ins for the SDK, GSP and the rest of the runtime ----------
std::vector<u32> arena(1u<<20);
u32* cmdBase=arena.data();u32 cmdSize=u32(arena.size()),cmdOffset=0;
bool drawUsed=false;
bool queueIdle=true;
bool queueFailed=false;
unsigned completes=0;
std::vector<std::string> events;
// Each queue wait: whether a journaled draw ran alone in it, and the first
// word of the journal on the card at that moment.
std::vector<std::pair<bool,std::string>> waits;
// FS calls on gpu-verified.bin (a rewrite must be one call).
unsigned verifiedReplaces=0,verifiedAppends=0;
std::string reportLog;
int program=2;
struct Restart {};
// Test hook: called inside the canary completion (the queue wait) with the
// list it just queued.
void (*onCanaryWait)(const u32* list,u32 words)=nullptr;
// The state words the next __real_C3D_DrawElements emits before its draw.
std::vector<std::pair<u32,u32>> drawState; // register, value
void add(u32 header,const u32* values,u32 count){
    if(!count)count=1;
    cmdBase[cmdOffset]=values[0];cmdBase[cmdOffset+1]=header|((count-1)<<20);
    for(u32 n=1;n<count;++n)cmdBase[cmdOffset+1+n]=values[n];
    cmdOffset+=count+1;if(cmdOffset&1)cmdBase[cmdOffset++]=0;
}
void write(u32 reg,u32 value,u32 mask=0xf){add((mask<<16)|reg,&value,1);}
std::string sdPath(const char* path){return std::string("sdmc:")+path;}
std::string readFile(const std::string& path){std::ifstream in(path,std::ios::binary);std::stringstream s;s<<in.rdbuf();return s.str();}
}
struct OSThread {int unused;};
namespace {OSThread owner{};}
extern "C" {
OSThread* OSGetCurrentThread(){return &owner;}
void OSReport(const char* format,...){
    char text[1024];va_list args;va_start(args,format);std::vsnprintf(text,sizeof(text),format,args);va_end(args);
    // The console cuts lines at 255 characters (the hang record is a known
    // multi-line exception written to the SD in full).
    if(std::strncmp(text,"Native GPU hang",15)&&std::strlen(text)>=255){std::printf("FAIL OSReport line of %zu characters: %s",std::strlen(text),text);++failures;}
    reportLog+=text;
}
void GPUCMD_GetBuffer(u32** addr,u32* size,u32* offset){if(addr)*addr=cmdBase;if(size)*size=cmdSize;if(offset)*offset=cmdOffset;}
void GPUCMD_SetBuffer(u32* addr,u32 size,u32 offset){cmdBase=addr;cmdSize=size;cmdOffset=offset;}
void GPUCMD_AddRawCommands(const u32* cmd,u32 size){std::memcpy(cmdBase+cmdOffset,cmd,size*4);cmdOffset+=size;}
void GPUCMD_Add(u32 header,const u32* param,u32 paramlength){add(header,param,paramlength);}
void __real_C3D_DrawElements(GPU_Primitive_t,int count,int,const void*){
    for(const auto& [reg,value]:drawState)write(reg,value);
    write(0x25e,0,2);write(0x25f,1);write(0x227,0);write(0x228,u32(count));write(0x22a,0);
    write(0x229,0x100,2);write(0x253,0x100,2);write(0x245,0,1);write(0x22f,1);write(0x245,1,1);
    write(0x229,0,2);write(0x253,0,2);write(0x231,1);write(0x25e,0,8);write(0x25e,0,8);
    drawUsed=true;
}
unsigned duskGxJobRingCopy(u32*,unsigned,u32* total){if(total)*total=0;return 0;}
u64 svcGetSystemTick(void){return 0;}
Result svcQueryMemory(MemInfo*,PageInfo*,u32){return -1;}
u32 osConvertVirtToPhys(const void*){return 0;}
}
namespace dusk::native {
[[noreturn]] void restartAfterGpuHang(const char*,bool){events.push_back("restart");throw Restart{};}
int gxBoundProgramKind(){return program;}
std::uint32_t gxProgramCodeHash(int){return 0;}
bool gxDrawMultiProjected(){return false;}
bool gpuRecentAssumedTransfer(unsigned&,unsigned&,std::uint32_t*){return false;}
GpuCompletion platformQueryGpu(){return queueIdle&&!cmdOffset?GpuCompletion::complete:GpuCompletion::notReady;}
[[noreturn]] void transferTestStop(const std::uint32_t*){throw Restart{};}
// C3D_FrameSplit: the span becomes one queued list (nothing for an empty span).
void submitSpan(){
    if(!cmdOffset)return;
    if(drawUsed){drawUsed=false;write(0x111,1);write(0x110,1);write(0x63,1);}
    write(0x10,0x12345678);write(0x10,0x12345678);
    u32* list=cmdBase;const u32 words=cmdOffset;
    gpuCanaryListSubmitted(list,words*4);
    cmdBase+=words;cmdSize-=words;cmdOffset=0;queueIdle=false;
}
GpuCompletion platformCompleteGpuFor(GpuCompleteReason reason,std::uint64_t){
    ++completes;
    u32* list=cmdBase;const u32 words=cmdOffset;submitSpan();
    // The wait, after the list it queued (journal writes made at the split come first).
    events.push_back(reason==GpuCompleteReason::canary?"complete":"complete-other");
    {const auto journal=readFile(sdPath(journalPath));waits.emplace_back(canary.active&&canary.journaled,journal.substr(0,journal.find_first_of(" \n")));}
    if(words&&onCanaryWait&&canary.active)onCanaryWait(list,words);
    queueIdle=true;
    return GpuCompletion::complete;
}
bool platformPrepareGpuDraw(){return true;}
OSThread* platformGpuOwner(){return &owner;}
bool platformGpuFailurePending(){return queueFailed;}
bool flushPendingGxGeometry(){return true;}
void flushNativeLog(){}
void kickNativeSdWriter(){gpuCanaryWriteSessionRecord();}
bool durableReplace(const char* path,const void* data,std::size_t bytes){
    verifiedReplaces+=!std::strcmp(path,verifiedPath);
    std::ofstream out(sdPath(path),std::ios::binary|std::ios::trunc);if(bytes)out.write(static_cast<const char*>(data),std::streamsize(bytes));return bool(out);}
bool durableAppend(const char* path,const void* data,std::size_t bytes){
    verifiedAppends+=!std::strcmp(path,verifiedPath);
    std::ofstream out(sdPath(path),std::ios::binary|std::ios::app);out.write(static_cast<const char*>(data),std::streamsize(bytes));return bool(out);}
bool durableReplaceText(const char* path,const char* text){return durableReplace(path,text,std::strlen(text));}
bool durableAppendText(const char* path,const char* text){return durableAppend(path,text,std::strlen(text));}
bool durableReadText(const char* path,char* out,std::size_t capacity){
    const auto text=readFile(sdPath(path));if(text.empty())return false;
    const auto n=std::min(text.size(),capacity-1);std::memcpy(out,text.data(),n);out[n]=0;return true;}
namespace {std::vector<std::string> slotPaths;}
bool durableSlotOpen(DurableSlot& slot,const char* path,std::uint32_t bytes){
    slotPaths.push_back(path);slot={std::uint32_t(slotPaths.size()),bytes,true};return true;}
bool durableSlotWrite(DurableSlot& slot,const char* text){
    if(!slot.open)return false;
    const std::string path=slotPaths[slot.handle-1];
    std::string record(slot.bytes,'\n');std::memcpy(record.data(),text,std::min<std::size_t>(std::strlen(text),slot.bytes-1));
    durableReplace(path.c_str(),record.data(),record.size());
    const std::string first=std::string(text).substr(0,std::string(text).find_first_of(" \n"));
    events.push_back((path.find("journal")!=std::string::npos?"journal ":"session ")+first);
    return true;
}
}

// ---- Alpha 6 parse and signature, copied unchanged (the reference) --------
namespace reference {
constexpr unsigned registerCount=0x300;
std::array<u32,registerCount> shadow{};
std::array<std::array<u32,4>,96> uniforms{};
std::array<std::uint8_t,96> uniformWordsStored{};
std::array<u32,4> uniformPending{};
unsigned uniformIndex{},uniformFill{};bool uniformFloat32{};
u32 codeRunning{0x811C9DC5u},programHash{};
std::array<std::uint8_t,registerCount> signatureRegister{};
bool signatureStale{true};
inline void applyWrite(unsigned reg,u32 value,unsigned mask){
    if(reg>=registerCount)return;
    u32 merged=value;
    if(mask!=0xf){
        u32 bits=0;for(unsigned b=0;b<4;++b)if(mask>>b&1)bits|=0xffu<<(8*b);
        merged=(shadow[reg]&~bits)|(value&bits);
    }
    if(merged!=shadow[reg]){shadow[reg]=merged;signatureStale|=bool(signatureRegister[reg]);}
    if(reg<0x2bf||reg>0x2d3)return;
    if(reg==0x2c0){uniformIndex=value&0xff;uniformFloat32=value>>31;uniformFill=0;}
    else if(reg>=0x2c1&&reg<=0x2c8){
        uniformPending[uniformFill++]=value;
        if(uniformFill==(uniformFloat32?4u:3u)){
            if(uniformIndex<uniforms.size()){uniforms[uniformIndex]=uniformPending;uniformWordsStored[uniformIndex]=std::uint8_t(uniformFill);}
            ++uniformIndex;uniformFill=0;
        }
    }
    else if(reg==0x2cb)codeRunning=0x811C9DC5u;
    else if(reg>=0x2cc&&reg<=0x2d3){codeRunning=(codeRunning^value)*0x9E3779B1u;codeRunning^=codeRunning>>15;}
    else if(reg==0x2bf){programHash=codeRunning;signatureStale=true;}
}
inline void feedUniformWords(const u32* words,unsigned count){
    const unsigned row=uniformFloat32?4u:3u;
    while(count&&uniformFill){
        uniformPending[uniformFill++]=*words++;--count;
        if(uniformFill==row){
            if(uniformIndex<uniforms.size()){uniforms[uniformIndex]=uniformPending;uniformWordsStored[uniformIndex]=std::uint8_t(row);}
            ++uniformIndex;uniformFill=0;
        }
    }
    for(;count>=row;words+=row,count-=row,++uniformIndex)if(uniformIndex<uniforms.size()){
        std::memcpy(uniforms[uniformIndex].data(),words,row*sizeof(u32));uniformWordsStored[uniformIndex]=std::uint8_t(row);
    }
    while(count){uniformPending[uniformFill++]=*words++;--count;}
}
u32 parse(const u32* list,u32 begin,u32 end){
    u32 trigger=~0u;
    for(u32 i=begin;i+1<end;){
        const u32 header=list[i+1];
        const unsigned reg=header&0xffff,mask=(header>>16)&0xf,extra=(header>>20)&0xff;
        if(i+2+extra>end)break;
        if(reg==0x2c1&&mask==0xf&&!(header>>31)){
            feedUniformWords(&list[i],1);feedUniformWords(&list[i+2],extra);
            shadow[reg]=extra?list[i+1+extra]:list[i];
            i+=2+extra;i+=i&1;continue;
        }
        applyWrite(reg,list[i],mask);
        if(reg==0x22e||reg==0x22f)trigger=i+1;
        for(unsigned n=0;n<extra;++n)applyWrite((header>>31)?reg+1+n:reg,list[i+2+n],mask);
        i+=2+extra;i+=i&1;
    }
    return trigger;
}
u32 signature(){
    u32 h=0x811C9DC5u;
    auto mix=[&h](u32 v){h=(h^v)*0x9E3779B1u;h^=h>>15;};
    static constexpr std::uint16_t plain[]={0x201,0x202,0x2bb,0x2bc,0x242,0x2b9,0x2ba,0x2bd,0x4f,0x50,0x51,0x52,0x53,0x54,0x55,0x56,
        0x64,0x6f,0x100,0x101,0x102,0x106,0x107,0x40,0x6d,0x62,0x118,0x61};
    for(const auto reg:plain)mix(shadow[reg]);
    mix(programHash);mix(shadow[0x80]&0x3f07);mix(shadow[0xe0]&0x00ffff07);mix(shadow[0x104]&0xff);mix(shadow[0x105]&0xff);mix(shadow[0x25e]&0xffff);
    static constexpr std::uint16_t units[3][4]={{0x82,0x83,0x84,0x8e},{0x92,0x93,0x94,0x96},{0x9a,0x9b,0x9c,0x9e}};
    for(unsigned unit=0;unit<3;++unit){
        if(shadow[0x80]>>unit&1)for(const auto reg:units[unit])mix(shadow[reg]);
        else mix(0x55aa0000u+unit);
    }
    static constexpr std::uint16_t stages[]={0xc0,0xc8,0xd0,0xd8,0xf0,0xf8};
    for(const auto base:stages){mix(shadow[base]);mix(shadow[base+1]);mix(shadow[base+2]);mix(shadow[base+4]);}
    return h?h:1;
}
void markSignatureRegisters(){
    static constexpr std::uint16_t all[]={0x201,0x202,0x2bb,0x2bc,0x242,0x2b9,0x2ba,0x2bd,0x4f,0x50,0x51,0x52,0x53,0x54,0x55,0x56,
        0x64,0x6f,0x100,0x101,0x102,0x106,0x107,0x40,0x6d,0x62,0x118,0x61,0x80,0xe0,0x104,0x105,0x25e,
        0x82,0x83,0x84,0x8e,0x92,0x93,0x94,0x96,0x9a,0x9b,0x9c,0x9e};
    for(const auto reg:all)signatureRegister[reg]=1;
    for(const unsigned base:{0xc0u,0xc8u,0xd0u,0xd8u,0xf0u,0xf8u})for(const unsigned n:{0u,1u,2u,4u})signatureRegister[base+n]=1;
}
}

namespace {
// ---- Command streams ------------------------------------------------------
struct Stream {
    std::vector<u32> words;
    void add(u32 header,const std::vector<u32>& values){
        words.push_back(values[0]);words.push_back(header|u32(values.size()-1)<<20);
        for(std::size_t n=1;n<values.size();++n)words.push_back(values[n]);
        if(words.size()&1)words.push_back(0);
    }
    void one(u32 reg,u32 value,u32 mask=0xf){add((mask<<16)|reg,{value});}
    void seq(u32 reg,const std::vector<u32>& values,u32 mask=0xf){add(0x80000000u|(mask<<16)|reg,values);}
    void rep(u32 reg,const std::vector<u32>& values,u32 mask=0xf){add((mask<<16)|reg,values);}
};
// What Citro3D's C3Di_UpdateContext and C3D_DrawElements emit, with the
// renderer's programs, textures, TEV and matrix palettes varied at random.
void citroDraw(Stream& s,std::mt19937& random,unsigned draw){
    auto r=[&random](u32 n){return u32(random()%n);};
    if(draw%7==0||r(10)==0){
        // A program switch uploads code, operand descriptors and constants.
        s.one(0x2cb,0);
        const unsigned code=40+r(300);
        for(unsigned at=0;at<code;at+=128){std::vector<u32> chunk;for(unsigned n=at;n<std::min(code,at+128);++n)chunk.push_back(u32(random()));s.rep(0x2cc,chunk);}
        s.one(0x2bf,1);s.one(0x2d5,0);
        std::vector<u32> op;for(unsigned n=0;n<1+r(30);++n)op.push_back(u32(random()));s.rep(0x2d6,op);
        for(unsigned n=0;n<r(4);++n)s.seq(0x2c0,{u32(90+n),u32(random()),u32(random()),u32(random())});
        s.one(0x2ba,0x7fff0000u|r(64));s.one(0x2bd,r(256));s.seq(0x4f,{r(8),u32(random()),u32(random()),u32(random()),u32(random()),u32(random()),u32(random()),u32(random())});
        s.one(0x64,r(2));s.one(0x6f,u32(random()));s.one(0x242,r(12));s.one(0x2b9,0xa0000000u|r(12));
    }
    if(r(3)==0){s.seq(0x200,{0x3000000,u32(random()),u32(random())});s.seq(0x2bb,{u32(random()),u32(random())});}
    if(r(2)==0){std::vector<u32> buffers{0x3000000};for(unsigned n=0;n<36;++n)buffers.push_back(u32(random()));s.seq(0x200,buffers);}
    if(r(2)==0){
        for(const u32 base:{0x81u,0x91u,0x99u})if(r(2)){std::vector<u32> unit;for(unsigned n=0;n<4;++n)unit.push_back(r(4)?u32(random()):0);s.seq(base,unit);s.one(base==0x81?0x8eu:base+5,r(14));}
        s.one(0x80,u32(random()),0xb);
    }
    if(r(2)==0)for(const u32 base:{0xc0u,0xc8u,0xd0u,0xd8u,0xf0u,0xf8u})if(r(2))s.seq(base,{u32(random()),u32(random()),u32(random()),u32(random()),u32(random())});
    if(r(3)==0){s.seq(0x100,{u32(random()),u32(random()),u32(random()),u32(random()),u32(random()),u32(random()),u32(random()),u32(random())});s.one(0x40,r(3));s.one(0x6d,r(2));s.one(0x61,r(8));s.one(0x62,r(2));s.one(0x118,r(2));}
    if(r(4)==0){s.one(0xe0,u32(random()),0x7);s.one(0xfd,u32(random()));s.one(0xe1,u32(random()));}
    if(r(6)==0){s.one(0xe6,0);std::vector<u32> lut;for(unsigned n=0;n<128;++n)lut.push_back(u32(random()));s.rep(0xe8,lut);}
    // Uniform rows in the 32-bit form, one or more runs (palettes up to 94 rows).
    for(unsigned runs=1+r(3);runs--;){
        const u32 first=r(96),rows=1+r(first<90?40:6);
        s.one(0x2c0,0x80000000u|first);std::vector<u32> data;for(unsigned n=0;n<rows*4;++n)data.push_back(u32(random()));
        for(std::size_t at=0;at<data.size();at+=256){std::vector<u32> chunk(data.begin()+long(at),data.begin()+long(std::min(data.size(),at+256)));s.rep(0x2c1,chunk);}
    }
    s.one(0x25e,0,2);s.one(0x25f,1);s.one(0x227,u32(random()));s.one(0x228,3*(1+r(500)));s.one(0x22a,0);
    s.one(0x229,0x100,2);s.one(0x253,0x100,2);s.one(0x245,0,1);s.one(r(8)?0x22f:0x22e,1);s.one(0x245,1,1);
    s.one(0x229,0,2);s.one(0x253,0,2);s.one(0x231,1);s.one(0x25e,0,8);s.one(0x25e,0,8);
}
// Any header: interesting registers, masks, both write forms, long runs.
void randomHeader(Stream& s,std::mt19937& random){
    static constexpr u32 pool[]={0x2c0,0x2c1,0x2c2,0x2c4,0x2c8,0x2cb,0x2cc,0x2cd,0x2d3,0x2bf,0x2be,0x2d4,0x2d5,0x2d6,0x22e,0x22f,0x232,
        0x201,0x202,0x2bb,0x80,0x82,0x8e,0x92,0x9e,0xc0,0xc4,0xc8,0xf8,0xfc,0xe0,0x100,0x104,0x105,0x25e,0x203,0x2b0,0x2ff,0x300,0x3ff,0x2f8};
    const u32 reg=random()%4?pool[random()%std::size(pool)]:u32(random()%0x400);
    const u32 mask=random()%3?0xf:u32(random()%16);
    const bool sequential=random()%3==0;
    const u32 count=1+(random()%4?random()%6:random()%256);
    std::vector<u32> values;for(u32 n=0;n<count;++n)values.push_back(random()%5?u32(random()):u32(random()%3));
    if(reg==0x2c0&&random()%2)values[0]=(random()%2?0x80000000u:0)|u32(random()%110);
    s.add((sequential?0x80000000u:0)|(mask<<16)|reg,values);
}
// Reset both shadows to one random state.
void resetBoth(std::mt19937& random){
    for(unsigned n=0;n<registerCount;++n)shadow[n]=reference::shadow[n]=random()%4?0:u32(random());
    for(auto& row:uniforms)row.fill(0);reference::uniforms=uniforms;uniformWordsStored.fill(0);reference::uniformWordsStored.fill(0);
    uniformPending.fill(0);reference::uniformPending.fill(0);
    uniformIndex=reference::uniformIndex=0;uniformFill=reference::uniformFill=0;uniformFloat32=reference::uniformFloat32=false;
    codeRunning=reference::codeRunning=0x811C9DC5u;programHash=reference::programHash=0;
    signatureDirty=7;reference::signatureStale=true;
}
bool sameState(){
    return shadow==reference::shadow&&uniforms==reference::uniforms&&uniformWordsStored==reference::uniformWordsStored&&
        uniformPending==reference::uniformPending&&uniformIndex==reference::uniformIndex&&uniformFill==reference::uniformFill&&
        uniformFloat32==reference::uniformFloat32&&codeRunning==reference::codeRunning&&programHash==reference::programHash;
}
// Parses the stream in random pieces (header boundaries, as the wrapper and the
// list hook do, sometimes anywhere) through both and compares after each piece.
bool compareStream(const std::vector<u32>& words,std::mt19937& random,unsigned& draws){
    std::vector<u32> bounds{0};
    for(u32 i=0;i+1<words.size();){const u32 extra=(words[i+1]>>20)&0xff;i+=2+extra;i+=i&1;if(i<=words.size()&&random()%3==0)bounds.push_back(i);}
    if(random()%4==0)bounds.push_back(u32(random()%words.size()));
    bounds.push_back(u32(words.size()));
    std::sort(bounds.begin(),bounds.end());
    for(std::size_t b=0;b+1<bounds.size();++b){
        const u32 begin=bounds[b],end=bounds[b+1];
        if(begin>=end)continue;
        const u32 expected=reference::parse(words.data(),begin,end),got=parse(words.data(),begin,end);
        if(expected!=got){std::printf("trigger %u vs %u at [%u,%u)\n",expected,got,begin,end);return false;}
        if(!sameState()){std::printf("state differs after [%u,%u)\n",begin,end);return false;}
        if(got!=~0u){
            ++draws;
            const u32 a=reference::signature(),c=currentSignature();
            if(a!=c){std::printf("signature %08x vs %08x\n",a,c);return false;}
        }
    }
    return true;
}
std::vector<std::vector<u32>> listsFromArguments(int argc,char** argv){
    std::vector<std::vector<u32>> lists;
    for(int n=2;n<argc;++n){
        const auto bytes=readFile(argv[n]);std::vector<u32> words(bytes.size()/4);
        std::memcpy(words.data(),bytes.data(),words.size()*4);if(!words.empty())lists.push_back(std::move(words));
    }
    return lists;
}

int parseEquivalence(int argc,char** argv){
    markSignatureRegisters();reference::markSignatureRegisters();
    std::mt19937 random(0x6a09e667u);
    unsigned draws=0,streams=0;std::uint64_t words=0;
    for(unsigned round=0;round<400;++round){
        resetBoth(random);
        Stream s;for(unsigned d=0;d<1+random()%40;++d)citroDraw(s,random,d);
        ++streams;words+=s.words.size();
        if(!compareStream(s.words,random,draws)){CHECK(!"citro stream");break;}
    }
    for(unsigned round=0;round<3000;++round){
        resetBoth(random);
        Stream s;for(unsigned h=0;h<1+random()%60;++h){if(random()%5==0)citroDraw(s,random,h);else randomHeader(s,random);}
        ++streams;words+=s.words.size();
        if(!compareStream(s.words,random,draws)){CHECK(!"random stream");break;}
    }
    unsigned files=0;
    for(const auto& list:listsFromArguments(argc,argv)){
        resetBoth(random);++files;words+=list.size();
        if(!compareStream(list,random,draws)){CHECK(!"recorded list");break;}
    }
    std::printf("parse-equivalence: %u streams, %u recorded lists, %llu words, %u draws, block words %llu, partial signatures %llu of %llu\n",
        streams,files,static_cast<unsigned long long>(words),draws,static_cast<unsigned long long>(guard.blockWords),
        static_cast<unsigned long long>(guard.partialSignatures),static_cast<unsigned long long>(guard.signatures));
    CHECK(draws>1000);CHECK(guard.blockWords>0);CHECK(guard.partialSignatures>0);
    return failures;
}

int signatureGroups(){
    markSignatureRegisters();reference::markSignatureRegisters();
    std::mt19937 random(0xbb67ae85u);resetBoth(random);
    static constexpr u32 group1[]={0x201,0x2bb,0x4f,0x56,0x100,0x107,0x40,0x61,0x80,0xe0,0x104,0x105,0x25e};
    static constexpr u32 group2[]={0x82,0x83,0x84,0x8e,0x92,0x96,0x9a,0x9e};
    static constexpr u32 group4[]={0xc0,0xc1,0xc2,0xc4,0xd8,0xf8,0xfc};
    unsigned checked=0;
    for(unsigned n=0;n<20000;++n){
        Stream s;const unsigned pick=random()%10;
        const u32 reg=pick<2?group1[random()%std::size(group1)]:pick<6?group2[random()%std::size(group2)]:pick<9?group4[random()%std::size(group4)]:0x203;
        s.one(reg,random()%3?u32(random()):0,random()%4?0xf:u32(random()%16));
        if(random()%50==0){s.one(0x2cb,0);s.rep(0x2cc,{u32(random()),u32(random())});s.one(0x2bf,1);}
        reference::parse(s.words.data(),0,u32(s.words.size()));parse(s.words.data(),0,u32(s.words.size()));
        if(random()%3==0){CHECK(reference::signature()==currentSignature());++checked;}
    }
    CHECK(guard.partialSignatures*2>guard.signatures);
    std::printf("signature-groups: %u checks, %llu mixes of which %llu partial\n",checked,
        static_cast<unsigned long long>(guard.signatures),static_cast<unsigned long long>(guard.partialSignatures));
    // The fragment-only signature ignores TEV and blending, not textures.
    resetBoth(random);shadow[0x80]=1;signatureDirty=7;
    const u32 coarse=coarseSignature(),full=currentSignature();
    shadow[0xc1]^=0x55;shadow[0x101]^=0x12;shadow[0x107]^=1;shadow[0x40]^=1;signatureDirty=7;
    CHECK(coarseSignature()==coarse);CHECK(currentSignature()!=full);
    shadow[0x8e]^=1;CHECK(coarseSignature()!=coarse);shadow[0x8e]^=1;
    shadow[0x201]^=1;CHECK(coarseSignature()!=coarse);shadow[0x201]^=1;
    shadow[0x6f]^=1;CHECK(coarseSignature()!=coarse);shadow[0x6f]^=1;
    programHash^=1;CHECK(coarseSignature()!=coarse);
    return failures;
}

std::vector<u32> fileEntries(){
    const auto bytes=readFile("sdmc:/3ds/DuskLight/gpu-verified.bin");
    std::vector<u32> words(bytes.size()/4);std::memcpy(words.data(),bytes.data(),words.size()*4);return words;
}
void writeEntries(const std::vector<u32>& entries){
    std::vector<u32> words{0x56474c44u,1};words.insert(words.end(),entries.begin(),entries.end());
    durableReplace(verifiedPath,words.data(),words.size()*4);
}
int verifiedStore(){
    // Older entries, repeated ones and a zero, more than the load limit.
    std::vector<u32> entries;
    for(u32 n=1;n<=9000;++n)entries.push_back(n*2654435761u|1);
    entries.push_back(5*2654435761u|1);entries.push_back(0);entries.push_back(7*2654435761u|1);
    for(u32 n=9001;n<=9100;++n)entries.push_back(n*2654435761u|1);
    writeEntries(entries);
    std::vector<u32> expected;std::set<u32> seen;
    for(auto at=entries.rbegin();at!=entries.rend()&&expected.size()<verifiedLoadLimit;++at)if(*at&&seen.insert(*at).second)expected.push_back(*at);
    std::reverse(expected.begin(),expected.end());
    verifiedReplaces=verifiedAppends=0;
    loadVerified();
    // Header and entries in one call: a switch-off cannot leave the header alone.
    CHECK(verifiedReplaces==1);CHECK(verifiedAppends==0);
    CHECK(verifiedLoaded==verifiedLoadLimit);CHECK(verifiedCount==verifiedLoadLimit);
    for(const u32 value:expected)CHECK(verifiedHas(value));
    CHECK(!verifiedHas(1*2654435761u|1));CHECK(verifiedHas(5*2654435761u|1));
    auto words=fileEntries();
    CHECK(words.size()==2+expected.size());
    CHECK(std::equal(expected.begin(),expected.end(),words.begin()+2));
    CHECK(reportLog.find("rewritten")!=std::string::npos);
    // A compact file is read as it is, not written again.
    reportLog.clear();const auto before=readFile("sdmc:/3ds/DuskLight/gpu-verified.bin");
    loadVerified();
    CHECK(readFile("sdmc:/3ds/DuskLight/gpu-verified.bin")==before);CHECK(reportLog.find("rewritten")==std::string::npos);
    // A partial last entry (a cut append) is dropped so later appends stay whole.
    {const auto whole=readFile("sdmc:/3ds/DuskLight/gpu-verified.bin");
     std::ofstream("sdmc:/3ds/DuskLight/gpu-verified.bin",std::ios::binary|std::ios::app).write("\x01\x02",2);
     reportLog.clear();verifiedReplaces=verifiedAppends=0;loadVerified();
     CHECK(readFile("sdmc:/3ds/DuskLight/gpu-verified.bin")==whole);CHECK(verifiedReplaces==1&&verifiedAppends==0);
     CHECK(reportLog.find("partial entry dropped")!=std::string::npos);CHECK(verifiedLoaded==verifiedLoadLimit);}
    // Alpha 6 cleared the table at 6000 entries: nothing known is forgotten now.
    std::vector<u32> added;
    for(u32 n=0;verifiedCount<6100;++n){const u32 value=(n+20000)*2246822519u|1;if(verifiedHas(value))continue;verifiedAdd(value);verifiedQueue(value);added.push_back(value);}
    for(const u32 value:expected)CHECK(verifiedHas(value));
    for(const u32 value:added)CHECK(verifiedHas(value));
    // Full within one session: the most recent ones come back from the file.
    for(u32 n=0;verifiedCount<verifiedLimit;++n){const u32 value=(n+90000)*2246822519u|1;if(verifiedHas(value))continue;verifiedAdd(value);verifiedQueue(value);added.push_back(value);}
    const u32 last=0x12345679u;verifiedAdd(last);
    CHECK(reportLog.find("known-good table full")!=std::string::npos);
    CHECK(verifiedHas(last));CHECK(verifiedCount==verifiedLoadLimit+1);
    for(std::size_t n=added.size()-1000;n<added.size();++n)CHECK(verifiedHas(added[n]));
    words=fileEntries();CHECK(words.size()==2+verifiedLoadLimit);
    std::printf("verified-store: %zu entries in the file -> %u kept; %zu added in one session, table %u after the reload\n",
        entries.size(),verifiedLoadLimit,added.size(),verifiedCount);
    return failures;
}

// One draw through the wrapper with the given state.
void draw(const std::vector<std::pair<u32,u32>>& state,int count=3){
    drawState=state;__wrap_C3D_DrawElements(GPU_Primitive_t(0),count,1,nullptr);
}
std::vector<std::pair<u32,u32>> stateA(){return {{0x201,0x3777ff},{0x202,0x5fc00000},{0x80,1},{0x82,0x01000100},{0x8e,0},{0xc0,0x10},{0xc1,0},{0x100,0xe40100},{0x40,0}};}
int countOf(const std::string& name){int n=0;for(const auto& e:events)n+=e==name;return n;}
int canaryFlow(){
    initializeGpuCanary();
    CHECK(enabled);CHECK(reportLog.find("known-good table 0 of 12288")!=std::string::npos);
    events.clear();
    auto a=stateA();
    // New signature, first in its list, queue idle: one wait, journal first.
    draw(a);
    CHECK(completes==1);CHECK(counters.canaries==1);
    CHECK(events.size()>=2&&events[events.size()-2]=="journal running"&&events.back()=="complete");
    const u32 sigA=currentSignature();CHECK(verifiedHas(sigA));
    // The same draw again: known, nothing waits.
    draw(a);draw(a);CHECK(completes==1);CHECK(guard.sameSignature==2);
    // A new kind after batched draws in the same list: the journal stops naming
    // the draw that completed before the batched list is queued, then drain,
    // journal, run alone.
    auto b=a;b[5].second=0x20;events.clear();
    draw(b);
    CHECK(completes==3);CHECK(counters.canaries==2);
    CHECK(events.size()==4&&events[0]=="journal idle"&&events[1]=="complete"&&events[2]=="journal running"&&events[3]=="complete");
    // Right after that canary (first in its list, queue idle): one wait only.
    auto c=a;c[5].second=0x30;draw(c);CHECK(completes==4);CHECK(guard.aloneWaits==0);
    // First in its list while an earlier list is still on the GPU: it waits
    // for that list before its journal record and its own run.
    draw(a);events.clear();submitSpan();CHECK(events.size()==1&&events[0]=="journal idle");
    events.clear();
    auto d=a;d[5].second=0x40;draw(d);
    CHECK(completes==6);CHECK(guard.aloneWaits==1);
    CHECK(events.size()==3&&events[0]=="complete"&&events[1]=="journal running"&&events[2]=="complete");
    CHECK(reportLog.find("waited for earlier lists first")!=std::string::npos);
    // A fragment-only difference still runs alone without the switch.
    auto e=a;e[7].second=0x12345;draw(e);CHECK(counters.canaries==5);
    // Frame end: journal idle, the 300-picture line, verified appends.
    events.clear();gpuCanaryFrameEnd(300,"F_SP102");
    CHECK(countOf("journal idle")==1);
    CHECK(reportLog.find("Native GPU guard, last 300 pictures: draws=8 ")!=std::string::npos);
    CHECK(reportLog.find("canaries=5 waits-added=1 coarse=0")!=std::string::npos);
    // Journal writes: 5 "running", "idle" before 2 batched lists, 1 at frame end.
    CHECK(reportLog.find("journal=8/2\n")!=std::string::npos);
    const auto words=fileEntries();CHECK(words.size()==2+5);
    // A draw run alone, batched draws, then the frame's last list: "idle" is on
    // the card before that list is queued, and frame end writes nothing.
    auto f=a;f[5].second=0x50;draw(f);draw(a);
    events.clear();submitSpan();CHECK(events.size()==1&&events[0]=="journal idle");
    events.clear();gpuCanaryFrameEnd(301,"F_SP102");CHECK(events.empty());
    // Diagnostic isolate-all mode: a known draw runs alone without a journal
    // record, so the record of the draw before it is closed first.
    isolateAll=true;auto g=a;g[5].second=0x60;draw(g);
    events.clear();draw(a);
    CHECK(events.size()==2&&events[0]=="journal idle"&&events[1]=="complete");
    isolateAll=false;
    // Every wait: "running" exactly while a journaled draw ran alone in it.
    unsigned runningWaits=0;
    for(const auto& [journaled,journal]:waits){CHECK(journal==(journaled?"running":"idle"));runningWaits+=journal=="running";}
    CHECK(runningWaits==7);
    gpuCanarySessionEnd();
    std::printf("canary-flow: %u completes for %llu canaries in %llu draws\n",completes,
        static_cast<unsigned long long>(counters.canaries),static_cast<unsigned long long>(counters.draws));
    return failures;
}
int coarse(){
    durableReplaceText("/3ds/DuskLight/gpu-canary-coarse.txt","on\n");
    initializeGpuCanary();
    CHECK(coarseKnown);CHECK(reportLog.find("run as known (SD switch gpu-canary-coarse.txt)")!=std::string::npos);
    auto a=stateA();draw(a);CHECK(counters.canaries==1);
    // TEV, blending and culling differ: known through the fragment-only signature.
    auto b=a;b[5].second=0x77;b[7].second=0x00e40000;b[8].second=1;draw(b);
    CHECK(counters.canaries==1);CHECK(guard.coarseKnown==1);
    draw(b);CHECK(guard.coarseKnown==1);
    // Another texture type is a new kind of draw.
    auto c=a;c[4].second=5;draw(c);CHECK(counters.canaries==2);
    gpuCanarySessionEnd();
    // Each canary appended its signature and its fragment-only one.
    CHECK(fileEntries().size()==2+4);
    // The next start knows the fragment-only signature from the file.
    loadVerified();lastKnown=0;
    auto d=a;d[6].second=0x99;draw(d);CHECK(counters.canaries==2);CHECK(guard.coarseKnown==2);
    std::printf("coarse: canaries %llu, known by fragment-only difference %llu\n",
        static_cast<unsigned long long>(counters.canaries),static_cast<unsigned long long>(guard.coarseKnown));
    return failures;
}
int hang(bool ownList){
    initializeGpuCanary();
    draw(stateA());
    const u32 sigA=currentSignature();
    static bool own;own=ownList;
    onCanaryWait=[](const u32* list,u32){
        // The queue wait finds a list silent for six seconds.
        const std::uint32_t entry[8]{0x01000101,own?u32(reinterpret_cast<std::uintptr_t>(list)):0x14000040u,0,0,0,0,0,1};
        gpuHang(entry,0,1,6000);
    };
    auto b=stateA();b[5].second=0x55;bool restarted=false;
    try{draw(b);}catch(const Restart&){restarted=true;}
    CHECK(restarted);
    const auto skip=readFile("sdmc:/3ds/DuskLight/gpu-skip.txt");
    char expected[96];std::snprintf(expected,sizeof(expected),"hang 1 sig=%08lx fmt=003777ff program=2",static_cast<unsigned long>(currentSignature()));
    CHECK(skip.rfind(std::string("build=")+DUSKLIGHT_RELEASE+"\n",0)==0);
    if(ownList){CHECK(skip.find(expected)!=std::string::npos);CHECK(readFile("sdmc:/3ds/DuskLight/gpu-resume.txt")=="resume\n");}
    else{CHECK(skip.find("hang 1 unattributed-list")!=std::string::npos);CHECK(skip.find("sig=")==std::string::npos);}
    CHECK(readFile("sdmc:/3ds/DuskLight/gpu-journal.txt").rfind("idle\n",0)==0);
    CHECK(readFile("sdmc:/3ds/DuskLight/gpu-session.txt").rfind("ended (GPU hang recorded)",0)==0);
    (void)sigA;
    std::printf("hang (%s list): %s",ownList?"its own":"another",skip.c_str());
    return failures;
}
int journalLearn(){
    durableReplaceText("/3ds/DuskLight/gpu-journal.txt","running sig=0000abcd fmt=12345678 program=2 code=00000000 projected=0 draw=5\nsig=0000abcd\n");
    initializeGpuCanary();
    const auto skip=readFile("sdmc:/3ds/DuskLight/gpu-skip.txt");
    CHECK(skip.find("hang 1 sig=0000abcd fmt=12345678 program=2 code=00000000 froze")!=std::string::npos);
    CHECK(skipCount==1&&skipSignatures[0]==0xabcd);CHECK(autoAdvance);
    std::printf("journal-learn: %s",skip.c_str());
    return failures;
}

// The session record carries a word of the start (gpuCanarySessionWord,
// "core2" while the second-core worker is on); the next start asks for it in
// every format the guard writes. A fatal error of a start with the worker on
// must leave it off next time: the record read back here is written by the
// guard's own fatal path.
int sessionWordCase(){
    gpuCanarySessionWord("core2");
    initializeGpuCanary();
    gpuCanarySessionFatal("Second-core worker test: fatal error on shape 300");
    const auto written=readFile("sdmc:/3ds/DuskLight/gpu-session.txt");
    CHECK(written.rfind("ended fatal core2: ",0)==0);
    std::snprintf(previousSession,sizeof(previousSession),"%s",written.c_str());
    CHECK(gpuCanaryPreviousSessionWord("core2"));
    const struct {const char* record;bool core2;} records[]{
        {"ended fatal core2: Second-core worker test\n",true},
        {"ended (GPU hang recorded) core2\n",true},
        {"running frame=60 stage=F_SP103 draws=1 canaries=0 left-out=0 core2\n",true},
        {"core2\n",true},
        {"ended fatal: the worker was off\n",false},
        {"running frame=60 stage=core22 draws=1 canaries=0 left-out=0\n",false},
        {"running frame=60 stage=xcore2 draws=1 canaries=0 left-out=0\n",false},
    };
    for(const auto& r:records){
        std::snprintf(previousSession,sizeof(previousSession),"%s",r.record);
        if(gpuCanaryPreviousSessionWord("core2")!=r.core2){std::printf("session word wrong for: %s",r.record);++failures;}
    }
    std::printf("session-word: %zu records\n",std::size(records)+1);
    return failures;
}

int fontOomSessionCase(){
    const char reason[]="Original font pane allocation failed: pane=41 heap=767792 free=360";
    const std::string oom=std::string("ended fatal core2: ")+reason;
    using End=GpuPreviousEnd;
    initializeGpuCanary();
    const auto check=[](const std::string& session,const char* journal,End expected){
        CHECK(durableSlotWrite(sessionSlot,session.c_str()));
        CHECK(durableSlotWrite(journalSlot,journal));
        initializeGpuCanary();
        CHECK(gpuCanaryPreviousSessionWord("core2"));
        CHECK(gpuCanaryPreviousEnd()==expected);
    };
    check(oom+"\n","idle\n",End::fontOom);
    CHECK(readFile(sdPath(historyPath)).find(reason)!=std::string::npos);
    check(oom+" [second-core worker]\n","idle\n",End::fatal);
    check(oom.substr(0,oom.find(" free="))+"\n","idle\n",End::fatal);
    check("ended fatal core2: CPU exception pc=12345678\n","idle\n",End::fatal);
    check("ended fatal core2: Second-core worker test: fatal error on shape 300\n","idle\n",End::fatal);
    check(oom+"\n","before a new draw\n",End::fatal);
    check(oom+"\n","running sig=0000abcd fmt=12345678\n",End::gpuStop);
    check("ended (GPU hang recorded) core2\n","idle\n",End::gpuStop);

    gpuCanarySessionWord("core2");
    queueFailed=true;
    gpuCanarySessionFatal(reason);
    queueFailed=false;
    const auto session=readFile(sdPath(sessionPath));
    CHECK(session.find("[GPU work unresolved]")!=std::string::npos);
    initializeGpuCanary();
    CHECK(gpuCanaryPreviousEnd()==End::fatal);
    CHECK(gpuCanaryPreviousSessionWord("core2"));
    std::printf("font-oom-session: Alpha 17 records and conservative GPU/worker controls\n");
    return failures;
}

int benchmark(int argc,char** argv){
    markSignatureRegisters();reference::markSignatureRegisters();
    std::mt19937 random(0x3c6ef372u);
    std::vector<std::vector<u32>> lists=listsFromArguments(argc,argv);
    {Stream s;for(unsigned d=0;d<260;++d)citroDraw(s,random,d);lists.push_back(s.words);}
    std::uint64_t words=0;for(const auto& list:lists)words+=list.size();
    auto time=[&](auto&& function){
        const auto start=std::chrono::steady_clock::now();
        u32 sink=0;for(unsigned round=0;round<200;++round)for(const auto& list:lists)sink+=function(list);
        const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
        return std::make_pair(ns/(200.0*double(words)),sink);
    };
    const auto old=time([](const std::vector<u32>& list){return reference::parse(list.data(),0,u32(list.size()));});
    const auto now=time([](const std::vector<u32>& list){return parse(list.data(),0,u32(list.size()));});
    std::printf("benchmark (host, information only): %llu words per round; Alpha 6 parse %.2f ns/word, now %.2f ns/word (%.2fx)\n",
        static_cast<unsigned long long>(words),old.first,now.first,old.first/now.first);
    return old.second==now.second?0:1;
}
}

// Alpha 11 default: no draw runs alone, no journal, no verified table; the
// draw still reaches the GPU through the wrapper.
int watchdogOnly(){
    initializeGpuCanary();
    CHECK(enabled);CHECK(!canaries);CHECK(reportLog.find("watchdog only")!=std::string::npos);
    CHECK(reportLog.find("known-good table")==std::string::npos);
    events.clear();
    auto a=stateA();
    draw(a);draw(a);a[0].second^=1;draw(a);
    CHECK(completes==0);CHECK(counters.canaries==0);CHECK(counters.draws==3);
    CHECK(countOf("journal running")==0);
    CHECK(readFile("sdmc:/3ds/DuskLight/gpu-verified.bin").empty());
    std::printf("watchdog-only: %llu draws, %llu canaries, %d completes\n",static_cast<unsigned long long>(counters.draws),
        static_cast<unsigned long long>(counters.canaries),completes);
    return failures;
}
int main(int argc,char** argv){
    const std::string name=argc>1?argv[1]:"";
    if(name=="watchdog-only")return watchdogOnly();
    // Every other case tests the canaries, opt-in since Alpha 11.
    std::ofstream("sdmc:/3ds/DuskLight/gpu-canary-on.txt")<<"1\n";
    if(name=="parse-equivalence")return parseEquivalence(argc,argv);
    if(name=="signature-groups")return signatureGroups();
    if(name=="verified-store")return verifiedStore();
    if(name=="canary-flow")return canaryFlow();
    if(name=="coarse")return coarse();
    if(name=="hang-attributed")return hang(true);
    if(name=="hang-other-list")return hang(false);
    if(name=="journal-learn")return journalLearn();
    if(name=="session-word")return sessionWordCase();
    if(name=="font-oom-session")return fontOomSessionCase();
    if(name=="benchmark")return benchmark(argc,argv);
    std::printf("unknown case %s\n",name.c_str());return 2;
}
