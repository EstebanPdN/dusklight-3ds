#pragma once
#include "gx_vertex_decode.hpp"
#include "gx_copy_setup.hpp"
#include "gx_batch_cache.hpp"
#include <bitset>
#include <cstdint>

namespace dusk::native::gx {
using MemoryHandle = std::uint64_t;
class CommandMemory {
public:
    static constexpr unsigned capacity=128;
    // Borrowed storage: unregister before freeing, with no active execute/sink.
    // Handles contain a slot and generation, enabling constant-time resolution.
    bool add(std::span<const u8> bytes,MemoryHandle& handle);
    bool remove(MemoryHandle handle);
    bool find(std::uint64_t address,u32 size,MemoryHandle& handle,u32& offset) const;
    bool resolve(MemoryHandle handle,u32 offset,u32 size,std::span<const u8>& bytes) const;
private:
    struct Region { MemoryHandle handle=0;std::span<const u8> bytes; };
    std::array<Region,capacity> regions{};
    MemoryHandle next=1;
};
enum class CommandError { none, truncated, unsupported, format, unregisteredMemory, staleMemory,
                          arrayBounds, invalidIndex, missingSink, sinkRejected, reentrant };
struct CommandRegisters {
    std::array<u32,256> cp{},bp{};
    std::array<u32,0x1058> xf{};
    std::bitset<256> cpValid{},bpValid{};
    std::bitset<0x1058> xfValid{};
    // BP E0-E7 addresses two separate color banks selected by bit 23.
    // Keep each latched RA/BG pair, not just the last raw BP value.
    std::array<std::array<u32,8>,2> tevColor{}; // signed TEV, unsigned konst
    std::bitset<16> tevColorValid{};
    // Native TLUT loads latch opaque bounded image/palette tokens in command
    // order. Address/capacity preserve original TMEM selection; no ARM reads.
    struct TlutLoad {u32 token{},offset{},entries{};};
    std::array<TlutLoad,32> tluts{};
    bool tlutOverflow{};
    // CP 0x30/0x40 and XF 0x1018/0x1019 select the same matrices.
    // Preserve command order across both register families.
    std::array<u32,2> matrixIndex{};
    std::bitset<2> matrixIndexValid{};
    u32 bpMask=0xffffff;
    std::uint64_t vertexCacheEpoch=0;
    CopySetup copy{};
};
struct CommandDraw {
    GXPrimitive primitive;
    u16 vertexCount;
    u32 indexCount;
    std::span<const u8> vertices,indices; // Prebuilt indices are host-endian u16.
    const VertexDecodePlan& plan;
    const VertexArrays& arrays;
    const CommandRegisters& registers;
    bool vertex(u32 index,DecodedVertex& output,VertexDecodeError* error=nullptr) const;
    bool triangleIndex(u32 index,u16& output) const;
};
using CommandDrawSink=bool(*)(const CommandDraw&,void*);
struct CommandStatistics { std::uint64_t commands=0,draws=0,plans=0,mergedPrimitives=0,reusedVertices=0,batchCacheHits=0,batchCacheMisses=0; };
struct PrimitiveBatchScratch {
    std::span<u8> vertices;
    std::span<u16> indices;
    unsigned maxVertices=4096;
    // Optional exact packed-vertex deduplication. Power-of-two hash capacity
    // must be at least twice the admitted input count; remap covers that count.
    std::span<u16> hashSlots{},remap{};
};
// A single render-thread context. Registered memory and callbacks must remain
// live for the call; a sink consumes/copies its borrowed draw before returning.
// Geometry is not expanded or allocated here. The sink handles transforms,
// lighting/materials and GPU submission, and must report unsupported states.
class CommandProcessor {
public:
    explicit CommandProcessor(CommandMemory& memory):memory(memory){}
    // Original game arrays are borrowed synchronously. A platform provider
    // validates their live allocation on bind and again before each read.
    // Diagnostics retain explicit generation-checked registrations by default.
    using LiveArrayResolver=bool(*)(std::uint64_t,u32);
    void setLiveArrayResolver(LiveArrayResolver value){if(!active)liveArrayResolver=value;}
    // Optional caller-owned temporary storage. Only consecutive triangle-like
    // packets sharing the same VAT are grouped; state writes are hard barriers.
    // Optional exact deduplication and packet snapshots preserve triangle order.
    void setPrimitiveBatchScratch(PrimitiveBatchScratch value){if(!active)batchScratch=value;}
    void setPrimitiveBatchCache(PrimitiveBatchCache* value){if(!active){batchCache=value;if(value)value->clear();}}
    void setSink(CommandDrawSink value,void* context=nullptr){
        if(active){reject(CommandError::reentrant,0);return;}
        sink=value;sinkContext=context;
    }
    bool execute(std::span<const u8> bytes);
    // Native typed setters use host words directly, sharing list semantics
    // without a serialize/parse round trip. Each successful call counts once.
    bool writeCp(u8 reg,u32 value);
    bool writeBp(u8 reg,u32 value);
    bool writeXf(u16 address,std::span<const u32> values);
    bool loadIndexed(u8 code,u16 index,u16 packed);
    bool setCopySource(bool texture,u32 left,u32 top,u32 width,u32 height);
    bool setCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap);
    bool setCopyControl(bool texture,u32 mask,u32 value);
    bool clearDescriptors();
    bool setDescriptor(unsigned attribute,unsigned type);
    bool queryDescriptor(unsigned attribute,unsigned& type) const;
    bool bindArray(unsigned slot,const void* data,u32 size,u8 stride,bool little);
    // Inspect one command prefix for incremental writers. Returns false while
    // its header is incomplete (required is the next header size), or on error.
    // On success required is the full packet size; payload may still be absent.
    // May prepare a VAT plan, but never applies registers or submits geometry.
    bool frame(std::span<const u8> prefix,size_t& required);
    CommandError error()const{return failure;}
    size_t errorOffset()const{return failureOffset;}
    const CommandRegisters& registers()const{return regs;}
    CommandStatistics statistics()const{return stats;}
    bool executing()const{return active;}
    void reset(); // Clears state/errors/bindings, preserves registry and sink.
private:
    struct Binding {MemoryHandle handle=0;u32 offset=0,size=0;bool little=false;std::uint64_t liveAddress=0;};
    struct Cached {VertexDecodePlan plan;aurora::gx::dl::VtxLayout layout;
        std::array<u32,5> signature{};bool ready=false,created=false;};
    CommandMemory& memory;
    CommandRegisters regs{};
    std::array<Binding,16> bindings{};
    std::array<Cached,8> cache{};
    CommandDrawSink sink=nullptr;void* sinkContext=nullptr;
    LiveArrayResolver liveArrayResolver=nullptr;
    PrimitiveBatchScratch batchScratch{};
    PrimitiveBatchCache* batchCache{};
    CommandError failure=CommandError::none;size_t failureOffset=0;
    CommandStatistics stats{};bool active=false;
    bool trustedIndices=false; // native engine: topology-cache indices were validated on store
    bool normalAliasesValid=false,hasNormal=false,hasNBT=false;
    u8 normalType=0;
    bool reject(CommandError error,size_t offset);
    bool applyCp(u8 reg,u32 value,size_t offset);
    void applyBp(u8 reg,u32 value);
    bool applyCopySource(bool texture,u32 left,u32 top,u32 width,u32 height,size_t offset);
    bool applyCopyDestination(bool texture,u32 width,u32 height,u32 format,bool mipmap,size_t offset);
    void updateMatrixIndices(u32 address,u32 count);
    bool submitDescriptors(u32 low,u32 high,bool normal,bool nbt,u8 type);
    bool applyBinding(unsigned slot,std::uint64_t address,u32 size,bool little,size_t offset);
    bool applyIndexed(u8 code,u16 index,u16 packed,size_t offset);
    bool resolveBinding(const Binding& binding,std::span<const u8>& bytes) const;
    bool prepare(unsigned vat);
    bool draw(std::span<const u8> vertices,std::span<const u8> indices,u8 opcode,u16 count,u32 indexCount,size_t offset);
    bool drawBatch(std::span<const u8> bytes,u8 vat,size_t offset,size_t& consumed);
};
}
