#include "gx_stream.hpp"
#include "gx_profile.hpp"
#include "perf_features.hpp"
#include <algorithm>
#include <cstring>

namespace dusk::native::gx {
namespace {
// Commands a native packet capture takes from write(): register loads only.
bool capturableOpcode(u8 code){
    return code==GX_NOP||code==GX_CMD_INVL_VC||code==GX_LOAD_CP_REG||code==GX_LOAD_BP_REG||code==GX_LOAD_XF_REG;
}
// The CP registers CommandProcessor::applyCp takes.
bool capturableCp(u32 reg){
    return (reg==0x30||reg==0x40||reg==0x50||reg==0x60||(reg>=0x70&&reg<0x98)||(reg>=0xb0&&reg<0xc0))&&
           !(reg>=0x78&&reg<0x80)&&!(reg>=0x88&&reg<0x90);
}
void putPointer(std::uint32_t* at,const void* pointer){
    const auto value=std::uint64_t(reinterpret_cast<std::uintptr_t>(pointer));
    at[0]=std::uint32_t(value);at[1]=std::uint32_t(value>>32);
}
}
// Native packets: one operation into the record of the captured packet.
// Null when the record is full (the caller ends the capture).
std::uint32_t* CommandStream::captureWords(PacketOp op,std::uint32_t words){
    auto& c=*nativeCapture;
    if(words>0xffffffu||c.capacity-c.used<words)return nullptr;
    std::uint32_t* const at=c.words+c.used;
    at[0]=std::uint32_t(op)|(words<<8);c.used+=words;++c.ops;
    return at;
}
bool CommandStream::write(std::span<const u8> bytes) {
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            // A command begins with this call (GXCmd writes the opcode alone):
            // anything but a register load ends the capture first. A command
            // is at most scratch-sized: the record keeps room for one when it
            // begins (executeScratch cannot end the capture, and the capture
            // never ends with part of a command collected).
            if(used==0&&!bytes.empty()){
                if(autoDraw||held||splitLeft||!capturableOpcode(bytes[0]))captureEnd("command");
                else if(nativeCapture->capacity-nativeCapture->used<scratch.size()/4+16)captureEnd("record full");
            }
        }else if(!settleNative())return false;
    }
    if(active)return reject(StreamError::reentrant);
    if(failure!=StreamError::none)return false;
    active=true;struct Guard{bool& flag;~Guard(){flag=false;}}guard{active};
    if(autoDraw){
        while(!bytes.empty()) {
            if(used==scratch.size()&&!flushAutoPrimitives())return false;
            const size_t count=std::min(bytes.size(),scratch.size()-used);
            std::memcpy(scratch.data()+used,bytes.data(),count);
            used+=count;bytes=bytes.subspan(count);peak=std::max(peak,used);
        }
        return true;
    }
    return writeCommands(bytes);
}
namespace {
// Vertices per primitive of the independent GX primitives; 0 for strips,
// fans and every non-draw command.
unsigned independentGroup(u8 opcode){
    const unsigned primitive=opcode&0xf8;
    return primitive==GX_TRIANGLES?3:primitive==GX_QUADS?4:primitive==GX_LINES?2:primitive==GX_POINTS?1:0;
}
constexpr size_t mergeLimit=1024; // vertices in one merged draw
size_t headerCount(std::span<const u8> scratch){return (size_t(scratch[1])<<8)|scratch[2];}
}
bool CommandStream::writeCommands(std::span<const u8> bytes) {
    while(!bytes.empty()){
        if(held){if(!continueHeld(bytes))return false;continue;}
        if(splitLeft){if(!continueSplit(bytes))return false;continue;}
        if(needed>scratch.size())return reject(StreamError::capacity);
        const size_t count=std::min(needed-used,bytes.size());
        std::memcpy(scratch.data()+used,bytes.data(),count);
        used+=count;bytes=bytes.subspan(count);
        if(used<needed)continue;
        size_t total;
        const bool headerComplete=processor.frame(scratch.first(used),total);
        if(processor.error()!=CommandError::none)return reject(StreamError::processor);
        needed=total;
        if(needed>scratch.size()&&headerComplete&&beginSplit())continue;
        peak=std::max(peak,needed);
        if(needed>scratch.size())return reject(StreamError::capacity);
        if(!headerComplete||used<needed)continue;
        if(stripAt){
            // The line strip that joined a held line list is complete: its
            // raw vertices become the line pairs of its segments.
            const size_t raw=headerCount(scratch),stride=(used-3)/raw;
            const size_t lines=raw-stripCount+2*(stripCount-1);
            expandStrip(stripAt,stripCount,stride);
            scratch[1]=u8(lines>>8);scratch[2]=u8(lines);
            used=needed=3+lines*stride;stripAt=stripCount=0;
        }
        // Merge scope: keep a complete primitive list for the next GXBegin.
        if(merge&&holdCompleted()){held=true;heldPhase=0;continue;}
        if(!executeScratch())return false;
    }
    return true;
}
bool CommandStream::executeScratch() {
    if(nativeCapture)[[unlikely]]{
        // One complete command into the packet's record. A command that is
        // not a register load (it can only come behind another one in the
        // same write) is kept as well, in its place: the packet then cannot
        // be shipped and runs here, in order (PacketCapture::ended).
        const std::uint32_t count=std::uint32_t(used);
        if(auto* at=captureWords(PacketOp::bytes,2+(count+3)/4)){
            at[1]=count;at[1+(count+3)/4]=0;std::memcpy(at+2,scratch.data(),count);
            if(!capturableOpcode(scratch[0])&&!nativeCapture->ended)nativeCapture->ended="command behind a register load";
            if(scratch[0]==GX_LOAD_CP_REG||scratch[0]==GX_LOAD_XF_REG)processor.touchVertexSetup();
            used=0;needed=1;return true;
        }
        // Not reached: write() keeps room for a scratch-sized command.
        return reject(StreamError::capacity);
    }
    if(gxSparseVertexProfiling){++gxFifoOpcodeCounts[scratch[0]];gxFifoOpcodeBytes[scratch[0]]+=used;}
    static const auto executeIndex=registerGxNamedProfile("stream.execute");GxNamedProfileScope executeProfile(executeIndex);
    if(!processor.execute(scratch.first(used)))return reject(StreamError::processor);
    if(failure!=StreamError::none)return false;
    used=0;needed=1;
    return true;
}
bool CommandStream::flushHeld() {
    if(!held)return true;
    // A following primitive header was begun and never completed.
    if(heldPhase)return reject(StreamError::truncated);
    held=false;
    const bool wasActive=active;active=true;
    const bool okay=executeScratch();
    active=wasActive;return okay;
}
// The complete primitive in scratch[0..used) may be held for the next
// GXBegin: quad and triangle lists always, line, line strip and point lists
// only under an orthographic projection. A held line strip stays as it is
// until another line primitive joins it (continueHeld).
bool CommandStream::holdCompleted() {
    const unsigned primitive=scratch[0]&0xf8;
    const size_t count=headerCount(scratch);
    if(used<=3||!count)return false;
    if(primitive==GX_QUADS||primitive==GX_TRIANGLES)return true;
    if(primitive!=GX_LINES&&primitive!=GX_LINESTRIP&&primitive!=GX_POINTS)return false;
    const auto& r=processor.registers();
    if(!r.xfValid[0x1026]||r.xf[0x1026]!=1)return false;
    return primitive!=GX_LINESTRIP||count>=2; // a one-vertex strip draws no segment
}
// Rewrites `count` strip vertices at `at` as line pairs (k, k+1). Back to
// front: every destination lies at or after its source, so each source is
// read before anything overwrites it.
void CommandStream::expandStrip(size_t at,size_t count,size_t stride) {
    u8* base=scratch.data()+at;
    for(size_t k=count-1;k-->0;){
        std::memmove(base+(2*k+1)*stride,base+(k+1)*stride,stride);
        std::memmove(base+2*k*stride,base+k*stride,stride);
    }
}
// The held draw is complete in scratch[0..used). Consume the header of the
// next command: the same opcode extends the held draw (any line primitive
// extends a held line list or strip), anything else draws it and is then
// processed normally.
bool CommandStream::continueHeld(std::span<const u8>& bytes) {
    while(held&&!bytes.empty()){
        const u8 value=bytes[0];
        if(heldPhase==0){
            if(heldOpcode(value)!=heldOpcode(scratch[0]))return flushHeld(); // not consumed
            heldNext=value;heldPhase=1;bytes=bytes.subspan(1);
        }else if(heldPhase==1){
            heldHigh=value;heldPhase=2;bytes=bytes.subspan(1);
        }else{
            bytes=bytes.subspan(1);heldPhase=0;
            const size_t count=headerCount(scratch),add=(size_t(heldHigh)<<8)|value;
            const size_t stride=(used-3)/count;
            // Line strips enter a merged line list as the two vertices of each
            // segment: `base` and `added` count the vertices as line pairs.
            const bool heldStrip=(scratch[0]&0xf8)==GX_LINESTRIP,strip=(heldNext&0xf8)==GX_LINESTRIP;
            const size_t base=heldStrip?2*(count-1):count;
            const size_t added=strip?(add>=2?2*(add-1):0):add;
            const unsigned group=heldStrip?2:independentGroup(scratch[0]);
            if(!group||!added||(!strip&&add%group)||base%group||base+added>mergeLimit||(base+added)*stride>scratch.size()-3){
                // Cannot extend: draw the held primitive and replay this header.
                const u8 header[3]{heldNext,heldHigh,value};
                if(!flushHeld())return false;
                return writeCommands(header);
            }
            if(heldStrip){
                // In place, inside the bound checked above.
                expandStrip(3,count,stride);
                scratch[0]=u8(GX_LINES|(scratch[0]&7));used=3+base*stride;
            }
            // Raw vertex count until a joining strip is complete and expanded.
            const size_t total=base+add;
            scratch[1]=u8(total>>8);scratch[2]=u8(total);
            if(strip){stripAt=used;stripCount=add;}
            needed=used+add*stride;peak=std::max(peak,needed);
            held=false;++merged; // the normal path completes the payload and holds it again
            if(group==2)++mergedLines; // line segments (minimap paths); points are group 1
        }
    }
    return true;
}
bool CommandStream::setImmediateMerge(bool enabled) {
    if(hooked)[[unlikely]]if(!settleNative("merge scope"))return false;
    if(active||processor.executing())return reject(StreamError::reentrant);
    if(!enabled&&!flushHeld())return false;
    merge=enabled;return true;
}
bool CommandStream::keepsHeldDraw(u8 reg,u32 mask,u32 value)const{
    if(hooked||!held||heldPhase)return false;
    // Registers whose write does nothing but store the value (no trigger,
    // no TLUT load, no mask latch).
    if(reg!=0x22&&(reg<0xe0||reg>0xe7))return false;
    const auto& r=processor.registers();
    return r.bpMask==0xffffffu&&r.bpValid[reg]&&((r.bp[reg]^value)&mask&0xffffffu)==0;
}
// Draws the whole primitives buffered in scratch and keeps a partial vertex
// as raw bytes (scalar writes may cross the scratch boundary). `last` draws
// everything left: the end of a split fixed-count primitive, a whole number
// of primitives by construction.
bool CommandStream::flushGroups(size_t stride,bool last) {
    // Split only independent primitives. Strips/fans require retained boundary
    // vertices and winding state, so they keep the existing explicit limit.
    const unsigned group=independentGroup(scratch[0]);
    if(!group||!stride)return reject(StreamError::capacity);
    size_t vertices=(used-3)/stride;
    if(!last)vertices=std::min(vertices,size_t(0xfffe))/group*group;
    if(!vertices||vertices>0xffff)return reject(StreamError::capacity);
    const size_t payload=vertices*stride;
    scratch[1]=u8(vertices>>8);scratch[2]=u8(vertices);
    if(!autoDraw&&gxSparseVertexProfiling){++gxFifoOpcodeCounts[scratch[0]];gxFifoOpcodeBytes[scratch[0]]+=3+payload;}
    if(!processor.execute(scratch.first(3+payload)))return reject(StreamError::processor);
    if(failure!=StreamError::none)return false;
    // The processor borrows scratch only during the call above.
    std::memmove(scratch.data()+3,scratch.data()+3+payload,used-3-payload);
    used-=payload;return true;
}
bool CommandStream::flushAutoPrimitives() {return flushGroups(autoStride,false);}
// The header of a fixed-count primitive larger than scratch is complete in
// scratch[0..3). Independent primitives whose count is a whole number of
// primitives stream through scratch; anything else keeps the capacity error.
bool CommandStream::beginSplit() {
    const unsigned group=independentGroup(scratch[0]);
    const size_t count=headerCount(scratch);
    if(!group||used!=3||!count||count%group||held||autoDraw)return false;
    const size_t stride=(needed-3)/count;
    if(!stride||(needed-3)%count||stride*group>scratch.size()-3)return false;
    splitStride=stride;splitLeft=needed-3;++split;
    splitLargest=std::max<std::uint32_t>(splitLargest,std::uint32_t(needed));
    peak=std::max(peak,used);
    return true;
}
bool CommandStream::continueSplit(std::span<const u8>& bytes) {
    const size_t count=std::min({splitLeft,bytes.size(),scratch.size()-used});
    std::memcpy(scratch.data()+used,bytes.data(),count);
    used+=count;splitLeft-=count;bytes=bytes.subspan(count);peak=std::max(peak,used);
    if(splitLeft&&used<scratch.size())return true;
    static const auto splitIndex=registerGxNamedProfile("stream.split");GxNamedProfileScope splitProfile(splitIndex);
    ++splitChunk;
    if(!flushGroups(splitStride,!splitLeft))return false;
    if(!splitLeft){used=0;needed=1;splitStride=0;}
    return true;
}
bool CommandStream::beginAuto(u8 opcode) {
    if(hooked)[[unlikely]]if(!settleNative("GX_AUTO draw"))return false;
    if(!finish())return false;
    if(scratch.size()<3)return reject(StreamError::capacity);
    scratch[0]=opcode;scratch[1]=0;scratch[2]=1;
    size_t total=0;
    if(!processor.frame(scratch.first(3),total)||processor.error()!=CommandError::none)
        return reject(StreamError::processor);
    if(total<=3)return reject(StreamError::processor);
    autoStride=total-3;used=3;autoDraw=true;return true;
}
bool CommandStream::endAuto() {
    if(active||processor.executing())return reject(StreamError::reentrant);
    if(failure!=StreamError::none)return false;
    // A held merge-scope primitive stays pending for the next GXBegin.
    if(!autoDraw)return (held&&!heldPhase)||finish();
    const size_t payload=used-3;
    if(!autoStride||payload%autoStride)return reject(StreamError::truncated);
    const size_t vertices=payload/autoStride;
    if(vertices>=0xffff)return reject(StreamError::capacity);
    // Empty auto draws emit no geometry. Validate every nonempty draw through
    // exactly the same processor/sink as fixed-count raw and original GD lists.
    scratch[1]=vertices>>8;scratch[2]=vertices;
    active=true;struct Guard{bool& flag;~Guard(){flag=false;}}guard{active};
    if(vertices&&!processor.execute(scratch.first(used)))return reject(StreamError::processor);
    if(failure!=StreamError::none)return false;
    autoDraw=false;autoStride=0;used=0;needed=1;return true;
}
bool CommandStream::call(std::span<const u8> list) {
    static const auto index=registerGxNamedProfile("stream.call");GxNamedProfileScope profile(index);
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            // By reference: display lists stay as they are while the picture
            // is painted (their bytes are checked when the packet is folded).
            if(!finish())return false;
            if(auto* at=captureWords(PacketOp::list,4)){
                putPointer(at+1,list.data());at[3]=std::uint32_t(list.size());processor.touchVertexSetup();return true;
            }
            captureEnd("record full");
        }else if(!settleNative())return false;
    }
    if(!finish())return false;
    active=true;struct Guard{bool& flag;~Guard(){flag=false;}}guard{active};
    if(!processor.executeList(list,features::on(features::Id::packetPaint)))return reject(StreamError::processor);
    return failure==StreamError::none;
}
bool CommandStream::callHeld(std::span<const u8> list) {
    if(hooked||su.enabled||!finish())return false;
    return processor.listHeld(list);
}
bool CommandStream::writeCp(u8 reg,u32 value) {
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            if(capturableCp(reg))if(auto* at=captureWords(PacketOp::cp,3)){
                at[1]=reg;at[2]=value;
                // A matrix array stride (CP 0xBC-0xBF) is what an indexed
                // load of this packet reads with.
                if(reg>=0xbc)nativeCapture->matrices[reg-0xbc].stride=std::uint8_t(value);
                processor.touchVertexSetup();
                return true;
            }
            captureEnd("cp register");
        }else if(!settleNative())return false;
    }
    return finish()&&(processor.writeCp(reg,value)||reject(StreamError::processor));
}
bool CommandStream::writeBp(u8 reg,u32 value) {
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            // EFB copies stay on the original path. Palette loads are
            // captured: the seal replays them in their order (native_packets.cpp).
            if(reg!=0x52)if(auto* at=captureWords(PacketOp::bp,3)){at[1]=reg;at[2]=value;return true;}
            captureEnd("bp register");
        }else if(!settleNative())return false;
    }
    return finish()&&(processor.writeBp(reg,value)||reject(StreamError::processor));
}
bool CommandStream::writeBpMasked(u8 reg,u32 mask,u32 value) {
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            if(reg!=0x52)if(auto* at=captureWords(PacketOp::bpMasked,4)){at[1]=reg;at[2]=mask;at[3]=value;return true;}
            captureEnd("bp register");
        }else if(!settleNative())return false;
    }
    return finish()&&(processor.writeBpMasked(reg,mask,value)||reject(StreamError::processor));
}
bool CommandStream::loadIndexed(u8 code,u16 index,u16 packed) {
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            // The matrix is read now, through the binding this packet made
            // (the original path reads it here as well).
            const u32 count=(packed>>12)+1,address=packed&0xfff;
            if((code==GX_LOAD_INDX_A||code==GX_LOAD_INDX_B||code==GX_LOAD_INDX_C||code==GX_LOAD_INDX_D)&&address<0x1058&&count<=0x1058-address){
                const auto& m=nativeCapture->matrices[(code-GX_LOAD_INDX_A)/8];
                const std::uint64_t at=std::uint64_t(index)*m.stride;
                const auto* source=reinterpret_cast<const u8*>(m.address);
                if(m.known&&source&&at<=m.size&&count*4<=m.size-at)
                    if(auto* out=captureWords(PacketOp::indexed,4+count)){
                        out[1]=code|(std::uint32_t(packed)<<16);out[2]=index;out[3]=m.little;
                        std::memcpy(out+4,source+at,count*4);
                        return true;
                    }
            }
            captureEnd("indexed load");
        }else if(!settleNative())return false;
    }
    return finish()&&(processor.loadIndexed(code,index,packed)||reject(StreamError::processor));
}
bool CommandStream::writeXf(u16 address,std::span<const u32> values) {
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            if(!values.empty()&&address<0x1058&&values.size()<=0x1058u-address)
                if(auto* at=captureWords(PacketOp::xf,3+std::uint32_t(values.size()))){
                    at[1]=address;at[2]=std::uint32_t(values.size());std::memcpy(at+3,values.data(),values.size()*4);
                    if(address<=0x1008&&address+values.size()>0x1008)processor.touchVertexSetup();
                    return true;
                }
            captureEnd("xf write");
        }else if(!settleNative())return false;
    }
    return finish()&&(processor.writeXf(address,values)||reject(StreamError::processor));
}
bool CommandStream::setCopySource(bool texture,u32 left,u32 top,u32 width,u32 height) {
    if(hooked)[[unlikely]]if(!settleNative("copy setup"))return false;
    return finish()&&(processor.setCopySource(texture,left,top,width,height)||reject(StreamError::processor));
}
bool CommandStream::setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap) {
    if(hooked)[[unlikely]]if(!settleNative("copy setup"))return false;
    return finish()&&(processor.setCopyDestination(texture,width,height,format,mipmap)||reject(StreamError::processor));
}
bool CommandStream::setCopyControl(bool texture,u32 mask,u32 value) {
    if(hooked)[[unlikely]]if(!settleNative("copy setup"))return false;
    return finish()&&(processor.setCopyControl(texture,mask,value)||reject(StreamError::processor));
}
bool CommandStream::finish() {
    if(active||processor.executing())return reject(StreamError::reentrant);
    if(failure!=StreamError::none)return false;
    if(held&&!flushHeld())return false;
    if(processor.error()!=CommandError::none)return reject(StreamError::processor);
    return (!autoDraw&&!used)||reject(StreamError::truncated);
}
bool CommandStream::clearDescriptors(){
    if(hooked)[[unlikely]]if(!settleNative("vertex descriptor"))return false;
    return finish()&&(processor.clearDescriptors()||reject(StreamError::processor));
}
bool CommandStream::setDescriptor(unsigned attribute,unsigned type){
    if(hooked)[[unlikely]]if(!settleNative("vertex descriptor"))return false;
    return finish()&&(processor.setDescriptor(attribute,type)||reject(StreamError::processor));
}
bool CommandStream::bindArray(unsigned slot,const void* data,u32 size,u8 stride,bool little){
    if(hooked)[[unlikely]]{
        if(nativeCapture){
            // Captures run only inside J3DMatPacket::draw, whose bindings are
            // the model's own matrix buffers: the liveness proof (it walks
            // the heap whenever any heap freed since the last proof, which
            // is every picture) is left to whoever applies the binding (the
            // worker's replay, the seal, a flush), as for the arrays a list
            // binds.
            const auto address=reinterpret_cast<std::uintptr_t>(data);
            if(slot<16&&(data||!size))if(auto* at=captureWords(PacketOp::bind,5)){
                at[1]=slot|(std::uint32_t(stride)<<8)|(std::uint32_t(little)<<16);putPointer(at+2,data);at[4]=size;
                if(slot>=12)nativeCapture->matrices[slot-12]={address,size,stride,little,true};
                processor.touchVertexSetup();
                return true;
            }
            captureEnd("array binding");
        }else if(!settleNative())return false;
    }
    return finish()&&(processor.bindArray(slot,data,size,stride,little)||reject(StreamError::processor));
}
bool CommandStream::reset() {
    if(hooked)[[unlikely]]if(!settleNative("reset"))return false;
    if(active)return reject(StreamError::reentrant);
    processor.reset();
    if(processor.error()!=CommandError::none)return reject(StreamError::processor);
    used=0;needed=1;autoDraw=false;autoStride=0;failure=StreamError::none;
    held=false;heldPhase=0;merge=false;stripAt=stripCount=0;splitLeft=splitStride=0;
    const bool enabled=su.enabled;su={};su.enabled=enabled;return true;
}
bool CommandStream::configureAutomaticSu(bool enabled) {
    if(hooked)[[unlikely]]if(!settleNative("automatic SU"))return false;
    if(!finish())return false;
    su={};su.enabled=enabled;return true;
}
bool CommandStream::resetAutomaticSuOwnerState() {
    if(!su.enabled)return true;
    if(!finish())return false;
    const bool enabled=su.enabled;su={};su.enabled=enabled;return true;
}
bool CommandStream::automaticSuTexture(unsigned map,u32 image,u32 mode) {
    if(!su.enabled)return true;
    if(!finish())return false;
    if(map>7)return reject(StreamError::automaticSuState);
    su.image[map]=image;su.mode[map]=mode;su.textureKnown|=1u<<map;su.dirty=true;return true;
}
bool CommandStream::automaticSuCount(unsigned count,bool indirect) {
    if(!su.enabled)return true;
    if(!finish())return false;
    if(indirect?count>4:count<1||count>16)return reject(StreamError::automaticSuState);
    (indirect?su.indirectCount:su.tevCount)=count;return true; // SDK: no SU dirty bit.
}
bool CommandStream::automaticSuOrder(unsigned stage,unsigned coord,unsigned map,bool indirect) {
    if(!su.enabled)return true;
    if(!finish())return false;
    if(stage>=(indirect?4u:16u)||(coord>7&&coord!=0xff)||
       ((map&~0x100u)>7&&map!=0xff)||(indirect&&(coord>7||map>7)))return reject(StreamError::automaticSuState);
    if(indirect){su.indirect[stage]={coord,map};su.indirectKnown|=1u<<stage;}
    else {su.tev[stage]={coord,map};su.orderKnown|=1u<<stage;}
    su.dirty=true;return true;
}
u32 CommandStream::automaticSuWord(unsigned slot)const {
    const auto& r=registers();const unsigned reg=0x30+slot;
    return ((r.bpValid[reg]?r.bp[reg]:0)&~su.fieldMask[slot])|su.fieldValue[slot];
}
bool CommandStream::automaticSuManual(unsigned coord,bool enabled,u16 ss,u16 ts) {
    if(!su.enabled||!finish())return false;
    if(coord>7||(enabled&&(!ss||!ts)))return reject(StreamError::automaticSuState);
    su.manual=(su.manual&~(1u<<coord))|(unsigned(enabled)<<coord);
    if(!enabled)return true; // SDK: no write, no SU dirty bit on disable.
    const unsigned slot=coord*2;
    for(unsigned axis=0;axis<2;++axis){
        su.fieldMask[slot+axis]|=65535u;
        su.fieldValue[slot+axis]=(su.fieldValue[slot+axis]&~65535u)|u16((axis?ts:ss)-1);
    }
    return writeBp(0x30+slot,automaticSuWord(slot))&&writeBp(0x31+slot,automaticSuWord(slot+1));
}
bool CommandStream::automaticSuField(unsigned coord,unsigned bit,bool s,bool t) {
    if(!su.enabled||!finish())return false;
    if(coord>7||(bit!=16&&bit!=17))return reject(StreamError::automaticSuState);
    for(unsigned axis=0;axis<2;++axis){const unsigned slot=coord*2+axis;
        su.fieldMask[slot]|=1u<<bit;
        su.fieldValue[slot]=(su.fieldValue[slot]&~(1u<<bit))|(u32(axis?t:s)<<bit);
    }
    if(!(su.manual&(1u<<coord)))return true; // SDK: staged until next auto update.
    return writeBp(0x30+coord*2,automaticSuWord(coord*2))&&writeBp(0x31+coord*2,automaticSuWord(coord*2+1));
}
bool CommandStream::automaticSuOffsets(unsigned coord,bool line,bool point) {
    if(!su.enabled||!finish())return false;
    if(coord>7)return reject(StreamError::automaticSuState);
    const unsigned slot=coord*2;
    su.fieldMask[slot]|=3u<<18;
    su.fieldValue[slot]=(su.fieldValue[slot]&~(3u<<18))|(u32(line)<<18)|(u32(point)<<19);
    return writeBp(0x30+slot,automaticSuWord(slot));
}
bool CommandStream::flushAutomaticSu() {
    if(!finish())return false;
    if(!su.enabled||!su.dirty)return true;
    // Validate the WHOLE update before emitting any BP words. Never invent
    // missing typed state from raw GD/J3D registers or assume a 1x1 texture.
    auto valid=[&](const AutomaticSu::Order& o){
        return o.coord==0xff||o.map==0xff||(su.manual&(1u<<o.coord))||
               (su.textureKnown&(1u<<(o.map&~0x100u)));
    };
    if(su.manual!=0xff){
        for(unsigned i=0;i<su.indirectCount;++i)
            if(!(su.indirectKnown&(1u<<i))||!valid(su.indirect[i]))return reject(StreamError::automaticSuState);
        for(unsigned i=0;i<su.tevCount;++i)
            if(!(su.orderKnown&(1u<<i))||!valid(su.tev[i]))return reject(StreamError::automaticSuState);
    }
    auto emit=[&](const AutomaticSu::Order& o){
        if(o.coord==0xff||o.map==0xff||(su.manual&(1u<<o.coord)))return true;
        const unsigned map=o.map&~0x100u,slot=o.coord*2;
        const u32 image=su.image[map],mode=su.mode[map];
        for(unsigned axis=0;axis<2;++axis){
            // __SetSURegs changes low16 and repeat-bias bit16 only. Keep cyl,
            // line/point and every other SU bit. Automatic wrap supersedes bias.
            const u32 size=(image>>(axis*10))&1023;
            const u32 bias=((mode>>(axis*2))&3)==1;
            su.fieldMask[slot+axis]|=0x1ffffu;
            su.fieldValue[slot+axis]=(su.fieldValue[slot+axis]&~0x1ffffu)|size|(bias<<16);
            const u32 value=automaticSuWord(slot+axis);
            // SDK does NOT reset BP mask: first S consumes any pending mask;
            // subsequent words see the processor's automatic full mask reset.
            if(!writeBp(0x30+slot+axis,value))return false;
        }
        return true;
    };
    if(su.manual!=0xff){
        for(unsigned i=0;i<su.indirectCount;++i)if(!emit(su.indirect[i]))return false;
        for(unsigned i=0;i<su.tevCount;++i)if(!emit(su.tev[i]))return false;
    }
    su.dirty=false;return true;
}

}
