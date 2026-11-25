#pragma once
// Native PICA blocks: the command words of a recorded shape, kept and sent
// again (gx_command_draw.cpp nativeReplay, gx_pica.cpp submitNativeRigid and
// submitPicaBlock).
//
// The first replay of a native shape record under a given set of inputs
// sends the shape's whole PICA state (every Citro3D group it uses marked
// changed: program configuration, attribute and buffer layout, raster state,
// texture units, combiners, lighting switch, its uniform rows and constants)
// and the draw; the words between the two ends are kept. A later replay of
// the same record whose inputs are proven unchanged copies those words into
// the command buffer (GPUCMD_AddRawCommands, never a jump into this storage)
// with only the matrix palette rows rewritten, and skips the material, raster,
// colour, fog, texgen and uniform planning, the state application and
// Citro3D's emission.
//
// What a block depends on, and how a replay proves it unchanged:
// - the record (plans, resident vertices and indices, palette layout): its
//   facts are kept beside the words and compared (gx_command_draw.cpp);
// - the register words outside the record key that feed the uniform rows
//   other than the palette (projection and viewport, fog curve, ambient and
//   material colours, the lights of the channel masks, the texture and post
//   matrices of the texgens): kept with their validity and compared;
// - TEV and fog colours: the record's dynamic signature;
// - texture identity and storage: the resolved handles and scales, and the
//   texture storage serial (a retired or moved image changes it);
// - the target extent.
// Anything else, or anything not provably identical, takes today's path.
//
// This header and pica_blocks.cpp are portable (host test:
// Tools/3ds/test_pica_blocks.py); the Citro3D side is in gx_pica.cpp.
#include "gx_commands.hpp"
#include "gx_rigid_vertex.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dusk::native::blocks {
enum class Mode : std::uint8_t { off, on, verify };
// Why a replay did not use a block.
enum class Miss : std::uint8_t {
    none,       // no block yet for this record (first replay: captured)
    record,     // the record changed (re-recorded, new constants, switches)
    inputs,     // a register word the uniform rows are made from changed
    dynamic,    // TEV or fog colours changed
    target,     // target extent changed
    storage,    // a cached texture was retired or moved
    texture,    // texture resolution, binding or scales differ
    palette,    // the palette placement refused or laid out differently
    resting,    // inputs keep changing: no capture for a while
    room,       // no room in the command buffer, or the submit refused
    kind,       // a record kind blocks do not take (projected, paired texgen...)
    count
};
// Why a capture was not kept.
enum class Refusal : std::uint8_t { kind, span, size, rows, pool, count };

// A run of register words a block's rows are made from: XF (bank 0) or BP (1).
struct Range { std::uint16_t first{},count{}; std::uint8_t bank{}; };
constexpr unsigned maxRanges=24;
struct InputPlan { std::array<Range,maxRanges> ranges{}; std::uint32_t count{}; };
// The ranges for a record whose texgens read `coordinates` (bit n: GX
// coordinate n). False: a texgen reads a position matrix slot (changes with
// every shape) or names a slot or post row that cannot be planned.
bool planInputs(const gx::CommandRegisters& r,std::uint8_t coordinates,InputPlan& out);
// Words the snapshot of `plan` takes (values, then one validity mask per range).
unsigned inputWords(const InputPlan& plan);
void takeInputs(const gx::CommandRegisters& r,const InputPlan& plan,std::uint32_t* out);
bool sameInputs(const gx::CommandRegisters& r,const InputPlan& plan,const std::uint32_t* kept);
// What kind of input differs first (-1: none): 0 colours, 1 projection and
// viewport, 2 fog, 3 a light, 4 a texture or post matrix.
enum InputKind : int { inputColour, inputProjection, inputFog, inputLight, inputTexgen, inputKinds };
int changedInput(const gx::CommandRegisters& r,const InputPlan& plan,const std::uint32_t* kept);

