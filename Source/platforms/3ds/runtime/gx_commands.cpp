#include "gx_commands.hpp"
#include "gx_bank_delta.hpp"
#include "gx_profile.hpp"
#include "gx_compact_cache.hpp"
#include "skin_palette.hpp"
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXAurora.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>

namespace dusk::native::gx {
namespace {
u32 word(const u8* p){return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3];}
u16 half(const u8* p){return (u16(p[0])<<8)|p[1];}
u32 field(u32 value,unsigned shift,unsigned bits){return (value>>shift)&((1u<<bits)-1);}
bool primitive(u8 code){return code==GX_QUADS||code==GX_TRIANGLES||code==GX_TRIANGLESTRIP||code==GX_TRIANGLEFAN||code==GX_LINES||code==GX_LINESTRIP||code==GX_POINTS;}
// Compiled register writes (runCompiled: the J2D material memo).
enum : u32 {opBp=1,opBpSpecial,opCp,opXf,opInvalidate};
// Incremental state key components (CommandRegisters::stateHash). A word's
// term is mix(word^seed)^mix(seed): zero for a zero word, so a zeroed bank
// hashes to 0, and a changed word changes its component by
// mix(old^seed)^mix(new^seed), never 0 (mix, the MurmurHash3 finalizer, is a
// bijection). The words and their split into components are nativeStateKey's.
inline u32 hashMix(u32 h){h^=h>>16;h*=0x85ebca6bu;h^=h>>13;h*=0xc2b2ae35u;h^=h>>16;return h;}
enum : u32 {laneBp=1,laneTexture=2,laneCp=3,laneXf=4};
constexpr u32 hashSeed(u32 lane,u32 index){return index*0x9e3779b1u+lane*0x632be5abu+0x7f4a7c15u;}
inline void hashWord(u32& component,u32 seed,u32 before,u32 after){
    if(before!=after)component^=hashMix(before^seed)^hashMix(after^seed);
}
// BP registers by component: 1 bp, 2 bp with only the fog type and
// projection kind (BP 0xf1 >> 20), 3 texture. TEV colour banks and the fog
// curve, range and colour (0xe0-0xf0, 0xf2) are refreshed per replay: none.
constexpr std::array<u8,256> bpLanes=[]{
    std::array<u8,256> t{};
    for(unsigned i=0;i<0x80;++i)t[i]=1;
    for(unsigned i=0x80;i<0xc0;++i)t[i]=3;
    for(unsigned i=0xc0;i<0xe0;++i)t[i]=1;
    t[0xf1]=2;for(unsigned i=0xf3;i<0x100;++i)t[i]=1;
    return t;
}();
// CP: vertex descriptors, attribute formats and array strides (0x30/0x40
// mirror the matrix indices, keyed per shape). XF: channel/texgen state
// except material/ambient colours, the matrix indices, viewport/projection.
inline bool cpHashed(u32 reg){
    return reg==0x50||reg==0x60||(reg>=0x70&&reg<0x78)||(reg>=0x80&&reg<0x88)||(reg>=0x90&&reg<0x98)||(reg>=0xb0&&reg<0xc0);
}
inline bool xfHashed(u32 at){return at>=0x1000&&at<0x1058&&!(at>=0x100a&&at<0x100e)&&!(at>=0x1018&&at<0x1026);}
inline void hashBp(CommandRegisters& r,u32 reg,u32 before,u32 after){
    switch(bpLanes[reg]){
    case 1:hashWord(r.stateHash.bp,hashSeed(laneBp,reg),before,after);break;
    case 2:hashWord(r.stateHash.bp,hashSeed(laneBp,reg),before>>20,after>>20);break;
    case 3:hashWord(r.stateHash.texture,hashSeed(laneTexture,reg),before,after);break;
    default:break;
    }
}
inline void hashCp(CommandRegisters& r,u32 reg,u32 before,u32 after){
    if(cpHashed(reg))hashWord(r.stateHash.cp,hashSeed(laneCp,reg),before,after);
}
inline void hashXf(CommandRegisters& r,u32 at,u32 before,u32 after){
    if(xfHashed(at))hashWord(r.stateHash.xf,hashSeed(laneXf,at),before,after);
}
// TLUT loads (two words each) and the overflow flag: texture component.
u32 tlutWord(const CommandRegisters::TlutLoad& load){return load.offset^(load.entries<<16);}
void setTlut(CommandRegisters& r,unsigned slot,const CommandRegisters::TlutLoad& value){
    auto& load=r.tluts[slot];
    hashWord(r.stateHash.texture,hashSeed(laneTexture,0x100+2*slot),load.token,value.token);
    hashWord(r.stateHash.texture,hashSeed(laneTexture,0x101+2*slot),tlutWord(load),tlutWord(value));
    load=value;
}
void setTlutOverflow(CommandRegisters& r){
    hashWord(r.stateHash.texture,hashSeed(laneTexture,0x140),r.tlutOverflow,1);r.tlutOverflow=true;
}
bool stateHashOn=true,stateHashVerify=false;
StateHashStatistics stateHashStats{};
const CommandRegisters* stateHashCheckedBank{};std::uint32_t stateHashCheckedEpoch{};
TypedWriteStatistics typedStats{};
// J2D material memo recording (see materialMemoLookup). It is bound to the
// register bank of the stream's processor and keeps the typed writes in the
// compiled-list format; texture binding writes are left to the live loads.
struct MaterialRecorder {
    const CommandRegisters* target{};
    CommandProcessor* processor{};
    u32* words{};unsigned capacity{},used{};
    u32* textureWords{};unsigned textureCapacity{},textureUsed{}; // verification only
    unsigned split{},commands{},splitCommands{},pendingMasks{};
    u32 mask=0xffffff;
    MaterialMemoRefusal refusal{}; // the first cause, none while storable
    bool splitDone{},afterTextures{};
    // Texture matrix sources: the object declared for the next matrix load
    // (materialMemoSource), and the ones whose matrix a load sent.
    const void* pendingObject{};const void* pendingMatrix{};std::size_t pendingSize{};
    struct Source {const void* object{};u32 size{},sumA{},sumB{};};
    std::array<Source,8> sources{};unsigned sourceCount{};
};
MaterialRecorder recorder;
bool materialRecording{};
void refuseRecording(MaterialMemoRefusal why){if(recorder.refusal==MaterialMemoRefusal::none)recorder.refusal=why;}
bool recordWords(u32* words,unsigned capacity,unsigned& used,std::initializer_list<u32> values){
    if(values.size()>capacity-used)return false;
    for(const u32 w:values)words[used++]=w;
    return true;
}
bool recordingOn(const CommandProcessor& p){return &p.registers()==recorder.target;}
// Any processor entry other than a typed register write cannot be replayed.
inline void noteForeignCommand(const CommandProcessor& p,MaterialMemoRefusal why=MaterialMemoRefusal::foreign){
    if(materialRecording&&recordingOn(p))[[unlikely]]refuseRecording(why);
}
void recordMaterialBp(CommandProcessor& p,u8 reg,u32 value){
    auto& r=recorder;if(!recordingOn(p))return;
    r.processor=&p;value&=0xffffff;
    if(reg==0xfe){r.mask=value;++r.pendingMasks;return;}
    const u32 mask=r.mask;r.mask=0xffffff;
    // TEV colours select their bank through the merged value, TLUT loads
    // always count: both replay through applyBp, as in compiled lists.
    const bool special=reg==0x65||(reg>=0xe0&&reg<=0xe7);
    const u32 head=(special?opBpSpecial:opBp)|(u32(reg)<<8);
    const u32 first=special?value:value&mask,second=special?mask:~mask&0xffffff;
    if(reg==0x64||reg==0x65||(reg>=0x80&&reg<0xc0)){
        // Texture loads run live on a replay: only the TEV block's own,
        // before any of its other writes.
        if(!r.splitDone||r.afterTextures)refuseRecording(MaterialMemoRefusal::textureOrder);
        if(r.textureWords&&!recordWords(r.textureWords,r.textureCapacity,r.textureUsed,{head,first,second}))
            refuseRecording(MaterialMemoRefusal::capacity);
        r.pendingMasks=0;return;
    }
    if(r.splitDone)r.afterTextures=true;
    r.commands+=r.pendingMasks+1;r.pendingMasks=0;
    if(!recordWords(r.words,r.capacity,r.used,{head,first,second}))refuseRecording(MaterialMemoRefusal::capacity);
}
void recordMaterialCp(CommandProcessor& p,u8 reg,u32 value){
    auto& r=recorder;if(!recordingOn(p))return;
    r.processor=&p;if(r.splitDone)r.afterTextures=true;++r.commands;
    if(!recordWords(r.words,r.capacity,r.used,{opCp|(u32(reg)<<8),value}))refuseRecording(MaterialMemoRefusal::capacity);
}
void recordMaterialXf(CommandProcessor& p,u16 address,std::span<const u32> values){
    auto& r=recorder;if(!recordingOn(p))return;
    r.processor=&p;if(r.splitDone)r.afterTextures=true;++r.commands;
    if(address<0x1000){
        // Matrix memory: only a texture matrix (TEXMTX0-9, identity) loaded
        // right after materialMemoSource declared its object, sending exactly
        // that object's matrix. The stored entry keeps a checksum of the
        // object, so equal bytes on a replay mean the same words here.
        const auto* matrix=static_cast<const u8*>(r.pendingMatrix);
        const auto* object=static_cast<const u8*>(r.pendingObject);
        const bool sourced=object&&address>=0x78&&address+values.size()<=0x100&&r.sourceCount<r.sources.size()&&
            matrix>=object&&matrix+values.size_bytes()<=object+r.pendingSize&&
            !std::memcmp(matrix,values.data(),values.size_bytes());
        r.pendingObject=r.pendingMatrix=nullptr;
        if(!sourced){refuseRecording(MaterialMemoRefusal::matrix);return;}
        MaterialMemoSum sum;materialMemoChecksum(sum,object,r.pendingSize);
        r.sources[r.sourceCount++]={object,u32(r.pendingSize),sum.a,sum.b};
    }
    if(values.size()+2>r.capacity-r.used){refuseRecording(MaterialMemoRefusal::capacity);return;}
    r.words[r.used++]=opXf|(u32(values.size())<<8);r.words[r.used++]=address;
    for(const u32 w:values)r.words[r.used++]=w;
}
}
bool CommandMemory::add(std::span<const u8> bytes,MemoryHandle& handle) {
    handle=0;const auto base=reinterpret_cast<std::uintptr_t>(bytes.data());
    if(bytes.empty()||!base||bytes.size()>std::numeric_limits<u32>::max()||
       bytes.size()>std::numeric_limits<std::uintptr_t>::max()-base||
       next>std::numeric_limits<MemoryHandle>::max()/capacity)return false;
    Region* free=nullptr;
    for(auto& r:regions){
        if(!r.handle){if(!free)free=&r;continue;}
        const auto other=reinterpret_cast<std::uintptr_t>(r.bytes.data());
        if(base<other+r.bytes.size()&&other<base+bytes.size())return false;
    }
    if(!free)return false;
    // The low bits identify the slot; the generation rejects recycled slots.
    handle=next++*capacity+static_cast<MemoryHandle>(free-regions.data());
    *free={handle,bytes};rebound();return true;
}
void CommandMemory::rebound(){
    lowest=std::numeric_limits<std::uintptr_t>::max();highest=0;
    for(const auto& r:regions)if(r.handle){
        const auto base=reinterpret_cast<std::uintptr_t>(r.bytes.data());
        lowest=std::min(lowest,base);highest=std::max(highest,base+r.bytes.size());
    }
}
bool CommandMemory::remove(MemoryHandle handle){
    auto& region=regions[handle%capacity];
    if(!handle||region.handle!=handle)return false;
    region={};rebound();return true;
}
bool CommandMemory::find(std::uint64_t address,u32 size,MemoryHandle& handle,u32& offset)const {
    handle=0;offset=0;if(!address||!size)return false;
    if(address<lowest||address>highest)return false;
    for(const auto& r:regions)if(r.handle){
        const auto base=reinterpret_cast<std::uintptr_t>(r.bytes.data());
        if(address>=base && address-base<=r.bytes.size() && size<=r.bytes.size()-(address-base)){
            handle=r.handle;offset=static_cast<u32>(address-base);return true;
        }
    }
    return false;
}
bool CommandMemory::resolve(MemoryHandle handle,u32 offset,u32 size,std::span<const u8>& bytes)const {
    bytes={};
    const auto& region=regions[handle%capacity];
    if(!handle||region.handle!=handle)return false;
    if(offset>region.bytes.size() || size>region.bytes.size()-offset)return false;
    bytes=region.bytes.subspan(offset,size);
    return true;
}
bool CommandDraw::vertex(u32 index,DecodedVertex& output,VertexDecodeError* error)const {
    if(index>=vertexCount){output={};if(error)*error=VertexDecodeError::packedBounds;return false;}
    return decodeVertex(plan,vertices.subspan(size_t(index)*plan.packedStride(),plan.packedStride()),arrays,output,error);
}
bool CommandDraw::triangleIndex(u32 index,u16& output)const {
    output=0;if(index>=indexCount)return false;std::memcpy(&output,indices.data()+size_t(index)*2,2);return output<vertexCount;
}
bool CommandProcessor::reject(CommandError value,size_t offset){if(failure==CommandError::none){failure=value;failureOffset=offset;}return false;}
void CommandProcessor::reset(){
    noteForeignCommand(*this);
    if(active){reject(CommandError::reentrant,0);return;}
    // The topology cache is cleared below: nobody else may be reading it.
    if(drawBarrier)drawBarrier();
    auto* mem=&memory;auto callback=sink;auto* context=sinkContext;auto resolver=liveArrayResolver;
    // In-place reset avoids materializing the register bank on a 3DS stack.
    const auto scratch=batchScratch;auto* topology=batchCache;const auto log=bankLog;const auto barrier=drawBarrier;
    const auto deferral=runDeferral;auto* deferralContext=runDeferralContext;const auto fixedBegin=immutableBegin,fixedEnd=immutableEnd;
    const auto epoch=regs.stateEpoch,setupEpoch=regs.vertexSetupEpoch; // never reuse an epoch for a different bank
    this->~CommandProcessor();new(this)CommandProcessor(*mem);setSink(callback,context);setLiveArrayResolver(resolver);setPrimitiveBatchScratch(scratch);setPrimitiveBatchCache(topology);
    regs.stateEpoch=epoch+1;regs.vertexSetupEpoch=setupEpoch+1;
    // The constructor above took the log's buffer away with everything else.
    bankLog={log.words,log.capacity,0,true,true};drawBarrier=barrier;
    runDeferral=deferral;runDeferralContext=deferralContext;immutableBegin=fixedBegin;immutableEnd=fixedEnd;
}
bool CommandProcessor::prepare(unsigned vat,const CommandRegisters& bank) {
    auto& prepared=cache[vat];if(prepared.ready)return true;
    if(!bank.cpValid[0x50]||!bank.cpValid[0x60])return false;
    const std::array<u32,5> signature{bank.cp[0x50],bank.cp[0x60],bank.cp[0x70+vat],bank.cp[0x80+vat],bank.cp[0x90+vat]};
    if(prepared.created&&prepared.signature==signature){prepared.ready=true;return true;}
    std::array<GXVtxDescList,GX_VA_TEX7+2> desc{};
    for(unsigned i=0;i<=GX_VA_TEX7;++i)desc[i].attr=static_cast<GXAttr>(i);
    const u32 low=bank.cp[0x50],high=bank.cp[0x60];
    for(unsigned i=0;i<9;++i)desc[i].type=static_cast<GXAttrType>(field(low,i,1));
    for(unsigned i=0;i<4;++i)desc[GX_VA_POS+i].type=static_cast<GXAttrType>(field(low,9+i*2,2));
    for(unsigned i=0;i<8;++i)desc[GX_VA_TEX0+i].type=static_cast<GXAttrType>(field(high,i*2,2));
    desc.back().attr=GX_VA_NULL;
    std::array<GXVtxAttrFmtList,13> fmt{};
    for(unsigned i=0;i<12;++i)fmt[i].attr=static_cast<GXAttr>(GX_VA_POS+i);
    fmt.back().attr=GX_VA_NULL;
    auto set=[&](unsigned attr,u32 count,u32 type,u32 frac){auto& f=fmt[attr-GX_VA_POS];f.cnt=static_cast<GXCompCnt>(count);f.type=static_cast<GXCompType>(type);f.frac=frac;};
    const u32 a=bank.cp[0x70+vat],b=bank.cp[0x80+vat],c=bank.cp[0x90+vat];
    set(GX_VA_POS,field(a,0,1),field(a,1,3),field(a,4,5));
    set(GX_VA_NRM,field(a,31,1)?GX_NRM_NBT3:field(a,9,1)?GX_NRM_NBT:GX_NRM_XYZ,field(a,10,3),0);
    set(GX_VA_CLR0,field(a,13,1),field(a,14,3),0);set(GX_VA_CLR1,field(a,17,1),field(a,18,3),0);
    set(GX_VA_TEX0,field(a,21,1),field(a,22,3),field(a,25,5));
    for(unsigned i=0;i<3;++i)set(GX_VA_TEX1+i,field(b,i*9,1),field(b,i*9+1,3),field(b,i*9+4,5));
    set(GX_VA_TEX4,field(b,27,1),field(b,28,3),field(c,0,5));
    for(unsigned i=0;i<3;++i)set(GX_VA_TEX5+i,field(c,5+i*9,1),field(c,6+i*9,3),field(c,9+i*9,5));
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)if(desc[attr].type!=GX_NONE){
        const bool first=attr<=GX_VA_TEX0,second=attr>=GX_VA_TEX1&&attr<=GX_VA_TEX4,third=attr>=GX_VA_TEX4;
        if((first&&!bank.cpValid[0x70+vat])||(second&&!bank.cpValid[0x80+vat])||(third&&!bank.cpValid[0x90+vat]))return false;
    }
    const auto layout=aurora::gx::dl::makeLayout(desc.data(),fmt.data());
    if(!layout||!planVertexDecode(*layout,fmt,prepared.plan))return false;
    prepared.layout=*layout;prepared.signature=signature;prepared.created=true;prepared.ready=true;++stats.plans;
    prepared.indexed=false;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)
        prepared.indexed|=layout->attrs[attr].type==GX_INDEX8||layout->attrs[attr].type==GX_INDEX16;
    return true;
}
// Every array the format indexes lies in immutable memory.
bool CommandProcessor::arraysImmutable(unsigned vat)const{
    const auto& layout=cache[vat].layout;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)if(layout.attrs[attr].type==GX_INDEX8||layout.attrs[attr].type==GX_INDEX16){
        const auto& bind=bindings[attr-GX_VA_POS];std::span<const u8> bytes;
        // Live game arrays are heap memory.
        if(bind.liveAddress||!memory.resolve(bind.handle,bind.offset,bind.size,bytes))return false;
        const auto begin=reinterpret_cast<std::uintptr_t>(bytes.data());
        if(begin<immutableBegin||begin>=immutableEnd||bytes.size()>immutableEnd-begin)return false;
    }
    return true;
}
bool CommandProcessor::draw(std::span<const u8> vertices,std::span<const u8> indices,u8 opcode,u16 count,u32 indexCount,size_t offset,const CommandRegisters& bank) {
    static const auto prepIndex=dusk::native::registerGxNamedProfile("proc.drawPrep");dusk::native::GxNamedProfileSpan prepSpan(prepIndex);
    if(!count)return indexCount?reject(CommandError::invalidIndex,offset):true;
    if(!sink)return reject(CommandError::missingSink,offset);
    if(drawBarrier)drawBarrier();
    const unsigned vat=opcode&7;VertexArrays arrays{};
    const auto& layout=cache[vat].layout;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)if(layout.attrs[attr].type==GX_INDEX8||layout.attrs[attr].type==GX_INDEX16){
        const unsigned slot=attr-GX_VA_POS;const auto& bind=bindings[slot];std::span<const u8> bytes;
        if(!resolveBinding(bind,bytes))return reject(CommandError::staleMemory,offset);
        arrays[attr]={bytes,bank.cp[0xb0+slot]&255,bind.little};
    }
    if(!trustedIndices)for(u32 i=0;i<indexCount;++i){u16 v;std::memcpy(&v,indices.data()+size_t(i)*2,2);if(v>=count)return reject(CommandError::invalidIndex,offset);}
    const CommandDraw packet{static_cast<GXPrimitive>(opcode&0xf8),count,indexCount,vertices,indices,cache[vat].plan,arrays,bank};
    ++staticGeometryHint.batch;staticGeometryHint.trustedIndices=trustedIndices;
    prepSpan.stop();
    if(!sink(packet,sinkContext))return reject(CommandError::sinkRejected,offset);
    if(failure!=CommandError::none)return false;
    ++stats.draws;return true;
}
bool CommandProcessor::drawBatch(std::span<const u8> bytes,u8 vat,size_t offset,size_t& consumed) {
    consumed=0;
    if(batchScratch.vertices.empty()||batchScratch.indices.empty()||!batchScratch.maxVertices)return true;
    if(drawBarrier)drawBarrier();
    const auto stride=cache[vat].plan.packedStride();
    if(!stride)return true;
    size_t cursor=0,vertexCount=0,indexCount=0,packets=0;
    // Validate sizes without copying first; one packet keeps the original
    // zero-copy path. The bounded second pass copies only admitted geometry.
    while(cursor+3<=bytes.size()) {
        const u8 op=bytes[cursor];if((op&7)!=vat)break;
        const unsigned primitive=op&0xf8,count=half(bytes.data()+cursor+1);
        size_t indices=0;
        if(primitive==GX_TRIANGLES&&count>=3&&count%3==0)indices=count;
        else if(primitive==GX_QUADS&&count>=4&&count%4==0)indices=size_t(count/4)*6;
        else if((primitive==GX_TRIANGLESTRIP||primitive==GX_TRIANGLEFAN)&&count>=3)indices=size_t(count-2)*3;
        else break;
        const size_t payload=size_t(count)*stride;
        if(payload>bytes.size()-cursor-3||count>std::min(batchScratch.maxVertices,65535u)-vertexCount||
           payload>batchScratch.vertices.size()-vertexCount*stride||indices>batchScratch.indices.size()-indexCount)break;
        vertexCount+=count;indexCount+=indices;cursor+=3+payload;++packets;
    }
    if(packets<2)return true;
    auto overlaps=[](std::span<const u8> a,std::span<const u8> b){
        if(a.empty()||b.empty())return false;
        const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
        return x<=y?y-x<a.size():x-y<b.size();
    };
    const auto vertexBytes=batchScratch.vertices.first(vertexCount*stride);
    const std::span<const u8> indexBytes{reinterpret_cast<const u8*>(batchScratch.indices.data()),indexCount*sizeof(u16)};
    if(overlaps(vertexBytes,bytes)||overlaps(indexBytes,bytes)||overlaps(vertexBytes,indexBytes))
        return reject(CommandError::format,offset);
    const bool deduplicate=!batchScratch.hashSlots.empty()||!batchScratch.remap.empty();
    if(deduplicate){
        const auto size=batchScratch.hashSlots.size();
        if(size<vertexCount*2||(size&(size-1))||batchScratch.remap.size()<vertexCount)
            return reject(CommandError::format,offset);
        const std::span<const u8> hashBytes{reinterpret_cast<const u8*>(batchScratch.hashSlots.data()),size*2};
        const std::span<const u8> remapBytes{reinterpret_cast<const u8*>(batchScratch.remap.data()),vertexCount*2};
        for(const auto extra:{hashBytes,remapBytes})if(overlaps(extra,bytes)||overlaps(extra,vertexBytes)||overlaps(extra,indexBytes))
            return reject(CommandError::format,offset);
        if(overlaps(hashBytes,remapBytes))return reject(CommandError::format,offset);
    }
    const bool cacheable=deduplicate&&batchCache&&!overlaps(batchCache->memory(),bytes)&&
        !overlaps(batchCache->memory(),vertexBytes)&&!overlaps(batchCache->memory(),indexBytes)&&
        !overlaps(batchCache->memory(),{reinterpret_cast<const u8*>(batchScratch.hashSlots.data()),batchScratch.hashSlots.size_bytes()})&&
        !overlaps(batchCache->memory(),{reinterpret_cast<const u8*>(batchScratch.remap.data()),batchScratch.remap.size_bytes()});
    if(cacheable){
        static const auto findIndex=dusk::native::registerGxNamedProfile("proc.topologyFind");dusk::native::GxNamedProfileSpan findSpan(findIndex);
        const auto saved=batchCache->find(bytes.first(cursor),stride,staticGeometryHint.list?staticGeometryHint.generation:0);findSpan.stop();
        if(saved){
            // Resolve all current arrays and consume the current register bank
            // through the normal sink; only unchanged packet topology is reused.
            trustedIndices=true; // validated when this topology was stored
            const bool drawn=draw(saved.vertices,saved.indices,GX_TRIANGLES|vat,saved.vertexCount,indexCount,offset,regs);
            trustedIndices=false;
            if(!drawn)return false;
            consumed=cursor;stats.commands+=packets-1;stats.mergedPrimitives+=packets;
            stats.reusedVertices+=vertexCount-saved.vertexCount;++stats.batchCacheHits;return true;
        }
        ++stats.batchCacheMisses;
    }
    if(deduplicate)std::fill(batchScratch.hashSlots.begin(),batchScratch.hashSlots.end(),0);
    size_t at=0,vertex=0,index=0,unique=0;
    while(at<cursor) {
        const auto primitive=static_cast<GXPrimitive>(bytes[at]&0xf8);
        const unsigned count=half(bytes.data()+at+1);
        const size_t payload=size_t(count)*stride;
        if(deduplicate)for(unsigned n=0;n<count;++n){
            const auto* input=bytes.data()+at+3+n*stride;
            u32 hash=2166136261u;for(unsigned b=0;b<stride;++b)hash=(hash^input[b])*16777619u;
            size_t slot=hash&(batchScratch.hashSlots.size()-1);
            while(batchScratch.hashSlots[slot]&&std::memcmp(input,batchScratch.vertices.data()+(batchScratch.hashSlots[slot]-1)*stride,stride))
                slot=(slot+1)&(batchScratch.hashSlots.size()-1);
            if(!batchScratch.hashSlots[slot]){
                std::memcpy(batchScratch.vertices.data()+unique*stride,input,stride);
                batchScratch.hashSlots[slot]=++unique;
            }
            batchScratch.remap[vertex+n]=batchScratch.hashSlots[slot]-1;
        }else std::memcpy(batchScratch.vertices.data()+vertex*stride,bytes.data()+at+3,payload);
        if(!aurora::gx::dl::expand_triangles(primitive,count,[&](u16 a,u16 b,u16 c){
            for(const auto local:{a,b,c})batchScratch.indices[index++]=deduplicate?batchScratch.remap[vertex+local]:vertex+local;
        }))return reject(CommandError::format,offset+at);
        vertex+=count;at+=3+payload;
    }
    // Keep the geometry arena's three-vertex minimum for fully degenerate
    // inputs. Padding is unused by indices and does not change their topology.
    const auto submittedVertices=deduplicate?std::max(size_t(3),unique):vertexCount;
    if(deduplicate)for(size_t n=unique;n<submittedVertices;++n)
        std::memcpy(batchScratch.vertices.data()+n*stride,batchScratch.vertices.data(),stride);
    if(cacheable)batchCache->store(bytes.first(cursor),stride,{
        batchScratch.vertices.first(submittedVertices*stride),
        {reinterpret_cast<const u8*>(batchScratch.indices.data()),indexCount*sizeof(u16)},unsigned(submittedVertices)},
        staticGeometryHint.list?staticGeometryHint.generation:0);
    if(!draw(batchScratch.vertices.first(submittedVertices*stride),
        {reinterpret_cast<const u8*>(batchScratch.indices.data()),indexCount*sizeof(u16)},
        GX_TRIANGLES|vat,submittedVertices,indexCount,offset,regs))return false;
    consumed=cursor;stats.commands+=packets-1;stats.mergedPrimitives+=packets;
    stats.reusedVertices+=vertexCount-submittedVertices;
    return true;
}
namespace {
// XF words outside the per-shape geometry matrices and matrix index registers.
inline bool epochWord(u32 at){return !(at<0x78||(at>=0x400&&at<0x45a)||at==0x1018||at==0x1019||dusk::native::skin::inPaletteXf(at,1));}
}
// Marks XF words [address,address+count) valid, a word of bits at a time
// (the bit layout is the one gx_bank_delta.hpp asserts). True when one of
// them was not valid before: the worker's copy then needs the valid bits.
inline bool markXfValid(CommandRegisters& regs,std::size_t address,std::size_t count){
    auto* words=bankWordsOf(regs)+bank_layout::xfValidAt;bool grew=false;
    for(const std::size_t last=address+count;address<last;){
        const std::size_t word=address/32,upTo=std::min(last,(word+1)*32),bits=upTo-address;
        const u32 mask=(bits==32?~0u:((1u<<bits)-1))<<(address%32);
        if((words[word]&mask)!=mask){words[word]|=mask;grew=true;}
        address=upTo;
    }
    return grew;
}
bool CommandProcessor::applyCp(u8 reg,u32 value,size_t offset) {
    if(reg==0x50)normalAliasesValid=false;
    if(!(reg==0x30||reg==0x40||reg==0x50||reg==0x60||(reg>=0x70&&reg<0x98)||(reg>=0xb0&&reg<0xc0)))return reject(CommandError::unsupported,offset);
    if(reg>=0x78&&reg<0x80)return reject(CommandError::unsupported,offset);
    if(reg>=0x88&&reg<0x90)return reject(CommandError::unsupported,offset);
    if(!regs.cpValid[reg]||regs.cp[reg]!=value){
        if(reg==0x50||reg==0x60)for(auto& entry:cache)entry.ready=false;
        else if(reg>=0x70&&reg<0x98)cache[reg&7].ready=false;
    }
    if(reg!=0x30&&reg!=0x40&&(!regs.cpValid[reg]||regs.cp[reg]!=value)){++regs.stateEpoch;++regs.vertexSetupEpoch;}
    hashCp(regs,reg,regs.cp[reg],value);
    if(!regs.cpValid[reg])bankLog.grewValid=true;
    if(regs.cp[reg]!=value)bankLog.store(bank_layout::cpAt+reg,value);
    regs.cp[reg]=value;regs.cpValid.set(reg);
    if(reg==0x30||reg==0x40){const unsigned slot=(reg-0x30)/16;
        regs.matrixIndex[slot]=value;regs.matrixIndexValid.set(slot);}
    return true;
}
void CommandProcessor::updateMatrixIndices(u32 address,u32 count) {
    if(address<=0x1008&&address+count>0x1008)normalAliasesValid=false;
    for(unsigned slot=0;slot<2;++slot)if(address<=0x1018+slot&&address+count>0x1018+slot){
        regs.matrixIndex[slot]=regs.xf[0x1018+slot];regs.matrixIndexValid.set(slot);}
}
void CommandProcessor::applyBp(u8 reg,u32 value) {
    value&=0xffffff;
    if(reg==0xfe){regs.bpMask=value;return;}

    const u32 previous=regs.bp[reg];
    const u32 mask=regs.bpMask;
    const u32 merged=previous^((previous^value)&mask);
    regs.bpMask=0xffffff;
    const bool color=unsigned(reg)-0xe0<8;
    const bool palette=reg==0x65;
    const bool valid=regs.bpValid[reg];
    if(!color&&!palette&&valid&&previous==merged)return;

    if(color){
        // A TEV colour load selects a latch after merging with the raw word.
        // Its epoch follows that latch, even when the raw word changes.
        const unsigned bank=(merged>>23)&1,slot=reg-0xe0;
        if(!regs.tevColorValid[bank*8+slot]||regs.tevColor[bank][slot]!=merged)++regs.stateEpoch;
        regs.tevColor[bank][slot]=merged;
        regs.tevColorValid.set(bank*8+slot);
    }else{
        // A palette load is a trigger even when the raw word is unchanged.
        ++regs.stateEpoch;
    }
    if(!valid){
        bankLog.grewValid=true;
        regs.bpValid.set(reg);
    }
    if(previous!=merged){
        hashBp(regs,reg,previous,merged);
        bankLog.store(bank_layout::bpAt+reg,merged);
        regs.bp[reg]=merged;
    }
    if(palette){
        const u32 offset=merged&1023,entries=((merged>>10)&2047)*16;
        const u32 begin=offset*512,end=begin+entries*2;
        unsigned available=unsigned(regs.tluts.size());
        for(unsigned slot=0;slot<regs.tluts.size();++slot){
            const auto& load=regs.tluts[slot];
            const u32 oldBegin=load.offset*512,oldEnd=oldBegin+load.entries*2;
            if(load.entries&&begin<oldEnd&&oldBegin<end)setTlut(regs,slot,{});
            if(!load.entries&&available==regs.tluts.size())available=slot;
        }
        if(available<regs.tluts.size())setTlut(regs,available,{regs.bpValid[0x64]?regs.bp[0x64]:0,offset,
            !(merged>>21)&&entries&&entries<=16384&&end<=0x80000?entries:0});
        else setTlutOverflow(regs);
    }
}
bool CommandProcessor::writeCp(u8 reg,u32 value) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none||!applyCp(reg,value,0))return false;
    ++stats.commands;
    if(materialRecording)[[unlikely]]recordMaterialCp(*this,reg,value);
    return true;
}
bool CommandProcessor::writeBp(u8 reg,u32 value) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    // BP52 starts a real EFB transfer. Until a native transfer sink exists,
    // reject it rather than latch the trigger and silently omit the copy.
    if(reg==0x52)return reject(CommandError::unsupported,0);
    applyBp(reg,value);++stats.commands;
    if(materialRecording)[[unlikely]]recordMaterialBp(*this,reg,value);
    return true;
}
bool CommandProcessor::writeBpMasked(u8 reg,u32 mask,u32 value) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    ++typedStats.masked;mask&=0xffffff;
    if(reg==0x52){
        // As the two writes: the mask is taken, then the trigger refused.
        regs.bpMask=mask;++stats.commands;
        if(materialRecording)[[unlikely]]recordMaterialBp(*this,0xfe,mask);
        return reject(CommandError::unsupported,0);
    }
    value&=0xffffff;
    // A known register equal under the mask: the merge would store the same
    // word, so the epoch and hash stay and only the mask resets. TEV colour
    // banks, TLUT loads and the mask register keep applyBp's own handling.
    const bool special=reg==0x65||reg==0xfe||(reg>=0xe0&&reg<=0xe7);
    if(!special&&regs.bpValid[reg]&&((regs.bp[reg]^value)&mask)==0){regs.bpMask=0xffffff;++typedStats.unchanged;}
    else{regs.bpMask=mask;applyBp(reg,value);}
    stats.commands+=2;
    if(materialRecording)[[unlikely]]{recordMaterialBp(*this,0xfe,mask);recordMaterialBp(*this,reg,value);}
    return true;
}
bool copyTileLayout(u32 format,unsigned& xShift,unsigned& yShift,unsigned& components) {
    components=1;
    switch(format){
    case GX_TF_I4:case 8:case GX_CTF_R4:case GX_CTF_Z4:xShift=3;yShift=3;return true;
    case GX_TF_I8:case GX_TF_IA4:case 9:case GX_TF_Z8:case GX_CTF_RA4:
    case GX_CTF_A8:case GX_CTF_R8:case GX_CTF_G8:case GX_CTF_B8:
    case GX_CTF_Z8M:case GX_CTF_Z8L:xShift=3;yShift=2;return true;
    case GX_TF_RGBA8:case GX_TF_Z24X8:components=2;[[fallthrough]];
    case GX_TF_IA8:case GX_TF_RGB565:case GX_TF_RGB5A3:case 10:case GX_TF_Z16:
    case GX_CTF_RA8:case GX_CTF_RG8:case GX_CTF_GB8:case GX_CTF_Z16L:
        xShift=2;yShift=2;return true;
    // YUVA8 has a control-field case but is rejected by the pinned original
    // tile-layout owner. Do not invent a stride for its unresolved layout.
    default:return false;
    }
}
bool CommandProcessor::applyCopySource(bool texture,u32 left,u32 top,u32 width,u32 height,size_t offset) {
    if(!width||!height||width>1024||height>1024||left>1023||top>1023||
       width>1024-left||height>1024-top)return reject(CommandError::format,offset);
    auto& surface=texture?regs.copy.texture:regs.copy.display;
    surface.source={u16(left),u16(top),u16(width),u16(height)};surface.sourceReady=true;
    return true;
}
bool CommandProcessor::applyCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap,size_t offset) {
    if(!width||!height||width>1024||height>1024)return reject(CommandError::format,offset);
    auto& surface=texture?regs.copy.texture:regs.copy.display;
    u32 stride,control=surface.control;
    if(texture){
        unsigned x,y,components;
        if(!copyTileLayout(format,x,y,components))return reject(CommandError::format,offset);
        stride=((width+(1u<<x)-1)>>x)*components;
        u32 hardware=format&15;if(format==GX_TF_Z16)hardware=11;
        const u32 intensity=format==GX_TF_I4||format==GX_TF_I8||format==GX_TF_IA4||format==GX_TF_IA8;
        constexpr u32 mask=(1u<<3)|(7u<<4)|(1u<<9)|(3u<<15);
        const u32 value=((hardware>>3)<<3)|((hardware&7)<<4)|(u32(mipmap)<<9)|((intensity?3u:2u)<<15);
        control=(control&~mask)|value;
    }else{
        if(width%16||format!=GX_TF_RGBA8||mipmap)return reject(CommandError::format,offset);
        stride=width/16;
    }
    surface.width=width;surface.height=height;surface.format=static_cast<GXTexFmt>(format);
    surface.mipmap=mipmap;surface.depth=(format&_GX_TF_ZTF)!=0;surface.stride=stride;
    surface.control=control;surface.destinationReady=true;
    return true;
}
bool CommandProcessor::setCopySource(bool texture,u32 left,u32 top,u32 width,u32 height) {
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none||!applyCopySource(texture,left,top,width,height,0))return false;
    ++stats.commands;return true;
}
bool CommandProcessor::setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap) {
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none||!applyCopyDestination(texture,width,height,format,mipmap,0))return false;
    ++stats.commands;return true;
}
bool CommandProcessor::setCopyControl(bool texture,u32 mask,u32 value) {
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if((mask|value)&~0xffffffu)return reject(CommandError::format,0);
    auto& control=texture?regs.copy.texture.control:regs.copy.display.control;
    control=(control&~mask)|(value&mask);++stats.commands;return true;
}
bool CommandProcessor::applyIndexed(u8 code,u16 index,u16 packed,size_t offset) {
    const unsigned slot=12+(code-GX_LOAD_INDX_A)/8;
    const u32 count=(packed>>12)+1,address=packed&0xfff;
    if(address>=regs.xf.size()||count>regs.xf.size()-address)return reject(CommandError::unsupported,offset);
    const auto& bind=bindings[slot];std::span<const u8> array;
    if(!resolveBinding(bind,array))return reject(CommandError::staleMemory,offset);
    const std::uint64_t at=std::uint64_t(index)*(regs.cp[0xb0+slot]&255);
    if(at>array.size()||count*4>array.size()-at)return reject(CommandError::arrayBounds,offset);
    applyIndexedWords(address,count,array.data()+at,bind.little);
    return true;
}
// The bank side of an indexed load: `count` words at `source` in the array's
// byte order (native packets replay it from the bytes read at capture).
void CommandProcessor::applyIndexedWords(u32 address,u32 count,const u8* source,bool little){
    // Geometry matrices (the common case) skip the change comparison.
    const bool geometry=address+count<=0x78||(address>=0x400&&address+count<=0x45a);
    bool changed=false;
    if(little&&std::endian::native==std::endian::little){
        if(!geometry)for(u32 i=0;i<count&&!changed;++i){
            u32 value;std::memcpy(&value,source+i*4,4);
            changed=epochWord(address+i)&&(!regs.xfValid[address+i]||regs.xf[address+i]!=value);
        }
        if(address+count>0x1000)for(u32 i=0;i<count;++i){
            u32 value;std::memcpy(&value,source+i*4,4);hashXf(regs,address+i,regs.xf[address+i],value);
        }
        std::memcpy(regs.xf.data()+address,source,count*4);
    }
    else for(u32 i=0;i<count;++i){u32 value;if(little){const auto* p=source+i*4;value=u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24);}else value=word(source+i*4);
        if(!geometry&&epochWord(address+i)&&(!regs.xfValid[address+i]||regs.xf[address+i]!=value))changed=true;
        hashXf(regs,address+i,regs.xf[address+i],value);regs.xf[address+i]=value;}
    if(markXfValid(regs,address,count))bankLog.grewValid=true;
    bankLog.store(bank_layout::xfAt+address,regs.xf.data()+address,count);
    if(changed){++regs.stateEpoch;if(address<=0x1008&&address+count>0x1008)++regs.vertexSetupEpoch;}
}
bool CommandProcessor::loadIndexedValues(u8 code,u16 packed,std::span<const u8> source,bool little){
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if(code!=GX_LOAD_INDX_A&&code!=GX_LOAD_INDX_B&&code!=GX_LOAD_INDX_C&&code!=GX_LOAD_INDX_D)return reject(CommandError::unsupported,0);
    const u32 count=(packed>>12)+1,address=packed&0xfff;
    if(address>=regs.xf.size()||count>regs.xf.size()-address)return reject(CommandError::unsupported,0);
    if(source.size()!=count*4)return reject(CommandError::arrayBounds,0);
    applyIndexedWords(address,count,source.data(),little);
    ++stats.commands;return true;
}
bool CommandProcessor::rebind(unsigned slot,std::uint64_t address,u32 size,bool little){
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    return applyBinding(slot,address,size,little,0);
}
bool CommandProcessor::loadIndexed(u8 code,u16 index,u16 packed) {
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if(code!=GX_LOAD_INDX_A&&code!=GX_LOAD_INDX_B&&code!=GX_LOAD_INDX_C&&code!=GX_LOAD_INDX_D)return reject(CommandError::unsupported,0);
    if(!applyIndexed(code,index,packed,0))return false;
    ++stats.commands;return true;
}
bool CommandProcessor::writeXf(u16 address,std::span<const u32> values) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if(values.empty()||address>=regs.xf.size()||values.size()>regs.xf.size()-address)return reject(CommandError::unsupported,0);
    if(materialRecording)[[unlikely]]recordMaterialXf(*this,address,values);
    if(address+values.size()<=0x78||(address>=0x400&&address+values.size()<=0x45a)||
       dusk::native::skin::inPaletteXf(address,std::uint32_t(values.size()))){
        // Per-shape position/normal matrices (and the skin palette of a merged
        // list, runtime/skin_palette.hpp): outside the state epoch and the
        // matrix index mirror, written for every shape of every frame.
        bankLog.store(bank_layout::xfAt+address,values.data(),std::uint32_t(values.size()));
        std::memcpy(regs.xf.data()+address,values.data(),values.size()*sizeof(u32));
        if(markXfValid(regs,address,values.size()))bankLog.grewValid=true;
        ++stats.commands;return true;
    }
    bool changed=false,moved=false;
    for(size_t i=0;i<values.size();++i){
        const u32 at=address+u32(i);
        const bool differs=!regs.xfValid[at]||regs.xf[at]!=values[i];
        if(differs){
            if(epochWord(at))changed=true;
            if(!regs.xfValid[at])bankLog.grewValid=true;
            if(regs.xf[at]!=values[i])moved=true;
        }
        hashXf(regs,at,regs.xf[at],values[i]);regs.xf[at]=values[i];regs.xfValid.set(at);
    }
    // The whole write as one run (a light, a texture matrix: several words
    // that change together).
    if(moved)bankLog.store(bank_layout::xfAt+address,values.data(),std::uint32_t(values.size()));
    if(changed){++regs.stateEpoch;if(address<=0x1008&&address+values.size()>0x1008)++regs.vertexSetupEpoch;}
    updateMatrixIndices(address,values.size());++stats.commands;return true;
}
unsigned CommandProcessor::collectChanges(const CommandRegisters& before,std::span<BankWord> out,bool& ok)const{
    ok=false;
    const auto& after=regs;
    if(active||failure!=CommandError::none||materialRecording||after.bpMask!=0xffffff)return 0;
    // A frame copy set up inside the screen is not a register a replay restores.
    if(std::memcmp(&before.copy,&after.copy,sizeof(before.copy))||before.vertexCacheEpoch!=after.vertexCacheEpoch||
       before.tlutOverflow!=after.tlutOverflow)return 0;
    unsigned count=0;bool fits=true;
    auto add=[&](BankWord::Kind kind,unsigned index,u32 value){
        if(count<out.size())out[count]={std::uint16_t(kind),std::uint16_t(index),value};else fits=false;
        ++count;
    };
    // A write only ever validates a word: one valid before and not now means
    // `before` is not a copy of this bank before plain writes.
    for(unsigned reg=0;reg<256;++reg){
        if(before.cpValid[reg]&&!after.cpValid[reg])return 0;
        if(after.cpValid[reg]&&!(before.cpValid[reg]&&before.cp[reg]==after.cp[reg]))add(BankWord::cp,reg,after.cp[reg]);
    }
    for(unsigned reg=0;reg<256;++reg){
        if(before.bpValid[reg]&&!after.bpValid[reg])return 0;
        if(reg!=0xfe&&after.bpValid[reg]&&!(before.bpValid[reg]&&before.bp[reg]==after.bp[reg]))add(BankWord::bp,reg,after.bp[reg]);
    }
    for(unsigned bit=0;bit<16;++bit){
        if(before.tevColorValid[bit]&&!after.tevColorValid[bit])return 0;
        const u32 value=after.tevColor[bit/8][bit%8];
        if(after.tevColorValid[bit]&&!(before.tevColorValid[bit]&&before.tevColor[bit/8][bit%8]==value))add(BankWord::tevColor,bit,value);
    }
    for(unsigned slot=0;slot<after.tluts.size();++slot){
        const auto& a=after.tluts[slot];const auto& b=before.tluts[slot];
        if(a.token==b.token&&a.offset==b.offset&&a.entries==b.entries)continue;
        add(BankWord::tlutToken,slot,a.token);add(BankWord::tlutOffset,slot,a.offset);add(BankWord::tlutEntries,slot,a.entries);
    }
    for(unsigned at=0;at<after.xf.size();++at){
        if(before.xfValid[at]&&!after.xfValid[at])return 0;
        if(after.xfValid[at]&&!(before.xfValid[at]&&before.xf[at]==after.xf[at]))add(BankWord::xf,at,after.xf[at]);
    }
    // CP 0x30/0x40 and XF 0x1018/0x1019 share the matrix index: the family
    // written last at the recording wins, so the index itself is kept.
    for(unsigned slot=0;slot<2;++slot)if(after.matrixIndexValid[slot])add(BankWord::matrixIndex,slot,after.matrixIndex[slot]);
    ok=fits;
    return count;
}
bool CommandProcessor::applyChanges(std::span<const BankWord> words){
    if(active||failure!=CommandError::none||materialRecording||regs.bpMask!=0xffffff)return false;
    for(std::size_t i=0;i<words.size();++i){
        const auto& w=words[i];const unsigned index=w.index;
        switch(BankWord::Kind(w.kind)){
        case BankWord::cp:
            if(index>255||!applyCp(u8(index),w.value,0))return false;
            break;
        case BankWord::bp:
            // The raw word only: colour latches and palette loads have their own entries.
            if(index>255)return false;
            if(!regs.bpValid[index]||regs.bp[index]!=w.value){
                if(!regs.bpValid[index])bankLog.grewValid=true;
                if(regs.bp[index]!=w.value)bankLog.store(bank_layout::bpAt+index,w.value);
                ++regs.stateEpoch;hashBp(regs,index,regs.bp[index],w.value);regs.bp[index]=w.value;regs.bpValid.set(index);
            }
            break;
        case BankWord::tevColor:
            if(index>15)return false;
            if(!regs.tevColorValid[index]||regs.tevColor[index/8][index%8]!=w.value){
                ++regs.stateEpoch;regs.tevColor[index/8][index%8]=w.value;regs.tevColorValid.set(index);
            }
            break;
        case BankWord::tlutToken:{
            if(index>=regs.tluts.size()||i+2>=words.size()||words[i+1].kind!=BankWord::tlutOffset||words[i+2].kind!=BankWord::tlutEntries||
               words[i+1].index!=index||words[i+2].index!=index)return false;
            ++regs.stateEpoch;setTlut(regs,index,{w.value,words[i+1].value,words[i+2].value});i+=2;
            break;}
        case BankWord::xf:{
            if(index>=regs.xf.size())return false;
            // One run of consecutive words inside one of the ranges writeXf
            // treats alike (matrix memory apart from the rest).
            const bool matrix=index<0x78||(index>=0x400&&index<0x45a);
            std::array<u32,16> values{};unsigned n=0;
            while(i+n<words.size()&&n<values.size()&&words[i+n].kind==BankWord::xf&&words[i+n].index==index+n&&
                  ((index+n)<0x78||((index+n)>=0x400&&(index+n)<0x45a))==matrix){values[n]=words[i+n].value;++n;}
            if(!writeXf(u16(index),{values.data(),n}))return false;
            i+=n-1;
            break;}
        case BankWord::matrixIndex:
            if(index>1)return false;
            regs.matrixIndex[index]=w.value;regs.matrixIndexValid.set(index);
            break;
        default:return false;
        }
    }
    return true;
}
CommandProcessor::ArrayBindings CommandProcessor::arrayBindings()const{
    static_assert(sizeof(bindings)==sizeof(ArrayBindings::bytes)&&alignof(Binding)>=alignof(ArrayBindings));
    ArrayBindings out;std::memcpy(out.bytes.data(),bindings.data(),sizeof(bindings));return out;
}
bool CommandProcessor::adopt(const CommandRegisters& bank,const ArrayBindings& arrays){
    // A J2D material recording (the material memo) targets the game thread's
    // processor only: that one may not take another bank in the middle of it.
    // Alpha 15 fix: the second-core worker's list processor refused here too
    // whenever the game thread was recording, and the queued shape or run was
    // then not drawn at all (2D and water blinking with the 2D passes queued;
    // owner dump alpha15/014: 7272 queued draws lost). The worker's processor
    // is never the recorder's target, so it no longer looks at the recording.
    if(active||(materialRecording&&recordingOn(*this)))return false;
    regs=bank;regs.stateHash.tracked=1;
    std::memcpy(bindings.data(),arrays.bytes.data(),sizeof(bindings));
    for(auto& entry:cache)entry.ready=false;
    normalAliasesValid=false;failure=CommandError::none;failureOffset=0;
    bankLog.lost=true;
    return true;
}
bool CommandProcessor::executeQueued(std::span<const u8> list,const CommandRegisters& bank,const ArrayBindings& arrays){
    if(active||(materialRecording&&recordingOn(*this)))return false;
    // GX primitive followed only by display-list alignment NOPs. It cannot
    // write the bank, change an array binding or merge with another primitive.
    if(!runDeferral&&!drawBarrier&&bank.stateHash.tracked==1&&list.size()>=3&&primitive(list[0]&0xf8)){
        const u8 opcode=list[0];
        const unsigned vat=opcode&7;
        auto& prepared=cache[vat];
        prepared.ready=false;
        if(prepare(vat,bank)){
            const u16 count=half(list.data()+1);
            const std::size_t end=3+std::size_t(count)*prepared.plan.packedStride();
            if(end<=list.size()&&list.size()-end<32&&std::all_of(list.begin()+end,list.end(),[](u8 b){return b==GX_NOP;})){
                dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::stream);
                failure=CommandError::none;failureOffset=0;
                // Indexed inputs still resolve their live bounds at the draw.
                if(prepared.indexed)adoptArrays(arrays);
                active=true;
                struct Guard {
                    bool& active;
                    bool& ready;
                    ~Guard(){active=false;ready=false;}
                } guard{active,prepared.ready};
                const bool drawn=draw(list.subspan(3,end-3),{},opcode,count,0,0,bank);
                if(drawn)stats.commands+=1+list.size()-end;
                // An ordinary call on this scratch processor must check its
                // own bank's format again even after a refused sink.
                return drawn;
            }
        }
    }
    return adopt(bank,arrays)&&execute(list);
}
bool CommandProcessor::submitDescriptors(u32 low,u32 high,bool normal,bool nbt,u8 type) {
    noteForeignCommand(*this); // also changes the normal aliases outside the bank
    low=(low&~(3u<<11))|((normal||nbt?u32(type):0)<<11);
    u32 colors=bool(low&(3u<<13))+bool(low&(3u<<15)),textures=0;
    for(unsigned i=0;i<8;++i)textures+=bool(high&(3u<<(2*i)));
    const u32 spec=colors|((nbt?2u:normal?1u:0u)<<2)|(textures<<4);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if((!regs.cpValid[0x50]||regs.cp[0x50]!=low)&&!writeCp(0x50,low))return false;
    if((!regs.cpValid[0x60]||regs.cp[0x60]!=high)&&!writeCp(0x60,high))return false;
    if((!regs.xfValid[0x1008]||regs.xf[0x1008]!=spec)&&!writeXf(0x1008,{&spec,1}))return false;
    hasNormal=normal;hasNBT=nbt;normalType=type;normalAliasesValid=true;return true;
}
bool CommandProcessor::clearDescriptors() {
    return submitDescriptors(1u<<9,0,false,false,0);
}
bool CommandProcessor::queryDescriptor(unsigned attribute,unsigned& type) const {
    type=0;
    if(failure!=CommandError::none||active||
       (attribute>GX_VA_TEX7&&attribute!=GX_VA_NBT))return false;
    const unsigned reg=attribute>=GX_VA_TEX0&&attribute<=GX_VA_TEX7?0x60:0x50;
    if(!regs.cpValid[reg])return false;
    if(attribute==GX_VA_NRM||attribute==GX_VA_NBT){
        const unsigned descriptor=(regs.cp[0x50]>>11)&3;
        if(normalAliasesValid){
            if(attribute==GX_VA_NRM?hasNormal:hasNBT)type=descriptor;
        }else if(descriptor){
            if(!regs.xfValid[0x1008])return false;
            const bool nbt=((regs.xf[0x1008]>>2)&3)==2;
            if(attribute==GX_VA_NBT?nbt:!nbt)type=descriptor;
        }
    }else if(attribute<GX_VA_POS)type=(regs.cp[reg]>>attribute)&1;
    else {
        const unsigned shift=reg==0x50?9+2*(attribute-GX_VA_POS):2*(attribute-GX_VA_TEX0);
        type=(regs.cp[reg]>>shift)&3;
    }
    return true;
}
bool CommandProcessor::setDescriptor(unsigned attribute,unsigned type) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if((attribute>GX_VA_TEX7&&attribute!=GX_VA_NBT)||type>3||(attribute<9&&type>1))return reject(CommandError::format,0);
    u32 low=regs.cp[0x50],high=regs.cp[0x60];
    bool normal=hasNormal,nbt=hasNBT;u8 normalFormat=normalType;
    if(!normalAliasesValid){
        normalFormat=(low>>11)&3;
        nbt=normalFormat&&regs.xfValid[0x1008]&&((regs.xf[0x1008]>>2)&3)==2;
        normal=normalFormat&&!nbt;
    }
    if(attribute==GX_VA_NRM||attribute==GX_VA_NBT){
        (attribute==GX_VA_NRM?normal:nbt)=type!=0;
        if(type)normalFormat=type;
    }else{
        u32* target=&low;unsigned shift=attribute,bits=1;
        if(attribute>=GX_VA_POS){bits=2;shift=9+2*(attribute-GX_VA_POS);}
        if(attribute>=GX_VA_TEX0){target=&high;shift=2*(attribute-GX_VA_TEX0);}
        const u32 mask=((1u<<bits)-1)<<shift;
        *target=(*target&~mask)|(type<<shift);
    }
    return submitDescriptors(low,high,normal,nbt,normalFormat);
}
bool CommandProcessor::applyBinding(unsigned slot,std::uint64_t address,u32 size,bool little,size_t offset) {
    if(slot>=bindings.size())return reject(CommandError::format,offset);
    const auto assign=[&](const Binding& value){
        auto& current=bindings[slot];
        if(current.handle!=value.handle||current.offset!=value.offset||current.size!=value.size||
           current.little!=value.little||current.liveAddress!=value.liveAddress)++regs.vertexSetupEpoch;
        current=value;return true;
    };
    if(!address&&!size)return assign({});
    MemoryHandle handle;u32 at;
    if(!memory.find(address,size,handle,at)){
        if(!address||!size||address>UINTPTR_MAX||size>UINTPTR_MAX-address||
           !liveArrayResolver||!liveArrayResolver(address,size))return reject(CommandError::unregisteredMemory,offset);
        return assign({0,0,size,little,address});
    }
    return assign({handle,at,size,little});
}
bool CommandProcessor::resolveBinding(const Binding& binding,std::span<const u8>& bytes) const {
    if(!binding.liveAddress)return memory.resolve(binding.handle,binding.offset,binding.size,bytes);
    bytes={};
    if(!liveArrayResolver||!liveArrayResolver(binding.liveAddress,binding.size))return false;
    bytes={reinterpret_cast<const u8*>(static_cast<std::uintptr_t>(binding.liveAddress)),binding.size};
    return true;
}
bool CommandProcessor::bindArray(unsigned slot,const void* data,u32 size,u8 stride,bool little) {
    noteForeignCommand(*this);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if(!applyBinding(slot,reinterpret_cast<std::uintptr_t>(data),size,little,0))return false;
    ++stats.commands;return writeCp(0xb0+slot,stride);
}
bool CommandProcessor::execute(std::span<const u8> bytes) {
    dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::stream);
    noteForeignCommand(*this,MaterialMemoRefusal::execute);
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    active=true;struct Guard{bool& flag;~Guard(){flag=false;}}guard{active};
    size_t cursor=0;
    while(cursor<bytes.size()) {
        const size_t start=cursor;const u8 code=bytes[cursor];const auto rest=bytes.subspan(cursor);
        size_t size=1;auto need=[&](size_t n){return rest.size()>=n||reject(CommandError::truncated,start);};
        if(code==GX_NOP){}
        else if(code==GX_CMD_INVL_VC){++regs.vertexCacheEpoch;}
        else if(code==GX_LOAD_CP_REG){
            if(!need(6))return false;
            size=6;const u8 reg=rest[1];const u32 value=word(rest.data()+2);
            if(!applyCp(reg,value,start))return false;
        } else if(code==GX_LOAD_BP_REG){
            if(!need(5))return false;
            size=5;const u32 value=word(rest.data()+1),reg=value>>24,data=value&0xffffff;
            if(reg==0x52)return reject(CommandError::unsupported,start);
            applyBp(reg,data);
        } else if(code==GX_LOAD_XF_REG){
            if(!need(5))return false;
            const u32 count=u32(half(rest.data()+1))+1,address=half(rest.data()+3);
            if(address>=regs.xf.size()||count>regs.xf.size()-address)return reject(CommandError::unsupported,start);
            size=5+size_t(count)*4;if(!need(size))return false;
            bool changed=false,moved=false;
            for(u32 i=0;i<count;++i){
                const u32 at=address+i,value=word(rest.data()+5+i*4);
                if(!regs.xfValid[at]||regs.xf[at]!=value){
                    if(epochWord(at))changed=true;
                    if(!regs.xfValid[at])bankLog.grewValid=true;
                    if(regs.xf[at]!=value)moved=true;
                }
                hashXf(regs,at,regs.xf[at],value);regs.xf[at]=value;regs.xfValid.set(at);
            }
            if(moved)bankLog.store(bank_layout::xfAt+address,regs.xf.data()+address,count);
            if(changed){++regs.stateEpoch;if(address<=0x1008&&address+count>0x1008)++regs.vertexSetupEpoch;}
            updateMatrixIndices(address,count);
        } else if(code==GX_LOAD_INDX_A||code==GX_LOAD_INDX_B||code==GX_LOAD_INDX_C||code==GX_LOAD_INDX_D){
            if(!need(5))return false;
            size=5;
            if(!applyIndexed(code,half(rest.data()+1),half(rest.data()+3),start))return false;
        } else if(code==GX_AURORA){
            if(!need(3))return false;
            const u16 sub=half(rest.data()+1);
            if(sub>=GX_AURORA_LOAD_ARRAYBASE&&sub<GX_AURORA_LOAD_ARRAYBASE+16){
                if(!need(16))return false;
                size=16;const auto address=(std::uint64_t(word(rest.data()+3))<<32)|word(rest.data()+7);const u32 length=word(rest.data()+11);const unsigned slot=sub-GX_AURORA_LOAD_ARRAYBASE;
                if(rest[15]>1)return reject(CommandError::format,start);
                if(!applyBinding(slot,address,length,rest[15]!=0,start))return false;
            } else if(sub==GX_AURORA_LOAD_COPY_SRC){
                if(!need(19))return false;
                size=19;
                if(!applyCopySource(true,word(rest.data()+3),word(rest.data()+7),word(rest.data()+11),word(rest.data()+15),start))return false;
            } else if(sub==GX_AURORA_LOAD_COPY_DST){
                if(!need(16))return false;
                size=16;
                if(rest[15]>1)return reject(CommandError::format,start);
                if(!applyCopyDestination(true,word(rest.data()+3),word(rest.data()+7),word(rest.data()+11),rest[15]!=0,start))return false;
            } else if(sub==GX_AURORA_LOAD_COPY_DEST){
                if(!need(11))return false;
                size=11;const u64 address=(u64(word(rest.data()+3))<<32)|word(rest.data()+7);
                if(!address||address%32)return reject(CommandError::format,start);
                regs.copy.textureDestination=address;regs.copy.textureDestinationReady=true;
            } else if(sub==GX_AURORA_DRAW_INDEXED){
                if(!need(10))return false;
                const u8 op=rest[3];const u16 count=half(rest.data()+4);const u32 indexCount=word(rest.data()+6);
                if((op&0xf8)!=GX_TRIANGLES||indexCount%3)return reject(CommandError::format,start);
                if(!prepare(op&7))return reject(CommandError::format,start);
                const std::uint64_t total=10+std::uint64_t(indexCount)*2+std::uint64_t(count)*cache[op&7].plan.packedStride();
                if(total>rest.size())return reject(CommandError::truncated,start);
                size=static_cast<size_t>(total);
                const size_t indexBytes=size_t(indexCount)*2;
                if(!draw(rest.subspan(10+indexBytes,size-10-indexBytes),rest.subspan(10,indexBytes),op,count,indexCount,start,regs))return false;
            } else return reject(CommandError::unsupported,start);
        } else if(primitive(code&0xf8)){
            if(!need(3))return false;
            if(!prepare(code&7))return reject(CommandError::format,start);
            const u16 count=half(rest.data()+1);size=3+size_t(count)*cache[code&7].plan.packedStride();if(!need(size))return false;
            if(runDeferral){
                // This packet and the well-formed primitive packets right
                // behind it, of any vertex format.
                size_t run=size;unsigned packets=1;bool fixed=!cache[code&7].indexed||arraysImmutable(code&7);
                while(run+3<=rest.size()&&primitive(rest[run]&0xf8)){
                    const unsigned vat=rest[run]&7;
                    if(!prepare(vat))break;
                    const size_t bytes=3+size_t(half(rest.data()+run+1))*cache[vat].plan.packedStride();
                    if(bytes>rest.size()-run)break;
                    if(fixed&&cache[vat].indexed&&!arraysImmutable(vat))fixed=false;
                    run+=bytes;++packets;
                }
                if(runDeferral(rest.first(run),fixed,runDeferralContext)){cursor+=run;stats.commands+=packets;continue;}
            }
            size_t batched=0;
            if(!drawBatch(rest,code&7,start,batched))return false;
            if(batched)size=batched;
            else if(!draw(rest.subspan(3,size-3),{},code,count,0,start,regs))return false;
        } else return reject(CommandError::unsupported,start);
        cursor+=size;++stats.commands;
    }
    return true;
}
namespace {
// Display list templates (Alpha 14). A list is parsed once into a template:
// two words per command, `kind | register << 8 | offset << 16` and an extra
// word. On every later call the bytes that decide the parse (opcodes,
// registers, XF counts and addresses, array base slots and byte orders, the
// NOP padding) are compared with the template; where they all match,
// execute() would take the list command for command as the template says, so
// the template runs the same functions (applyBp, applyCp, the XF store,
// applyBinding) with the values read from the list itself. A list J3D
// rewrites with new values every picture (the differential lists, written
// into alternating buffers) keeps its template: no checksum, no compile, no
// interpreter. Before Alpha 14 such a list was checksummed, compiled and
// interpreted on every call until it was left to the interpreter (Azahar
// village, per picture: 103 replays, 121 compilations, 176 interpreted).
// Lists holding anything else (draws, indexed loads, copies, other Aurora
// commands, malformed bytes) stay with the interpreter.
enum : u32 {tBp=1,tCp,tXf,tInvalidate,tBase,tNops};
struct ListTemplate {
    const u8* data{};u32 size{},first{},count{},commands{};
    u8 state{}; // 0 empty, 1 template, 2 interpreter-only
    u8 heat{};
};
constexpr unsigned compiledCapacity=1024,compiledProbe=4,compiledPoolWords=96*1024;
std::array<ListTemplate,compiledCapacity> compiledLists{};
std::array<u32,compiledPoolWords> compiledPool;
unsigned compiledUsed{};
bool compiledEnabled=true,compiledVerify=false;
std::uint64_t compiledHits{},compiledCompiles{},compiledInterpreted{},compiledMismatches{};
std::uint64_t paintFastRuns{},paintHeldLists{},paintHeldMismatches{};
bool cpLoadable(u32 reg){ // CommandProcessor::applyCp
    return (reg==0x30||reg==0x40||reg==0x50||reg==0x60||(reg>=0x70&&reg<0x98)||(reg>=0xb0&&reg<0xc0))&&
           !(reg>=0x78&&reg<0x80)&&!(reg>=0x88&&reg<0x90);
}
// The template of `bytes` at the end of the pool. False: interpreter-only
// (or no room: the caller resets the pool).
bool buildTemplate(std::span<const u8> bytes,ListTemplate& out,bool& full){
    full=false;
    const std::size_t size=bytes.size();
    if(2*size>compiledPoolWords-compiledUsed){full=true;return false;}
    u32* const base=compiledPool.data()+compiledUsed;u32 n=0,commands=0;
    auto entry=[&](u32 kind,u32 reg,std::size_t offset,u32 extra){base[2*n]=kind|(reg<<8)|(u32(offset)<<16);base[2*n+1]=extra;++n;};
    long nops=-1;
    for(std::size_t cursor=0;cursor<size;){
        const u8* at=bytes.data()+cursor;const u8 code=*at;const std::size_t rest=size-cursor;std::size_t step=1;
        if(code==GX_NOP){if(nops<0)nops=long(cursor);++cursor;++commands;continue;}
        if(nops>=0){entry(tNops,0,std::size_t(nops),u32(cursor-std::size_t(nops)));nops=-1;}
        if(code==GX_CMD_INVL_VC)entry(tInvalidate,0,cursor,0);
        else if(code==GX_LOAD_CP_REG){
            if(rest<6||!cpLoadable(at[1]))return false;
            step=6;entry(tCp,at[1],cursor,0);
        }else if(code==GX_LOAD_BP_REG){
            if(rest<5||at[1]==0x52)return false;
            step=5;entry(tBp,at[1],cursor,0);
        }else if(code==GX_LOAD_XF_REG){
            if(rest<5)return false;
            const u32 count=u32(half(at+1))+1,address=half(at+3);
            if(address>=0x1058||count>0x1058-address)return false;
            step=5+std::size_t(count)*4;if(rest<step)return false;
            entry(tXf,0,cursor,word(at+1));
        }else if(code==GX_AURORA){
            if(rest<16)return false;
            const u16 sub=half(at+1);
            if(sub<GX_AURORA_LOAD_ARRAYBASE||sub>=GX_AURORA_LOAD_ARRAYBASE+16||at[15]>1)return false;
            step=16;entry(tBase,sub-GX_AURORA_LOAD_ARRAYBASE,cursor,at[15]);
        }else return false;
        cursor+=step;++commands;
    }
    if(nops>=0)entry(tNops,0,std::size_t(nops),u32(size-std::size_t(nops)));
    out.data=bytes.data();out.size=u32(size);out.first=compiledUsed;out.count=n;out.commands=commands;
    compiledUsed+=2*n;
    return true;
}
// The bytes still parse as the template says.
bool templateMatches(const ListTemplate& t,const u8* bytes){
    const u32* e=compiledPool.data()+t.first;
    for(u32 i=0;i<t.count;++i,e+=2){
        const u32 head=e[0],reg=(head>>8)&0xff;const u8* at=bytes+(head>>16);
        switch(head&0xff){
        case tBp:if(at[0]!=GX_LOAD_BP_REG||at[1]!=reg)return false;break;
        case tCp:if(at[0]!=GX_LOAD_CP_REG||at[1]!=reg)return false;break;
        case tXf:if(at[0]!=GX_LOAD_XF_REG||word(at+1)!=e[1])return false;break;
        case tInvalidate:if(at[0]!=GX_CMD_INVL_VC)return false;break;
        case tBase:if(at[0]!=GX_AURORA||half(at+1)!=GX_AURORA_LOAD_ARRAYBASE+reg||at[15]!=e[1])return false;break;
        default:for(u32 k=0;k<e[1];++k)if(at[k]!=GX_NOP)return false;break;
        }
    }
    return true;
}
}
void compiledListStatistics(std::uint64_t& hits,std::uint64_t& compiles,std::uint64_t& interpreted,std::uint64_t& mismatches){
    hits=compiledHits;compiles=compiledCompiles;interpreted=compiledInterpreted;mismatches=compiledMismatches;
}
void paintListStatistics(std::uint64_t& fast,std::uint64_t& held,std::uint64_t& heldMismatches){
    fast=paintFastRuns;held=paintHeldLists;heldMismatches=paintHeldMismatches;
}
void setCompiledLists(bool enabled,bool verify){
    compiledEnabled=enabled;compiledVerify=verify;compiledUsed=0;for(auto& entry:compiledLists)entry={};
}
// The J2D material memo still replays words in the compiled format.
void CommandProcessor::runCompiled(const u32* w,unsigned count){
    const u32* const end=w+count;
    while(w<end){
        const u32 head=*w++,reg=(head>>8)&0xff;
        switch(head&0xff){
        case opBp:{
            const u32 merged=(regs.bp[reg]&w[1])|w[0];w+=2;
            if(!regs.bpValid[reg]||regs.bp[reg]!=merged){
                if(!regs.bpValid[reg])bankLog.grewValid=true;
                if(regs.bp[reg]!=merged)bankLog.store(bank_layout::bpAt+reg,merged);
                ++regs.stateEpoch;hashBp(regs,reg,regs.bp[reg],merged);regs.bp[reg]=merged;regs.bpValid.set(reg);
            }
            break;}
        case opBpSpecial:regs.bpMask=w[1];applyBp(u8(reg),w[0]);w+=2;break;
        case opCp:applyCp(u8(reg),*w++,0);break;
        case opXf:{
            const u32 n=head>>8,address=*w++;bool changed=false,moved=false;
            for(u32 i=0;i<n;++i){
                const u32 at=address+i,value=w[i];
                if(!regs.xfValid[at]||regs.xf[at]!=value){
                    if(epochWord(at))changed=true;
                    if(!regs.xfValid[at])bankLog.grewValid=true;
                    if(regs.xf[at]!=value)moved=true;
                }
                hashXf(regs,at,regs.xf[at],value);regs.xf[at]=value;regs.xfValid.set(at);
            }
            if(moved)bankLog.store(bank_layout::xfAt+address,w,n);
            w+=n;
            if(changed){++regs.stateEpoch;if(address<=0x1008&&address+n>0x1008)++regs.vertexSetupEpoch;}
            updateMatrixIndices(address,n);
            break;}
        case opInvalidate:++regs.vertexCacheEpoch;break;
        default:return;
        }
    }
}
// execute(), command by command, for a list whose template matches.
bool CommandProcessor::runTemplate(const u32* e,unsigned count,const u8* bytes){
    for(const u32* const end=e+2*count;e!=end;e+=2){
        const u32 head=e[0],reg=(head>>8)&0xff,offset=head>>16;const u8* at=bytes+offset;
        switch(head&0xff){
        case tBp:applyBp(u8(reg),word(at+1)&0xffffff);break;
        case tCp:if(!applyCp(u8(reg),word(at+2),offset))return false;break;
        case tXf:{
            const u32 count=(e[1]>>16)+1,address=e[1]&0xffff;const u8* values=at+5;
            bool changed=false,moved=false;
            for(u32 i=0;i<count;++i){
                const u32 a=address+i,value=word(values+i*4);
                if(!regs.xfValid[a]||regs.xf[a]!=value){
                    if(epochWord(a))changed=true;
                    if(!regs.xfValid[a])bankLog.grewValid=true;
                    if(regs.xf[a]!=value)moved=true;
                }
                hashXf(regs,a,regs.xf[a],value);regs.xf[a]=value;regs.xfValid.set(a);
            }
            if(moved)bankLog.store(bank_layout::xfAt+address,regs.xf.data()+address,count);
            if(changed){++regs.stateEpoch;if(address<=0x1008&&address+count>0x1008)++regs.vertexSetupEpoch;}
            updateMatrixIndices(address,count);
            break;}
        case tInvalidate:++regs.vertexCacheEpoch;break;
        case tBase:
            if(!applyBinding(reg,(std::uint64_t(word(at+3))<<32)|word(at+7),word(at+11),at[15]!=0,offset))return false;
            break;
        default:break; // padding
        }
    }
    return true;
}
// packetPaint (perf_features.hpp). Display lists are half of what a J3D
// material packet costs the game thread (Alpha 21 emulator attribution: the
// material list, the vertex setup list and the differential list, about
// 13,000 instructions each), and in most of their commands the register
// already holds the value: the previous material set it the same way, or the
// previous shape's differential list did. runTemplate answers those through
// applyBp/applyCp, which find it out after the call; here the same
// conditions are tested first. Everything else goes through the same
// functions, in the same order, with the same values.
namespace {
// BP registers applyBp treats specially: the mask register, the TEV colour
// latches (0xe0-0xe7) and the TLUT load trigger (0x65).
constexpr std::array<bool,256> plainBpTable=[]{
    std::array<bool,256> t{};
    for(unsigned i=0;i<256;++i)t[i]=!(i==0xfe||i==0x65||(i>=0xe0&&i<=0xe7));
    return t;
}();
}
// applyBp(reg,value) for a plain register under the full mask whose value
// changes or is not valid yet: the branch applyBp takes there, without the
// branches it cannot take.
inline void CommandProcessor::applyPlainBpChange(u32 reg,u32 value){
    const u32 previous=regs.bp[reg];
    ++regs.stateEpoch;
    if(!regs.bpValid[reg]){bankLog.grewValid=true;regs.bpValid.set(reg);}
    if(previous!=value){
        hashBp(regs,reg,previous,value);
        bankLog.store(bank_layout::bpAt+reg,value);
        regs.bp[reg]=value;
    }
}
bool CommandProcessor::runTemplateFast(const u32* e,unsigned count,const u8* bytes){
    for(const u32* const end=e+2*count;e!=end;e+=2){
        const u32 head=e[0],reg=(head>>8)&0xff,offset=head>>16;const u8* at=bytes+offset;
        switch(head&0xff){
        case tBp:{
            const u32 value=word(at+1)&0xffffff;
            if(regs.bpMask==0xffffff&&plainBpTable[reg]){
                // applyBp: a valid register equal to the value returns at once.
                if(regs.bp[reg]!=value||!regs.bpValid[reg])applyPlainBpChange(reg,value);
            }else applyBp(u8(reg),value);
            break;}
        case tCp:{
            const u32 value=word(at+2);
            // applyCp of a valid equal value stores nothing, except the
            // matrix index mirror (0x30/0x40) and the normal aliases (0x50).
            if(regs.cp[reg]==value&&regs.cpValid[reg]&&reg!=0x30&&reg!=0x40&&reg!=0x50)break;
            if(!applyCp(u8(reg),value,offset))return false;
            break;}
        case tXf:{
            const u32 count=(e[1]>>16)+1,address=e[1]&0xffff;const u8* values=at+5;
            u32 i=0;
            while(i<count&&regs.xfValid[address+i]&&regs.xf[address+i]==word(values+i*4))++i;
            if(i==count){updateMatrixIndices(address,count);break;} // every word held: no store, no log, no epoch
            bool changed=false,moved=false;
            for(i=0;i<count;++i){
                const u32 a=address+i,value=word(values+i*4);
                if(!regs.xfValid[a]||regs.xf[a]!=value){
                    if(epochWord(a))changed=true;
                    if(!regs.xfValid[a])bankLog.grewValid=true;
                    if(regs.xf[a]!=value)moved=true;
                }
                hashXf(regs,a,regs.xf[a],value);regs.xf[a]=value;regs.xfValid.set(a);
            }
            if(moved)bankLog.store(bank_layout::xfAt+address,regs.xf.data()+address,count);
            if(changed){++regs.stateEpoch;if(address<=0x1008&&address+count>0x1008)++regs.vertexSetupEpoch;}
            updateMatrixIndices(address,count);
            break;}
        case tInvalidate:++regs.vertexCacheEpoch;break;
        case tBase:
            if(!applyBinding(reg,(std::uint64_t(word(at+3))<<32)|word(at+7),word(at+11),at[15]!=0,offset))return false;
            break;
        default:break; // padding
        }
    }
    return true;
}
// The binding applyBinding would make for these arguments, compared with the
// slot's own (the liveness proof included: a dead array is not held).
bool CommandProcessor::bindingHeld(unsigned slot,std::uint64_t address,u32 size,bool little)const{
    if(slot>=bindings.size())return false;
    Binding value{};
    if(address||size){
        MemoryHandle handle;u32 at;
        if(memory.find(address,size,handle,at))value={handle,at,size,little};
        else{
            if(!address||!size||address>UINTPTR_MAX||size>UINTPTR_MAX-address||
               !liveArrayResolver||!liveArrayResolver(address,size))return false;
            value={0,0,size,little,address};
        }
    }
    const auto& current=bindings[slot];
    return current.handle==value.handle&&current.offset==value.offset&&current.size==value.size&&
           current.little==value.little&&current.liveAddress==value.liveAddress;
}
// listHeld's test: every command a CP load, XF load, array base or NOP the
// processor accepts, and every value already in place. `commands` counts
// them as a template does (each NOP byte is one).
bool CommandProcessor::listHeldCheck(std::span<const u8> bytes,unsigned& commands,bool& normals)const{
    commands=0;normals=false;
    if(active||failure!=CommandError::none||bytes.empty())return false;
    for(std::size_t cursor=0;cursor<bytes.size();++commands){
        const u8* at=bytes.data()+cursor;const std::size_t rest=bytes.size()-cursor;
        switch(at[0]){
        case GX_NOP:++cursor;break;
        case GX_LOAD_CP_REG:{
            if(rest<6||!cpLoadable(at[1]))return false;
            const u32 reg=at[1],value=word(at+2);
            if(!regs.cpValid[reg]||regs.cp[reg]!=value)return false;
            if(reg==0x30||reg==0x40){
                const unsigned slot=(reg-0x30)/16;
                if(!regs.matrixIndexValid[slot]||regs.matrixIndex[slot]!=value)return false;
            }
            normals|=reg==0x50;cursor+=6;break;}
        case GX_LOAD_XF_REG:{
            if(rest<5)return false;
            const u32 count=u32(half(at+1))+1,address=half(at+3);
            if(address>=0x1058||count>0x1058-address||rest<5+std::size_t(count)*4)return false;
            for(u32 i=0;i<count;++i)if(!regs.xfValid[address+i]||regs.xf[address+i]!=word(at+5+i*4))return false;
            for(unsigned slot=0;slot<2;++slot)if(address<=0x1018+slot&&address+count>0x1018+slot&&
               (!regs.matrixIndexValid[slot]||regs.matrixIndex[slot]!=regs.xf[0x1018+slot]))return false;
            normals|=address<=0x1008&&address+count>0x1008;cursor+=5+std::size_t(count)*4;break;}
        case GX_AURORA:{
            if(rest<16)return false;
            const u16 sub=half(at+1);
            if(sub<GX_AURORA_LOAD_ARRAYBASE||sub>=GX_AURORA_LOAD_ARRAYBASE+16||at[15]>1||
               !bindingHeld(sub-GX_AURORA_LOAD_ARRAYBASE,(std::uint64_t(word(at+3))<<32)|word(at+7),word(at+11),at[15]!=0))return false;
            cursor+=16;break;}
        default:return false;
        }
    }
    return true;
}
bool CommandProcessor::listHeld(std::span<const u8> bytes){
    unsigned commands;bool normals;
    if(!listHeldCheck(bytes,commands,normals))return false;
    if(compiledVerify){
        // Run it after all: the bank and the bindings must come out unchanged.
        static CommandRegisters before;
        before=regs;const auto savedBindings=bindings;
        const bool ran=executeList(bytes,false);
        if(!ran||std::memcmp(&before,&regs,sizeof(regs))||std::memcmp(&savedBindings,&bindings,sizeof(bindings)))++paintHeldMismatches;
        ++paintHeldLists;
        return ran;
    }
    noteForeignCommand(*this,MaterialMemoRefusal::list);
    if(normals)normalAliasesValid=false;
    stats.commands+=commands;++paintHeldLists;
    return true;
}
bool CommandProcessor::executeList(std::span<const u8> bytes,bool fast) {
    noteForeignCommand(*this,MaterialMemoRefusal::list);
    // A pending BP mask from outside the list is runtime state a template
    // run starts from as execute() does; still kept with the interpreter, as
    // before (it is rare).
    if(!compiledEnabled||bytes.size()<16||bytes.size()>65536||active||failure!=CommandError::none||regs.bpMask!=0xffffff)
        return execute(bytes);
    static const auto lookupIndex=dusk::native::registerGxNamedProfile("list.lookup");dusk::native::GxNamedProfileSpan lookupSpan(lookupIndex);
    // Lists are 32-byte aligned and sized: take the well-mixed high bits.
    const unsigned home=unsigned((((std::uint32_t(reinterpret_cast<std::uintptr_t>(bytes.data()))>>5)^(std::uint32_t(bytes.size())<<11))*2654435761u)>>22)%compiledCapacity;
    ListTemplate* entry=nullptr;
    for(unsigned n=0;n<compiledProbe&&!entry;++n){
        auto& candidate=compiledLists[(home+n)%compiledCapacity];
        if(candidate.state&&candidate.data==bytes.data()&&candidate.size==bytes.size())entry=&candidate;
    }
    if(entry&&entry->state==2){
        // Re-examine an interpreter-only address now and then: its storage may
        // hold a different list by now.
        if(++entry->heat==255)entry->state=0;
        lookupSpan.stop();
        ++compiledInterpreted;return execute(bytes);
    }
    auto run=[&](const ListTemplate& t)->bool{
        if(compiledVerify){
            static CommandRegisters before,compiled;
            before=regs;const auto savedStats=stats;const auto savedBindings=bindings;
            if(fast)++paintFastRuns;
            const bool ran=fast?runTemplateFast(compiledPool.data()+t.first,t.count,bytes.data()):
                runTemplate(compiledPool.data()+t.first,t.count,bytes.data());
            compiled=regs;regs=before;stats=savedStats;bindings=savedBindings;clearFailure();
            const bool okay=execute(bytes);
            if(ran!=okay||std::memcmp(&compiled,&regs,sizeof(regs))){++compiledMismatches;entry->state=2;entry->heat=0;}
            return okay;
        }
        dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::stream);
        static const auto runIndex=dusk::native::registerGxNamedProfile("list.run");dusk::native::GxNamedProfileScope runScope(runIndex);
        if(fast){
            ++paintFastRuns;
            if(!runTemplateFast(compiledPool.data()+t.first,t.count,bytes.data()))return false;
        }else if(!runTemplate(compiledPool.data()+t.first,t.count,bytes.data()))return false;
        stats.commands+=t.commands;return true;
    };
    if(entry&&templateMatches(*entry,bytes.data())){++compiledHits;lookupSpan.stop();return run(*entry);}
    lookupSpan.stop();
    if(!entry){
        for(unsigned n=0;n<compiledProbe&&!entry;++n){auto& candidate=compiledLists[(home+n)%compiledCapacity];if(!candidate.state)entry=&candidate;}
        static unsigned victim{};
        if(!entry)entry=&compiledLists[(home+(victim++%compiledProbe))%compiledCapacity];
    }
    // A list whose structure changed is built again; a new one in a place
    // that fits takes the old one's pool words.
    const bool had=entry->state==1;const unsigned oldFirst=entry->first,oldCount=entry->count;
    ListTemplate built;bool full=false;
    if(!buildTemplate(bytes,built,full)&&full){
        // Pool exhausted: forget every template; each one is built again on its next call.
        compiledUsed=0;for(auto& item:compiledLists)item={};
        (void)buildTemplate(bytes,built,full);
        *entry={};
    }
    if(!built.data){*entry={};entry->data=bytes.data();entry->size=u32(bytes.size());entry->state=2;++compiledInterpreted;return execute(bytes);}
    if(had&&built.count<=oldCount&&oldFirst+2*oldCount<=built.first){
        std::memmove(compiledPool.data()+oldFirst,compiledPool.data()+built.first,2*built.count*sizeof(u32));
        compiledUsed=built.first;built.first=oldFirst;
    }
    *entry=built;entry->state=1;entry->heat=0;++compiledCompiles;
    return run(*entry);
}
bool CommandProcessor::frame(std::span<const u8> prefix,size_t& required) {
    required=1;
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if(prefix.empty())return false;
    const u8 code=prefix[0];
    if(code==GX_NOP||code==GX_CMD_INVL_VC)return true;
    if(code==GX_LOAD_CP_REG){required=6;return true;}
    if(code==GX_LOAD_BP_REG||code==GX_LOAD_INDX_A||code==GX_LOAD_INDX_B||
       code==GX_LOAD_INDX_C||code==GX_LOAD_INDX_D){required=5;return true;}
    if(code==GX_LOAD_XF_REG){
        required=5;if(prefix.size()<required)return false;
        required+=4*(u32(half(prefix.data()+1))+1);return true;
    }
    u8 op=code;u16 count;u32 indices=0;size_t header=3;
    if(code==GX_AURORA){
        required=3;if(prefix.size()<required)return false;
        const u16 sub=half(prefix.data()+1);
        if(sub>=GX_AURORA_LOAD_ARRAYBASE&&sub<GX_AURORA_LOAD_ARRAYBASE+16){required=16;return true;}
        if(sub==GX_AURORA_LOAD_COPY_SRC){required=19;return true;}
        if(sub==GX_AURORA_LOAD_COPY_DST){required=16;return true;}
        if(sub==GX_AURORA_LOAD_COPY_DEST){required=11;return true;}
        if(sub!=GX_AURORA_DRAW_INDEXED)return reject(CommandError::unsupported,0);
        required=10;if(prefix.size()<required)return false;
        header=10;op=prefix[3];count=half(prefix.data()+4);indices=word(prefix.data()+6);
        if((op&0xf8)!=GX_TRIANGLES||indices%3)return reject(CommandError::format,0);
    }else if(primitive(code&0xf8)){
        required=3;if(prefix.size()<required)return false;
        count=half(prefix.data()+1);
    }else return reject(CommandError::unsupported,0);
    if(!prepare(op&7))return reject(CommandError::format,0);
    const std::uint64_t size=header+std::uint64_t(indices)*2+std::uint64_t(count)*cache[op&7].plan.packedStride();
    if(size>std::numeric_limits<size_t>::max())return reject(CommandError::format,0);
    required=static_cast<size_t>(size);return true;
}
struct MaterialMemoAccess {
    static void run(CommandProcessor& p,const u32* words,unsigned count,unsigned commands){
        p.runCompiled(words,count);p.stats.commands+=commands;
    }
    static CommandRegisters& registers(CommandProcessor& p){return p.regs;}
};
CommandRegisters::StateHash computeStateHash(const CommandRegisters& r){
    CommandRegisters::StateHash h{};
    auto term=[](u32& component,u32 seed,u32 value){component^=hashMix(value^seed)^hashMix(seed);};
    for(unsigned reg=0;reg<256;++reg)switch(bpLanes[reg]){
    case 1:term(h.bp,hashSeed(laneBp,reg),r.bp[reg]);break;
    case 2:term(h.bp,hashSeed(laneBp,reg),r.bp[reg]>>20);break;
    case 3:term(h.texture,hashSeed(laneTexture,reg),r.bp[reg]);break;
    default:break;
    }
    for(unsigned slot=0;slot<r.tluts.size();++slot){
        term(h.texture,hashSeed(laneTexture,0x100+2*slot),r.tluts[slot].token);
        term(h.texture,hashSeed(laneTexture,0x101+2*slot),tlutWord(r.tluts[slot]));
    }
    term(h.texture,hashSeed(laneTexture,0x140),r.tlutOverflow);
    for(unsigned reg=0;reg<256;++reg)if(cpHashed(reg))term(h.cp,hashSeed(laneCp,reg),r.cp[reg]);
    for(unsigned at=0x1000;at<0x1058;++at)if(xfHashed(at))term(h.xf,hashSeed(laneXf,at),r.xf[at]);
    return h;
}
bool stateHashComponents(const CommandRegisters& r,std::uint32_t& bp,std::uint32_t& texture,std::uint32_t& cp,std::uint32_t& xf){
    if(!stateHashOn||!r.stateHash.tracked)return false;
    ++stateHashStats.keys;
    auto h=r.stateHash;
    // Every write that changes a hashed word also moves the state epoch, so
    // one check per epoch sees every change.
    if(stateHashVerify&&(stateHashCheckedBank!=&r||stateHashCheckedEpoch!=r.stateEpoch)){
        stateHashCheckedBank=&r;stateHashCheckedEpoch=r.stateEpoch;++stateHashStats.checks;
        const auto full=computeStateHash(r);
        if(full.bp!=h.bp||full.texture!=h.texture||full.cp!=h.cp||full.xf!=h.xf){
            if(!stateHashStats.firstComponent)
                stateHashStats.firstComponent=full.bp!=h.bp?'B':full.texture!=h.texture?'T':full.cp!=h.cp?'C':'X';
            ++stateHashStats.mismatches;h=full; // the words themselves decide
        }
    }
    bp=h.bp;texture=h.texture;cp=h.cp;xf=h.xf;return true;
}
void setStateHash(bool enabled,bool verify){stateHashOn=enabled;stateHashVerify=enabled&&verify;stateHashCheckedBank=nullptr;}
void stateHashStatistics(StateHashStatistics& out,bool reset){out=stateHashStats;if(reset)stateHashStats={};}
void typedWriteStatistics(TypedWriteStatistics& out,bool reset){out=typedStats;if(reset)typedStats={};}
namespace {
// Material memo storage, allocated on first use from the heap (the static
// size of the executable is bounded by the New 3DS application region).
// A slot holds up to two variants of one material: the same material drawn
// after two different neighbours keeps both. A variant's pool region holds
// its recorded words, then one record per texture matrix source: object
// address (two words), size and checksum.
constexpr unsigned materialSlots=1024,materialProbe=4,materialPoolWords=96*1024,
    materialScratchWords=4096,materialTextureWords=1024,materialHeatLimit=32,
    materialSourceWords=5,materialGiveUpAfter=3;
struct MaterialVariant {
    std::uint32_t sumA{},sumB{},first{};
    std::uint32_t materialA{}; // the material bytes alone (footprint-miss diagnostics)
    std::uint16_t wordsA{},wordsB{},capacity{},commandsA{},commandsB{};
    std::uint8_t state{}; // 0 empty, 1 recorded, 2 refused (the original setters run)
    std::uint8_t sources{};
};
struct MaterialSlot {
    const void* material{};
    std::array<MaterialVariant,2> variants{};
    std::uint8_t heat{},skips{},older{};
    // Refused recordings and refused lookups in a row (a replay resets the
    // count); at materialGiveUpAfter the material goes to the setters before
    // any checksum, for givenUpCalls calls.
    std::uint8_t refusals{},givenUpCalls{};bool givenUp{};
};
struct MaterialStore {
    std::array<MaterialSlot,materialSlots> slots{};
    std::array<u32,materialPoolWords> pool{};
    std::array<u32,materialScratchWords> scratch{};
    std::array<u32,materialTextureWords> textures{};
    unsigned used{},victim{};
};
struct MaterialBanks {CommandRegisters before,actual;};
// Pool regions stay with their slot when it changes owner or a variant is
// refused, so the next recording there reuses them; only a pool reset (or a
// processor change) frees space. Without this every eviction leaked its
// region and busy menus reset the whole pool again and again.
void reuseSlot(MaterialSlot& slot,const void* material){
    const auto old=slot.variants;slot={};slot.material=material;
    for(unsigned v=0;v<2;++v){slot.variants[v].first=old[v].first;slot.variants[v].capacity=old[v].capacity;}
}
void refuseVariant(MaterialVariant& variant,std::uint32_t sumA,std::uint32_t sumB){
    const auto first=variant.first;const auto capacity=variant.capacity;
    variant={};variant.first=first;variant.capacity=capacity;variant.sumA=sumA;variant.sumB=sumB;variant.state=2;
}
MaterialStore* materialStore{};
MaterialBanks* materialBanks{};
bool materialEnabled=true,materialVerify=false,materialStoreFailed=false;
CommandProcessor* materialProcessor{};
MaterialMemoStatistics materialStats{};
const void* recordingMaterial{};MaterialMemoSum recordingSum{};std::uint32_t recordingMaterialA{};
bool materialStorage(){
    if(!materialStore&&!materialStoreFailed){materialStore=new(std::nothrow)MaterialStore{};materialStoreFailed=!materialStore;}
    if(materialVerify&&!materialBanks&&!(materialBanks=new(std::nothrow)MaterialBanks{}))materialVerify=false;
    return materialStore;
}
void clearMaterialVariants(){
    if(!materialStore)return;
    for(auto& slot:materialStore->slots)slot.variants={};
    materialStore->used=0;
}
unsigned materialHome(const void* material){
    const auto address=std::uint32_t(reinterpret_cast<std::uintptr_t>(material));
    return unsigned(((address>>3)*2654435761u)>>22)%materialSlots;
}
MaterialSlot* findMaterialSlot(const void* material){
    const unsigned home=materialHome(material);
    for(unsigned n=0;n<materialProbe;++n){
        auto& slot=materialStore->slots[(home+n)%materialSlots];
        if(slot.material==material)return &slot;
    }
    return nullptr;
}
void mixWord(MaterialMemoSum& sum,u32 w){sum.a=(sum.a^w)*16777619u;sum.b=((sum.b<<5)|(sum.b>>27))+w;}
// Every register the J2D material setters read before a write: generation
// mode (channel/texgen counts and cull mode keep its other bits), PE control
// (blend mode and dither), the XF channel and texgen state they skip when
// unchanged, and the texgen matrix selectors they rebuild (CP and XF
// aliases). The TEV, indirect and alpha-compare setters write fixed fields
// through the BP mask and read nothing, so the state they overwrite (the
// previous pane's colours and stages) need not match: a recorded masked
// write merges with the current bank exactly as the setter's write does.
void mixMaterialFootprint(const CommandRegisters& r,MaterialMemoSum& sum,unsigned texGens){
    u32 bits=0;unsigned count=0;
    auto valid=[&](bool v){bits=(bits<<1)|u32(v);if(++count==32){mixWord(sum,bits);bits=0;count=0;}};
    auto bp=[&](unsigned reg){mixWord(sum,r.bp[reg]);valid(r.bpValid[reg]);};
    auto xf=[&](unsigned at){mixWord(sum,r.xf[at]);valid(r.xfValid[at]);};
    bp(0);bp(0x41);
    for(const unsigned at:{0x1009u,0x100cu,0x100du,0x100eu,0x100fu,0x1010u,0x1011u,0x103fu})xf(at);
    for(unsigned i=0;i<texGens;++i){xf(0x1040+i);xf(0x1050+i);}
    for(unsigned slot=0;slot<2&&texGens>slot*4;++slot){
        mixWord(sum,r.matrixIndex[slot]);valid(r.matrixIndexValid[slot]);
        mixWord(sum,r.cp[0x30+16*slot]);valid(r.cpValid[0x30+16*slot]);xf(0x1018+slot);
    }
    mixWord(sum,bits);mixWord(sum,count);
}
// The texture matrix objects a variant's recording read still hold the
// bytes they had (their addresses are part of the material bytes).
bool materialSourcesUnchanged(const MaterialVariant& v){
    const u32* s=materialStore->pool.data()+v.first+v.wordsA+v.wordsB;
    for(unsigned i=0;i<v.sources;++i,s+=materialSourceWords){
        const auto address=std::uint64_t(s[0])|(std::uint64_t(s[1])<<32);
        MaterialMemoSum sum;materialMemoChecksum(sum,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),s[2]);
        if(sum.a!=s[3]||sum.b!=s[4])return false;
    }
    return true;
}
void beginMaterialRecording(const CommandRegisters& r,const void* material,MaterialMemoSum sum,std::uint32_t materialA){
    auto& store=*materialStore;
    recorder={};recorder.target=&r;recorder.words=store.scratch.data();recorder.capacity=materialScratchWords;
    if(materialVerify&&materialBanks){
        recorder.textureWords=store.textures.data();recorder.textureCapacity=materialTextureWords;
        materialBanks->before=r;
    }
    recordingMaterial=material;recordingSum=sum;recordingMaterialA=materialA;materialRecording=true;
}
// First difference between the original setters' bank and the replay's.
MaterialMemoVerdict compareMaterialBanks(const CommandRegisters& o,const CommandRegisters& r){
    MaterialMemoVerdict v{};
    auto differ=[&](char space,unsigned index,u32 original,u32 replayed){
        v.mismatch=true;v.reason="register bank";v.space=space;v.index=index;v.original=original;v.replayed=replayed;return v;};
    for(unsigned i=0;i<256;++i)if(o.cpValid[i]!=r.cpValid[i]||o.cp[i]!=r.cp[i])return differ('C',i,o.cp[i],r.cp[i]);
    for(unsigned i=0;i<256;++i)if(o.bpValid[i]!=r.bpValid[i]||o.bp[i]!=r.bp[i])return differ('B',i,o.bp[i],r.bp[i]);
    for(unsigned i=0;i<o.xf.size();++i)if(o.xfValid[i]!=r.xfValid[i]||o.xf[i]!=r.xf[i])return differ('X',i,o.xf[i],r.xf[i]);
    for(unsigned i=0;i<16;++i)if(o.tevColorValid[i]!=r.tevColorValid[i]||o.tevColor[i/8][i%8]!=r.tevColor[i/8][i%8])
        return differ('T',i,o.tevColor[i/8][i%8],r.tevColor[i/8][i%8]);
    for(unsigned i=0;i<o.tluts.size();++i){const auto& a=o.tluts[i];const auto& b=r.tluts[i];
        if(a.token!=b.token||a.offset!=b.offset||a.entries!=b.entries)return differ('E',0x100+i,a.token,b.token);}
    if(o.tlutOverflow!=r.tlutOverflow)return differ('E',1,o.tlutOverflow,r.tlutOverflow);
    for(unsigned i=0;i<2;++i)if(o.matrixIndexValid[i]!=r.matrixIndexValid[i]||o.matrixIndex[i]!=r.matrixIndex[i])
        return differ('E',0x10+i,o.matrixIndex[i],r.matrixIndex[i]);
    if(o.bpMask!=r.bpMask)return differ('E',2,o.bpMask,r.bpMask);
    if(o.stateEpoch!=r.stateEpoch)return differ('E',3,o.stateEpoch,r.stateEpoch);
    if(o.vertexSetupEpoch!=r.vertexSetupEpoch)return differ('E',4,o.vertexSetupEpoch,r.vertexSetupEpoch);
    if(o.vertexCacheEpoch!=r.vertexCacheEpoch)return differ('E',5,u32(o.vertexCacheEpoch),u32(r.vertexCacheEpoch));
    const auto& x=o.stateHash;const auto& y=r.stateHash;
    if(x.bp!=y.bp||x.texture!=y.texture||x.cp!=y.cp||x.xf!=y.xf)return differ('E',6,x.bp^x.texture^x.cp^x.xf,y.bp^y.texture^y.cp^y.xf);
    return v;
}
// Replays the given segments around the recorded texture writes on the bank
// the original setters started from, and compares with what they produced.
MaterialMemoVerdict verifyMaterialReplay(CommandProcessor& p,const u32* first,unsigned firstWords,const u32* second,unsigned secondWords){
    auto& banks=*materialBanks;auto& regs=MaterialMemoAccess::registers(p);
    banks.actual=regs;regs=banks.before;
    MaterialMemoAccess::run(p,first,firstWords,0);
    MaterialMemoAccess::run(p,recorder.textureWords,recorder.textureUsed,0);
    MaterialMemoAccess::run(p,second,secondWords,0);
    const auto verdict=compareMaterialBanks(banks.actual,regs);
    regs=banks.actual;
    return verdict;
}
}
void materialMemoStatistics(MaterialMemoStatistics& out,bool reset){
    out=materialStats;out.poolWords=materialStore?materialStore->used:0;out.storageFailed=materialStoreFailed;
    if(reset)materialStats={};
}
void setMaterialMemo(bool enabled,bool verify){
    if(materialRecording)return;
    materialEnabled=enabled;materialVerify=enabled&&verify;
    if(materialStore)for(auto& slot:materialStore->slots)slot={};
    clearMaterialVariants();
}
bool materialMemoEnabled(){return materialEnabled;}
bool materialMemoVerifying(){return materialVerify;}
bool materialMemoRecording(){return materialRecording;}
void materialMemoChecksum(MaterialMemoSum& sum,const void* data,std::size_t size){
    const auto* bytes=static_cast<const u8*>(data);std::size_t i=0;
    for(;i+4<=size;i+=4){u32 w;std::memcpy(&w,bytes+i,4);mixWord(sum,w);}
    for(;i<size;++i)mixWord(sum,bytes[i]);
}
void materialMemoBypass(){++materialStats.calls;++materialStats.bypassed;}
bool materialMemoGivenUp(const void* material){
    if(!materialEnabled||!materialStore||!material||materialRecording)return false;
    auto* slot=findMaterialSlot(material);
    if(!slot||!slot->givenUp)return false;
    if(++slot->givenUpCalls==255){
        // Examined again: one more refusal gives up for another round.
        slot->givenUp=false;slot->givenUpCalls=0;slot->refusals=materialGiveUpAfter-1;
    }
    ++materialStats.calls;++materialStats.givenUp;return true;
}
void materialMemoSource(const CommandRegisters& r,const void* object,std::size_t size,const void* matrix){
    if(!materialRecording||&r!=recorder.target)return;
    if(!object||!matrix||!size||size>0xffff){refuseRecording(MaterialMemoRefusal::matrix);return;}
    recorder.pendingObject=object;recorder.pendingSize=size;recorder.pendingMatrix=matrix;
}
MaterialMemoTicket materialMemoLookup(const CommandRegisters& r,const void* material,MaterialMemoSum sum,unsigned tevStages,unsigned texGens){
    ++materialStats.calls;
    MaterialMemoTicket ticket{};
    // A pending BP mask from outside is runtime state the recorded writes
    // do not model (as for compiled lists).
    if(!materialEnabled||!material||materialRecording||r.bpMask!=0xffffff||tevStages>16||texGens>8||!materialStorage()){
        ++materialStats.bypassed;return ticket;
    }
    if(materialProcessor&&&materialProcessor->registers()!=&r){
        // Another processor behind the stream: the recorded writes were for the old one.
        for(auto& slot:materialStore->slots)slot={};
        clearMaterialVariants();materialProcessor=nullptr;
    }
    const std::uint32_t materialA=sum.a;
    mixMaterialFootprint(r,sum,texGens);
    auto& store=*materialStore;
    const unsigned home=materialHome(material);
    MaterialSlot* slot=nullptr;MaterialSlot* empty=nullptr;
    for(unsigned n=0;n<materialProbe&&!slot;++n){
        auto& candidate=store.slots[(home+n)%materialSlots];
        if(candidate.material==material)slot=&candidate;
        else if(!candidate.material&&!empty)empty=&candidate;
    }
    if(!slot){
        slot=empty?empty:&store.slots[(home+(store.victim++%materialProbe))%materialSlots];
        reuseSlot(*slot,material);
    }
    ticket.slot=std::uint16_t(slot-store.slots.data());
    int stale=-1; // a variant whose texture matrix objects changed: recorded again in place
    for(unsigned v=0;v<2;++v){
        const auto& variant=slot->variants[v];
        if(!variant.state||variant.sumA!=sum.a||variant.sumB!=sum.b)continue;
        ticket.variant=std::uint8_t(v);
        if(variant.state==2){
            // A stable key that stays refused pays the checksum and probe on
            // every call: it counts toward giving the material up as well.
            ++materialStats.refused;
            if(++slot->refusals>=materialGiveUpAfter){slot->givenUp=true;slot->givenUpCalls=0;}
            return ticket;
        }
        if(variant.sources&&!materialSourcesUnchanged(variant)){++materialStats.sourceChanges;stale=int(v);break;}
        if(slot->heat)--slot->heat;
        slot->older=std::uint8_t(v^1);
        if(materialVerify){
            beginMaterialRecording(r,material,sum,materialA);
            ticket.action=MaterialMemoAction::record;ticket.verify=ticket.stored=true;return ticket;
        }
        slot->refusals=0; // refusals in a row only
        ++materialStats.replays;ticket.action=MaterialMemoAction::replay;return ticket;
    }
    if(slot->heat>=materialHeatLimit){
        // Changes on nearly every call (animated colours): left to the
        // setters, and examined again now and then.
        if(++slot->skips==255){slot->heat=0;slot->skips=0;}
        ++materialStats.volatileSkips;return ticket;
    }
    for(const auto& variant:slot->variants)
        if(variant.state==1&&variant.materialA==materialA&&stale<0){++materialStats.footprintMisses;break;}
    if(slot->variants[0].state||slot->variants[1].state)slot->heat=std::uint8_t(std::min(255u,slot->heat+4u));
    const unsigned v=stale>=0?unsigned(stale):!slot->variants[0].state?0:!slot->variants[1].state?1:slot->older;
    ticket.variant=std::uint8_t(v);slot->older=std::uint8_t(v^1);
    beginMaterialRecording(r,material,sum,materialA);
    ticket.action=MaterialMemoAction::record;ticket.verify=materialVerify;++materialStats.records;
    return ticket;
}
void materialMemoReplay(const MaterialMemoTicket& ticket,unsigned segment){
    if(ticket.action!=MaterialMemoAction::replay||!materialStore||!materialProcessor)return;
    const auto& v=materialStore->slots[ticket.slot].variants[ticket.variant];
    const u32* words=materialStore->pool.data()+v.first;
    if(!segment)MaterialMemoAccess::run(*materialProcessor,words,v.wordsA,v.commandsA);
    else MaterialMemoAccess::run(*materialProcessor,words+v.wordsA,v.wordsB,v.commandsB);
}
void materialMemoSplit(){
    if(!materialRecording)return;
    auto& r=recorder;
    if(r.splitDone||r.mask!=0xffffff||r.pendingMasks)refuseRecording(MaterialMemoRefusal::split);
    r.split=r.used;r.splitCommands=r.commands;r.splitDone=true;
}
MaterialMemoVerdict materialMemoFinish(const MaterialMemoTicket& ticket){
    MaterialMemoVerdict verdict{};
    if(ticket.action!=MaterialMemoAction::record||!materialRecording)return verdict;
    materialRecording=false;
    const auto& rec=recorder;auto& store=*materialStore;
    auto& slot=store.slots[ticket.slot];auto& variant=slot.variants[ticket.variant];
    if(slot.material!=recordingMaterial)return verdict; // forgotten meanwhile
    auto refusal=rec.refusal;
    if(refusal==MaterialMemoRefusal::none){
        if(!rec.splitDone)refusal=MaterialMemoRefusal::split;
        else if(rec.mask!=0xffffff||rec.pendingMasks)refusal=MaterialMemoRefusal::mask;
        else if(!rec.processor||(materialProcessor&&materialProcessor!=rec.processor))refusal=MaterialMemoRefusal::processor;
    }
    const bool complete=refusal==MaterialMemoRefusal::none;
    // A fresh recording the recorder refused (foreign command, unsourced
    // matrix) is a refusal, not a mismatch; a stored entry whose setters now
    // do something a replay cannot is one.
    if(ticket.verify&&materialBanks&&rec.processor&&(complete||ticket.stored)){
        ++materialStats.verified;
        const u32* first=rec.words;unsigned firstWords=rec.split,secondWords=rec.used-rec.split;
        if(ticket.stored){first=store.pool.data()+variant.first;firstWords=variant.wordsA;secondWords=variant.wordsB;}
        if(!complete){verdict.mismatch=true;verdict.reason="recording refused";}
        else{
            verdict=verifyMaterialReplay(*rec.processor,first,firstWords,first+firstWords,secondWords);
            if(!verdict.mismatch&&ticket.stored&&(firstWords!=rec.split||secondWords!=rec.used-rec.split||
               variant.commandsA!=rec.splitCommands||variant.commandsB!=rec.commands-rec.splitCommands||
               variant.sources!=rec.sourceCount||std::memcmp(first,rec.words,rec.used*sizeof(u32)))){
                verdict.mismatch=true;verdict.reason="recorded writes differ";
            }
        }
        if(verdict.mismatch){
            ++materialStats.mismatches;
            refuseVariant(variant,recordingSum.a,recordingSum.b);
            return verdict;
        }
    }
    if(ticket.stored){++materialStats.replays;return verdict;}
    if(!complete){
        refuseVariant(variant,recordingSum.a,recordingSum.b);
        ++materialStats.refused;++materialStats.refusedBy[unsigned(refusal)];
        if(++slot.refusals>=materialGiveUpAfter){slot.givenUp=true;slot.givenUpCalls=0;}
        return verdict;
    }
    const unsigned words=rec.used,total=words+rec.sourceCount*materialSourceWords;
    if(variant.capacity<total){
        if(total>materialPoolWords-store.used){
            // Pool exhausted: forget every recording; each records again on its next call.
            clearMaterialVariants();++materialStats.poolResets;
        }
        variant.first=store.used;variant.capacity=std::uint16_t(total);store.used+=total;
    }
    u32* out=store.pool.data()+variant.first;
    std::memcpy(out,rec.words,words*sizeof(u32));out+=words;
    for(unsigned i=0;i<rec.sourceCount;++i,out+=materialSourceWords){
        const auto& s=rec.sources[i];const auto address=std::uint64_t(reinterpret_cast<std::uintptr_t>(s.object));
        out[0]=u32(address);out[1]=u32(address>>32);out[2]=s.size;out[3]=s.sumA;out[4]=s.sumB;
    }
    variant.sumA=recordingSum.a;variant.sumB=recordingSum.b;variant.materialA=recordingMaterialA;
    variant.wordsA=std::uint16_t(rec.split);variant.wordsB=std::uint16_t(words-rec.split);
    variant.commandsA=std::uint16_t(rec.splitCommands);variant.commandsB=std::uint16_t(rec.commands-rec.splitCommands);
    variant.sources=std::uint8_t(rec.sourceCount);
    variant.state=1;materialProcessor=rec.processor;++materialStats.stores;slot.refusals=0;
    return verdict;
}
void materialMemoForget(const void* material){
    if(!materialStore||!material||materialRecording)return;
    if(auto* slot=findMaterialSlot(material))reuseSlot(*slot,nullptr);
}
}
