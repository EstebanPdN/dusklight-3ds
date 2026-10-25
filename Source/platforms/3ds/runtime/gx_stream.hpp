#pragma once
#include "gx_commands.hpp"

namespace dusk::native::gx {
enum class StreamError { none, capacity, truncated, processor, reentrant, automaticSuState };
// Incremental raw GX commands use caller-owned scratch. Complete display lists
// bypass scratch and remain borrowed through synchronous command execution.
// One render thread owns processor, scratch, stream and registered resources.
class CommandStream {
public:
    CommandStream(CommandProcessor& processor,std::span<u8> scratch):processor(processor),scratch(scratch){}
    CommandStream(const CommandStream&)=delete;
    CommandStream& operator=(const CommandStream&)=delete;
    // Private experiment: idle-only, per-owner, default OFF; no global cache.
    bool configureAutomaticSu(bool enabled);
    bool resetAutomaticSuOwnerState();
    bool automaticSuEnabled()const{return su.enabled;}
    bool automaticSuTexture(unsigned map,u32 image,u32 mode);
    bool automaticSuCount(unsigned count,bool indirect);
    bool automaticSuOrder(unsigned stage,unsigned coord,unsigned map,bool indirect);
    bool automaticSuManual(unsigned coord,bool enabled,u16 ss,u16 ts);
    bool automaticSuField(unsigned coord,unsigned bit,bool s,bool t);
    bool automaticSuOffsets(unsigned coord,bool line,bool point);
    bool flushAutomaticSu(); // ONLY typed GXBegin/Indexed, GXCallDisplayList and GXFlush.
    bool write(std::span<const u8> bytes);
    bool call(std::span<const u8> list);
    // GX_AUTO uses bounded scratch. Independent primitives may submit complete
    // groups before GXEnd; incomplete vertices/primitives remain buffered.
    bool beginAuto(u8 opcode);
    bool endAuto();
    bool recordingAuto()const{return autoDraw;}
    bool writeCp(u8 reg,u32 value);
    bool writeBp(u8 reg,u32 value);
    bool writeXf(u16 address,std::span<const u32> values);
    bool loadIndexed(u8 code,u16 index,u16 packed);
    bool setCopySource(bool texture,u32 left,u32 top,u32 width,u32 height);
    bool setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap);
    bool setCopyControl(bool texture,u32 mask,u32 value);
    bool clearDescriptors();
    bool setDescriptor(unsigned attribute,unsigned type);
    bool queryDescriptor(unsigned attribute,unsigned& type) const {return idle()&&processor.queryDescriptor(attribute,type);}
    bool bindArray(unsigned slot,const void* data,u32 size,u8 stride,bool little);
    bool finish(); // Must be checked at an explicit frame/state boundary.
    bool reset(); // Explicitly discards incomplete bytes and resets processor.
    bool idle()const{return !active&&!processor.executing()&&!autoDraw&&used==0;}
    StreamError error()const{return failure;}
    CommandError commandError()const{return processor.error();}
    size_t commandErrorOffset()const{return processor.errorOffset();}
    size_t scratchCapacity()const{return scratch.size();}
    size_t pendingBytes()const{return needed;}
    size_t peakBytes()const{return peak;}
    unsigned pendingOpcode()const{return used?scratch[0]:0;}
    const CommandRegisters& registers()const{return processor.registers();}
private:
    struct AutomaticSu {
        bool enabled=false,dirty=false;
        u8 manual=0,textureKnown=0,indirectKnown=0;
        u16 orderKnown=0;
        unsigned tevCount=1,indirectCount=0;
        std::array<u32,8> image{},mode{};
        struct Order {unsigned coord=0,map=0xff;};
        std::array<Order,16> tev{};
        std::array<Order,4> indirect{};
        // Only explicitly typed SU field overrides; unrelated raw upper bits
        // stay in the shared register bank. Bias is replaced by automatic wrap.
        std::array<u32,16> fieldMask{},fieldValue{};
    } su;
    u32 automaticSuWord(unsigned slot)const;
    CommandProcessor& processor;
    std::span<u8> scratch;
    size_t used=0,needed=1,peak=0;
    bool active=false,autoDraw=false;
    size_t autoStride=0;
    StreamError failure=StreamError::none;
    bool flushAutoPrimitives();
    bool reject(StreamError value){if(failure==StreamError::none)failure=value;return false;}
};
// TLS routing for the original GXCmd/GXCallDisplayList ABI. Install only while
// idle. The caller must detach before destroying the stream; no implicit sink.
bool setCommandStream(CommandStream* stream);
CommandStream* commandStream(); // Render-thread adapter access; caller owns lifetime.
}
