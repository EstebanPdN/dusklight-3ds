// Native packets (runtime/native_packets.hpp) against the original path.
//
// Two command processors start from the same bank. The reference runs every
// operation of a stream of synthetic J3D material packets through its own
// stream as the original path does, and notes the bank and bindings each
// shape is issued under. The game processor runs the same operations through
// a stream with the packets captured: each packet goes to the queue as one
// native record, the queue's mirror processor runs it, and the test's draw
// adapter notes the bank and bindings each shape is drawn under. The packets
// look like a room model's: textures, a material list, per shape a
// differential list rewritten every picture (lights, fog, TEV colours with
// masks and both colour banks), a vertex setup list with Aurora array bases,
// the current matrix indices written through GXCmd bytes, the two matrix
// arrays and two indexed matrix loads. Between packets other code writes
// registers, queues ordinary shapes and reads the bank (which seals the
// pending packets). Some packets end their capture early (a vertex
// descriptor call, GPU work) or cannot be folded (a palette load, an indexed
// load inside a list); they must run on the original path, in order.
//
// Checked for every shape, in order: the same list, generation and caster;
// the same bindings; the same bank, every word but the state and vertex
// setup epochs (the seal moves those past the worker's on purpose). Checked
// on the worker's copy across consecutive shapes: neither epoch ever goes
// down, and a changed word that the epoch covers always comes with a changed
// epoch (caches compare epochs for equality). Checked at the end: the game
// bank equals the reference's, epochs aside. The verify mode must report no
// mismatch on the same stream. Deferred and threaded queues (the threaded one
// also under ThreadSanitizer).
#include "../runtime/native_packets.hpp"
#include "../runtime/gx_bank_delta.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include "../runtime/gx_profile.hpp"
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXAurora.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>
using namespace dusk::native;
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
thread_local dusk::native::gx::StaticGeometryHint dusk::native::gx::staticGeometryHint{};
namespace dusk::native {std::uint64_t gxProfileTick(){return 0;}}
namespace dusk::native::gx {
CommandStream* testStream{};
CommandStream* commandStream(){return testStream;}
}