// Where the float32 uniform data of a captured command run lies: `rowWord`
// counts C3D_FVec words (row * 4 + word; word 0 is w ... word 3 is x, the
// order Citro3D uploads them in), `at` is the word of the run.
struct Segment { std::uint16_t at{},count{},rowWord{}; };
constexpr unsigned maxSegments=16;
struct UniformLayout {
    std::array<Segment,maxSegments> segments{};
    std::uint32_t count{};
    // Rows written as float32 data and as float24 data (bit n of word n/32).
    std::array<std::uint32_t,3> float32Rows{},float24Rows{};
};
// False: a malformed run, a masked uniform write, a row past 95 or too many segments.
bool findUniformWords(std::span<const std::uint32_t> run,UniformLayout& out);
// The register and uniform row a word of a run belongs to (verify reports).
struct WordPlace { std::uint32_t reg{};std::int32_t row{-1};bool header{}; };
WordPlace describeWord(std::span<const std::uint32_t> run,std::uint32_t where);

// The palette of a record as a replay places it (nativeReplay: the first
// matrix, then the recorded order, compact when recorded so).
struct PaletteShape {
    std::array<std::int8_t,10> matrixOffsets{},normalOffsets{};
    std::array<std::uint8_t,10> matrixOrder{};
    std::uint8_t matrixCount{},multiRowUnits{3},lights{};
    bool compactPalette{},multiTexture{},normalsUnused{};
    std::array<float,3> origin{};
};
// Places the palette into `plan` (rows 0-5 and from paletteBase(); the other
// rows are left as they are) and folds the record origin into the position
// rows exactly as finishCompactRigidUniforms does. False when today's path
// would refuse the placement or lay it out differently.
bool placePalette(const gx::CommandRegisters& r,unsigned position,const PaletteShape& shape,gx::RigidVertexPlan& plan);
// A set of uniform rows: [0,firstEnd) and [second,end).
struct PaletteRows { unsigned firstEnd{},second{},end{}; bool operator==(const PaletteRows&)const=default; };
// The rows placePalette writes.
PaletteRows paletteRows(const gx::RigidVertexPlan& plan);
// Every row submitNativeRigid uploads for `plan` (uploadRigidUniforms in
// gx_pica.cpp): the palette and everything below it, then the unit and
// material rows of the multi-texture program, or the material row of the
// single-texture one.
PaletteRows uploadRows(const gx::RigidVertexPlan& plan);
// True when every word of those rows (the shader's array starts at uniform
// `base`) is uploaded as float32 by the run and no float24 constant writes them.
bool rowsCovered(const UniformLayout& layout,unsigned base,const PaletteRows& rows);
// Writes those rows of `plan` into the run's uniform data words.
void patchRows(std::uint32_t* run,const UniformLayout& layout,unsigned base,const gx::RigidVertexPlan& plan,const PaletteRows& rows);

