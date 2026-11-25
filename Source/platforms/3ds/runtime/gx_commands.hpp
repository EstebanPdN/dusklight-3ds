#pragma once
#include <limits>
#include "gx_vertex_decode.hpp"
#include "gx_copy_setup.hpp"
#include "gx_batch_cache.hpp"
#include <bitset>
#include <cstring>
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
    // Address span of all live regions: most array bindings are live game
    // arrays outside it and skip the table scan.
    std::uintptr_t lowest=std::numeric_limits<std::uintptr_t>::max(),highest=0;
    void rebound();
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
    // Bumped by every value-changing register write except the ones made for
    // each shape and always re-read by a draw: position/normal matrix memory
    // (XF 0x000-0x077, 0x400-0x459) and the matrix index registers (CP 0x30/
    // 0x40, XF 0x1018/0x1019). Equal epochs prove that plans derived from the
    // rest of the bank (raster, material, lights, texgen, fog, textures,
    // vertex layout) still apply. Array bindings are not covered.
    std::uint32_t stateEpoch=0;
    // Bumped when the vertex descriptor/format registers, XF 0x1008 or an
    // array binding change. A vertex-setup display list identical to the last
    // one executed is a no-op while this stands.
    std::uint32_t vertexSetupEpoch=0;
    CopySetup copy{};
    // Hashes of the register words the native state key reads
    // (nativeStateKey, gx_command_draw.cpp), one per component, updated by
    // every write that changes one of those words. Each is the XOR over its
    // words of mix(word^seed)^mix(seed), so it depends only on the current
    // values, never on the order of the writes, and is 0 for a zeroed bank;
    // a single changed word always changes it (mix is a bijection). Bank
    // copies carry it along. tracked: kept by a CommandProcessor; a bank
    // built by hand has no valid hash (stateHashComponents refuses it).
    struct StateHash {std::uint32_t bp{},texture{},cp{},xf{},tracked{};} stateHash{};
};
// The words a processor stores into its bank's CP, BP and XF arrays, in the
// order it stores them, for the second-core worker's copy of the bank
// (gx_bank_delta.hpp). The log is a list of runs in that copy's own format:
// one word (index of the first bank word << 16 | words), then the values.
// A single register is a run of one (joining neighbours cost more on every
// store than the header word it saved now and then). Every store into those
// three arrays that changes a word is logged (gx_commands.cpp; matrix memory,
// which changes with every shape, is logged without the comparison).
// `grewValid`: a store made a word valid; the valid bits are then compared
// like the rest of the bank (colour latches, palette loads, epochs, hashes),
// which is small. `lost`: not every store since the last restart is in the
// log (no buffer, it filled up, or the whole bank was replaced); the worker
// then gets the whole bank.
struct BankLog {
    std::uint32_t* words{};std::uint32_t capacity{},used{};
    bool lost{true},grewValid{true};
    void restart(){used=0;lost=words==nullptr;grewValid=false;}
    void store(std::uint32_t index,std::uint32_t value){
        if(lost)return;
        if(used+2>capacity){lost=true;return;}
        words[used]=index<<16|1u;words[used+1]=value;used+=2;
    }
    void store(std::uint32_t index,const std::uint32_t* values,std::uint32_t count){
        if(lost||!count)return;
        if(count>0xffffu||used+1+count>capacity){lost=true;return;}
        words[used++]=index<<16|count;std::memcpy(words+used,values,count*sizeof(std::uint32_t));used+=count;
    }
};
// One changed word of the bank (CommandProcessor::collectChanges).
struct BankWord {
    enum Kind : std::uint16_t {cp,bp,tevColor,tlutToken,tlutOffset,tlutEntries,xf,matrixIndex};
    std::uint16_t kind,index;std::uint32_t value;
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
// Display list templates (CommandProcessor::executeList): replays, templates
// built, calls left to the interpreter and, with verification on, replays
// whose registers differed from the interpreter (must stay zero).
void compiledListStatistics(std::uint64_t& hits,std::uint64_t& compiles,std::uint64_t& interpreted,std::uint64_t& mismatches);
// packetPaint: template replays that took the fast run, vertex setup lists
// left out because the bank held them, and (verify mode) held lists whose
// run would have changed the bank or the bindings (must stay zero).
void paintListStatistics(std::uint64_t& fast,std::uint64_t& held,std::uint64_t& heldMismatches);
// enabled: A/B switch. verify: run both and compare the complete register bank.
void setCompiledLists(bool enabled,bool verify);
// J2D material memo (J2DMaterial::setGX; only the thread that owns the
// native stream). The typed setters of a material are recorded once as
// compiled register writes, the format of compiled display lists, and
// replayed while the material's bytes and every register its setters read
// (its footprint) hold the values they had at the recording: the setters
// would then make the same writes, so the replay leaves the register bank,
// epochs included, exactly as they would (a recorded write merges with the
// bank as the setter's write does). A setter that reads a register to choose
// or skip a write needs that register in the footprint (mixMaterialFootprint);
// the 'v' mode catches one that is missing. Texture matrices come from
// J2DTexMtx objects outside the material: each load is declared with
// materialMemoSource and the replay requires the same object bytes. The TEV
// block's texture loads stay live, between the two recorded segments.
// Why a recording could not be stored (the first cause seen): a processor
// entry other than a typed register write (execute, display-list call, any
// other: reset, copy, indexed load, descriptors, array binding), texture
// words outside the TEV block's first writes, matrix memory without a
// declared, matching source object, the split missing, repeated or behind a
// pending BP mask, a BP mask left pending at the end, recorder capacity, or
// no writes / another processor.
enum class MaterialMemoRefusal:std::uint8_t {none,execute,list,foreign,textureOrder,matrix,split,mask,capacity,processor,count};
struct MaterialMemoStatistics {
    std::uint64_t calls{},replays{},records{},stores{},refused{},volatileSkips{},bypassed{},poolResets{},verified{},mismatches{};
    std::array<std::uint64_t,unsigned(MaterialMemoRefusal::count)> refusedBy{}; // refused recordings by cause
    // givenUp: calls on a material whose recordings kept being refused, sent
    // to the setters before any checksum. sourceChanges: a variant matched
    // but a texture matrix object it read had changed. footprintMisses:
    // recordings of material bytes already recorded under other footprint
    // registers (the state left by the previous draw differed).
    std::uint64_t givenUp{},sourceChanges{},footprintMisses{};
    unsigned poolWords{}; // pool words in use when read
    bool storageFailed{}; // the store could not be allocated: every call bypassed
};
void materialMemoStatistics(MaterialMemoStatistics& out,bool reset);
// enabled: A/B switch. verify: every replay and recording is checked against
// the original setters (register bank and recorded writes); a mismatch
// retires that entry.
void setMaterialMemo(bool enabled,bool verify);
bool materialMemoEnabled();
bool materialMemoVerifying();
struct MaterialMemoSum {std::uint32_t a=2166136261u,b=0x9e3779b9u;};
void materialMemoChecksum(MaterialMemoSum& sum,const void* data,std::size_t size);
enum class MaterialMemoAction:std::uint8_t {plain,replay,record};
struct MaterialMemoTicket {
    MaterialMemoAction action{};
    bool verify{},stored{}; // stored: a verified replay, checked against its entry
    std::uint8_t variant{};std::uint16_t slot{};
};
// plain: run the original setters unrecorded. replay: segment 0, the TEV
// block's texture loads, segment 1. record: run the original setters with
// materialMemoSplit() before the TEV block, then materialMemoFinish().
// The caller has finished the stream and uses its registers.
MaterialMemoTicket materialMemoLookup(const CommandRegisters& registers,const void* material,MaterialMemoSum sum,unsigned tevStages,unsigned texGens);
void materialMemoBypass(); // a call the caller kept on the original path
void materialMemoReplay(const MaterialMemoTicket& ticket,unsigned segment); // 0 colour/texgen, 1 TEV/indirect/PE
void materialMemoSplit();
struct MaterialMemoVerdict {
    bool mismatch{};const char* reason{};
    char space{}; // 'B' BP, 'C' CP, 'X' XF, 'T' TEV colour bank, 'E' epochs/other, 0 none
    unsigned index{};std::uint32_t original{},replayed{};
};
MaterialMemoVerdict materialMemoFinish(const MaterialMemoTicket& ticket);
void materialMemoForget(const void* material);
// Before the checksum: true when this material's recordings kept being
// refused (a structural cause); the caller then runs the setters plainly.
// Such a material is examined again every 256 calls. Counts as a call.
bool materialMemoGivenUp(const void* material);
// J2DTexGenBlock::setGX, right before a texture matrix load: the J2DTexMtx
// object (size bytes) and the matrix inside it that the load sends. Only
// acts while a recording runs on these registers: the next matrix-memory
// write must send exactly that matrix, and the stored entry then requires
// the same object bytes on every replay.
void materialMemoSource(const CommandRegisters& registers,const void* object,std::size_t size,const void* matrix);
bool materialMemoRecording(); // a recording runs: lets callers skip the stream lookup
// Incremental state key components (CommandRegisters::stateHash). On, the
// four components of nativeStateKey come from the processor's running hashes
// instead of a rehash of about 450 register words whenever the state epoch
// moves. 'v' recomputes them from the registers on each epoch change and
// counts differences (must stay zero). Returns false (caller rehashes) when
// off or for a bank no processor keeps.
bool stateHashComponents(const CommandRegisters& registers,std::uint32_t& bp,std::uint32_t& texture,std::uint32_t& cp,std::uint32_t& xf);
CommandRegisters::StateHash computeStateHash(const CommandRegisters& registers); // from the words, tracked=0
void setStateHash(bool enabled,bool verify);
struct StateHashStatistics {std::uint64_t keys{},checks{},mismatches{};char firstComponent{};};
void stateHashStatistics(StateHashStatistics& out,bool reset);
// Typed masked BP writes (writeBpMasked): calls, and those whose value left
// the register as it was (no merge or epoch work).
struct TypedWriteStatistics {std::uint64_t masked{},unchanged{};};
void typedWriteStatistics(TypedWriteStatistics& out,bool reset);
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
    explicit CommandProcessor(CommandMemory& memory):memory(memory){regs.stateHash.tracked=1;}
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
    // Read counters only from their owning draw sink, after the worker has drained.
    const void* contextForSink(CommandDrawSink expected) const {
        return sink == expected ? sinkContext : nullptr;
    }
    void setSink(CommandDrawSink value,void* context=nullptr){
        if(active){reject(CommandError::reentrant,0);return;}
        sink=value;sinkContext=context;
    }
    // Called before each draw this processor prepares, ahead of anything it
    // shares with another processor (the topology cache) and of the sink.
    // The game thread's processor waits there for the second-core worker
    // (gx_worker.hpp); the worker's own processor has none.
    using DrawBarrier=void(*)();
    void setDrawBarrier(DrawBarrier value){if(!active)drawBarrier=value;}
    // Called with each run of consecutive primitive packets before this
    // processor draws them (and before the draw barrier). True: the callee
    // took the run (the second-core worker's queue copies its bytes and
    // draws it later under the bank and bindings of this moment); the
    // processor then goes on behind it. `arraysFixed`: every array the run's
    // vertex formats index lies in immutable memory (setImmutableMemory), or
    // they index none; otherwise whoever reads the run later depends on the
    // game leaving those arrays alone until then.
    using RunDeferral=bool(*)(std::span<const u8> run,bool arraysFixed,void* context);
    void setRunDeferral(RunDeferral value,void* context=nullptr){if(!active){runDeferral=value;runDeferralContext=context;}}
    // Memory that never changes while the program runs (its code and
    // constant data).
    void setImmutableMemory(const void* begin,const void* end){
        if(!active){immutableBegin=reinterpret_cast<std::uintptr_t>(begin);immutableEnd=reinterpret_cast<std::uintptr_t>(end);}
    }
    bool execute(std::span<const u8> bytes);
    // Display-list call. A list made only of register loads (J3D material and
    // shape-packet lists) is compiled once into host-word register writes and
    // replayed while its bytes are unchanged; every other list, a changed
    // list and the first sight of any list take execute(). Same register
    // results, epochs and command counts as execute().
    bool executeList(std::span<const u8> bytes){return executeList(bytes,false);}
    // packetPaint (perf_features.hpp; the caller asks the switch): a template
    // hit replays through runTemplateFast. Same bank, epochs, hashes, log and
    // command counts.
    bool executeList(std::span<const u8> bytes,bool fast);
    // packetPaint: a list of CP loads, XF loads and array bases only (a J3D
    // vertex setup list) whose every value the bank and the array bindings
    // already hold. Running it would store no word, move no epoch and log
    // nothing; true: what it would still do (the command count, the normal
    // alias reset of CP 0x50 and XF 0x1008, the material recorder's note)
    // is done and the caller does not run it. False: run it.
    bool listHeld(std::span<const u8> bytes);
    // Native typed setters use host words directly, sharing list semantics
    // without a serialize/parse round trip. Each successful call counts once.
    bool writeCp(u8 reg,u32 value);
    bool writeBp(u8 reg,u32 value);
    // writeBp(0xfe,mask) then writeBp(reg,value) in one call: same bank,
    // epochs, command count (two) and material recording. A value that
    // leaves the register as it is skips the merge and epoch work.
    bool writeBpMasked(u8 reg,u32 mask,u32 value);
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
    // Retained 2D screens (screen_tape.hpp). collectChanges lists every word,
    // colour latch and palette load of this bank that differs from `before`,
    // a copy of it taken before a recorded screen. applyChanges gives the
    // bank those values again, through the same bookkeeping a write has
    // (epochs, state hash, decode cache, matrix index). `ok` is false when
    // the list does not fit `out` or the screen left something a replay
    // cannot restore (a pending BP mask, a frame-copy setup, a palette table
    // overflow); the count is then not usable.
    unsigned collectChanges(const CommandRegisters& before,std::span<BankWord> out,bool& ok)const;
    bool applyChanges(std::span<const BankWord> words);
    // The stores into the bank's arrays since the last restart (BankLog).
    // `buffer` stays with the processor; an empty one turns the log off.
    void attachBankLog(std::span<std::uint32_t> buffer){bankLog={buffer.data(),std::uint32_t(buffer.size()),0,true,true};bankLog.restart();}
    const BankLog& bankStores()const{return bankLog;}
    void restartBankLog(){bankLog.restart();}
    // Second-core worker (gx_worker.hpp): this processor takes a whole bank
    // and the array bindings another processor had when a shape was issued,
    // to run that shape's display list as that processor would have. Not
    // while executing. Derived state (vertex format plans) is checked against
    // the bank on demand.
    struct ArrayBindings {alignas(8) std::array<std::uint8_t,16*32> bytes{};};
    ArrayBindings arrayBindings()const;
    // The same bytes in place (they change with the next array binding).
    const ArrayBindings& currentArrayBindings()const{return *reinterpret_cast<const ArrayBindings*>(bindings.data());}
    bool adopt(const CommandRegisters& bank,const ArrayBindings& arrays);
    // Execute a worker list under its issued state. A single primitive can read
    // the queue's bank for the synchronous sink call. Mixed lists still need an
    // independent, writable bank. The processor is scratch for this operation.
    bool executeQueued(std::span<const u8> list,const CommandRegisters& bank,const ArrayBindings& arrays);
    // ---- Native packets (runtime/native_packets.hpp) ----
    // The setup a second processor needs to resolve the same arrays.
    CommandMemory& memoryRegistry()const{return memory;}
    LiveArrayResolver liveResolver()const{return liveArrayResolver;}
    // The second-core worker's copy of the game bank lives in a processor of
    // its own (gx_worker.hpp): the queue writes bank records into it, the
    // native packet records run on it. No other user.
    CommandRegisters& mirrorRegisters(){return regs;}
    void adoptArrays(const ArrayBindings& arrays){if(!active)std::memcpy(bindings.data(),arrays.bytes.data(),sizeof(bindings));}
    // One array binding as the Aurora array-base command of a display list
    // makes it (no stride write; bindArray adds CP 0xB0+slot).
    bool rebind(unsigned slot,std::uint64_t address,u32 size,bool little);
    // An indexed matrix load whose array bytes were read already: the same
    // bank result as loadIndexed with `source` at the array position it
    // reads (`count` words, packed>>12 plus one).
    bool loadIndexedValues(u8 code,u16 packed,std::span<const u8> source,bool little);
    // Monotonic epoch advance (a run of native packets sealed into the bank:
    // the worker's copy counted its own changes, the bank must end above).
    void advanceEpochs(std::uint32_t state,std::uint32_t setup){regs.stateEpoch+=state;regs.vertexSetupEpoch+=setup;}
    void invalidateVertexCache(std::uint32_t count){regs.vertexCacheEpoch+=count;}
    // A captured operation may change the vertex setup: the epoch moves so
    // that a cache keyed on it (the vertex setup memo) does not skip work.
    void touchVertexSetup(){++regs.vertexSetupEpoch;}
    // The worker's copy goes on after an op its game thread had accepted
    // failed there (counted by the caller).
    void clearFailure(){if(!active){failure=CommandError::none;failureOffset=0;}}
    std::uint32_t pendingBpMask()const{return regs.bpMask;}
private:
    // Replays recorded material writes. Any new state-changing entry point
    // must feed the material recorder (recordMaterial* or noteForeignCommand
    // in gx_commands.cpp), or recordings would miss its writes.
    friend struct MaterialMemoAccess;
    struct Binding {MemoryHandle handle=0;u32 offset=0,size=0;bool little=false;std::uint64_t liveAddress=0;};
    struct Cached {VertexDecodePlan plan;aurora::gx::dl::VtxLayout layout;
        std::array<u32,5> signature{};bool ready=false,created=false;
        bool indexed=false;}; // an attribute reads an array
    CommandMemory& memory;
    CommandRegisters regs{};
    BankLog bankLog{};
    std::array<Binding,16> bindings{};
    std::array<Cached,8> cache{};
    CommandDrawSink sink=nullptr;void* sinkContext=nullptr;
    DrawBarrier drawBarrier=nullptr;
    RunDeferral runDeferral=nullptr;void* runDeferralContext=nullptr;
    std::uintptr_t immutableBegin=0,immutableEnd=0;
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
    void applyIndexedWords(u32 address,u32 count,const u8* source,bool little);
    bool resolveBinding(const Binding& binding,std::span<const u8>& bytes) const;
    void runCompiled(const u32* words,unsigned count);
    // A display list by its template (gx_commands.cpp, executeList): each
    // command through the interpreter's own functions, its values read from
    // the list. False: a command was refused (as execute would refuse it).
    bool runTemplate(const u32* entries,unsigned count,const u8* bytes);
    // runTemplate with the no-op answers inline (gx_commands.cpp).
    bool runTemplateFast(const u32* entries,unsigned count,const u8* bytes);
    void applyPlainBpChange(u32 reg,u32 value);
    bool bindingHeld(unsigned slot,std::uint64_t address,u32 size,bool little)const;
    bool listHeldCheck(std::span<const u8> bytes,unsigned& commands,bool& normals)const;
    bool prepare(unsigned vat,const CommandRegisters& bank);
    bool prepare(unsigned vat){return prepare(vat,regs);}
    bool arraysImmutable(unsigned vat)const;
    bool draw(std::span<const u8> vertices,std::span<const u8> indices,u8 opcode,u16 count,u32 indexCount,size_t offset,const CommandRegisters& bank);
    bool drawBatch(std::span<const u8> bytes,u8 vat,size_t offset,size_t& consumed);
};
}
