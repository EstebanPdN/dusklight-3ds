#pragma once
#include <cstring>
#include "gx_commands.hpp"

namespace dusk::native::gx {
enum class StreamError { none, capacity, truncated, processor, reentrant, automaticSuState };
class CommandStream;
// Native packets (runtime/native_packets.hpp). While a J3D packet is
// captured, the register operations reaching the stream are not run: they
// are written, in their order and in this word format, into the packet's
// record (the second-core worker runs them on its copy of the bank, the game
// thread folds them into its bank later). Words: a header `op | words << 8`
// (the whole operation), then:
//   bp       reg, value                    CommandProcessor::writeBp
//   bpMasked reg, mask, value              writeBpMasked
//   cp       reg, value                    writeCp
//   xf       address, count, values        writeXf
//   list     pointer (2 words), bytes      executeList (a display list, by reference)
//   bytes    count, bytes (padded)         execute (one command written through write())
//   bind     slot | stride << 8 | little << 16, pointer (2 words), bytes   bindArray
//   indexed  code | packed << 16, index, little, the array bytes (padded)   loadIndexed
//   shape    generation, list (2 words), bytes, caster slot   (gx_command_draw.cpp)
enum class PacketOp : std::uint32_t {bp=1,bpMasked,cp,xf,list,bytes,bind,indexed,shape};
struct PacketCapture {
    std::uint32_t* words{};std::uint32_t capacity{},used{};
    // Ops in the record (an upper bound of the epoch changes they make) and
    // the shapes.
    std::uint32_t ops{},shapes{};
    // Matrix arrays (slots 12-15) bound by this packet: an indexed load reads
    // its bytes at capture through them.
    struct Matrix {std::uintptr_t address{};std::uint32_t size{};std::uint8_t stride{};bool little{},known{};};
    std::array<Matrix,4> matrices{};
    std::uint32_t staleReads{}; // registers() read while capturing (stale by design: see native_packets.hpp)
    const char* ended{};        // why the stream ended the capture early (static string)
};
// The native packet module's answer to an entry point that cannot be
// captured, or to any entry point while captured packets are pending: end
// the capture (its operations run on the processor as the original path
// would have run them) and bring the bank up to date. False: the stream
// failed meanwhile.
using PacketSettle=bool(*)(CommandStream&);
// Incremental raw GX commands use caller-owned scratch. Complete display lists
// bypass scratch and remain borrowed through synchronous command execution.
// One render thread owns processor, scratch, stream and registered resources.
class CommandStream {
public:
    CommandStream(CommandProcessor& processor,std::span<u8> scratch):processor(processor),scratch(scratch){}
    // ---- Native packets (runtime/native_packets.hpp) ----
    // The module installs its settle function once; capture and pending are
    // set and cleared by the module only.
    static void setPacketSettle(PacketSettle settle){nativeSettle=settle;}
    void setPacketCapture(PacketCapture* capture){nativeCapture=capture;hooked=nativeCapture||nativePending;}
    void setNativePending(bool pending){nativePending=pending;hooked=nativeCapture||nativePending;}
    PacketCapture* capturing()const{return nativeCapture;}
    bool nativePendingRun()const{return nativePending;}
    // The processor and stream state a capture may begin in: nothing
    // collected, held or failed, no pending BP mask, automatic SU off.
    bool nativeCaptureReady()const{
        return !active&&!processor.executing()&&!autoDraw&&!used&&!held&&!splitLeft&&!stripAt&&!merge&&!su.enabled&&
               failure==StreamError::none&&processor.error()==CommandError::none&&processor.pendingBpMask()==0xffffff;
    }
    // The module's direct access (no hook, no settle).
    CommandProcessor& nativeProcessor(){return processor;}
    bool nativeReject(){return reject(StreamError::processor);}
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
    // The bytes of one value (a typed GX vertex call, a GXCmd). When they
    // only add to what the stream is collecting, which is every value inside
    // a primitive but the one that completes it, this is all write() does
    // after its checks; every other case goes through write(). The call
    // chain into write() cost about 2 microseconds per vertex call (the
    // 1,200 stars of a night sky are 7,200 calls a picture; a line of menu
    // text is a dozen per letter).
    bool writeValue(const u8* bytes,size_t count){
        if(!active&&failure==StreamError::none){
            if(autoDraw){
                if(count<=scratch.size()-used){
                    std::memcpy(scratch.data()+used,bytes,count);used+=count;if(used>peak)peak=used;return true;
                }
            }else if(!held&&!splitLeft&&needed<=scratch.size()&&used+count<needed){
                std::memcpy(scratch.data()+used,bytes,count);used+=count;return true;
            }
        }
        return write({bytes,count});
    }
    bool call(std::span<const u8> list);
    // packetPaint (perf_features.hpp): GXCallDisplayList's own steps up to the
    // list (automatic SU flush, finish), then CommandProcessor::listHeld.
    // True: the list was held and needs no call. False: call it as usual
    // (also while a native packet is captured or pending).
    bool callHeld(std::span<const u8> list);
    // GX_AUTO uses bounded scratch. Independent primitives may submit complete
    // groups before GXEnd; incomplete vertices/primitives remain buffered.
    bool beginAuto(u8 opcode);
    bool endAuto();
    bool recordingAuto()const{return autoDraw;}
    // Native PICA engine: inside a merge scope a complete fixed-count quad or
    // triangle list is held instead of drawn. A following GXBegin with the
    // same opcode appends its vertices to it; any other stream operation
    // (they all pass through finish()) draws it first, so the state every
    // merged primitive was issued under is the same. Only loops whose draws
    // all go through this stream may open a scope.
    // Orthographic line, line strip and point draws merge the same way. Line
    // strips joining a line draw become the line list of their segments:
    // each segment is widened on its own, as GX draws it, so the quads are
    // the same ones in the same order. A strip nothing joins is drawn as the
    // strip it is. Orthographic w is 1, so no segment can be refused for
    // crossing the camera plane and take its merged neighbours with it.
    bool setImmediateMerge(bool enabled);
    bool continuesHeld(u8 opcode)const{return !hooked&&held&&!heldPhase&&used&&heldOpcode(opcode)==heldOpcode(scratch[0]);}
    // A draw is held and writing `value` under `mask` to BP `reg` would leave
    // the register bank exactly as it is: the line/point size or a TEV colour
    // register (an equal raw word is also the value of its bank), known, equal
    // under the mask, no GD mask pending. A typed setter may then return
    // without finish() and without the write, so the next GXBegin can still
    // extend the held draw (the minimap repeats GXSetLineWidth and
    // GXSetTevColor before every line strip).
    bool keepsHeldDraw(u8 reg,u32 mask,u32 value)const;
    std::uint64_t mergedDraws()const{return merged;}
    std::uint64_t mergedLineDraws()const{return mergedLines;}
    // A fixed-count triangle, quad, line or point list larger than scratch
    // is drawn in chunks of whole primitives as scratch fills, in order and
    // under the same state, like GX_AUTO. Strips and fans keep the limit.
    std::uint64_t splitDraws()const{return split;}
    std::uint64_t splitChunks()const{return splitChunk;}
    std::uint32_t largestSplitBytes()const{return splitLargest;}
    bool writeCp(u8 reg,u32 value);
    bool writeBp(u8 reg,u32 value);
    // BP mask then register in one processor call (CommandProcessor::
    // writeBpMasked): the typed TEV setters' masked field updates.
    bool writeBpMasked(u8 reg,u32 mask,u32 value);
    bool writeXf(u16 address,std::span<const u32> values);
    bool loadIndexed(u8 code,u16 index,u16 packed);
    bool setCopySource(bool texture,u32 left,u32 top,u32 width,u32 height);
    bool setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap);
    bool setCopyControl(bool texture,u32 mask,u32 value);
    bool clearDescriptors();
    bool setDescriptor(unsigned attribute,unsigned type);
    bool queryDescriptor(unsigned attribute,unsigned& type) const {
        if(hooked)[[unlikely]]if(!const_cast<CommandStream*>(this)->settleNative("vertex descriptor"))return false;
        return idle()&&processor.queryDescriptor(attribute,type);
    }
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
    // While a packet is captured this is the bank as it stood before the
    // packet (counted in PacketCapture::staleReads); while captured packets
    // are pending, reading it brings them into the bank first.
    const CommandRegisters& registers()const{
        if(hooked)[[unlikely]]const_cast<CommandStream*>(this)->nativeRead();
        return processor.registers();
    }
    // Retained 2D screens: see CommandProcessor::collectChanges. Idle stream only.
    unsigned collectChanges(const CommandRegisters& before,std::span<BankWord> out,bool& ok)const{
        ok=false;
        if(hooked)[[unlikely]]if(!const_cast<CommandStream*>(this)->settleNative("bank changes"))return 0;
        return idle()?processor.collectChanges(before,out,ok):0;
    }
    bool applyChanges(std::span<const BankWord> words){
        if(hooked)[[unlikely]]if(!settleNative("bank changes"))return false;
        return idle()&&processor.applyChanges(words);
    }
    // Second-core worker (gx_worker.hpp): the bank blocks written since the
    // last call, and the array bindings a shape issued now is drawn with.
    void attachBankLog(std::span<std::uint32_t> buffer){if(hooked)[[unlikely]](void)settleNative("bank log");processor.attachBankLog(buffer);}
    const BankLog& bankStores()const{
        if(hooked)[[unlikely]]const_cast<CommandStream*>(this)->nativeRead();
        return processor.bankStores();
    }
    void restartBankLog(){if(hooked)[[unlikely]](void)settleNative("bank log");processor.restartBankLog();}
    const CommandProcessor::ArrayBindings& arrayBindings()const{
        if(hooked)[[unlikely]]const_cast<CommandStream*>(this)->nativeRead();
        return processor.currentArrayBindings();
    }
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
    // Native packets (see setPacketCapture).
    static inline PacketSettle nativeSettle{};
    PacketCapture* nativeCapture{};
    bool nativePending=false,hooked=false;
    bool settleNative(const char* why=nullptr){
        if(nativeCapture&&why&&!nativeCapture->ended)nativeCapture->ended=why;
        return !nativeSettle||nativeSettle(*this);
    }
    void nativeRead(){if(nativeCapture)++nativeCapture->staleReads;else (void)settleNative();}
    // Capture appends (gx_stream.cpp). False: the operation could not be
    // captured; the capture was ended and settled, the caller goes on.
    std::uint32_t* captureWords(PacketOp op,std::uint32_t words);
    bool captureEnd(const char* why){if(nativeCapture&&!nativeCapture->ended)nativeCapture->ended=why;return settleNative(),false;}
    std::span<u8> scratch;
    size_t used=0,needed=1,peak=0;
    bool active=false,autoDraw=false;
    size_t autoStride=0;
    StreamError failure=StreamError::none;
    bool flushAutoPrimitives();
    bool flushGroups(size_t stride,bool last);
    bool merge=false,held=false;
    u8 heldPhase=0,heldHigh=0; // header bytes of the next primitive consumed so far
    u8 heldNext=0; // opcode of the primitive asking to join the held one
    // A line strip joining a held line list: its raw vertices start at
    // stripAt and become line pairs once complete (0: none pending).
    size_t stripAt=0,stripCount=0;
    size_t splitLeft=0,splitStride=0; // payload bytes still to come, vertex size
    std::uint64_t merged=0,mergedLines=0,split=0,splitChunk=0;
    std::uint32_t splitLargest=0;
    // Merge key: line lists and line strips of one vertex format join.
    static u8 heldOpcode(u8 opcode){return (opcode&0xf8)==GX_LINESTRIP?u8(GX_LINES|(opcode&7)):opcode;}
    bool executeScratch();
    bool flushHeld();
    bool continueHeld(std::span<const u8>& bytes);
    bool holdCompleted();
    void expandStrip(size_t at,size_t count,size_t stride);
    bool beginSplit();
    bool continueSplit(std::span<const u8>& bytes);
    bool writeCommands(std::span<const u8> bytes);
    bool reject(StreamError value){if(failure==StreamError::none)failure=value;return false;}
};
// TLS routing for the original GXCmd/GXCallDisplayList ABI. Install only while
// idle. The caller must detach before destroying the stream; no implicit sink.
bool setCommandStream(CommandStream* stream);
CommandStream* commandStream(); // Render-thread adapter access; caller owns lifetime.
}