// A kept block (in the pool, followed by its words, input snapshot, input
// plan, uniform layout and the caller's facts). Alignment 4: no 64-bit members.
struct Block {
    std::uint32_t wordCount{},inputsAt{},planAt{},layoutAt{},factsAt{},factBytes{};
    std::uint32_t capacity{}; // pool words this block holds (a new capture of its slot that fits reuses them)
    PaletteShape palette{};
    std::uint16_t uniformBase{};
    std::uint8_t textureUnits{},flags{};
    // Rows mode: inputs changed, today's planner made every row, the block
    // takes them all. Valid when its words upload exactly `uploaded`.
    bool rowsMode{};PaletteRows uploaded{};
    std::uint32_t dynamic{},storageSerial{};
    std::array<std::uint32_t,6> handles{}; // three 64-bit texture handles, low word first
    std::array<std::array<float,2>,3> scales{};
    void setHandle(unsigned unit,std::uint64_t h){handles[2*unit]=std::uint32_t(h);handles[2*unit+1]=std::uint32_t(h>>32);}
    std::uint64_t handle(unsigned unit)const{return handles[2*unit]|std::uint64_t(handles[2*unit+1])<<32;}
};
static_assert(alignof(Block)<=4);
struct Statistics {
    std::uint64_t replays{},hits{},captures{},wordsCopied{},recycles{};
    std::array<std::uint64_t,unsigned(Miss::count)> misses{};
    std::array<std::uint64_t,unsigned(Refusal::count)> refusals{};
    std::uint64_t verified{},verifyMismatches{},verifySkipped{};
    std::uint64_t hitTicks{},profiledHits{};
    // Of the hits: with every uniform row from today's planner (inputs changed).
    std::uint64_t rowsHits{},rowsRefused{};
    std::uint64_t inPlace{}; // captures written over their slot's previous block
    std::array<std::uint64_t,inputKinds> inputsChanged{}; // `inputs` misses by the first kind that changed
};
constexpr unsigned maxBlockWords=2048;
class Store {
public:
    // A pool of `words` words for `slots` record slots; `offsets` and `heat`
    // hold `slots` and 2 * `slots` entries.
    void attach(std::uint32_t* pool,std::uint32_t words,unsigned slots,std::uint32_t* offsets,std::uint8_t* heat);
    bool attached()const{return pool!=nullptr;}
    Block* find(unsigned slot){
        if(slot>=slots||offsets[slot]==none)return nullptr;
        return reinterpret_cast<Block*>(pool+offsets[slot]);
    }
    void drop(unsigned slot){if(slot<slots)offsets[slot]=none;}
    void clear();
    // A new block for `slot` (the old one is dropped): words, the input
    // snapshot, plan and the caller's facts are copied in. Null when it does
    // not fit (a full pool is cleared: later captures start again from its
    // beginning) or the run has no usable uniform layout.
    Block* keep(unsigned slot,std::span<const std::uint32_t> words,const gx::CommandRegisters& r,const InputPlan& inputs,
                const UniformLayout& layout,std::span<const std::byte> facts);
    std::uint32_t* words(const Block& b){return reinterpret_cast<std::uint32_t*>(const_cast<Block*>(&b)+1);}
    const std::uint32_t* inputs(const Block& b)const{return reinterpret_cast<const std::uint32_t*>(&b)+b.inputsAt;}
    std::uint32_t* inputs(Block& b){return reinterpret_cast<std::uint32_t*>(&b)+b.inputsAt;}
    const InputPlan& plan(const Block& b)const{return *reinterpret_cast<const InputPlan*>(reinterpret_cast<const std::uint32_t*>(&b)+b.planAt);}
    const UniformLayout& layout(const Block& b)const{return *reinterpret_cast<const UniformLayout*>(reinterpret_cast<const std::uint32_t*>(&b)+b.layoutAt);}
    const std::byte* facts(const Block& b)const{return reinterpret_cast<const std::byte*>(reinterpret_cast<const std::uint32_t*>(&b)+b.factsAt);}
    // Inputs that keep changing: after two misses in a row (no hit between)
    // a record rests for 16, 32, 64 then 128 replays: no capture meanwhile.
    bool resting(unsigned slot);
    // True when this miss starts a rest.
    bool missed(unsigned slot);
    void hit(unsigned slot){if(slot<slots)heat[2*slot]=0;}
    std::uint32_t used()const{return poolUsed;}
    std::uint32_t capacity()const{return poolWords;}
    unsigned kept()const;
    Statistics stats{};
    static constexpr std::uint32_t none=~std::uint32_t(0);
private:
    std::uint32_t* pool{};std::uint32_t poolWords{},poolUsed{};
    std::uint32_t* offsets{};std::uint8_t* heat{};unsigned slots{};
};
// Line 0 (replays by outcome) or 1 (store, captures, verify, time) of the
// cumulative counts, each under 240 characters. `hitMilliseconds`: time of
// the block replays per profiled picture.
int formatReport(char* out,std::size_t size,unsigned line,Mode mode,const Statistics& s,const Store& store,double hitMilliseconds);
}
