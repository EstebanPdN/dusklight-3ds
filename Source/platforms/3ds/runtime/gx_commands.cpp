#include "gx_commands.hpp"
#include "gx_profile.hpp"
#include "gx_compact_cache.hpp"
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXAurora.h>
#include <bit>
#include <cstring>
#include <limits>
#include <new>

namespace dusk::native::gx {
namespace {
u32 word(const u8* p){return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3];}
u16 half(const u8* p){return (u16(p[0])<<8)|p[1];}
u32 field(u32 value,unsigned shift,unsigned bits){return (value>>shift)&((1u<<bits)-1);}
bool primitive(u8 code){return code==GX_QUADS||code==GX_TRIANGLES||code==GX_TRIANGLESTRIP||code==GX_TRIANGLEFAN||code==GX_LINES||code==GX_LINESTRIP||code==GX_POINTS;}
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
    *free={handle,bytes};return true;
}
bool CommandMemory::remove(MemoryHandle handle){
    auto& region=regions[handle%capacity];
    if(!handle||region.handle!=handle)return false;
    region={};return true;
}
bool CommandMemory::find(std::uint64_t address,u32 size,MemoryHandle& handle,u32& offset)const {
    handle=0;offset=0;if(!address||!size)return false;
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
    if(active){reject(CommandError::reentrant,0);return;}
    auto* mem=&memory;auto callback=sink;auto* context=sinkContext;auto resolver=liveArrayResolver;
    // In-place reset avoids materializing the register bank on a 3DS stack.
    const auto scratch=batchScratch;auto* topology=batchCache;
    this->~CommandProcessor();new(this)CommandProcessor(*mem);setSink(callback,context);setLiveArrayResolver(resolver);setPrimitiveBatchScratch(scratch);setPrimitiveBatchCache(topology);
}
bool CommandProcessor::prepare(unsigned vat) {
    auto& prepared=cache[vat];if(prepared.ready)return true;
    if(!regs.cpValid[0x50]||!regs.cpValid[0x60])return false;
    const std::array<u32,5> signature{regs.cp[0x50],regs.cp[0x60],regs.cp[0x70+vat],regs.cp[0x80+vat],regs.cp[0x90+vat]};
    if(prepared.created&&prepared.signature==signature){prepared.ready=true;return true;}
    std::array<GXVtxDescList,GX_VA_TEX7+2> desc{};
    for(unsigned i=0;i<=GX_VA_TEX7;++i)desc[i].attr=static_cast<GXAttr>(i);
    const u32 low=regs.cp[0x50],high=regs.cp[0x60];
    for(unsigned i=0;i<9;++i)desc[i].type=static_cast<GXAttrType>(field(low,i,1));
    for(unsigned i=0;i<4;++i)desc[GX_VA_POS+i].type=static_cast<GXAttrType>(field(low,9+i*2,2));
    for(unsigned i=0;i<8;++i)desc[GX_VA_TEX0+i].type=static_cast<GXAttrType>(field(high,i*2,2));
    desc.back().attr=GX_VA_NULL;
    std::array<GXVtxAttrFmtList,13> fmt{};
    for(unsigned i=0;i<12;++i)fmt[i].attr=static_cast<GXAttr>(GX_VA_POS+i);
    fmt.back().attr=GX_VA_NULL;
    auto set=[&](unsigned attr,u32 count,u32 type,u32 frac){auto& f=fmt[attr-GX_VA_POS];f.cnt=static_cast<GXCompCnt>(count);f.type=static_cast<GXCompType>(type);f.frac=frac;};
    const u32 a=regs.cp[0x70+vat],b=regs.cp[0x80+vat],c=regs.cp[0x90+vat];
    set(GX_VA_POS,field(a,0,1),field(a,1,3),field(a,4,5));
    set(GX_VA_NRM,field(a,31,1)?GX_NRM_NBT3:field(a,9,1)?GX_NRM_NBT:GX_NRM_XYZ,field(a,10,3),0);
    set(GX_VA_CLR0,field(a,13,1),field(a,14,3),0);set(GX_VA_CLR1,field(a,17,1),field(a,18,3),0);
    set(GX_VA_TEX0,field(a,21,1),field(a,22,3),field(a,25,5));
    for(unsigned i=0;i<3;++i)set(GX_VA_TEX1+i,field(b,i*9,1),field(b,i*9+1,3),field(b,i*9+4,5));
    set(GX_VA_TEX4,field(b,27,1),field(b,28,3),field(c,0,5));
    for(unsigned i=0;i<3;++i)set(GX_VA_TEX5+i,field(c,5+i*9,1),field(c,6+i*9,3),field(c,9+i*9,5));
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)if(desc[attr].type!=GX_NONE){
        const bool first=attr<=GX_VA_TEX0,second=attr>=GX_VA_TEX1&&attr<=GX_VA_TEX4,third=attr>=GX_VA_TEX4;
        if((first&&!regs.cpValid[0x70+vat])||(second&&!regs.cpValid[0x80+vat])||(third&&!regs.cpValid[0x90+vat]))return false;
    }
    const auto layout=aurora::gx::dl::makeLayout(desc.data(),fmt.data());
    if(!layout||!planVertexDecode(*layout,fmt,prepared.plan))return false;
    prepared.layout=*layout;prepared.signature=signature;prepared.created=true;prepared.ready=true;++stats.plans;return true;
}
bool CommandProcessor::draw(std::span<const u8> vertices,std::span<const u8> indices,u8 opcode,u16 count,u32 indexCount,size_t offset) {
    static const auto prepIndex=dusk::native::registerGxNamedProfile("proc.drawPrep");dusk::native::GxNamedProfileSpan prepSpan(prepIndex);
    if(!count)return indexCount?reject(CommandError::invalidIndex,offset):true;
    if(!sink)return reject(CommandError::missingSink,offset);
    const unsigned vat=opcode&7;VertexArrays arrays{};
    const auto& layout=cache[vat].layout;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)if(layout.attrs[attr].type==GX_INDEX8||layout.attrs[attr].type==GX_INDEX16){
        const unsigned slot=attr-GX_VA_POS;const auto& bind=bindings[slot];std::span<const u8> bytes;
        if(!resolveBinding(bind,bytes))return reject(CommandError::staleMemory,offset);
        arrays[attr]={bytes,regs.cp[0xb0+slot]&255,bind.little};
    }
    if(!trustedIndices)for(u32 i=0;i<indexCount;++i){u16 v;std::memcpy(&v,indices.data()+size_t(i)*2,2);if(v>=count)return reject(CommandError::invalidIndex,offset);}
    const CommandDraw packet{static_cast<GXPrimitive>(opcode&0xf8),count,indexCount,vertices,indices,cache[vat].plan,arrays,regs};
    ++staticGeometryHint.batch;staticGeometryHint.trustedIndices=trustedIndices;
    prepSpan.stop();
    if(!sink(packet,sinkContext))return reject(CommandError::sinkRejected,offset);
    if(failure!=CommandError::none)return false;
    ++stats.draws;return true;
}
bool CommandProcessor::drawBatch(std::span<const u8> bytes,u8 vat,size_t offset,size_t& consumed) {
    consumed=0;
    if(batchScratch.vertices.empty()||batchScratch.indices.empty()||!batchScratch.maxVertices)return true;
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
            const bool drawn=draw(saved.vertices,saved.indices,GX_TRIANGLES|vat,saved.vertexCount,indexCount,offset);
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
        GX_TRIANGLES|vat,submittedVertices,indexCount,offset))return false;
    consumed=cursor;stats.commands+=packets-1;stats.mergedPrimitives+=packets;
    stats.reusedVertices+=vertexCount-submittedVertices;
    return true;
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
    if(reg==0xfe)regs.bpMask=value;
    else {
        const u32 merged=(regs.bp[reg]&~regs.bpMask)|(value&regs.bpMask);
        regs.bp[reg]=merged;regs.bpValid.set(reg);regs.bpMask=0xffffff;
        if(reg==0x65){
            const u32 offset=merged&1023,entries=((merged>>10)&2047)*16;
            const u32 begin=offset*512,end=begin+entries*2;
            CommandRegisters::TlutLoad* available=nullptr;
            for(auto& load:regs.tluts){
                const u32 oldBegin=load.offset*512,oldEnd=oldBegin+load.entries*2;
                if(load.entries&&begin<oldEnd&&oldBegin<end)load={};
                if(!load.entries&&!available)available=&load;
            }
            if(available)*available={regs.bpValid[0x64]?regs.bp[0x64]:0,offset,
                !(merged>>21)&&entries&&entries<=16384&&end<=0x80000?entries:0};
            else regs.tlutOverflow=true;
        }
        if(reg>=0xe0&&reg<=0xe7){
            // Match original BP dispatch: merge with the last raw value first,
            // then select the destination bank using the effective selector.
            const unsigned bank=(merged>>23)&1,slot=reg-0xe0;
            regs.tevColor[bank][slot]=merged;regs.tevColorValid.set(bank*8+slot);
        }
    }
}
bool CommandProcessor::writeCp(u8 reg,u32 value) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none||!applyCp(reg,value,0))return false;
    ++stats.commands;return true;
}
bool CommandProcessor::writeBp(u8 reg,u32 value) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    // BP52 starts a real EFB transfer. Until a native transfer sink exists,
    // reject it rather than latch the trigger and silently omit the copy.
    if(reg==0x52)return reject(CommandError::unsupported,0);
    applyBp(reg,value);++stats.commands;return true;
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
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none||!applyCopySource(texture,left,top,width,height,0))return false;
    ++stats.commands;return true;
}
bool CommandProcessor::setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none||!applyCopyDestination(texture,width,height,format,mipmap,0))return false;
    ++stats.commands;return true;
}
bool CommandProcessor::setCopyControl(bool texture,u32 mask,u32 value) {
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
    if(bind.little&&std::endian::native==std::endian::little)std::memcpy(regs.xf.data()+address,array.data()+at,count*4);
    else for(u32 i=0;i<count;++i){u32 value;if(bind.little){const auto* p=array.data()+at+i*4;value=u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24);}else value=word(array.data()+at+i*4);
        regs.xf[address+i]=value;}
    for(u32 i=0;i<count;++i)regs.xfValid.set(address+i);
    return true;
}
bool CommandProcessor::loadIndexed(u8 code,u16 index,u16 packed) {
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
    for(size_t i=0;i<values.size();++i){regs.xf[address+i]=values[i];regs.xfValid.set(address+i);}
    updateMatrixIndices(address,values.size());++stats.commands;return true;
}
bool CommandProcessor::submitDescriptors(u32 low,u32 high,bool normal,bool nbt,u8 type) {
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
    if(!address&&!size){bindings[slot]={};return true;}
    MemoryHandle handle;u32 at;
    if(!memory.find(address,size,handle,at)){
        if(!address||!size||address>UINTPTR_MAX||size>UINTPTR_MAX-address||
           !liveArrayResolver||!liveArrayResolver(address,size))return reject(CommandError::unregisteredMemory,offset);
        bindings[slot]={0,0,size,little,address};return true;
    }
    bindings[slot]={handle,at,size,little};return true;
}
bool CommandProcessor::resolveBinding(const Binding& binding,std::span<const u8>& bytes) const {
    if(!binding.liveAddress)return memory.resolve(binding.handle,binding.offset,binding.size,bytes);
    bytes={};
    if(!liveArrayResolver||!liveArrayResolver(binding.liveAddress,binding.size))return false;
    bytes={reinterpret_cast<const u8*>(static_cast<std::uintptr_t>(binding.liveAddress)),binding.size};
    return true;
}
bool CommandProcessor::bindArray(unsigned slot,const void* data,u32 size,u8 stride,bool little) {
    if(active)return reject(CommandError::reentrant,0);
    if(failure!=CommandError::none)return false;
    if(!applyBinding(slot,reinterpret_cast<std::uintptr_t>(data),size,little,0))return false;
    ++stats.commands;return writeCp(0xb0+slot,stride);
}
bool CommandProcessor::execute(std::span<const u8> bytes) {
    dusk::native::GxProfileScope profile(dusk::native::GxProfilePart::stream);
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
            for(u32 i=0;i<count;++i){regs.xf[address+i]=word(rest.data()+5+i*4);regs.xfValid.set(address+i);}
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
                if(!draw(rest.subspan(10+indexBytes,size-10-indexBytes),rest.subspan(10,indexBytes),op,count,indexCount,start))return false;
            } else return reject(CommandError::unsupported,start);
        } else if(primitive(code&0xf8)){
            if(!need(3))return false;
            if(!prepare(code&7))return reject(CommandError::format,start);
            const u16 count=half(rest.data()+1);size=3+size_t(count)*cache[code&7].plan.packedStride();if(!need(size))return false;
            size_t batched=0;
            if(!drawBatch(rest,code&7,start,batched))return false;
            if(batched)size=batched;
            else if(!draw(rest.subspan(3,size-3),{},code,count,0,start))return false;
        } else return reject(CommandError::unsupported,start);
        cursor+=size;++stats.commands;
    }
    return true;
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
}
