#include "gpu_canary.hpp"
#include "gpu_sync.hpp"
#include "gx_profile.hpp"
#include "bootstrap.hpp"
#include "durable_file.hpp"
#include "../gx_geometry_pica.hpp"
#include "release.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
extern "C" OSThread* OSGetCurrentThread();
extern "C" void OSReport(const char*,...);
extern "C" void __real_C3D_DrawElements(GPU_Primitive_t,int,int,const void*);
namespace dusk::native {
[[noreturn]] void restartAfterGpuHang(const char* text,bool restart);
int gxBoundProgramKind(); // gx_pica.cpp: 0 colour, 1 rigid, 2 rigid multi-texture, 3 shadow caster, 4 a multi-texture variant, -1 other
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
// "idle" before any other command list is queued after it (or at the end of
// the frame). A start that finds "running" knows the last session froze on
// that draw and learns it.
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
// write changed one of them (most consecutive draws keep it). It mixes three
// groups in order: 1 vertex input, shader, raster and blend; 2 texture units;
// 4 TEV stages. The hash after the first two is kept, so a draw that changed
// only textures or stages mixes from there (the same value as a full mix).
std::array<std::uint8_t,registerCount> signatureRegister{};
unsigned signatureDirty{7};u32 signatureCached{},hashAfterVertex{},hashAfterUnits{};
// Steady-state cost of the guard, reported every 300 pictures: words parsed,
// words taken as a block (a run of writes to one register keeps only its last
// value; uniform rows are copied whole), signature mixes (partly), draws that
// kept the last known signature, draws known by a fragment-only difference
// (canaries saved), waits added so that a canary runs alone, journal SD writes
// and the "idle" ones written before a batched list (not at frame end).
struct GuardCounters {std::uint64_t words{},blockWords{},signatures{},partialSignatures{},sameSignature{},coarseKnown{},aloneWaits{},
    journalWrites{},journalIdleBeforeList{};};
GuardCounters guard{},guardReported{};
std::uint64_t drawsReported{},canariesReported{};

bool enabled{},isolateAll{},autoAdvance{};
// Canaries: the first draw of each new signature runs alone, after a GPU
// drain and an SD journal write. Off by default since Alpha 11: the console
// Alpha 10 run spent up to 3.7 s per 300 pictures in those drains, and 400-650
// ms pictures when a menu or effect appeared for the first time (the owner's
// "first time it stutters, second time not"); every new build started from an
// empty known-good table, so each CIA stuttered again. No P3D draw has stopped
// the PICA since the output rule of Alpha 0 (CIA j). The rest of the guard
// stays: the stopped-job watchdog with its evidence, the transfer, shadow and
// HUD-tape stop classes and the learned skips. SD switch gpu-canary-on.txt
// (or a hand-written gpu-isolate.txt) turns canaries on again.
bool canaries{};
// Transfer-engine stops recorded on this console with this build. One: no
// transfer jobs inside a frame (GPU-resident captures and bloom off). Two or
// more: no offscreen render targets either (the map texture is not redrawn).
unsigned transferStops{};
// Stops in a command list that drew into the model-shadow image (recorded as
// "shadow-stop"). The next start keeps blob shadows only (no caster pass).
unsigned shadowStops{};
// Physical address >> 3 of the shadow image, as the PICA colour buffer
// register holds it (gx_pica.cpp, before each caster pass). 0 = no pass yet.
u32 shadowTargetLocation{};
// Command lists queued while the model-shadow pass ran (the pass is split
// into several lists; only the first one switches the colour target).
bool shadowPassActive{};
std::array<const u32*,16> shadowPassLists{};
unsigned shadowPassListCount{};
bool shadowPassList(const void* list){
    for(const auto* known:shadowPassLists)if(known&&known==list)return true;
    return false;
}
// HUD tape (hud_tape.hpp): the command lists that carry a recorded or replayed
// 2D pass. A stop in one of them is recorded as "hud-tape-stop" and the next
// start draws the pass in every picture (tapeStops, as shadowStops).
bool tapePending{};
std::array<const u32*,16> tapeLists{};
unsigned tapeListCount{},tapeStops{};
bool tapeList(const void* list){
    for(const auto* known:tapeLists)if(known&&known==list)return true;
    return false;
}
// True when the command list writes `location` into COLORBUFFER_LOC (0x11d):
// it rendered into that target. Words: parameter, header, extra parameters.
bool listRendersInto(const u32* words,u32 count,u32 location){
    for(u32 i=0;i+1<count;){
        const u32 header=words[i+1],reg=header&0xffff,extra=(header>>20)&0xff;
        const bool sequential=header>>31;
        for(u32 k=0;k<=extra&&i+1+k<count;++k){
            const u32 value=k==0?words[i]:words[i+1+k];
            if((sequential?reg+k:reg)==0x11d&&value==location)return true;
        }
        i+=2+extra;if(i&1)++i;
    }
    return false;
}
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
// The same for the variants of the multi-texture program (kind 4): their
// plans take the general program on the next start (gpuCanaryVertexVariantsRefused).
bool vertexVariantsRefused{};
// Known-good signatures (open addressing, 0 = empty). A start loads the most
// recent verifiedLoadLimit distinct entries of gpu-verified.bin; the table is
// never cleared in a session. Alpha 6 cleared it at 6000 entries and loaded at
// most 5500, while the console file already held 5567: every later start
// would run thousands of known draws alone again.
constexpr unsigned verifiedSlots=16384,verifiedLoadLimit=8192,verifiedLimit=12288;
std::array<u32,verifiedSlots> verified{};unsigned verifiedCount{};
// The signature of the last draw found known (consecutive draws repeat it).
u32 lastKnown{};
// SD switch gpu-canary-coarse.txt: a draw that differs from a completed one
// only in fragment state (TEV, blending, tests, masks, culling) is not run
// alone. Off by default: it narrows what a stop can be learned from.
bool coarseKnown{};
// Newly completed signatures waiting for one batched SD append.
std::array<u32,256> verifiedPending{};unsigned verifiedPendingCount{},verifiedLoaded{};unsigned long long verifiedFlushFrame{};
struct Counters {std::uint64_t draws{},canaries{},skipped{},slow{};} counters;
// `journaled`: the journal names this draw ("running") while its list runs.
struct Canary {bool active{};u32 signature{},format{};int program{-1};u32 shader{};bool projected{};u32 list{};bool journaled{};} canary;
std::array<u32,4096> scratch;
// Journal and session records: fixed-size slots kept open, one in-place SD
// write each (a new signature costs one write; one "idle" follows the draws
// that ran alone back to back, before the next batched list).
DurableSlot journalSlot{},sessionSlot{};
bool journalRunning{};
void journalWrite(const char* text){++guard.journalWrites;durableSlotWrite(journalSlot,text);}
// Set once the session record is final (a GPU hang, a fatal error or an
// exception): nothing may overwrite it before the process ends.
std::atomic<bool> sessionFinal{};
// The "running" record waiting for the SD writer thread; sessionLock orders
// it with the final records (a final record is never overwritten).
LightLock sessionLock=1;char sessionPending[200]{};
// Final records: wait (bounded) for a session write in flight on the writer
// thread, so "running" cannot land after them. Never blocks for long: the
// writer may be the faulting thread.
void finalSessionWrite(const char* record){
    bool locked=false;
    for(unsigned n=0;n<300&&!(locked=LightLock_TryLock(&sessionLock)==0);++n)svcSleepThread(1000000);
    durableSlotWrite(sessionSlot,record);
    if(locked)LightLock_Unlock(&sessionLock);
}
// gpuCanaryTraceNextPicture: armed until the frame ends, then on for one picture.
bool traceArmed{},tracing{};unsigned traceIndex{};
// The previous start's session record and what it means; this start's word.
char previousSession[200]{};
dusk::native::GpuPreviousEnd previousEnd{};
char sessionWord[18]{};
// Emulator test of the hang path (the PICA never stops there): the Nth canary
// is reported as a hang after it completed. SD file gpu-canary-test.txt.
unsigned testHangAt{};

// The shadow part of a register write (masked bytes merged), marking the
// signature groups it changes.
inline void storeRegister(unsigned reg,u32 value,unsigned mask){
    u32 merged=value;
    if(mask!=0xf){
        u32 bits=0;for(unsigned b=0;b<4;++b)if(mask>>b&1)bits|=0xffu<<(8*b);
        merged=(shadow[reg]&~bits)|(value&bits);
    }
    if(merged!=shadow[reg]){shadow[reg]=merged;signatureDirty|=signatureRegister[reg];}
}
// Uniform and shader-code uploads: every write counts. For any other register
// a run of writes leaves only its last value (masked bytes merge the same way).
constexpr bool uploadRegister(unsigned reg){return reg>=0x2bf&&reg<=0x2d3;}
inline void mixCode(u32 value){codeRunning=(codeRunning^value)*0x9E3779B1u;codeRunning^=codeRunning>>15;}
inline void applyWrite(unsigned reg,u32 value,unsigned mask){
    if(reg>=registerCount)return;
    storeRegister(reg,value,mask);
    // Only the uniform and shader-code uploads need more than the shadow.
    if(!uploadRegister(reg))return;
    if(reg==0x2c0){uniformIndex=value&0xff;uniformFloat32=value>>31;uniformFill=0;}
    else if(reg>=0x2c1&&reg<=0x2c8){
        uniformPending[uniformFill++]=value;
        if(uniformFill==(uniformFloat32?4u:3u)){
            if(uniformIndex<uniforms.size()){uniforms[uniformIndex]=uniformPending;uniformWordsStored[uniformIndex]=std::uint8_t(uniformFill);}
            ++uniformIndex;uniformFill=0;
        }
    }
    else if(reg==0x2cb)codeRunning=0x811C9DC5u;
    else if(reg>=0x2cc&&reg<=0x2d3)mixCode(value);
    else if(reg==0x2bf){programHash=codeRunning;signatureDirty|=1;}
}
// Float uniform data words, in order, into the uniform shadow. Matrix uploads
// are most of the words of a frame (about 130 thousand in the title). Rows of
// four words (the 32-bit form Citro3D uploads) go in as one copy per run
// (Alpha 6 called memcpy once per row).
inline void feedUniformWords(const u32* words,unsigned count){
    const unsigned row=uniformFloat32?4u:3u;
    while(count&&uniformFill){
        uniformPending[uniformFill++]=*words++;--count;
        if(uniformFill==row){
            if(uniformIndex<uniforms.size()){uniforms[uniformIndex]=uniformPending;uniformWordsStored[uniformIndex]=std::uint8_t(row);}
            ++uniformIndex;uniformFill=0;
        }
    }
    if(row==4){
        const unsigned rows=count/4;
        if(rows&&uniformIndex<uniforms.size()){
            const unsigned stored=std::min<unsigned>(rows,unsigned(uniforms.size())-uniformIndex);
            std::memcpy(uniforms.data()+uniformIndex,words,stored*4*sizeof(u32));
            std::memset(uniformWordsStored.data()+uniformIndex,4,stored);
        }
        uniformIndex+=rows;words+=rows*4;count-=rows*4;
    }else for(;count>=row;words+=row,count-=row,++uniformIndex)if(uniformIndex<uniforms.size()){
        std::memcpy(uniforms[uniformIndex].data(),words,row*sizeof(u32));uniformWordsStored[uniformIndex]=std::uint8_t(row);
    }
    while(count){uniformPending[uniformFill++]=*words++;--count;}
}
// PICA command words [begin,end) of one buffer. Returns the word index of the
// header of a draw trigger (DRAWARRAYS/DRAWELEMENTS), or ~0 when there is none.
// The shadow it leaves is the one a write-by-write pass leaves (the host test
// gpu_canary_test.cpp compares both); runs of writes to one register cost one
// store instead of one per word.
u32 parse(const u32* list,u32 begin,u32 end){
    u32 trigger=~0u;
    if(end>begin)guard.words+=end-begin;
    for(u32 i=begin;i+1<end;){
        const u32 header=list[i+1];
        // Most commands write one whole word to one register that is neither
        // an upload nor a draw trigger: only the shadow store is left of the
        // general case below.
        if((header&0x8fff0000u)==0x000f0000u){
            const unsigned plain=header&0xffff;
            if(plain<0x22e||(plain>0x22f&&plain<0x2bf)||(plain>0x2d3&&plain<registerCount)){
                const u32 value=list[i];
                if(value!=shadow[plain]){shadow[plain]=value;signatureDirty|=signatureRegister[plain];}
                i+=2;i+=i&1;continue;
            }
        }
        const unsigned reg=header&0xffff,mask=(header>>16)&0xf,extra=(header>>20)&0xff;
        if(i+2+extra>end)break;
        const u32 first=list[i];const u32* rest=list+i+2;
        const bool sequential=header>>31;
        if(reg==0x22e||reg==0x22f)trigger=i+1;
        if(!sequential){
            if(reg==0x2c1&&mask==0xf){
                // Uniform data repeated on one register (the only form the SDK
                // and the renderer emit for it).
                feedUniformWords(&first,1);feedUniformWords(rest,extra);
                shadow[reg]=extra?rest[extra-1]:first;
                guard.blockWords+=1+extra;
            }else if(reg>=registerCount){
            }else if(!uploadRegister(reg)){
                storeRegister(reg,extra?rest[extra-1]:first,mask);
                guard.blockWords+=extra;
            }else if(reg>=0x2cc&&reg<=0x2d3){
                // Shader code: every word goes into the program hash.
                mixCode(first);for(unsigned n=0;n<extra;++n)mixCode(rest[n]);
                storeRegister(reg,extra?rest[extra-1]:first,mask);
            }else{
                applyWrite(reg,first,mask);for(unsigned n=0;n<extra;++n)applyWrite(reg,rest[n],mask);
            }
        }else if(mask==0xf&&reg+extra<registerCount&&(reg+extra<0x2bf||reg>0x2d3)){
            // Consecutive registers, none of them an upload: shadow stores only.
            storeRegister(reg,first,0xf);for(unsigned n=0;n<extra;++n)storeRegister(reg+1+n,rest[n],0xf);
        }else{
            applyWrite(reg,first,mask);for(unsigned n=0;n<extra;++n)applyWrite(reg+1+n,rest[n],mask);
        }
        i+=2+extra;i+=i&1;
    }
    return trigger;
}
// Vertex format, shader, textures and raster state: what a draw is, without
// its addresses, sizes, uniforms or viewport. Mixed in three groups, in this
// order; the result is the value one pass over all of them gives (it is
// stored in gpu-verified.bin and gpu-skip.txt, so it must not change).
inline void mix(u32& h,u32 v){h=(h^v)*0x9E3779B1u;h^=h>>15;}
constexpr std::uint16_t signaturePlain[]={0x201,0x202,0x2bb,0x2bc,0x242,0x2b9,0x2ba,0x2bd,0x4f,0x50,0x51,0x52,0x53,0x54,0x55,0x56,
    0x64,0x6f,0x100,0x101,0x102,0x106,0x107,0x40,0x6d,0x62,0x118,0x61};
constexpr std::uint16_t signatureUnits[3][4]={{0x82,0x83,0x84,0x8e},{0x92,0x93,0x94,0x96},{0x9a,0x9b,0x9c,0x9e}};
constexpr std::uint16_t signatureStages[]={0xc0,0xc8,0xd0,0xd8,0xf0,0xf8};
u32 hashVertexGroup(){
    u32 h=0x811C9DC5u;
    for(const auto reg:signaturePlain)mix(h,shadow[reg]);
    mix(h,programHash);mix(h,shadow[0x80]&0x3f07);mix(h,shadow[0xe0]&0x00ffff07);mix(h,shadow[0x104]&0xff);mix(h,shadow[0x105]&0xff);mix(h,shadow[0x25e]&0xffff);
    return h;
}
u32 hashUnitGroup(u32 h){
    for(unsigned unit=0;unit<3;++unit){
        if(shadow[0x80]>>unit&1)for(const auto reg:signatureUnits[unit])mix(h,shadow[reg]);
        else mix(h,0x55aa0000u+unit);
    }
    return h;
}
u32 hashStageGroup(u32 h){
    for(const auto base:signatureStages){mix(h,shadow[base]);mix(h,shadow[base+1]);mix(h,shadow[base+2]);mix(h,shadow[base+4]);}
    return h;
}
// Every register the groups read, with its group bit (keep the lists above
// and these together). 0x80 also selects the units: it is in the first group.
void markSignatureRegisters(){
    for(const auto reg:signaturePlain)signatureRegister[reg]=1;
    for(const unsigned reg:{0x80u,0xe0u,0x104u,0x105u,0x25eu})signatureRegister[reg]=1;
    for(const auto& unit:signatureUnits)for(const auto reg:unit)signatureRegister[reg]=2;
    for(const auto base:signatureStages)for(const unsigned n:{0u,1u,2u,4u})signatureRegister[base+n]=4;
}
u32 currentSignature(){
    if(signatureDirty){
        ++guard.signatures;
        if(signatureDirty&1)hashAfterVertex=hashVertexGroup();else ++guard.partialSignatures;
        if(signatureDirty&3)hashAfterUnits=hashUnitGroup(hashAfterVertex);
        const u32 h=hashStageGroup(hashAfterUnits);
        signatureCached=h?h:1;signatureDirty=0;
    }
    return signatureCached;
}
// What a draw can stop the PICA with: vertex input and layout, shader code and
// outputs (map, mode and clock), primitive, texture units and the fragment
// operation and fog/gas modes. Left out: TEV stages, blending, logic operation,
// alpha, stencil and depth tests, write masks, culling, early depth and
// w-buffer. No console stop (Alpha 0-6) and no PICA pitfall on 3dbrew (MOVA
// pairs, shader outputs, vertex stride, 4-bit VRAM textures on unit 1) depends
// on those: they are per-fragment arithmetic over buffers neither signature
// covers. Its own seed keeps these values apart from full signatures in the
// shared table.
constexpr unsigned signaturePlainVertex=18; // signaturePlain up to the output clock
static_assert(signaturePlain[signaturePlainVertex-1]==0x6f&&signaturePlain[signaturePlainVertex]==0x100);
u32 coarseSignature(){
    u32 h=0x2545F491u;
    for(unsigned n=0;n<signaturePlainVertex;++n)mix(h,shadow[signaturePlain[n]]);
    mix(h,programHash);mix(h,shadow[0x80]&0x3f07);mix(h,shadow[0x100]&3);mix(h,shadow[0xe0]&7);mix(h,shadow[0x25e]&0xffff);
    h=hashUnitGroup(h);
    return h?h:1;
}
bool verifiedHas(u32 value){
    for(unsigned i=value&(verifiedSlots-1),n=0;n<verifiedSlots;i=(i+1)&(verifiedSlots-1),++n){
        if(verified[i]==value)return true;
        if(!verified[i])return false;
    }
    return false;
}
void verifiedInsert(u32 value){
    for(unsigned i=value&(verifiedSlots-1),n=0;n<verifiedSlots;i=(i+1)&(verifiedSlots-1),++n){
        if(verified[i]==value)return;
        if(!verified[i]){verified[i]=value;++verifiedCount;return;}
    }
}
void flushVerified(unsigned long long frame){
    if(!verifiedPendingCount)return;
    durableAppend(verifiedPath,verifiedPending.data(),verifiedPendingCount*sizeof(u32));
    verifiedPendingCount=0;verifiedFlushFrame=frame;
}
void verifiedQueue(u32 value){
    if(verifiedPendingCount==verifiedPending.size())flushVerified(verifiedFlushFrame);
    verifiedPending[verifiedPendingCount++]=value;
}
// gpu-verified.bin: "DLGV", version 1, then signatures in the order they were
// completed. The table takes the most recent verifiedLoadLimit distinct ones:
// signatures of shaders an earlier build used are never completed again and
// age out instead of filling it. A file with repeated, empty or aged-out
// entries, or a length that is not whole entries (later appends would be
// misread), is written again with the kept ones in their order.
void loadVerified(){
    verified.fill(0);verifiedCount=0;verifiedLoaded=0;lastKnown=0;
    bool valid=false,partial=false,ragged=false;std::size_t count=0;
    // Two words ahead of the entries take the header of a rewrite: the file is
    // written again in one FS call (truncate and write on one handle). Header
    // and entries in two calls left a header-only file if the console was
    // switched off between them: every known draw would run alone again.
    u32* block=nullptr;u32* entries=nullptr;
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/gpu-verified.bin","rb")){
        u32 header[2]{};
        if(std::fread(header,sizeof(u32),2,file)==2&&header[0]==0x56474c44u&&header[1]==1){
            valid=true;long bytes=-1;
            if(!std::fseek(file,0,SEEK_END))bytes=std::ftell(file);
            if(bytes>8){
                ragged=std::size_t(bytes-8)%sizeof(u32)!=0;
                const std::size_t total=std::size_t(bytes-8)/sizeof(u32);
                block=static_cast<u32*>(std::malloc((total+2)*sizeof(u32)));
                // Short of memory: the last entries only, and the file is kept.
                std::size_t skip=0;
                if(block){entries=block+2;count=total;}
                else{entries=scratch.data();count=total<scratch.size()?total:scratch.size();skip=total-count;partial=true;}
                if(std::fseek(file,long(8+skip*sizeof(u32)),SEEK_SET)||std::fread(entries,sizeof(u32),count,file)!=count){count=0;partial=true;}
            }
        }
        std::fclose(file);
    }
    std::size_t kept=0;
    for(std::size_t i=count;i-->0;){
        const u32 value=entries[i];
        if(!value||kept>=verifiedLoadLimit||verifiedHas(value))continue;
        verifiedInsert(value);entries[count-1-kept++]=value;
    }
    verifiedLoaded=unsigned(kept);
    if(!partial&&(!valid||ragged||kept!=count)){
        // kept>0 implies the heap block (the scratch path is partial).
        u32 headerOnly[2];
        u32* out=kept?entries+count-kept-2:headerOnly;
        out[0]=0x56474c44u;out[1]=1;
        durableReplace(verifiedPath,out,(kept+2)*sizeof(u32));
        if(valid&&(count||ragged))OSReport("Native GPU canary: gpu-verified.bin rewritten: %lu entries -> %lu signatures (the most recent kept)%s\n",
            static_cast<unsigned long>(count),static_cast<unsigned long>(kept),ragged?"; partial entry dropped":"");
    }
    if(block)std::free(block);
}
void verifiedAdd(u32 value){
    if(verifiedCount>=verifiedLimit){
        // Full within one session (thousands of new kinds of draw): the most
        // recent ones again, from the file. Never a cleared table.
        flushVerified(verifiedFlushFrame);loadVerified();
        OSReport("Native GPU canary: known-good table full: %u most recent signatures reloaded from gpu-verified.bin\n",verifiedLoaded);
    }
    verifiedInsert(value);
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
    const u32 coarse=coarseKnown?coarseSignature():0;
    // Everything emitted or queued before this draw goes first and must
    // complete. A draw first in its list (nothing emitted before it, as after
    // the drain of the canary before it) waits only while earlier lists are
    // still on the GPU: until Alpha 6 it ran right behind them, and a stop in
    // one of them was blamed on it (in the journal too, had the console frozen).
    // With the span emptied, platformQueryGpu is complete exactly when every
    // queued job has completed (a busy IRQ gate or stopped queue counts as not).
    GPUCMD_SetBuffer(buffer,size,before);
    const bool earlierQueued=!before&&platformQueryGpu()!=GpuCompletion::complete;
    if(before||earlierQueued){
        guard.aloneWaits+=earlierQueued;
        if(platformCompleteGpuFor(GpuCompleteReason::canary,500000000)!=GpuCompletion::complete){
            u32* now{};u32 nowSize{},nowOffset{};GPUCMD_GetBuffer(&now,&nowSize,&nowOffset);
            if(nowSize-nowOffset>=words)GPUCMD_AddRawCommands(scratch.data(),words);
            verifiedAdd(value);return;
        }
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
        journalWrite(text);journalRunning=true;
    }
    // The list the completion below queues starts at the span's base: a job
    // that stops is this draw only if it is that list.
    u32* list{};u32 listSize{},listWords{};GPUCMD_GetBuffer(&list,&listSize,&listWords);
    canary={true,value,format,gxBoundProgramKind(),programHash,gxBoundProgramKind()==2&&gxDrawMultiProjected(),
        u32(reinterpret_cast<std::uintptr_t>(list)),journal};
    ++counters.canaries;
    const u64 start=svcGetSystemTick();
    const auto result=platformCompleteGpuFor(GpuCompleteReason::canary,500000000);
    const unsigned milliseconds=unsigned((svcGetSystemTick()-start)*1000/SYSCLOCK_ARM11);
    if(testHangAt&&counters.canaries==testHangAt){
        const std::uint32_t entry[8]{0x01000101,canary.list,listWords*4,0,0,0,0,1};
        gpuHang(entry,0,1,milliseconds);
    }
    canary.active=false;
    verifiedAdd(value);lastKnown=value;
    // Only signatures this console had not completed before (isolate-all runs
    // known ones alone too; appending those grew the file to 570 KB in Alpha 2).
    if(journal&&result==GpuCompletion::complete){
        verifiedQueue(value);
        if(coarse&&!verifiedHas(coarse)){verifiedAdd(coarse);verifiedQueue(coarse);}
    }
    // Which kinds of draw this console has completed, and how long each took.
    if(counters.canaries<=600)OSReport("Native GPU canary ok %llu: %u ms program=%d projected=%d sig=%08lx fmt=%08lx units=%lx indices=%lu%s\n",
        static_cast<unsigned long long>(counters.canaries),milliseconds,gxBoundProgramKind(),int(canary.projected),static_cast<unsigned long>(value),
        static_cast<unsigned long>(format),static_cast<unsigned long>(shadow[0x80]&7),static_cast<unsigned long>(shadow[0x228]),
        earlierQueued?" (waited for earlier lists first)":"");
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
    dusk::native::gxCountOwnerDraw(count); // measured pictures: draws and indices by owner
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
    if(tracing)[[unlikely]]{
        u32 sum=2166136261u;
        for(const auto& row:uniforms)for(const u32 word:row)sum=(sum^word)*16777619u;
        // The pixels each enabled unit samples, as they are right now.
        u32 pixels[3]{};
        static constexpr std::uint16_t unitRegisters[3][3]={{0x82,0x85,0x8e},{0x92,0x95,0x96},{0x9a,0x9d,0x9e}};
        static constexpr std::uint8_t halfBytes[]={8,6,4,4,4,4,4,2,2,2,1,1,1,2};
        for(unsigned unit=0;unit<3;++unit)if(shadow[0x80]>>unit&1){
            const u32 size=shadow[unitRegisters[unit][0]],type=shadow[unitRegisters[unit][2]]&15;
            const u32 bytes=(size>>16)*(size&0xffff)*(type<sizeof(halfBytes)?halfBytes[type]:8)/2;
            const auto* data=static_cast<const std::uint8_t*>(physicalToVirtual(shadow[unitRegisters[unit][1]]<<3,bytes));
            u32 h=2166136261u;
            if(data)for(u32 i=0;i<bytes;++i)h=(h^data[i])*16777619u;
            pixels[unit]=data?h:0xdeadu;
        }
        OSReport("Native GPU trace %u: sig=%08lx indices=%lu uniforms=%08lx fmt=%08lx units=%lx program=%d pixels=%08lx/%08lx/%08lx words=%lu\n",traceIndex++,
            static_cast<unsigned long>(value),static_cast<unsigned long>(shadow[0x228]),static_cast<unsigned long>(sum),
            static_cast<unsigned long>(format),static_cast<unsigned long>(shadow[0x80]&7),gxBoundProgramKind(),
            static_cast<unsigned long>(pixels[0]),static_cast<unsigned long>(pixels[1]),static_cast<unsigned long>(pixels[2]),
            static_cast<unsigned long>(end-before));
    }
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
    if(!canaries)return;
    if(!isolateAll){
        // Most draws repeat the signature of the draw before them.
        if(value==lastKnown){++guard.sameSignature;return;}
        if(verifiedHas(value)){lastKnown=value;return;}
        if(coarseKnown&&verifiedHas(coarseSignature())){
            // Only fragment state differs from a draw this console completed.
            verifiedAdd(value);lastKnown=value;++guard.coarseKnown;return;
        }
    }
    runCanary(buffer,size,before,end,value,format);
}
namespace dusk::native {
// gpu_sync_ctru.cpp: every command list about to be queued.
void gpuCanaryListSubmitted(const u32* list,u32 bytes){
    if(!enabled||platformGpuOwner()!=OSGetCurrentThread())return;
    // Any list but the journaled draw's own: that draw completed, so the
    // journal stops naming it before this list can reach the PICA. Until
    // Alpha 6 "running" stayed until frame end, over the batched lists, chunks
    // and the wait before the next draw run alone: a freeze in any of them
    // would teach the next start to leave out a draw that had completed. Draws
    // that run alone back to back still share one "idle" (frame end otherwise).
    if(journalRunning&&!(canary.active&&canary.journaled&&u32(reinterpret_cast<std::uintptr_t>(list))==canary.list)){
        journalWrite("idle\n");journalRunning=false;++guard.journalIdleBeforeList;
    }
    if(shadowPassActive)shadowPassLists[shadowPassListCount++%shadowPassLists.size()]=list;
    if(tapePending){tapeLists[tapeListCount++%tapeLists.size()]=list;tapePending=false;}
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
static bool previousFontPaneOom(const char* session,const char* journal){
    // Keep learning for ambiguous records and any journal with pending work.
    // Match the complete legacy panic, including the absence of a worker suffix.
    const auto endOfRecord=[](const char* at){return *at=='\n'&&at[std::strspn(at,"\n")]=='\0';};
    if(std::strncmp(journal,"idle",4)!=0||!endOfRecord(journal+4))return false;
    unsigned pane;unsigned long heap,free;int end=0;
    return std::sscanf(session,"ended fatal core2: Original font pane allocation failed: pane=%u heap=%lu free=%lu%n",
                       &pane,&heap,&free,&end)==3&&end>0&&pane<70&&heap>0&&free<=heap&&
        endOfRecord(session+end);
}
void initializeGpuCanary(){
    enabled=true;markSignatureRegisters();
    if(auto* off=std::fopen("sdmc:/3ds/DuskLight/gpu-canary-off.txt","rb")){std::fclose(off);enabled=false;}
    if(auto* on=std::fopen("sdmc:/3ds/DuskLight/gpu-canary-on.txt","rb")){std::fclose(on);canaries=enabled;}
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
        }else if(enabled&&std::strncmp(sessionText,"ended fatal",11)==0){
            // A CPU-side end (fatal error or exception): no GPU stop, nothing learned.
            const char* reason=sessionText+12;while(*reason==' ')++reason;
            const char* end=std::strchr(reason,'\n');
            std::snprintf(record,sizeof(record),"Note: the last start ended on a fatal error, not a GPU stop (see log-1.txt): %.*s\n",
                int(end?end-reason:std::strlen(reason)),reason);
            durableAppendText(historyPath,record);
        }else if(enabled&&std::strncmp(sessionText,"running",7)==0){
            // Switched off while playing, or frozen outside any draw: the same
            // file. Recorded only; it never slows the next start down (Alpha 2
            // ran every draw alone after this).
            std::snprintf(record,sizeof(record),"Note: the last start ended without exiting (switched off, or frozen outside a draw): %s",sessionText);
            durableAppendText(historyPath,record);
        }
        previousHangPending=std::strncmp(sessionText,"ended (GPU hang",15)==0;
        if(enabled){
            std::snprintf(previousSession,sizeof(previousSession),"%s",sessionText);
            previousEnd=std::sscanf(journalText,"running sig=%lx fmt=%lx",&value,&format)==2||previousHangPending?GpuPreviousEnd::gpuStop:
                previousFontPaneOom(sessionText,journalText)?GpuPreviousEnd::fontOom:
                !std::strncmp(sessionText,"ended fatal",11)?GpuPreviousEnd::fatal:!std::strncmp(sessionText,"running",7)?GpuPreviousEnd::unfinished:
                !std::strncmp(sessionText,"ended",5)?GpuPreviousEnd::clean:GpuPreviousEnd::unknown;
        }
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
                if(program==4)vertexVariantsRefused=true;
                ++priorHangs;
            }else if(std::sscanf(line,"hang %u",&number)==1){
                ++priorHangs;if(std::strstr(line,"transfer-stop"))++transferStops;
                if(std::strstr(line,"shadow-stop"))++shadowStops;
                if(std::strstr(line,"hud-tape-stop"))++tapeStops;
            }
        }
        std::fclose(file);
    }
    // Diagnostic only, never requested by the guard itself: an SD file
    // gpu-isolate.txt written by hand makes one start run every draw alone.
    if(auto* isolate=std::fopen("sdmc:/3ds/DuskLight/gpu-isolate.txt","rb")){
        std::fclose(isolate);isolateAll=enabled;canaries=enabled;std::remove("sdmc:/3ds/DuskLight/gpu-isolate.txt");
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
    if(canaries)loadVerified();
    if(auto* coarse=std::fopen("sdmc:/3ds/DuskLight/gpu-canary-coarse.txt","rb")){std::fclose(coarse);coarseKnown=canaries;}
    if(auto* test=std::fopen("sdmc:/3ds/DuskLight/gpu-canary-test.txt","rb")){if(std::fscanf(test,"%u",&testHangAt)!=1)testHangAt=0;std::fclose(test);}
    if(auto* resume=std::fopen("sdmc:/3ds/DuskLight/gpu-resume.txt","rb")){std::fclose(resume);std::remove("sdmc:/3ds/DuskLight/gpu-resume.txt");autoAdvance=true;}
    OSReport("Native GPU canary: %s; earlier hangs=%u learned signatures=%u formats left out=%u isolate-all=%d auto-advance=%d known-good signatures=%u transfer stops=%u\n",
        !enabled?"off (SD switch)":canaries?"on (first draw of each signature runs alone)":"on, watchdog only (no draw runs alone; SD switch gpu-canary-on.txt)",priorHangs,skipCount,skipFormatCount,int(isolateAll),int(autoAdvance),verifiedLoaded,transferStops);
    if(canaries)OSReport("Native GPU canary: known-good table %u of %u (most recent %u of the file); fragment-only differences %s\n",verifiedCount,verifiedLimit,
        verifiedLoadLimit,coarseKnown?"run as known (SD switch gpu-canary-coarse.txt)":"run alone like any new signature");
    for(unsigned i=0;i<skipFormatCount;++i)OSReport("Native GPU canary: vertex format %08lx with program %d is left out\n",static_cast<unsigned long>(skipFormats[i]),skipFormatPrograms[i]);
    OSReport("Native GPU canary: shader code hashes colour=%08lx rigid=%08lx multi-texture=%08lx\n",static_cast<unsigned long>(gxProgramCodeHash(0)),
        static_cast<unsigned long>(gxProgramCodeHash(1)),static_cast<unsigned long>(gxProgramCodeHash(2)));
    if(rigidSingleRefused)OSReport("Native GPU canary: the single-texture rigid program hung on this console; its draws take the multi-texture or CPU routes\n");
    if(multiProjectedRefused)OSReport("Native GPU canary: the multi-texture program with a projected unit hung on this console; those draws take the CPU route\n");
    if(vertexVariantsRefused)OSReport("Native GPU canary: a variant of the multi-texture program hung on this console; its plans take the general program\n");
    if(shadowStops)OSReport("Native GPU canary: a model-shadow pass stopped on this console: blob shadows only\n");
    if(tapeStops)OSReport("Native GPU canary: a HUD tape list stopped on this console: the 2D pass is drawn in every picture\n");
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
bool gpuCanaryShadowRefused(){return shadowStops!=0;}
bool gpuCanaryHudTapeRefused(){return tapeStops!=0;}
void gpuCanaryHudTapeList(){tapePending=true;}
void gpuCanaryShadowTarget(std::uint32_t physicalAddress){shadowTargetLocation=physicalAddress>>3;}
void gpuCanaryShadowPass(bool active){shadowPassActive=active;}
bool gpuCanaryRigidSingleRefused(){return rigidSingleRefused;}
bool gpuCanaryMultiProjectedRefused(){return multiProjectedRefused;}
bool gpuCanaryVertexVariantsRefused(){return vertexVariantsRefused;}
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
    const bool list=type==1;
    // A list that drew into the shadow image: whatever in it stopped the PICA,
    // the caster pass is what the next start can leave out (blob shadows).
    const bool shadowPass=list&&!transfer&&entry[1]&&(shadowPassList(reinterpret_cast<const void*>(entry[1]))||
        (shadowTargetLocation&&entry[2]&&entry[2]<=1024*1024&&!(entry[2]&3)&&
         listRendersInto(reinterpret_cast<const u32*>(entry[1]),entry[2]/4,shadowTargetLocation)));
    // The draw running alone, only if its own list is the one that went silent.
    const bool attributed=list&&canary.active&&!transfer&&!shadowPass&&entry[1]==canary.list;
    // A list with the words of a recorded or replayed 2D pass.
    const bool tapeStop=list&&!transfer&&!shadowPass&&!attributed&&entry[1]&&tapeList(reinterpret_cast<const void*>(entry[1]));
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
        transfer?"the transfer engine stopped: ":shadowPass?"a list of the model-shadow pass (it drew into the shadow image)":tapeStop?"a list with the HUD tape's 2D pass":attributed?"the draw running alone: ":list?"no single draw to blame (batched list)":"no single draw to blame (not a command list)",text,jobText);
    // 1. What the next start must do differently.
    char line[160];bool progress=false;
    if(transfer){std::snprintf(line,sizeof(line),"hang %u transfer-stop type=%u\n",number,afterLostTransfer?assumedType:type);progress=true;}
    else if(shadowPass){std::snprintf(line,sizeof(line),"hang %u shadow-stop\n",number);progress=true;}
    else if(tapeStop){std::snprintf(line,sizeof(line),"hang %u hud-tape-stop\n",number);progress=true;}
    else if(attributed){std::snprintf(line,sizeof(line),"hang %u sig=%08lx fmt=%08lx program=%d code=%08lx%s\n",number,static_cast<unsigned long>(canary.signature),static_cast<unsigned long>(canary.format),canary.program,static_cast<unsigned long>(canary.shader),canary.projected?" projected=1":"");progress=true;}
    else if(list){
        // Recorded with the list itself (CMDS chunk), which is decoded on a
        // computer. Alpha 2 and Alpha 5 asked the next start to run every draw
        // alone instead: 1.4 to 3 pictures per second for a whole session, and
        // the stopped scene was not revisited. That mode now needs the SD file
        // gpu-isolate.txt, written by hand.
        std::snprintf(line,sizeof(line),"hang %u unattributed-list\n",number);
        progress=true;
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
    // Final: an exception after this point must not relabel the hang as a fatal.
    sessionFinal.store(true,std::memory_order_release);
    {char record[64];std::snprintf(record,sizeof(record),"ended (GPU hang recorded)%s\n",sessionWord);finalSessionWrite(record);}
    OSReport("Native GPU hang %u: evidence written; %s\n",number,restart?"starting the game again":"nothing new to learn; stopping");
    flushNativeLog();
    char message[400];
    std::snprintf(message,sizeof(message),"Hang %u (%s).\n%.250s",number,
        transfer?"GPU transfer stopped; the next start avoids transfers":shadowPass?"model-shadow pass; the next start uses blob shadows":tapeStop?"HUD tape list; the next start draws the 2D pass in every picture":attributed?"one draw, now left out":list?"batched list":"GPU job",transfer||shadowPass?"":text);
    restartAfterGpuHang(message,restart);
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
    // Draws that ran alone with no list after them this frame: they completed.
    if(journalRunning){journalWrite("idle\n");journalRunning=false;}
    if(frame>=30)previousHangPending=false;
    // The guard's own steady-state work, every 300 pictures with draws: words
    // parsed and taken as blocks, signatures mixed (partly), draws that kept
    // the last known signature, canaries, waits added so that a draw first in
    // its list runs alone, draws known by a fragment-only difference, and
    // journal SD writes (of them "idle" written before a batched list).
    if(frame%300==0&&counters.draws!=drawsReported){
        const auto delta=[](std::uint64_t now,std::uint64_t then){return static_cast<unsigned long>(now-then);};
        OSReport("Native GPU guard, last 300 pictures: draws=%lu words=%lu block=%lu signatures=%lu partial=%lu same=%lu canaries=%lu waits-added=%lu coarse=%lu known=%u journal=%lu/%lu\n",
            delta(counters.draws,drawsReported),delta(guard.words,guardReported.words),delta(guard.blockWords,guardReported.blockWords),
            delta(guard.signatures,guardReported.signatures),delta(guard.partialSignatures,guardReported.partialSignatures),
            delta(guard.sameSignature,guardReported.sameSignature),delta(counters.canaries,canariesReported),
            delta(guard.aloneWaits,guardReported.aloneWaits),delta(guard.coarseKnown,guardReported.coarseKnown),verifiedCount,
            delta(guard.journalWrites,guardReported.journalWrites),delta(guard.journalIdleBeforeList,guardReported.journalIdleBeforeList));
        guardReported=guard;drawsReported=counters.draws;canariesReported=counters.canaries;
    }
    if(tracing){tracing=false;OSReport("Native GPU trace: end, %u draws\n",traceIndex);}
    if(traceArmed){traceArmed=false;tracing=true;traceIndex=0;OSReport("Native GPU trace: the picture after frame %llu\n",frame);}
    if(frame%60||sessionFinal.load(std::memory_order_acquire))return;
    // Written by the SD writer thread: an SD write with flush on the game
    // thread every 60 pictures was a periodic stall.
    LightLock_Lock(&sessionLock);
    std::snprintf(sessionPending,sizeof(sessionPending),"running frame=%llu stage=%.8s draws=%llu canaries=%llu left-out=%llu%s\n",frame,stage&&stage[0]?stage:"-",
        static_cast<unsigned long long>(counters.draws),static_cast<unsigned long long>(counters.canaries),static_cast<unsigned long long>(counters.skipped),
        sessionWord);
    LightLock_Unlock(&sessionLock);
    kickNativeSdWriter();
}
void gpuCanaryWriteSessionRecord(){
    if(!enabled)return;
    LightLock_Lock(&sessionLock);
    if(sessionPending[0]&&!sessionFinal.load(std::memory_order_acquire))durableSlotWrite(sessionSlot,sessionPending);
    sessionPending[0]=0;
    LightLock_Unlock(&sessionLock);
}
void gpuCanaryShadowState(const std::uint32_t*& registers,std::size_t& registerWords,const std::uint32_t*& uniformRows,std::size_t& uniformWords){
    registers=shadow.data();registerWords=shadow.size();
    uniformRows=uniforms[0].data();uniformWords=uniforms.size()*4;
}
void gpuCanaryTraceNextPicture(){traceArmed=true;}
void gpuCanarySessionEnd(){if(enabled){flushVerified(verifiedFlushFrame);sessionFinal.store(true,std::memory_order_release);finalSessionWrite("ended\n");}}
GpuPreviousEnd gpuCanaryPreviousEnd(){return previousEnd;}
void gpuCanarySessionWord(const char* word){std::snprintf(sessionWord,sizeof(sessionWord)," %.15s",word?word:"");if(!sessionWord[1])sessionWord[0]=0;}
bool gpuCanaryPreviousSessionWord(const char* word){
    // A whole word of the record, between characters that cannot be part of
    // one: "running ... core2", "ended fatal core2: <reason>" (a colon, which
    // the first version did not take: a worker that ended a start on a fatal
    // error was not learned off), "ended (GPU hang recorded) core2".
    const auto inWord=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';};
    const std::size_t length=word?std::strlen(word):0;
    for(const char* at=previousSession;length&&(at=std::strstr(at,word));at+=length)
        if((at==previousSession||!inWord(at[-1]))&&!inWord(at[length]))return true;
    return false;
}
void gpuCanarySessionFatal(const char* text){
    if(!enabled||!sessionSlot.open||sessionFinal.exchange(true,std::memory_order_acq_rel))return;
    char record[256];
    // The word first: the reason is cut to what the record holds.
    std::snprintf(record,sizeof(record),"ended fatal%s: %.180s%s\n",sessionWord,text?text:"",
        platformGpuFailurePending()?" [GPU work unresolved]":"");
    for(char* at=record+13;*at;++at)if(*at=='\n'&&at[1])*at=' ';
    finalSessionWrite(record);
    // A draw that ran alone this frame and completed is not to blame: without
    // this the next start would learn it as a hang. A draw still in flight,
    // or a failed GPU queue, keeps the journal as evidence.
    if(journalRunning&&!canary.active&&!platformGpuFailurePending()){durableSlotWrite(journalSlot,"idle\n");journalRunning=false;}
}
}

// The map and other offscreen render targets (d_map_path.cpp).
extern "C" bool duskNativeOffscreenRefused(){return dusk::native::gpuCanaryTransferLevel()>=2;}