namespace {
std::uint32_t seed=0x9E3779B9u;
std::uint32_t random32(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
unsigned pick(unsigned n){return random32()%n;}
void streamState();
#define CHECK(condition) do{if(!(condition)){std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#condition);streamState();std::fflush(stdout);std::exit(1);}}while(0)

// ---- game memory ----
constexpr unsigned models=4,materialsPerModel=6,shapesPerMaterial=3;
alignas(32) float positions[models][512][3],normals[models][512][3];
alignas(32) std::uint8_t colours[models][512][4];
alignas(32) float drawMatrices[models][2][10][12],normalMatrices[models][2][10][9];
alignas(32) std::uint8_t shapeLists[64]; // never read by the test's draw adapter
bool live(std::uint64_t address,u32 size){
    auto inside=[&](const void* p,std::size_t bytes){
        const auto a=reinterpret_cast<std::uintptr_t>(p);return address>=a&&address+size<=a+bytes;};
    return inside(positions,sizeof(positions))||inside(normals,sizeof(normals))||inside(colours,sizeof(colours))||
           inside(drawMatrices,sizeof(drawMatrices))||inside(normalMatrices,sizeof(normalMatrices));
}

// ---- display lists ----
struct List {std::vector<u8> bytes;};
void put32(std::vector<u8>& v,std::uint32_t w){for(int s=24;s>=0;s-=8)v.push_back(u8(w>>s));}
void put16(std::vector<u8>& v,std::uint16_t w){v.push_back(u8(w>>8));v.push_back(u8(w));}
void bp(std::vector<u8>& v,u32 reg,u32 value){v.push_back(GX_LOAD_BP_REG);put32(v,(reg<<24)|(value&0xffffff));}
void cp(std::vector<u8>& v,u32 reg,u32 value){v.push_back(GX_LOAD_CP_REG);v.push_back(u8(reg));put32(v,value);}
void xf(std::vector<u8>& v,u32 address,std::initializer_list<u32> values){
    v.push_back(GX_LOAD_XF_REG);put16(v,u16(values.size()-1));put16(v,u16(address));for(u32 w:values)put32(v,w);
}
void arrayBase(std::vector<u8>& v,unsigned slot,const void* data,u32 size){
    v.push_back(GX_AURORA);put16(v,u16(GX_AURORA_LOAD_ARRAYBASE+slot));
    const auto address=std::uint64_t(reinterpret_cast<std::uintptr_t>(data));put32(v,u32(address>>32));put32(v,u32(address));
    put32(v,size);v.push_back(1);
}
void pad(std::vector<u8>& v){while(v.size()%32)v.push_back(GX_NOP);}
// A material list: TEV stages and orders, masked writes, both TEV colour
// banks, colour channels and texgens, now and then the matrix index or a
// vertex cache invalidation. `refused`: a palette load or an indexed load
// the fold does not take.
bool wild{}; // fuzz scenario: any register the fold takes, in any order
// bench scenarios: material lists of a real J3D material's size (60-100
// commands) and four lights per differential list; the game path is timed.
bool bench{};
std::uint64_t benchNanos{},benchPackets{},benchShapes{},benchDirectNanos{},benchDirect{};
List materialList(unsigned refused){
    List l;auto& v=l.bytes;
    if(wild)for(unsigned n=8+pick(40);n;--n){
        switch(pick(7)){
        case 0:{u32 reg=pick(256);if(reg==0x52||reg==0x65||reg==0xfe)reg=0x41;bp(v,reg,random32());break;}
        case 1:bp(v,0xfe,random32()&0xffffff);{u32 reg=pick(256);if(reg==0x52||reg==0x65||reg==0xfe)reg=0xe3;bp(v,reg,random32());}break;
        case 2:{const u32 regs[]{0x30,0x40,0x50,0x60,0x70,0x75,0x80,0x87,0x90,0x97,0xb0,0xbc,0xbf};cp(v,regs[pick(13)],random32()&0x3fffffff);break;}
        case 3:{const u32 bases[]{0x0,0x40,0x400,0x430,0x500,0x600,0x1000,0x1008,0x1010,0x1018,0x101a,0x1040,0x1050};
            const u32 at=bases[pick(13)]+pick(6);const unsigned count=1+pick(5);
            v.push_back(GX_LOAD_XF_REG);put16(v,u16(count-1));put16(v,u16(at));for(unsigned i=0;i<count;++i)put32(v,random32());break;}
        case 4:v.push_back(GX_CMD_INVL_VC);break;
        case 5:bp(v,0xe0+pick(8),(pick(2)<<23)|random32());break;
        default:v.push_back(GX_NOP);break;
        }
    }
    else for(unsigned n=bench?60+pick(40):4+pick(12);n;--n){
        switch(pick(8)){
        case 0:bp(v,0xfe,random32()&0xffffff);bp(v,0xc0+pick(32),random32());break;
        case 1:bp(v,0xe0+pick(8),(pick(2)<<23)|(random32()&0x7ff7ff));break;
        case 2:bp(v,0xfe,0x00ff00);bp(v,0xe0+pick(8),(pick(2)<<23)|(random32()&0x7ff7ff));break;
        case 3:xf(v,0x1009+pick(6),{random32()&0xffff});break;
        case 4:xf(v,0x1040+pick(8),{random32()&0xffffff,random32()&0xffffff});break;
        case 5:if(!pick(4))xf(v,0x1018,{random32()&0xffffff,random32()&0xffffff});else bp(v,0x28+pick(8),random32());break;
        case 6:if(!pick(6))v.push_back(GX_CMD_INVL_VC);else bp(v,0xf3+pick(6),random32());break;
        default:bp(v,0x40+pick(4),random32());break;
        }
    }
    if(refused==1){bp(v,0x64,random32()&0xffffff);bp(v,0x65,(pick(16)<<10)|pick(8));}
    if(refused==2){v.push_back(GX_LOAD_INDX_A);put16(v,u16(pick(10)));put16(v,0xb000);}
    pad(v);return l;
}
// A vertex setup list: array bases, descriptors, attribute formats, XF 0x1008.
List setupList(unsigned model,unsigned variant){
    List l;auto& v=l.bytes;
    arrayBase(v,0,positions[model],sizeof(positions[model]));
    arrayBase(v,1,normals[model],sizeof(normals[model]));
    if(variant&1)arrayBase(v,2,colours[model],sizeof(colours[model]));
    cp(v,0x50,0x600|(variant&1?0x2000:0));cp(v,0x60,variant&2?1:0);
    cp(v,0x70+(variant&3),0x40000000u|variant);cp(v,0x80+(variant&3),variant*3);
    cp(v,0xb0,12);cp(v,0xb1,12);
    xf(v,0x1008,{1u|(variant&2?16u:0u)});
    pad(v);return l;
}
// The differential list of a material, rewritten every picture.
void differential(std::vector<u8>& v){
    v.clear();
    for(unsigned light=0;light<(bench?4u:2+pick(3));++light){
        std::initializer_list<u32> words{random32(),random32(),random32(),random32(),random32(),random32(),random32(),
                                         random32(),random32(),random32(),random32(),random32(),random32()};
        xf(v,0x600+light*16+3,words);
    }
    for(unsigned r=0xee;r<=0xf2;++r)bp(v,r,random32());
    // J3D writes the same registers into a differential list every picture
    // (the bench keeps that; the other scenarios also move them).
    const unsigned colour=bench?unsigned(reinterpret_cast<std::uintptr_t>(&v)>>5)%8:pick(8);
    bp(v,0xfe,0x0007ff);bp(v,0xe0+colour,(pick(2)<<23)|random32());
    bp(v,0xe2,(1u<<23)|(random32()&0x7ff7ff));
    xf(v,0x100a,{random32()});
    pad(v);
}

struct Material {
    unsigned model{};List list;unsigned refused{};
    std::array<std::vector<u8>,2> diff; // the double buffer of J3DDisplayListObj
    std::array<unsigned,shapesPerMaterial> setup{};
    std::array<std::array<u16,2>,shapesPerMaterial> current{};
    std::array<u16,shapesPerMaterial> matrix{};
    std::array<u8,shapesPerMaterial> caster{};
    std::array<std::uint32_t,4> textures{};
};
std::vector<Material> materials;
std::vector<List> setups; // per model x 4 variants

// ---- what the shapes are issued and drawn under ----
struct Seen {
    const void* list{};u32 size{},generation{},caster{};
    std::uint32_t bank{},arrays{},content{},setup{};
    std::uint32_t stateEpoch{},setupEpoch{};
    CommandProcessor::ArrayBindings arrayBytes{}; // for the failure report
};
std::uint32_t fnv(std::uint32_t h,std::uint32_t w){return (h^w)*16777619u;}
// The bank without the two epochs the seal moves.
std::uint32_t bankHash(const CommandRegisters& r){
    constexpr std::size_t state=offsetof(CommandRegisters,stateEpoch)/4,setup=offsetof(CommandRegisters,vertexSetupEpoch)/4;
    const auto* w=bankWordsOf(r);std::uint32_t h=0x811C9DC5u;
    for(std::size_t i=0;i<bank_layout::words;++i)if(i!=state&&i!=setup)h=fnv(h,w[i]);
    return h;
}
// Bindings without the padding bytes of each record (handle, offset, size,
// byte order, 7 bytes of padding, live address).
std::uint32_t bindingsHash(const CommandProcessor::ArrayBindings& arrays){
    std::uint32_t h=7;
    for(unsigned slot=0;slot<16;++slot)for(unsigned i=0;i<32;++i)if(i<17||i>=24)h=fnv(h,arrays.bytes[slot*32+i]);
    return h;
}
// What the state epoch covers (CommandProcessor: every BP word and latch,
// CP but the matrix indices, XF but matrix memory and the matrix indices).
std::uint32_t contentHash(const CommandRegisters& r){
    std::uint32_t h=11;
    for(unsigned i=0;i<256;++i)if(i!=0x30&&i!=0x40&&r.cpValid[i])h=fnv(fnv(h,i),r.cp[i]);
    for(unsigned i=0;i<256;++i)if(r.bpValid[i])h=fnv(fnv(h,i|0x100),r.bp[i]);
    for(unsigned b=0;b<2;++b)for(unsigned i=0;i<8;++i)if(r.tevColorValid[b*8+i])h=fnv(h,r.tevColor[b][i]);
    for(unsigned a=0;a<r.xf.size();++a){
        if(a<0x78||(a>=0x400&&a<0x45a)||a==0x1018||a==0x1019||!r.xfValid[a])continue;
        h=fnv(fnv(h,a|0x10000),r.xf[a]);
    }
    return h;
}
std::uint32_t setupHash(const CommandRegisters& r,const CommandProcessor::ArrayBindings& arrays){
    std::uint32_t h=13;
    for(unsigned i=0;i<256;++i)if(i!=0x30&&i!=0x40&&r.cpValid[i])h=fnv(fnv(h,i),r.cp[i]);
    if(r.xfValid[0x1008])h=fnv(h,r.xf[0x1008]);
    return fnv(h,bindingsHash(arrays));
}
Seen seen(const void* list,u32 size,u32 generation,u32 caster,const CommandRegisters& bank,const CommandProcessor::ArrayBindings& arrays){
    return {list,size,generation,caster,bankHash(bank),bindingsHash(arrays),contentHash(bank),setupHash(bank,arrays),
            bank.stateEpoch,bank.vertexSetupEpoch,arrays};
}
std::vector<std::string> reportLines;
std::vector<Seen> issued;
std::vector<Seen> drawn;std::mutex drawnLock;
// Whole banks of the first shapes, for the failure report.
constexpr std::size_t keptBanks=256;
std::vector<CommandRegisters> issuedBanks,drawnBanks;
void describe(const CommandRegisters& a,const CommandRegisters& b){
    const auto* x=bankWordsOf(a);const auto* y=bankWordsOf(b);unsigned shown=0;
    for(std::size_t i=0;i<bank_layout::words&&shown<8;++i)if(x[i]!=y[i]){
        ++shown;const char* space=i<bank_layout::bpAt?"CP":i<bank_layout::xfAt?"BP":i<bank_layout::xfAt+bank_layout::xfWords?"XF":"tail";
        const std::size_t base=i<bank_layout::bpAt?0:i<bank_layout::xfAt?bank_layout::bpAt:i<bank_layout::xfAt+bank_layout::xfWords?bank_layout::xfAt:0;
        std::printf("  word %zu (%s 0x%zx): drawn %08x issued %08x\n",i,space,i-base,x[i],y[i]);
    }
}
std::atomic<std::size_t> byWorker{};
std::thread::id gameThread;

// ---- the two paths ----
CommandMemory memory;
CommandProcessor* reference{};CommandProcessor* game{};CommandProcessor* mirror{};
CommandStream* referenceStream{};CommandStream* gameStream{};
std::array<u8,64*1024> scratchA{},scratchB{};
std::array<std::uint32_t,64*1024> logWords{};
gxworker::Queue* queue{};
unsigned casterSlot{};
gx::CommandStream& stream(bool isGame){return isGame?*gameStream:*referenceStream;}

// The original path of a shape (duskNativeDrawShape): queued with the bank
// words changed since the last one.
bool queueShape(std::uint32_t generation,const void* list,std::uint32_t size){
    auto& s=*gameStream;
    gxworker::QueuedShape shape;shape.list=list;shape.listBytes=size;shape.generation=generation;shape.casterSlot=casterSlot;
    if(!s.idle()||!queue->push(s.registers(),s.bankStores(),s.arrayBindings(),currentTargetExtent,shape))return false;
    s.restartBankLog();
    return true;
}
void drawAdapter(const gxworker::QueuedShape& shape,const CommandRegisters& bank,const CommandProcessor::ArrayBindings& arrays,const TargetExtent&){
    const auto s=seen(shape.list,shape.listBytes,shape.generation,shape.casterSlot,bank,arrays);
    {std::lock_guard lock(drawnLock);drawn.push_back(s);if(drawnBanks.size()<keptBanks)drawnBanks.push_back(bank);}
    if(std::this_thread::get_id()!=gameThread)++byWorker;
    queue->sync(); // the draw code syncs: a no-op on the consumer
}
void nativeApply(const std::uint32_t* body,std::size_t words,CommandProcessor& processor,const TargetExtent& at){
    packets::replay(body,words,processor,at,drawAdapter,nullptr);
}

// One operation of a packet on one path.
void writeCurrentMatrix(CommandStream& s,u16 a,u16 b){
    // J3DCurrentMtx::load: GXCmd bytes, one value at a time.
    auto u8w=[&](u8 v){CHECK(s.writeValue(&v,1));};
    auto u32w=[&](u32 v){const u8 b4[4]{u8(v>>24),u8(v>>16),u8(v>>8),u8(v)};CHECK(s.writeValue(b4,4));};
    u8w(GX_LOAD_CP_REG);u8w(0x30);u32w(a);
    u8w(GX_LOAD_CP_REG);u8w(0x40);u32w(b);
    u8w(GX_LOAD_XF_REG);{const u8 h[4]{0,1,0x10,0x18};CHECK(s.writeValue(h,4));}u32w(a);u32w(b);
}
// A material packet. `isGame`: the captured path.
unsigned frameNumber{};
// Model 1 is loaded as the game loads every model (flag 0x10, ConcatView):
// its shapes' matrix loads are J3DShapeMtxConcatView::load, the CPU product
// written with GXNativeLoadXf after GXSETARRAY of the animation matrices;
// the direct record runs that code too, into the capture.
bool concatView(const Material& m){return m.model==1;}
// One shape of a material packet (J3DShapePacket::drawFast). `direct`: as
// the direct record enters it (J3DShape::nativeDirectDrawFast) until a GX
// call of the original code ends the capture; from there on (direct turns
// false) the shape and the packet go on on the original path.
void shapeOps(bool isGame,Material& m,unsigned shape,unsigned visibleMask,unsigned endEarly,bool& direct){
    auto& s=stream(isGame);
    auto list=[&](const u8* data,u32 size){
        if(direct)CHECK(packets::directList(data,size));
        else CHECK(s.call({data,size}));
    };
    if(shape==0)list(m.diff[frameNumber&1].data(),u32(m.diff[frameNumber&1].size()));
    else if(pick(2))list(nullptr,0);
    if(!(visibleMask>>shape&1))return;
    if(shape){const auto& setup=setups[m.model*4+m.setup[shape]].bytes;list(setup.data(),u32(setup.size()));}
    if(shape&&pick(3)==0){
        if(direct)CHECK(packets::directCurrentMatrix(m.current[shape][0],m.current[shape][1]));
        else writeCurrentMatrix(s,m.current[shape][0],m.current[shape][1]);
    }
    if(endEarly==1&&shape==1){CHECK(s.setDescriptor(GX_VA_CLR1,0));} // not captured: the capture ends here
    if(endEarly==2&&shape==1&&isGame){queue->sync();}                 // GPU work: the capture ends here
    const unsigned view=frameNumber&1;
    if(direct){
        CHECK(packets::directBind(12,drawMatrices[m.model][view],sizeof(drawMatrices[m.model][view]),48));
        CHECK(packets::directBind(13,normalMatrices[m.model][view],sizeof(normalMatrices[m.model][view]),36));
    }else{
        CHECK(s.bindArray(12,drawMatrices[m.model][view],sizeof(drawMatrices[m.model][view]),48,true));
        CHECK(s.bindArray(13,normalMatrices[m.model][view],sizeof(normalMatrices[m.model][view]),36,true));
    }
    if(concatView(m)){
        // The shape matrix load, the original code on both paths.
        CHECK(s.bindArray(12,drawMatrices[m.model][view^1],sizeof(drawMatrices[m.model][view^1]),48,true));
        float product[12],normal[9];
        const auto& a=drawMatrices[m.model][view^1][m.matrix[shape]];const auto& b=drawMatrices[m.model][view][m.matrix[shape]];
        for(unsigned i=0;i<12;++i)product[i]=a[i]*0.5f+b[(i+3)%12];
        for(unsigned i=0;i<9;++i)normal[i]=normalMatrices[m.model][view][m.matrix[shape]][i]*2.f;
        u32 words[12];std::memcpy(words,product,sizeof(words));
        CHECK(s.writeXf(0,{words,12}));
        // Now and then a call the capture cannot keep inside the load.
        if(pick(10)==0)CHECK(s.setDescriptor(GX_VA_CLR1,0));
        std::memcpy(words,normal,sizeof(normal));
        CHECK(s.writeXf(0x400,{words,9}));
        if(direct&&!packets::directOpen())direct=false;
    }else if(direct){
        CHECK(packets::directIndexed(GX_LOAD_INDX_A,m.matrix[shape],0xb000));
        CHECK(packets::directIndexed(GX_LOAD_INDX_B,m.matrix[shape],0x8000|0x400));
    }else{
        CHECK(s.loadIndexed(GX_LOAD_INDX_A,m.matrix[shape],0xb000));
        CHECK(s.loadIndexed(GX_LOAD_INDX_B,m.matrix[shape],0x8000|0x400));
    }
    const bool read=pick(4)==0;
    if(isGame&&read&&!direct)(void)s.registers(); // a stale read while captured is allowed
    const std::uint32_t generation=1+unsigned(&m-materials.data())*8+shape;
    const void* shapeList=shapeLists+shape;const u32 size=16+shape;
    casterSlot=m.caster[shape];
    if(direct)CHECK(packets::directShape(generation,shapeList,size));
    else if(isGame){
        if(!(packets::capturing()&&packets::captureShape(generation,shapeList,size,casterSlot)))CHECK(queueShape(generation,shapeList,size));
    }else{issued.push_back(seen(shapeList,size,generation,casterSlot,reference->registers(),reference->currentArrayBindings()));
        if(issuedBanks.size()<keptBanks)issuedBanks.push_back(reference->registers());}
    casterSlot=0;
}
void packet(bool isGame,Material& m,unsigned visibleMask,unsigned endEarly){
    auto& s=stream(isGame);
    // Room models are the models of even index; model 3 has CPU-rewritten
    // arrays (not rigid); the others are rigid actors.
    const int kind=m.model%2==0?packets::roomModel:m.model==3?packets::notRigid:packets::rigid;
    const bool captured=isGame&&packets::begin(kind);
    // Textures (commandTextureBinding).
    for(unsigned unit=0;unit<2;++unit){
        const unsigned o=unit;
        CHECK(s.writeBp(0xfe,0xffffff));CHECK(s.writeBp(0x80+o,m.textures[0]+unit));CHECK(s.writeBp(0x84+o,m.textures[1]));
        CHECK(s.writeBp(0x88+o,m.textures[2]));CHECK(s.writeBp(0x94+o,0x800000|(m.textures[3]+unit)));
    }
    CHECK(s.call(m.list.bytes));
    if(wild)for(unsigned n=pick(6);n;--n){
        switch(pick(4)){
        case 0:{u32 reg=pick(256);if(reg==0x52||reg==0x65)reg=0x42;CHECK(s.writeBpMasked(u8(reg),random32(),random32()));break;}
        case 1:{std::uint32_t values[6];for(auto& w:values)w=random32();const u16 at=u16(0x1000+pick(0x58));
            CHECK(s.writeXf(at,{values,std::min<std::size_t>(1+pick(6),0x1058-at)}));break;}
        case 2:{const u8 regs[]{0x30,0x40,0x50,0x60,0x72,0x81,0x93,0xb4};CHECK(s.writeCp(regs[pick(8)],random32()&0x3fffffff));break;}
        default:CHECK(s.writeBp(u8(0xe0+pick(8)),(pick(2)<<23)|random32()));break;
        }
    }
    // First shape: vertex setup and current matrix (loadPreDrawSetting).
    CHECK(s.call(setups[m.model*4+m.setup[0]].bytes));
    writeCurrentMatrix(s,m.current[0][0],m.current[0][1]);
    for(unsigned shape=0;shape<shapesPerMaterial;++shape){bool direct=false;shapeOps(isGame,m,shape,visibleMask,endEarly,direct);}
    if(wild&&pick(2)){
        // The packet ends with the two matrix index families apart: which one
        // was written last decides the processor's matrix index.
        const u32 a=random32()&0xffffff,b=random32()&0xffffff;
        if(pick(2)){CHECK(s.writeCp(0x30,a));CHECK(s.writeXf(0x1018,{&b,1}));}
        else{CHECK(s.writeXf(0x1018,{&b,1}));CHECK(s.writeCp(0x30,a));}
        if(pick(2)){CHECK(s.writeCp(0x40,b));}else{CHECK(s.writeXf(0x1019,{&a,1}));}
    }
    if(captured)packets::end();
}
// The same packet as packet(true, ...) without wild writes or an early end,
// as J3D builds a direct record of it (J3DMatPacket::drawNativeDirect): the
// texture loads still go through the stream (captured), everything else is
// entered directly. The same random choices in the same order.
bool directMode{};
bool directPacket(Material& m,unsigned visibleMask){
    auto& s=*gameStream;
    const int kind=m.model%2==0?packets::roomModel:m.model==3?packets::notRigid:packets::rigid;
    if(kind==packets::notRigid||!packets::directBegin(kind))return false;
    for(unsigned unit=0;unit<2;++unit){
        const unsigned o=unit;
        CHECK(s.writeBp(0xfe,0xffffff));CHECK(s.writeBp(0x80+o,m.textures[0]+unit));CHECK(s.writeBp(0x84+o,m.textures[1]));
        CHECK(s.writeBp(0x88+o,m.textures[2]));CHECK(s.writeBp(0x94+o,0x800000|(m.textures[3]+unit)));
    }
    CHECK(packets::directOpen());
    CHECK(packets::directList(m.list.bytes.data(),u32(m.list.bytes.size())));
    const auto& setup0=setups[m.model*4+m.setup[0]].bytes;
    CHECK(packets::directList(setup0.data(),u32(setup0.size())));
    CHECK(packets::directCurrentMatrix(m.current[0][0],m.current[0][1]));
    for(unsigned shape=0;shape<shapesPerMaterial;++shape){
        bool direct=true;
        shapeOps(true,m,shape,visibleMask,0,direct);
        if(!direct){
            // J3DMatPacket::drawNativeDirect, nativeEnded: the rest of the
            // packet on the original path, the record given up.
            for(++shape;shape<shapesPerMaterial;++shape)shapeOps(true,m,shape,visibleMask,0,direct);
            packets::directFallback("direct: shape ended the record");
            packets::end();
            return true;
        }
    }
    packets::directEnd(0x5eed);
    return !packets::directVerifying();
}
// Code between packets: register writes, a list, an ordinary shape, a read.
void between(bool isGame,unsigned what,std::uint32_t a,std::uint32_t b){
    auto& s=stream(isGame);
    switch(what){
    case 0:CHECK(s.writeBp(u8(0x20+a%32),b));break;
    case 1:{std::vector<u8> v;bp(v,0xfe,a&0xffffff);bp(v,0x41,b);xf(v,0x1010+a%8,{b});pad(v);CHECK(s.call(v));break;}
    case 2:{
        CHECK(s.writeXf(u16(a%10*12),{reinterpret_cast<const u32*>(drawMatrices[0][0][a%10]),12}));
        const std::uint32_t generation=0x8000+a%100;const void* list=shapeLists+40;const u32 size=24;
        if(isGame)CHECK(queueShape(generation,list,size));
        else{issued.push_back(seen(list,size,generation,0,reference->registers(),reference->currentArrayBindings()));
            if(issuedBanks.size()<keptBanks)issuedBanks.push_back(reference->registers());}
        break;}
    case 3:if(isGame)(void)s.registers();break; // seals what is pending
    default:CHECK(s.writeCp(u8(0x70+a%8),b));break;
    }
}

void streamState(){
    for(auto* st:{referenceStream,gameStream})if(st)std::printf("  %s stream: error %u command %u at %zu\n",st==gameStream?"game":"reference",
        unsigned(st->error()),unsigned(st->commandError()),st->commandErrorOffset());
}
void setUp(){
    for(auto& model:positions)for(auto& v:model)for(auto& f:v)f=float(pick(1000))/9.f;
    for(auto& model:normals)for(auto& v:model)for(auto& f:v)f=float(pick(100))/99.f;
    for(auto& model:colours)for(auto& v:model)for(auto& c:v)c=u8(random32());
    setups.clear();
    for(unsigned model=0;model<models;++model)for(unsigned variant=0;variant<4;++variant)setups.push_back(setupList(model,variant));
    materials.clear();
    for(unsigned model=0;model<models;++model)for(unsigned k=0;k<materialsPerModel;++k){
        Material m;m.model=model;
        m.refused=pick(14)==0?1+pick(2):0;
        m.list=materialList(m.refused);
        for(auto& s:m.setup)s=pick(4);
        for(auto& c:m.current)c={u16(pick(1<<12)),u16(pick(1<<12))};
        for(auto& i:m.matrix)i=u16(pick(10));
        for(auto& c:m.caster)c=u8(pick(5)==0?1+pick(3):0);
        for(auto& t:m.textures)t=random32()&0xfffff;
        materials.push_back(std::move(m));
    }
    // At least one material of each refused kind.
    materials[3].refused=1;materials[3].list=materialList(1);
    materials[9].refused=2;materials[9].list=materialList(2);
}
void newPicture(){
    ++frameNumber;
    // Now and then a material list is patched in place (same address and
    // size, new bytes): the fold's list cache must see it.
    if(pick(3)==0){
        auto& v=materials[pick(unsigned(materials.size()))].list.bytes;
        for(std::size_t k=0;k<v.size();){
            const u8 code=v[k];
            if(code==GX_LOAD_BP_REG){if(v[k+1]!=0x52&&v[k+1]!=0x65&&v[k+1]!=0xfe&&v[k+1]!=0x64){v[k+4]=u8(random32());break;}k+=5;}
            else if(code==GX_LOAD_CP_REG)k+=6;
            else if(code==GX_LOAD_XF_REG)k+=5+4*(std::size_t((v[k+1]<<8)|v[k+2])+1);
            else if(code==GX_LOAD_INDX_A)k+=5;
            else k+=1; // NOP, invalidation
        }
    }
    for(auto& model:drawMatrices)for(auto& m:model[frameNumber&1])for(auto& f:m)f=float(int(random32()%20000)-10000)/31.f;
    for(auto& model:normalMatrices)for(auto& m:model[frameNumber&1])for(auto& f:m)f=float(int(random32()%2000)-1000)/97.f;
    for(auto& m:materials)differential(m.diff[frameNumber&1]);
}

std::mutex gate;std::condition_variable workerWake,gameWake;bool workerSignal{},gameSignal{};
gxworker::Platform threadedPlatform(){
    gxworker::Platform p;
    p.wakeWorker=+[]{{std::lock_guard lock(gate);workerSignal=true;}workerWake.notify_one();};
    p.waitForWorker=+[](unsigned microseconds){std::unique_lock lock(gate);
        gameWake.wait_for(lock,std::chrono::microseconds(microseconds),[]{return gameSignal;});gameSignal=false;};
    p.workerProgress=+[]{{std::lock_guard lock(gate);gameSignal=true;}gameWake.notify_one();};
    p.workerIdle=+[](unsigned microseconds){std::unique_lock lock(gate);
        workerWake.wait_for(lock,std::chrono::microseconds(microseconds),[]{return workerSignal;});workerSignal=false;};
    p.gpuToWorker=+[]{};p.gpuToGame=+[]{};
    p.now=+[]{return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    p.workerStuck=+[]{std::printf("FAIL the worker did not stop between two shapes within 20 s\n");std::fflush(stdout);std::_Exit(1);};
    return p;
}

void run(const char* name,gxworker::Mode mode,bool queueVerify,packets::Mode packetMode,bool verify,unsigned pictures){
    issued.clear();drawn.clear();issuedBanks.clear();drawnBanks.clear();byWorker=0;frameNumber=0;gameThread=std::this_thread::get_id();
    setUp();
    reference=new CommandProcessor(memory);game=new CommandProcessor(memory);mirror=new CommandProcessor(memory);
    for(auto* p:{reference,game,mirror})p->setLiveArrayResolver(live);
    referenceStream=new CommandStream(*reference,scratchA);gameStream=new CommandStream(*game,scratchB);
    testStream=gameStream;
    gameStream->attachBankLog(logWords);
    // The matrix arrays a list's indexed load may read before any packet binds them.
    for(auto* st:{referenceStream,gameStream}){
        CHECK(st->bindArray(12,drawMatrices[0][0],sizeof(drawMatrices[0][0]),48,true));
        CHECK(st->bindArray(13,normalMatrices[0][0],sizeof(normalMatrices[0][0]),36,true));
    }
    const std::size_t bytes=gxworker::Queue::minimumStorage+64*1024;
    void* storage=std::aligned_alloc(8,bytes);
    // native_packets.cpp uses the process's one queue: one run per process
    // (the runner starts the test once per scenario).
    auto& q=gxworker::queue();queue=&q;
    gxworker::Adapter adapter{drawAdapter,mirror,nativeApply};
    CHECK(q.start(mode,storage,bytes,adapter,mode==gxworker::Mode::threaded?threadedPlatform():gxworker::Platform{},queueVerify));
    packets::Hooks hooks;
    hooks.canQueue=+[]{return gxworker::queue().accepting()&&gxworker::queue().nativeAvailable();};
    hooks.drawShape=queueShape;
    hooks.casterSlot=+[]{return casterSlot;};hooks.setCasterSlot=+[](unsigned slot){casterSlot=slot;};
    hooks.staticGeometry=+[](std::uint32_t,const void*){};
    hooks.report=+[](const char* line){
        const std::size_t length=std::strlen(line);
        if(length>=255){std::printf("FAIL report line of %zu characters (the log keeps 254): %s",length,line);std::exit(1);}
        if(std::strstr(line,"Native packets ")==line)reportLines.emplace_back(line);
    };
    hooks.ticksPerMillisecond=1000;
    if(bench){
        hooks.now=+[]{return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
        dusk::native::gxSparseVertexProfiling=true;
    }
    CHECK(packets::install(hooks));
    packets::configure(packetMode,verify,directMode);
    std::thread worker;
    if(mode==gxworker::Mode::threaded)worker=std::thread([&]{q.run();});
    const auto before=packets::statistics();
    for(unsigned picture=0;picture<pictures;++picture){
        newPicture();
        // The same order of packets and code between them on both paths.
        struct Step {int material;unsigned visible,endEarly,what;std::uint32_t a,b;};
        std::vector<Step> steps;
        for(unsigned model=0;model<models;++model)for(unsigned k=0;k<materialsPerModel;++k){
            if(pick(8)==0)steps.push_back({-1,0,0,pick(5),random32(),random32()});
            const unsigned visible=pick(10)==0?pick(8):7;
            const unsigned endEarly=pick(25)==0?1+pick(2):0;
            steps.push_back({int(model*materialsPerModel+k),visible,endEarly,0,0,0});
        }
        if(!pick(4))currentTargetExtent.logicalWidth=currentTargetExtent.pixelWidth=256+pick(64);
        for(const auto& step:steps){
            if(step.material<0){
                between(false,step.what,step.a,step.b);
                const auto began=std::chrono::steady_clock::now();
                between(true,step.what,step.a,step.b);
                benchNanos+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());
                continue;}
            const auto saved=seed;
            const unsigned endEarly=directMode||bench?0:step.endEarly;
            packet(false,materials[step.material],step.visible,endEarly);
            seed=saved; // the same choices inside the packet on both paths
            const auto began=std::chrono::steady_clock::now();
            ++benchPackets;benchShapes+=unsigned(std::popcount(step.visible&7u));
            auto lap=[&]{benchNanos+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());};
            if(directMode){
                if(directPacket(materials[step.material],step.visible)){
                    const auto before=benchNanos;lap();benchDirectNanos+=benchNanos-before;++benchDirect;continue;}
                seed=saved; // verify: the original code follows, captured, and is compared
                if(packets::directVerifying())packets::directRealState(0x5eed);
            }
            packet(true,materials[step.material],step.visible,endEarly);
            lap();
        }
        // The picture ends: the game reads its bank (seals), the queue is drawn.
        {const auto began=std::chrono::steady_clock::now();
        (void)gameStream->registers();
        benchNanos+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());}
        q.sync();
        if(picture%30==29)packets::report(30); // as the worker's report does, with its picture count
        if(pick(3)==0){
            // Now and then a sync in the middle of the next picture's start.
            const std::uint32_t a=random32(),b=random32();between(false,1,a,b);between(true,1,a,b);
        }
    }
    (void)gameStream->registers();
    // The last bank goes to the worker with one more ordinary shape.
    between(false,2,7,0);between(true,2,7,0);
    q.sync();
    if(mode==gxworker::Mode::threaded){q.stop();worker.join();}
    // Every shape in order under the original bank and bindings.
    CHECK(drawn.size()==issued.size());
    std::size_t natives=0;
    for(std::size_t i=0;i<issued.size();++i){
        const auto& want=issued[i];const auto& got=drawn[i];
        if(got.list!=want.list||got.size!=want.size||got.generation!=want.generation||got.caster!=want.caster){
            std::printf("FAIL %s: shape %zu differs in identity (generation %u against %u)\n",name,i,got.generation,want.generation);std::exit(1);}
        if(got.arrays!=want.arrays){
            std::printf("FAIL %s: shape %zu drawn with other bindings\n",name,i);
            for(unsigned slot=0;slot<16;++slot){
                std::uint64_t a[4],b[4];std::memcpy(a,got.arrayBytes.bytes.data()+slot*32,32);std::memcpy(b,want.arrayBytes.bytes.data()+slot*32,32);
                if(std::memcmp(a,b,32))std::printf("  slot %u drawn %016llx %016llx %016llx %016llx issued %016llx %016llx %016llx %016llx\n",slot,
                    (unsigned long long)a[0],(unsigned long long)a[1],(unsigned long long)a[2],(unsigned long long)a[3],
                    (unsigned long long)b[0],(unsigned long long)b[1],(unsigned long long)b[2],(unsigned long long)b[3]);
            }
            std::exit(1);}
        if(got.bank!=want.bank){std::printf("FAIL %s: shape %zu (generation %u) drawn under another bank\n",name,i,want.generation);
            if(i<drawnBanks.size()&&i<issuedBanks.size())describe(drawnBanks[i],issuedBanks[i]);
            std::exit(1);}
        if(i){
            const auto& prior=drawn[i-1];
            CHECK(got.stateEpoch>=prior.stateEpoch&&got.setupEpoch>=prior.setupEpoch);
            if(got.content!=prior.content)CHECK(got.stateEpoch!=prior.stateEpoch);
            if(got.setup!=prior.setup)CHECK(got.setupEpoch!=prior.setupEpoch);
        }
    }
    // The game bank is the reference's (epochs aside), bindings too.
    CHECK(bankHash(game->registers())==bankHash(reference->registers()));
    CHECK(bindingsHash(game->currentArrayBindings())==bindingsHash(reference->currentArrayBindings()));
    CHECK(game->registers().stateEpoch>=reference->registers().stateEpoch);
    const auto& st=packets::statistics();
    const auto& c=q.counters();
    natives=c.natives;
    CHECK(c.damaged==0&&c.unmarkedBlocks==0&&c.sumMismatches==0);
    CHECK(st.workerFailures==0);
    // Room packets were captured; actor packets were not eligible in room mode.
    if(packetMode!=packets::Mode::off){
        CHECK(st.captured>before.captured);
        CHECK(st.notEligible>before.notEligible);
        CHECK(!reportLines.empty());
        bool verifyLine=false;for(const auto& l:reportLines)verifyLine|=l.find("Native packets verify")==0;
        CHECK(verifyLine==verify);
    }
    if(directMode){
        std::printf("  direct: tried %llu built %llu compared %llu record mismatches %llu state mismatches %llu incomplete %llu %s\n",
            static_cast<unsigned long long>(st.directTried),static_cast<unsigned long long>(st.directRecords),
            static_cast<unsigned long long>(st.directCompared),static_cast<unsigned long long>(st.directMismatches),
            static_cast<unsigned long long>(st.directStateMismatches),static_cast<unsigned long long>(st.directIncomplete),st.firstDirect);
        CHECK(st.directRecords>0&&st.directMismatches==0&&st.directStateMismatches==0&&st.directIncomplete==0);
        if(verify)CHECK(st.directCompared==st.directRecords);
    }
    if(std::getenv("PACKETS_DEBUG")){
        std::printf("  stats: captured %llu shipped %llu ended %llu refused %llu queue %llu stream %llu storage %llu not eligible %llu verified %llu first end %s first refusal %s\n",
            (unsigned long long)st.captured,(unsigned long long)st.packets,(unsigned long long)st.ended,(unsigned long long)st.foldRefused,
            (unsigned long long)st.queueRefused,(unsigned long long)st.streamRefused,(unsigned long long)st.storageRefused,(unsigned long long)st.notEligible,
            (unsigned long long)st.verifiedPackets,st.firstEnd?st.firstEnd:"-",st.firstFold?st.firstFold:"-");
        for(const auto& l:reportLines)std::printf("  log: %s",l.c_str());
    }
    if(verify){
        CHECK(st.verifiedPackets>before.verifiedPackets&&st.verifiedShapes>before.verifiedShapes);
        CHECK(st.mirrorMismatches==0&&st.sealMismatches==0);
        CHECK(natives==0);
    }else if(packetMode!=packets::Mode::off){
        CHECK(st.packets>before.packets&&st.runs>before.runs);
        CHECK((directMode||bench||st.ended>before.ended)&&st.foldRefused>before.foldRefused);
    }
    if(mode==gxworker::Mode::threaded&&packetMode!=packets::Mode::off&&!verify)CHECK(byWorker.load()>0);
    std::printf("PASS %s: %zu shapes in order under the original bank and bindings (%zu by the worker); %llu packets shipped in %llu "
        "records of %llu words, %llu runs sealed (%llu words written); original path: %llu ended early (first: %s), %llu not folded "
        "(first: %s); %llu stale reads; verify: %llu packets, %llu shapes, %llu + %llu mismatches; game bank equal to the reference\n",
        name,issued.size(),byWorker.load(),
        static_cast<unsigned long long>(st.packets-before.packets),static_cast<unsigned long long>(natives),
        static_cast<unsigned long long>(st.recordWords-before.recordWords),static_cast<unsigned long long>(st.runs-before.runs),
        static_cast<unsigned long long>(st.sealedWords-before.sealedWords),static_cast<unsigned long long>(st.ended-before.ended),
        st.firstEnd?st.firstEnd:"none",static_cast<unsigned long long>(st.foldRefused-before.foldRefused),st.firstFold?st.firstFold:"none",
        static_cast<unsigned long long>(st.staleReads-before.staleReads),static_cast<unsigned long long>(st.verifiedPackets-before.verifiedPackets),
        static_cast<unsigned long long>(st.verifiedShapes-before.verifiedShapes),static_cast<unsigned long long>(st.mirrorMismatches),
        static_cast<unsigned long long>(st.sealMismatches));
    std::printf("  list cache: %llu lists from the cache, %llu parsed and kept, %llu parsed as rewritten\n",
        static_cast<unsigned long long>(st.listsCached),static_cast<unsigned long long>(st.listsParsed),static_cast<unsigned long long>(st.listsChanging));
    if(packetMode!=packets::Mode::off)CHECK(st.listsCached>0&&st.listsChanging>0);
    for(std::size_t i=reportLines.size()>4?reportLines.size()-4:0;i<reportLines.size();++i)std::printf("  log: %s",reportLines[i].c_str());
    if(bench)std::printf("BENCH %s: game path %.0f ns per packet (%.2f shapes per packet), %llu packets; fold %.0f ns per shipped packet, seal %.0f ns per run\n",name,
        double(benchNanos)/double(benchPackets),double(benchShapes)/double(benchPackets),static_cast<unsigned long long>(benchPackets),
        st.packets?double(st.foldTicks)/double(st.packets):0.,st.runs?double(st.sealTicks)/double(st.runs):0.);
    if(bench&&benchDirect)std::printf("BENCH %s: %llu direct records, %.0f ns each; the rest %.0f ns each\n",name,static_cast<unsigned long long>(benchDirect),
        double(benchDirectNanos)/double(benchDirect),double(benchNanos-benchDirectNanos)/double(benchPackets-benchDirect));
    q.accept(false);
    delete gameStream;delete referenceStream;testStream=nullptr;
    std::free(storage);
}
}
int main(int argc,char** argv){
    // One scenario per process (native_packets.cpp uses the one queue).
    const char* which=argc>1?argv[1]:"";
    auto is=[&](const char* name){return !std::strcmp(which,name);};
    if(is("shipped"))run("deferred queue, packets shipped",gxworker::Mode::deferred,false,packets::Mode::all,false,120);
    else if(is("queue-verify"))run("deferred queue with its verify mode",gxworker::Mode::deferred,true,packets::Mode::all,false,60);
    else if(is("verify"))run("deferred queue, native packet verify mode",gxworker::Mode::deferred,false,packets::Mode::all,true,60);
    else if(is("rooms"))run("deferred queue, room model packets only",gxworker::Mode::deferred,false,packets::Mode::bg,false,60);
    else if(is("rooms-verify"))run("deferred queue, room model packets, verify mode",gxworker::Mode::deferred,false,packets::Mode::bg,true,60);
    else if(is("direct")){directMode=true;run("deferred queue, direct records",gxworker::Mode::deferred,false,packets::Mode::all,false,100);}
    else if(is("direct-verify")){directMode=true;run("direct records, verify mode",gxworker::Mode::deferred,false,packets::Mode::all,true,60);}
    else if(is("threads-direct")){directMode=true;run("queue with a consumer thread, direct records",gxworker::Mode::threaded,false,packets::Mode::all,false,100);}
    else if(is("off"))run("deferred queue, packets off",gxworker::Mode::deferred,false,packets::Mode::off,false,20);
    else if(is("bench-off")){bench=true;run("bench, original path",gxworker::Mode::deferred,false,packets::Mode::off,false,300);}
    else if(is("bench-capture")){bench=true;run("bench, captured packets",gxworker::Mode::deferred,false,packets::Mode::all,false,300);}
    else if(is("bench-direct")){bench=true;directMode=true;run("bench, direct records",gxworker::Mode::deferred,false,packets::Mode::all,false,300);}
    else if(is("fuzz")){wild=true;run("deferred queue, random register traffic in every packet",gxworker::Mode::deferred,false,packets::Mode::all,false,150);}
    else if(is("fuzz-verify")){wild=true;run("random register traffic, native packet verify mode",gxworker::Mode::deferred,false,packets::Mode::all,true,80);}
    else if(is("threads"))run("queue with a consumer thread, packets shipped",gxworker::Mode::threaded,false,packets::Mode::all,false,120);
    else if(is("threads-verify"))run("queue with a consumer thread and its verify mode",gxworker::Mode::threaded,true,packets::Mode::all,false,60);
    else if(is("threads-fuzz")){wild=true;run("queue with a consumer thread, random register traffic",gxworker::Mode::threaded,false,packets::Mode::all,false,100);}
    else {std::printf("usage: native_packets_test shipped|direct|direct-verify|threads-direct|queue-verify|verify|rooms|rooms-verify|off|fuzz|fuzz-verify|threads|threads-verify|threads-fuzz\n");return 2;}
    return 0;
}
