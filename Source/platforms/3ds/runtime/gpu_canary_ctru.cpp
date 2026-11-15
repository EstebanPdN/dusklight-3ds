#include "gpu_canary.hpp"
#include "gpu_sync.hpp"
#include "bootstrap.hpp"
#include "durable_file.hpp"
#include "../gx_geometry_pica.hpp"
#include "release.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
extern "C" OSThread* OSGetCurrentThread();
extern "C" void OSReport(const char*,...);
extern "C" void __real_C3D_DrawElements(GPU_Primitive_t,int,int,const void*);
namespace dusk::native {
[[noreturn]] void restartAfterGpuHang(const char* text,bool restart);
int gxBoundProgramKind(); // gx_pica.cpp: 0 colour, 1 rigid, 2 rigid multi-texture, -1 other
// Hash of a program's shader code, computed like the guard hashes an upload.
// A learned hang counts against a program only while its code is unchanged.
std::uint32_t gxProgramCodeHash(int kind);
// The draw being emitted uses the multi-texture program with its unit 0
// projected from the position (gx_pica.cpp).
bool gxDrawMultiProjected();
// gpu_sync_ctru.cpp: the last transfer counted complete without its event.
bool gpuRecentAssumedTransfer(unsigned& ageMilliseconds,unsigned& type,std::uint32_t words[8]);
}
// ctru_gxqueue.c: the last jobs sent to GSP (14 words per record).
extern "C" unsigned duskGxJobRingCopy(u32* out,unsigned max,u32* submittedTotal);
namespace dusk::native {
// gpu_sync_ctru.cpp: the emulator test marks a completed transfer as stopped.
[[noreturn]] void transferTestStop(const std::uint32_t words[8]);
}
namespace {
using namespace dusk::native;
// SD-relative paths, written with durable_file (FS service + flush): the
// evidence is on the card before any step that can block, even if the
// console then freezes and has to be switched off.
constexpr char skipPath[]="/3ds/DuskLight/gpu-skip.txt";
constexpr char resumePath[]="/3ds/DuskLight/gpu-resume.txt";
constexpr char historyPath[]="/3ds/DuskLight/gpu-hangs.txt";
// "running <draw>" is written just before a new kind of draw runs alone and
// "idle" at the end of that frame. A start that finds "running" knows the
// last session froze on that draw and learns it.
constexpr char journalPath[]="/3ds/DuskLight/gpu-journal.txt";
// Every 30 frames "running ..." with the frame and stage; "ended" on a clean
// exit. A start that finds "running" (and an idle journal) records a freeze
// outside any single draw.
constexpr char sessionPath[]="/3ds/DuskLight/gpu-session.txt";
// Draw signatures this console has completed, kept across sessions: a
// signature in this file is not run alone again (no GPU wait, no journal
// write). Signatures cover the shader code, so another build's entries never
// match a changed shader. Header "DLGV", version, then 32-bit signatures.
constexpr char verifiedPath[]="/3ds/DuskLight/gpu-verified.bin";
// One-shot: the next start runs every draw alone (an unexplained stop in a
// batched list). Removed as soon as that start reads it.
constexpr char isolatePath[]="/3ds/DuskLight/gpu-isolate.txt";
constexpr unsigned registerCount=0x300,maximumRestarts=12;
// Shadow of the PICA registers, float uniforms and shader code, rebuilt from
// every command this process emits. It describes the state a draw runs with.
std::array<u32,registerCount> shadow{};
std::array<std::array<u32,4>,96> uniforms{};
std::array<std::uint8_t,96> uniformWordsStored{};
std::array<u32,4> uniformPending{};
unsigned uniformIndex{},uniformFill{};bool uniformFloat32{};
u32 codeRunning{0x811C9DC5u},programHash{};
const u32* parsedBuffer{};u32 parsedOffset{};
// signature() reads a few dozen registers: it is computed again only after a
// write changed one of them (most consecutive draws keep it).
std::array<std::uint8_t,registerCount> signatureRegister{};
bool signatureStale{true};u32 signatureCached{};

bool enabled{},isolateAll{},autoAdvance{};
// Transfer-engine stops recorded on this console with this build. One: no
// transfer jobs inside a frame (GPU-resident captures and bloom off). Two or
// more: no offscreen render targets either (the map texture is not redrawn).
unsigned transferStops{};
// The last start recorded a hang and the console may not have been switched
// off since: the PICA can still be stopped. Cleared after 30 good frames.
bool previousHangPending{};
unsigned priorHangs{};
std::array<u32,64> skipSignatures{},skipSignatureFormats{},skipSignatureShaders{};std::array<int,64> skipSignaturePrograms{};unsigned skipCount{};
// A vertex format left out only together with the shader program that hung
// with it (two learned draws): the same format works with other programs.
std::array<u32,8> skipFormats{};std::array<int,8> skipFormatPrograms{};unsigned skipFormatCount{};
// A learned hang on the single-texture rigid program switches its draws to
// the multi-texture or CPU routes on the next start (gpuCanaryRigidSingleRefused).
bool rigidSingleRefused{};
// The same for the multi-texture program with a projected unit: its draws
// take the CPU vertex route on the next start (gpuCanaryMultiProjectedRefused).
bool multiProjectedRefused{};
std::array<u32,8192> verified{};unsigned verifiedCount{};
// Newly completed signatures waiting for one batched SD append.
std::array<u32,256> verifiedPending{};unsigned verifiedPendingCount{},verifiedLoaded{};unsigned long long verifiedFlushFrame{};
struct Counters {std::uint64_t draws{},canaries{},skipped{},slow{};} counters;
struct Canary {bool active{};u32 signature{},format{};int program{-1};u32 shader{};bool projected{};} canary;
std::array<u32,4096> scratch;
// Journal and session records: fixed-size slots kept open, one in-place SD
// write each (a new signature costs one write; "idle" follows at frame end).
DurableSlot journalSlot{},sessionSlot{};
bool journalRunning{};
// Emulator test of the hang path (the PICA never stops there): the Nth canary
// is reported as a hang after it completed. SD file gpu-canary-test.txt.
unsigned testHangAt{};

inline void applyWrite(unsigned reg,u32 value,unsigned mask){
    if(reg>=registerCount)return;
    u32 merged=value;
    if(mask!=0xf){
        u32 bits=0;for(unsigned b=0;b<4;++b)if(mask>>b&1)bits|=0xffu<<(8*b);
        merged=(shadow[reg]&~bits)|(value&bits);
    }
    if(merged!=shadow[reg]){shadow[reg]=merged;signatureStale|=bool(signatureRegister[reg]);}
    // Only the uniform and shader-code uploads need more than the shadow.
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
// Float uniform data words, in order, into the uniform shadow: whole rows are
// copied at once. Matrix uploads are most of the words of a frame (about 130
// thousand in the title); taking them one by one through applyWrite cost a
// tenth of the frame.
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
// PICA command words [begin,end) of one buffer. Returns the word index of the
// header of a draw trigger (DRAWARRAYS/DRAWELEMENTS), or ~0 when there is none.
u32 parse(const u32* list,u32 begin,u32 end){
    u32 trigger=~0u;
    for(u32 i=begin;i+1<end;){
        const u32 header=list[i+1];
        const unsigned reg=header&0xffff,mask=(header>>16)&0xf,extra=(header>>20)&0xff;
        if(i+2+extra>end)break;
        if(reg==0x2c1&&mask==0xf&&!(header>>31)){
            // Uniform data repeated on one register (the only form the SDK and
            // the renderer emit for it).
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
// Vertex format, shader, textures and raster state: what a draw is, without
// its addresses, sizes, uniforms or viewport.
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
// Every register signature() reads (keep the two lists together).
void markSignatureRegisters(){
    static constexpr std::uint16_t all[]={0x201,0x202,0x2bb,0x2bc,0x242,0x2b9,0x2ba,0x2bd,0x4f,0x50,0x51,0x52,0x53,0x54,0x55,0x56,
        0x64,0x6f,0x100,0x101,0x102,0x106,0x107,0x40,0x6d,0x62,0x118,0x61,0x80,0xe0,0x104,0x105,0x25e,
        0x82,0x83,0x84,0x8e,0x92,0x93,0x94,0x96,0x9a,0x9b,0x9c,0x9e};
    for(const auto reg:all)signatureRegister[reg]=1;
    for(const unsigned base:{0xc0u,0xc8u,0xd0u,0xd8u,0xf0u,0xf8u})for(const unsigned n:{0u,1u,2u,4u})signatureRegister[base+n]=1;
}
u32 currentSignature(){
    if(signatureStale){signatureCached=signature();signatureStale=false;}
    return signatureCached;
}
bool verifiedHas(u32 value){
    for(unsigned i=value&8191,n=0;n<verified.size();i=(i+1)&8191,++n){
        if(verified[i]==value)return true;
        if(!verified[i])return false;
    }
    return false;
}
void verifiedAdd(u32 value){
    if(verifiedCount>=6000){verified.fill(0);verifiedCount=0;}
    for(unsigned i=value&8191,n=0;n<verified.size();i=(i+1)&8191,++n){
        if(verified[i]==value)return;
        if(!verified[i]){verified[i]=value;++verifiedCount;return;}
    }
}
bool learned(u32 value,u32 format){
    const int program=gxBoundProgramKind();
    for(unsigned i=0;i<skipFormatCount;++i)if(skipFormats[i]==format&&skipFormatPrograms[i]==program)return true;
    for(unsigned i=0;i<skipCount;++i)if(skipSignatures[i]==value)return true;
    return false;
}
// The state of the draw just parsed, as one log line.
void describe(char* out,std::size_t size,u32 value){
    int used=std::snprintf(out,size,"sig=%08lx fmt=%08lx/%08lx shader=%08lx indices=%lu units=%lx",
        static_cast<unsigned long>(value),static_cast<unsigned long>(shadow[0x201]),static_cast<unsigned long>(shadow[0x202]),
        static_cast<unsigned long>(programHash),static_cast<unsigned long>(shadow[0x228]),static_cast<unsigned long>(shadow[0x80]&7));
    static constexpr std::uint16_t units[3][4]={{0x82,0x83,0x84,0x8e},{0x92,0x93,0x94,0x96},{0x9a,0x9b,0x9c,0x9e}};
    for(unsigned unit=0;unit<3&&used>0&&std::size_t(used)<size;++unit)if(shadow[0x80]>>unit&1)
        used+=std::snprintf(out+used,size-used," tex%u=%lux%lu type=%lu param=%08lx lod=%08lx",unit,
            static_cast<unsigned long>(shadow[units[unit][0]]>>16),static_cast<unsigned long>(shadow[units[unit][0]]&0xffff),
            static_cast<unsigned long>(shadow[units[unit][3]]),static_cast<unsigned long>(shadow[units[unit][1]]),static_cast<unsigned long>(shadow[units[unit][2]]));
    if(used>0&&std::size_t(used)<size)std::snprintf(out+used,size-used," blend=%08lx/%08lx alpha=%08lx stencil=%08lx/%08lx depth=%08lx fog=%08lx cull=%lx",
        static_cast<unsigned long>(shadow[0x100]),static_cast<unsigned long>(shadow[0x101]),static_cast<unsigned long>(shadow[0x104]),
        static_cast<unsigned long>(shadow[0x105]),static_cast<unsigned long>(shadow[0x106]),static_cast<unsigned long>(shadow[0x107]),
        static_cast<unsigned long>(shadow[0xe0]),static_cast<unsigned long>(shadow[0x40]));
}
// Memory this process maps for a physical GPU address, or null.
const void* physicalToVirtual(u32 physical,u32 bytes){
    u32 address=0;
    if(physical>=OS_VRAM_PADDR&&physical-OS_VRAM_PADDR<OS_VRAM_SIZE)address=physical-OS_VRAM_PADDR+OS_VRAM_VADDR;
    else if(physical>=OS_FCRAM_PADDR&&physical-OS_FCRAM_PADDR<0x10000000u){
        for(const u32 base:{u32(OS_OLD_FCRAM_VADDR),u32(OS_FCRAM_VADDR)}){
            const u32 candidate=physical-OS_FCRAM_PADDR+base;
            if(osConvertVirtToPhys(reinterpret_cast<const void*>(candidate))==physical){address=candidate;break;}
        }
    }
    if(!address||!bytes)return nullptr;
    for(const u32 at:{address,address+bytes-1}){
        MemInfo info{};PageInfo page{};
        if(R_FAILED(svcQueryMemory(&info,&page,at))||info.state==MEMSTATE_FREE||!(info.perm&MEMPERM_READ))return nullptr;
    }
    return reinterpret_cast<const void*>(address);
}
void runCanary(u32* buffer,u32 size,u32 before,u32 end,u32 value,u32 format){
    const u32 words=end-before;
    // A draw too large to copy cannot run alone; let it through as verified.
    if(words>scratch.size()){verifiedAdd(value);return;}
    std::memcpy(scratch.data(),buffer+before,words*sizeof(u32));
    const bool journal=!verifiedHas(value);
    // Everything emitted before this draw goes first and must complete.
    GPUCMD_SetBuffer(buffer,size,before);
    if(before&&platformCompleteGpu(500000000)!=GpuCompletion::complete){
        u32* now{};u32 nowSize{},nowOffset{};GPUCMD_GetBuffer(&now,&nowSize,&nowOffset);
        if(nowSize-nowOffset>=words)GPUCMD_AddRawCommands(scratch.data(),words);
        verifiedAdd(value);return;
    }
    // The split moved the command span; keep the reserve of an ordinary draw.
    if(!platformPrepareGpuDraw()){verifiedAdd(value);return;}
    GPUCMD_AddRawCommands(scratch.data(),words);
    if(before){
        // The list above took citro3d's pending framebuffer flush: this draw's
        // own pixels must reach the target before any copy or readback.
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH,1);GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE,1);GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR,1);
    }
    // Its vertices can still be a pending arena reservation: publish them.
    flushPendingGxGeometry();
    if(journal){
        // On the card before the PICA sees this draw: a freeze from here on
        // is attributed to it by the next start. Everything before it has
        // completed, so the record names this draw only.
        char text[700];
        int used=std::snprintf(text,sizeof(text),"running sig=%08lx fmt=%08lx program=%d code=%08lx projected=%d draw=%llu\n",static_cast<unsigned long>(value),
            static_cast<unsigned long>(format),gxBoundProgramKind(),static_cast<unsigned long>(programHash),int(gxBoundProgramKind()==2&&gxDrawMultiProjected()),
            static_cast<unsigned long long>(counters.draws));
        if(used>0&&std::size_t(used)+16<sizeof(text)){describe(text+used,sizeof(text)-used-8,value);std::strcat(text,"\n");}
        durableSlotWrite(journalSlot,text);journalRunning=true;
    }
    canary={true,value,format,gxBoundProgramKind(),programHash,gxBoundProgramKind()==2&&gxDrawMultiProjected()};++counters.canaries;
    const u64 start=svcGetSystemTick();
    const auto result=platformCompleteGpu(500000000);
    const unsigned milliseconds=unsigned((svcGetSystemTick()-start)*1000/SYSCLOCK_ARM11);
    if(testHangAt&&counters.canaries==testHangAt){
        const std::uint32_t entry[8]{0x01000101,reinterpret_cast<u32>(scratch.data()),words*4,0,0,0,0,1};
        gpuHang(entry,0,1,milliseconds);
    }
    canary.active=false;
    verifiedAdd(value);
    // Only signatures this console had not completed before (isolate-all runs
    // known ones alone too; appending those grew the file to 570 KB in Alpha 2).
    if(journal&&result==GpuCompletion::complete&&verifiedPendingCount<verifiedPending.size())verifiedPending[verifiedPendingCount++]=value;
    // Which kinds of draw this console has completed, and how long each took.
    if(counters.canaries<=600)OSReport("Native GPU canary ok %llu: %u ms program=%d projected=%d sig=%08lx fmt=%08lx units=%lx indices=%lu\n",
        static_cast<unsigned long long>(counters.canaries),milliseconds,gxBoundProgramKind(),int(canary.projected),static_cast<unsigned long>(value),
        static_cast<unsigned long>(format),static_cast<unsigned long>(shadow[0x80]&7),static_cast<unsigned long>(shadow[0x228]));
    if(result==GpuCompletion::complete&&milliseconds>=30&&counters.slow<64){
        ++counters.slow;char text[512];describe(text,sizeof(text),value);
        OSReport("Native GPU canary slow draw: %u ms %s\n",milliseconds,text);
    }
    if((counters.canaries&(counters.canaries-1))==0)reportGpuCanary();
    u32* now{};u32 nowSize{},nowOffset{};GPUCMD_GetBuffer(&now,&nowSize,&nowOffset);
    parsedBuffer=now;parsedOffset=nowOffset;
}
}
// Every draw of the game renderer passes here (linker --wrap).
extern "C" void __wrap_C3D_DrawElements(GPU_Primitive_t primitive,int count,int type,const void* indices){
    if(!enabled||platformGpuOwner()!=OSGetCurrentThread()){__real_C3D_DrawElements(primitive,count,type,indices);return;}
    u32* buffer{};u32 size{},before{};GPUCMD_GetBuffer(&buffer,&size,&before);
    __real_C3D_DrawElements(primitive,count,type,indices);
    u32* after{};u32 afterSize{},end{};GPUCMD_GetBuffer(&after,&afterSize,&end);
    if(after!=buffer||end<before){
        // The SDK moved the span inside the call: keep the shadow, no canary.
        parse(after,0,end);parsedBuffer=after;parsedOffset=end;return;
    }
    if(parsedBuffer!=buffer){parsedBuffer=buffer;parsedOffset=0;}
    if(parsedOffset<before)parse(buffer,parsedOffset,before);
    const u32 trigger=parse(buffer,before,end);parsedOffset=end;
    if(trigger==~0u)return;
    ++counters.draws;
    const u32 format=shadow[0x201],value=currentSignature();
    if(skipCount||skipFormatCount){
        if(learned(value,format)){
            // Leave the state in place and turn the trigger into a harmless
            // write of a register the same sequence already sets.
            buffer[trigger]=(buffer[trigger]&~0xffffu)|0x25f;
            if(!counters.skipped++)OSReport("Native GPU canary: leaving out draws learned on this console (first sig=%08lx fmt=%08lx)\n",
                static_cast<unsigned long>(value),static_cast<unsigned long>(format));
            return;
        }
    }
    if(!isolateAll&&verifiedHas(value))return;
    runCanary(buffer,size,before,end,value,format);
}
namespace dusk::native {
// gpu_sync_ctru.cpp: every command list about to be queued.
void gpuCanaryListSubmitted(const u32* list,u32 bytes){
    if(!enabled||platformGpuOwner()!=OSGetCurrentThread())return;
    const u32 words=bytes/4;
    if(parsedBuffer!=list){parsedBuffer=list;parsedOffset=0;}
    if(parsedOffset<words)parse(list,parsedOffset,words);
    parsedBuffer=list+words;parsedOffset=0;
}
// Learned entries belong to the build that learned them. Alpha 2 recorded a
// stopped transfer as two innocent draws (left out from then on: missing
// pictures) and as "isolate-all" (every later start ran each draw alone, 3-5
// pictures per second). A start of another build moves the old file aside.
void startLearnedStateForThisBuild(){
    char first[96]{};
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-skip.txt","rb")){
        if(!std::fgets(first,sizeof(first),file))first[0]=0;
        std::fclose(file);
    }else return; // nothing learned yet: the file starts with the first hang
    char expected[64];std::snprintf(expected,sizeof(expected),"build=%s\n",DUSKLIGHT_RELEASE);
    if(!std::strcmp(first,expected))return;
    std::remove("sdmc:/3ds/DuskLight/gpu-skip-old.txt");
    std::rename("sdmc:/3ds/DuskLight/gpu-skip.txt","sdmc:/3ds/DuskLight/gpu-skip-old.txt");
    std::remove("sdmc:/3ds/DuskLight/gpu-skip.txt");
    std::remove("sdmc:/3ds/DuskLight/gpu-resume.txt");
    std::remove("sdmc:/3ds/DuskLight/gpu-isolate.txt");
    char note[160];std::snprintf(note,sizeof(note),"=== DuskLight %s: entries learned by an earlier build moved to gpu-skip-old.txt ===\n",DUSKLIGHT_RELEASE);
    durableAppendText(historyPath,note);
    OSReport("Native GPU canary: %s",note);
}
void appendSkipLine(const char* line){
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-skip.txt","rb"))std::fclose(file);
    else{char header[64];std::snprintf(header,sizeof(header),"build=%s\n",DUSKLIGHT_RELEASE);durableAppendText(skipPath,header);}
    durableAppendText(skipPath,line);
}
void initializeGpuCanary(){
    enabled=true;markSignatureRegisters();
    if(auto* off=std::fopen("sdmc:/3ds/DuskLight/gpu-canary-off.txt","rb")){std::fclose(off);enabled=false;}
    if(enabled)startLearnedStateForThisBuild();
    // What the last start left: a draw it froze on, or a freeze elsewhere.
    char journalText[700]{},sessionText[200]{};
    durableReadText(journalPath,journalText,sizeof(journalText));
    durableReadText(sessionPath,sessionText,sizeof(sessionText));
    {
        unsigned earlier=0;
        if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-skip.txt","rb")){char line[160];while(std::fgets(line,sizeof(line),file))earlier+=std::strncmp(line,"hang ",5)==0;std::fclose(file);}
        unsigned long value{},format{};char record[900];
        if(enabled&&std::sscanf(journalText,"running sig=%lx fmt=%lx",&value,&format)==2){
            // The console froze while this new kind of draw ran alone: learn it.
            int program=-1;if(const char* at=std::strstr(journalText,"program="))program=std::atoi(at+8);
            unsigned long code=0;if(const char* at=std::strstr(journalText,"code="))code=std::strtoul(at+5,nullptr,16);
            std::snprintf(record,sizeof(record),"hang %u sig=%08lx fmt=%08lx program=%d code=%08lx%s froze\n",earlier+1,value,format,program,code,
                std::strstr(journalText,"projected=1")?" projected=1":"");
            appendSkipLine(record);
            std::snprintf(record,sizeof(record),"Hang %u: the last start froze on this draw (found at boot):\n%s\n",earlier+1,journalText);
            durableAppendText(historyPath,record);autoAdvance=true;
        }else if(enabled&&std::strncmp(journalText,"before",6)==0){
            // Recorded only: nothing here names a draw or a job.
            std::snprintf(record,sizeof(record),"Note: the last start stopped in the batch before a new draw (found at boot); nothing learned:\n%s\n",journalText);
            durableAppendText(historyPath,record);
        }else if(enabled&&std::strncmp(sessionText,"running",7)==0){
            // Switched off while playing, or frozen outside any draw: the same
            // file. Recorded only; it never slows the next start down (Alpha 2
            // ran every draw alone after this).
            std::snprintf(record,sizeof(record),"Note: the last start ended without exiting (switched off, or frozen outside a draw): %s",sessionText);
            durableAppendText(historyPath,record);
        }
        previousHangPending=std::strncmp(sessionText,"ended (GPU hang",15)==0;
        if(durableSlotOpen(journalSlot,journalPath,768))durableSlotWrite(journalSlot,"idle\n");
        if(durableSlotOpen(sessionSlot,sessionPath,256))durableSlotWrite(sessionSlot,"running frame=0 stage=boot\n");
    }
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-skip.txt","rb")){
        char line[160];
        while(std::fgets(line,sizeof(line),file)){
            unsigned number{};unsigned long value{},format{};
            if(std::sscanf(line,"hang %u sig=%lx fmt=%lx",&number,&value,&format)==3){
                int program=-1;if(const char* at=std::strstr(line,"program="))program=std::atoi(at+8);
                // Records of an older build carry no code hash (or another one):
                // they never count against the program of this build.
                u32 code=0;if(const char* at=std::strstr(line,"code="))code=u32(std::strtoul(at+5,nullptr,16));
                const bool current=program>=0&&code&&code==gxProgramCodeHash(program);
                if(!current)program=-1;
                if(skipCount<skipSignatures.size()){skipSignatures[skipCount]=u32(value);skipSignatureFormats[skipCount]=u32(format);skipSignatureShaders[skipCount]=code;skipSignaturePrograms[skipCount++]=program;}
                if(program==1)rigidSingleRefused=true;
                if(program==2&&std::strstr(line,"projected=1"))multiProjectedRefused=true;
                ++priorHangs;
            }else if(std::sscanf(line,"hang %u",&number)==1){
                ++priorHangs;if(std::strstr(line,"transfer-stop"))++transferStops;
            }
        }
        std::fclose(file);
    }
    // One start in isolate-all mode after an unexplained stop, then normal again.
    if(auto* isolate=std::fopen("sdmc:/3ds/DuskLight/gpu-isolate.txt","rb")){
        std::fclose(isolate);isolateAll=enabled;std::remove("sdmc:/3ds/DuskLight/gpu-isolate.txt");
    }
    // Two different materials stopping the PICA with one vertex format point
    // at its shader or layout: leave that whole format out and move on.
    for(unsigned i=0;i<skipCount;++i){
        if(skipSignaturePrograms[i]<0)continue; // older records: program unknown
        unsigned same=0;bool listed=false;
        for(unsigned j=0;j<skipCount;++j)same+=skipSignatureFormats[j]==skipSignatureFormats[i]&&skipSignaturePrograms[j]==skipSignaturePrograms[i];
        for(unsigned j=0;j<skipFormatCount;++j)listed|=skipFormats[j]==skipSignatureFormats[i]&&skipFormatPrograms[j]==skipSignaturePrograms[i];
        if(same>=2&&!listed&&skipFormatCount<skipFormats.size()){skipFormats[skipFormatCount]=skipSignatureFormats[i];skipFormatPrograms[skipFormatCount++]=skipSignaturePrograms[i];}
    }
    // Signatures completed in earlier sessions on this console. A file with
    // repeated entries (Alpha 2 appended every draw in isolate-all mode) is
    // written again with each signature once.
    if(enabled){
        bool valid=false;unsigned entries=0;
        if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-verified.bin","rb")){
            u32 header[2]{};
            if(std::fread(header,sizeof(u32),2,file)==2&&header[0]==0x56474c44u&&header[1]==1){
                valid=true;u32 chunk[256];std::size_t n;
                while((n=std::fread(chunk,sizeof(u32),std::size(chunk),file))>0)
                    for(std::size_t i=0;i<n;++i)if(chunk[i]){
                        ++entries;
                        if(verifiedLoaded<5500&&!verifiedHas(chunk[i])){verifiedAdd(chunk[i]);++verifiedLoaded;}
                    }
            }
            std::fclose(file);
        }
        if(!valid||entries>verifiedLoaded){
            const u32 header[2]{0x56474c44u,1};durableReplace(verifiedPath,header,sizeof(header));
            if(valid&&verifiedLoaded){
                std::size_t at=0;
                for(const u32 value:verified)if(value){
                    verifiedPending[at++]=value;
                    if(at==verifiedPending.size()){durableAppend(verifiedPath,verifiedPending.data(),at*sizeof(u32));at=0;}
                }
                if(at)durableAppend(verifiedPath,verifiedPending.data(),at*sizeof(u32));
                OSReport("Native GPU canary: gpu-verified.bin rewritten: %u entries -> %u signatures\n",entries,verifiedLoaded);
            }
        }
    }
    if(auto* test=std::fopen("sdmc:/3ds/DuskLight/gpu-canary-test.txt","rb")){if(std::fscanf(test,"%u",&testHangAt)!=1)testHangAt=0;std::fclose(test);}
    if(auto* resume=std::fopen("sdmc:/3ds/DuskLight/gpu-resume.txt","rb")){std::fclose(resume);std::remove("sdmc:/3ds/DuskLight/gpu-resume.txt");autoAdvance=true;}
    OSReport("Native GPU canary: %s; earlier hangs=%u learned signatures=%u formats left out=%u isolate-all=%d auto-advance=%d known-good signatures=%u transfer stops=%u\n",
        enabled?"on (first draw of each signature runs alone)":"off (SD switch)",priorHangs,skipCount,skipFormatCount,int(isolateAll),int(autoAdvance),verifiedLoaded,transferStops);
    for(unsigned i=0;i<skipFormatCount;++i)OSReport("Native GPU canary: vertex format %08lx with program %d is left out\n",static_cast<unsigned long>(skipFormats[i]),skipFormatPrograms[i]);
    OSReport("Native GPU canary: shader code hashes colour=%08lx rigid=%08lx multi-texture=%08lx\n",static_cast<unsigned long>(gxProgramCodeHash(0)),
        static_cast<unsigned long>(gxProgramCodeHash(1)),static_cast<unsigned long>(gxProgramCodeHash(2)));
    if(rigidSingleRefused)OSReport("Native GPU canary: the single-texture rigid program hung on this console; its draws take the multi-texture or CPU routes\n");
    if(multiProjectedRefused)OSReport("Native GPU canary: the multi-texture program with a projected unit hung on this console; those draws take the CPU route\n");
    if(transferStops)OSReport("Native GPU canary: the transfer engine stopped on this console: no transfer jobs inside a frame (GPU captures and bloom off)%s\n",
        transferStops>=2?"; no offscreen render targets (map texture kept)":"");
}
void reportGpuCanary(){
    OSReport("Native GPU canary: draws=%llu signatures run alone=%llu left out=%llu slow=%llu\n",
        static_cast<unsigned long long>(counters.draws),static_cast<unsigned long long>(counters.canaries),
        static_cast<unsigned long long>(counters.skipped),static_cast<unsigned long long>(counters.slow));
}
bool gpuCanaryAutoAdvance(){return autoAdvance;}
unsigned gpuCanaryTransferLevel(){return transferStops>=2?2u:transferStops;}
bool gpuCanaryRigidSingleRefused(){return rigidSingleRefused;}
bool gpuCanaryMultiProjectedRefused(){return multiProjectedRefused;}
// A held for about 120 ms every two seconds: one press edge per screen.
bool gpuCanaryPressA(){return autoAdvance&&svcGetSystemTick()%(2ull*SYSCLOCK_ARM11)<SYSCLOCK_ARM11/8;}
void gpuCanaryWorldEntered(){
    if(autoAdvance){autoAdvance=false;OSReport("Native GPU canary: world entered, automatic A presses stopped\n");}
}
void gpuSlowJob(const std::uint32_t entry[8],unsigned milliseconds){
    char text[512]="not a canary";
    if(canary.active)describe(text,sizeof(text),canary.signature);
    OSReport("Native GPU job completed late: %u ms type=%lu bytes=%lu; %s\n",milliseconds,
        static_cast<unsigned long>(entry[0]&0xff),static_cast<unsigned long>(entry[2]),text);
}
[[noreturn]] void gpuHang(const std::uint32_t entry[8],unsigned index,unsigned count,unsigned milliseconds){
    // Nothing here talks to GSP or the GPU before the evidence is on the card:
    // a stopped PICA can also stop the GSP service (CIA f saved nothing).
    const unsigned type=entry[0]&0xff,number=priorHangs+1;
    if(previousHangPending){
        // Nothing new: the PICA stopped in the last start is still stopped.
        char note[200];
        std::snprintf(note,sizeof(note),"Start after hang %u: the GPU was still stopped (job type %u); the console was not switched off in between.\n",priorHangs,type);
        durableAppendText(historyPath,note);OSReport("Native GPU guard: %s",note);flushNativeLog();
        restartAfterGpuHang("The GPU is still stopped from\nthe last start.",true);
    }
    // A transfer counted complete without its event shortly before: this job
    // waits behind a stopped transfer engine. Alpha 2 blamed the draw that
    // happened to run alone next (a capture or bloom quad) and left it out.
    unsigned assumedAge=0,assumedType=0;std::uint32_t assumedWords[8]{};
    const bool afterLostTransfer=gpuRecentAssumedTransfer(assumedAge,assumedType,assumedWords)&&assumedAge<15000;
    const bool transfer=type==3||type==4||afterLostTransfer;
    const bool list=type==1,attributed=list&&canary.active&&!transfer;
    char text[512]="";
    if(attributed)describe(text,sizeof(text),canary.signature);
    else if(transfer)std::snprintf(text,sizeof(text),"transfer type=%u words=%08lx %08lx %08lx %08lx %08lx %08lx %08lx counted complete %u ms before",
        afterLostTransfer?assumedType:type,static_cast<unsigned long>(assumedWords[0]),static_cast<unsigned long>(assumedWords[1]),static_cast<unsigned long>(assumedWords[2]),
        static_cast<unsigned long>(assumedWords[3]),static_cast<unsigned long>(assumedWords[4]),static_cast<unsigned long>(assumedWords[5]),static_cast<unsigned long>(assumedWords[6]),
        afterLostTransfer?assumedAge:0u);
    // The last GPU jobs, oldest first (ctru_gxqueue.c ring).
    static u32 jobs[14*32];u32 submittedTotal=0;
    const unsigned jobCount=duskGxJobRingCopy(jobs,32,&submittedTotal);
    char jobText[1300]="";
    {int used=0;const u64 now=svcGetSystemTick();
     for(unsigned i=jobCount>10?jobCount-10:0;i<jobCount&&used>=0&&std::size_t(used)<sizeof(jobText)-1;++i){
        const u32* j=jobs+i*14;const u64 submitted=u64(j[8])|(u64(j[9])<<32),completed=u64(j[10])|(u64(j[11])<<32);
        const unsigned ago=unsigned((now-submitted)*1000/SYSCLOCK_ARM11);
        used+=std::snprintf(jobText+used,sizeof(jobText)-used,"  job %lu type=%lu %08lx %08lx %08lx %08lx %08lx %08lx sent %u ms ago, %s%s\n",
            static_cast<unsigned long>(submittedTotal-jobCount+i),static_cast<unsigned long>(j[0]&0xff),static_cast<unsigned long>(j[1]),static_cast<unsigned long>(j[2]),
            static_cast<unsigned long>(j[3]),static_cast<unsigned long>(j[4]),static_cast<unsigned long>(j[5]),static_cast<unsigned long>(j[6]),ago,
            completed?"done":"never signalled",j[13]?" (counted complete, no event)":"");
     }}
    char record[2900];
    std::snprintf(record,sizeof(record),"Hang %u: job %u of %u type=%u silent for %u ms; words=%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n%s%s\nlast GPU jobs:\n%s",
        number,index,count,type,milliseconds,
        static_cast<unsigned long>(entry[0]),static_cast<unsigned long>(entry[1]),static_cast<unsigned long>(entry[2]),static_cast<unsigned long>(entry[3]),
        static_cast<unsigned long>(entry[4]),static_cast<unsigned long>(entry[5]),static_cast<unsigned long>(entry[6]),static_cast<unsigned long>(entry[7]),
        transfer?"the transfer engine stopped: ":attributed?"the draw running alone: ":list?"no single draw to blame (batched list)":"no single draw to blame (not a command list)",text,jobText);
    // 1. What the next start must do differently.
    char line[160];bool progress=false;
    if(transfer){std::snprintf(line,sizeof(line),"hang %u transfer-stop type=%u\n",number,afterLostTransfer?assumedType:type);progress=true;}
    else if(attributed){std::snprintf(line,sizeof(line),"hang %u sig=%08lx fmt=%08lx program=%d code=%08lx%s\n",number,static_cast<unsigned long>(canary.signature),static_cast<unsigned long>(canary.format),canary.program,static_cast<unsigned long>(canary.shader),canary.projected?" projected=1":"");progress=true;}
    else if(list){
        std::snprintf(line,sizeof(line),"hang %u unattributed-list\n",number);
        // The next start (only that one) runs every draw alone to find it.
        if(!isolateAll){durableReplaceText(isolatePath,"isolate-all\n");progress=true;}
    }
    else std::snprintf(line,sizeof(line),"hang %u unattributed type=%u\n",number,type);
    appendSkipLine(line);
    durableSlotWrite(journalSlot,"idle\n");journalRunning=false;
    const bool restart=progress&&number<maximumRestarts;
    // Automatic A presses after the restart only while learning draws.
    if(restart&&attributed)durableReplaceText(resumePath,"resume\n");
    // 2. The readable record and the log.
    durableAppendText(historyPath,record);
    OSReport("Native GPU hang %s",record);
    flushNativeLog();
    // 3. The draw, the shadow PICA state, uniforms, indices and vertices.
    char path[64];std::snprintf(path,sizeof(path),"/3ds/DuskLight/gpu-hang-%02u.bin",number);
    durableReplace(path,nullptr,0);
    auto chunk=[&path](const char* tag,const void* data,u32 bytes){
        u8 header[8];std::memcpy(header,tag,4);std::memcpy(header+4,&bytes,4);
        durableAppend(path,header,8);if(bytes)durableAppend(path,data,bytes);
    };
    chunk("ENTR",entry,32);
    chunk("REGS",shadow.data(),u32(shadow.size()*4));
    chunk("UNIF",uniforms.data(),u32(uniforms.size()*16));
    chunk("UNIW",uniformWordsStored.data(),u32(uniformWordsStored.size()));
    if(jobCount)chunk("JOBS",jobs,jobCount*14*4);
    if(list&&entry[1]&&entry[2]&&entry[2]<=1024*1024&&!(entry[2]&3))chunk("CMDS",reinterpret_cast<const void*>(entry[1]),entry[2]);
    if(attributed){
        const u32 base=shadow[0x200]<<3,indices=shadow[0x228],stride=(shadow[0x205]>>16)&0xff;
        const bool wide=shadow[0x227]>>31;const u32 indexBytes=indices*(wide?2:1);
        const auto* indexData=static_cast<const u8*>(physicalToVirtual(base+(shadow[0x227]&0x7fffffffu),indexBytes));
        u32 highest=0;
        if(indexData&&indexBytes<=512*1024){
            chunk("INDX",indexData,indexBytes);
            for(u32 i=0;i<indices;++i){const u32 v=wide?u32(indexData[2*i]|indexData[2*i+1]<<8):indexData[i];if(v>highest)highest=v;}
            const u32 vertexBytes=(highest+1)*stride;
            if(stride&&vertexBytes<=1024*1024)if(const auto* vertexData=physicalToVirtual(base+shadow[0x203],vertexBytes))chunk("VERT",vertexData,vertexBytes);
        }
    }
    durableSlotWrite(sessionSlot,"ended (GPU hang recorded)\n");
    OSReport("Native GPU hang %u: evidence written; %s\n",number,restart?"starting the game again":"nothing new to learn; stopping");
    flushNativeLog();
    char message[400];
    std::snprintf(message,sizeof(message),"Hang %u (%s).\n%.250s",number,
        transfer?"GPU transfer stopped; the next start avoids transfers":attributed?"one draw, now left out":list?"batched list":"GPU job",transfer?"":text);
    restartAfterGpuHang(message,restart);
}
namespace {
void flushVerified(unsigned long long frame){
    if(!verifiedPendingCount)return;
    durableAppend(verifiedPath,verifiedPending.data(),verifiedPendingCount*sizeof(u32));
    verifiedPendingCount=0;verifiedFlushFrame=frame;
}
}
namespace {
// Emulator test of the transfer-stop path (Azahar never stops a transfer):
// gpu-transfer-test.txt with "frame=N" reports the first texture copy sent
// at or after picture N as a stopped transfer engine.
unsigned long long transferTestFrame{};bool transferTestRead{};u32 transferTestScanned{};
void transferTest(unsigned long long frame){
    if(!transferTestRead){
        transferTestRead=true;
        if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-transfer-test.txt","rb")){
            char text[64]{};std::fread(text,1,sizeof(text)-1,file);std::fclose(file);
            if(const char* at=std::strstr(text,"frame="))transferTestFrame=std::strtoull(at+6,nullptr,10);
            if(transferTestFrame)OSReport("Native GPU canary: transfer-stop test armed at picture %llu\n",transferTestFrame);
        }
    }
    if(!transferTestFrame)return;
    static u32 jobs[14*32];u32 total=0;const unsigned n=duskGxJobRingCopy(jobs,32,&total);
    for(unsigned i=0;i<n;++i){
        const u32 number=total-n+i;if(number<transferTestScanned)continue;
        transferTestScanned=number+1;
        if(frame<transferTestFrame||(jobs[i*14]&0xff)!=4)continue;
        transferTestFrame=0;
        transferTestStop(jobs+i*14);
    }
}
}
void gpuCanaryFrameEnd(unsigned long long frame,const char* stage){
    if(!enabled)return;
    transferTest(frame);
    // At most one SD append every 300 pictures while new kinds of draw appear.
    if(verifiedPendingCount&&(frame-verifiedFlushFrame>=300||verifiedPendingCount>=verifiedPending.size()/2))flushVerified(frame);
    // The draws that ran alone this frame completed: one write for all of them.
    if(journalRunning){durableSlotWrite(journalSlot,"idle\n");journalRunning=false;}
    if(frame>=30)previousHangPending=false;
    if(frame%60)return;
    char text[200];
    std::snprintf(text,sizeof(text),"running frame=%llu stage=%.8s draws=%llu canaries=%llu left-out=%llu\n",frame,stage&&stage[0]?stage:"-",
        static_cast<unsigned long long>(counters.draws),static_cast<unsigned long long>(counters.canaries),static_cast<unsigned long long>(counters.skipped));
    durableSlotWrite(sessionSlot,text);
}
void gpuCanaryShadowState(const std::uint32_t*& registers,std::size_t& registerWords,const std::uint32_t*& uniformRows,std::size_t& uniformWords){
    registers=shadow.data();registerWords=shadow.size();
    uniformRows=uniforms[0].data();uniformWords=uniforms.size()*4;
}
void gpuCanarySessionEnd(){if(enabled){flushVerified(verifiedFlushFrame);durableSlotWrite(sessionSlot,"ended\n");}}
}

// The map and other offscreen render targets (d_map_path.cpp).
extern "C" bool duskNativeOffscreenRefused(){return dusk::native::gpuCanaryTransferLevel()>=2;}
