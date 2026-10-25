#include "gx_stream.hpp"
#include "gx_profile.hpp"
#include <algorithm>
#include <cstring>

namespace dusk::native::gx {
bool CommandStream::write(std::span<const u8> bytes) {
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
    while(!bytes.empty()){
        if(needed>scratch.size())return reject(StreamError::capacity);
        const size_t count=std::min(needed-used,bytes.size());
        std::memcpy(scratch.data()+used,bytes.data(),count);
        used+=count;bytes=bytes.subspan(count);
        if(used<needed)continue;
        size_t total;
        const bool headerComplete=processor.frame(scratch.first(used),total);
        if(processor.error()!=CommandError::none)return reject(StreamError::processor);
        needed=total;
        peak=std::max(peak,needed);
        if(needed>scratch.size())return reject(StreamError::capacity);
        if(!headerComplete||used<needed)continue;
        if(gxSparseVertexProfiling){++gxFifoOpcodeCounts[scratch[0]];gxFifoOpcodeBytes[scratch[0]]+=used;}
        if(!processor.execute(scratch.first(used)))return reject(StreamError::processor);
        if(failure!=StreamError::none)return false;
        used=0;needed=1;
    }
    return true;
}
bool CommandStream::flushAutoPrimitives() {
    // Split only independent primitives. Strips/fans require retained boundary
    // vertices and winding state, so they keep the existing explicit limit.
    const unsigned primitive=scratch[0]&0xf8;
    const unsigned group=primitive==GX_TRIANGLES?3:primitive==GX_QUADS?4:
        primitive==GX_LINES?2:primitive==GX_POINTS?1:0;
    if(!group||!autoStride)return reject(StreamError::capacity);
    const size_t vertices=std::min((used-3)/autoStride,size_t(0xfffe))/group*group;
    if(!vertices)return reject(StreamError::capacity);
    const size_t payload=vertices*autoStride;
    scratch[1]=vertices>>8;scratch[2]=vertices;
    if(!processor.execute(scratch.first(3+payload)))return reject(StreamError::processor);
    if(failure!=StreamError::none)return false;
    // Keep a partial vertex as raw bytes; scalar writes may cross the scratch
    // boundary. The processor borrows scratch only during the call above.
    std::memmove(scratch.data()+3,scratch.data()+3+payload,used-3-payload);
    used-=payload;return true;
}
bool CommandStream::beginAuto(u8 opcode) {
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
    if(!autoDraw)return finish();
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
    if(!finish())return false;
    active=true;struct Guard{bool& flag;~Guard(){flag=false;}}guard{active};
    if(!processor.execute(list))return reject(StreamError::processor);
    return failure==StreamError::none;
}
bool CommandStream::writeCp(u8 reg,u32 value) {
    return finish()&&(processor.writeCp(reg,value)||reject(StreamError::processor));
}
bool CommandStream::writeBp(u8 reg,u32 value) {
    return finish()&&(processor.writeBp(reg,value)||reject(StreamError::processor));
}
bool CommandStream::loadIndexed(u8 code,u16 index,u16 packed) {
    return finish()&&(processor.loadIndexed(code,index,packed)||reject(StreamError::processor));
}
bool CommandStream::writeXf(u16 address,std::span<const u32> values) {
    return finish()&&(processor.writeXf(address,values)||reject(StreamError::processor));
}
bool CommandStream::setCopySource(bool texture,u32 left,u32 top,u32 width,u32 height) {
    return finish()&&(processor.setCopySource(texture,left,top,width,height)||reject(StreamError::processor));
}
bool CommandStream::setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap) {
    return finish()&&(processor.setCopyDestination(texture,width,height,format,mipmap)||reject(StreamError::processor));
}
bool CommandStream::setCopyControl(bool texture,u32 mask,u32 value) {
    return finish()&&(processor.setCopyControl(texture,mask,value)||reject(StreamError::processor));
}
bool CommandStream::finish() {
    if(active||processor.executing())return reject(StreamError::reentrant);
    if(failure!=StreamError::none)return false;
    if(processor.error()!=CommandError::none)return reject(StreamError::processor);
    return (!autoDraw&&!used)||reject(StreamError::truncated);
}
bool CommandStream::clearDescriptors(){return finish()&&(processor.clearDescriptors()||reject(StreamError::processor));}
bool CommandStream::setDescriptor(unsigned attribute,unsigned type){return finish()&&(processor.setDescriptor(attribute,type)||reject(StreamError::processor));}
bool CommandStream::bindArray(unsigned slot,const void* data,u32 size,u8 stride,bool little){return finish()&&(processor.bindArray(slot,data,size,stride,little)||reject(StreamError::processor));}
bool CommandStream::reset() {
    if(active)return reject(StreamError::reentrant);
    processor.reset();
    if(processor.error()!=CommandError::none)return reject(StreamError::processor);
    used=0;needed=1;autoDraw=false;autoStride=0;failure=StreamError::none;
    const bool enabled=su.enabled;su={};su.enabled=enabled;return true;
}
bool CommandStream::configureAutomaticSu(bool enabled) {
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
