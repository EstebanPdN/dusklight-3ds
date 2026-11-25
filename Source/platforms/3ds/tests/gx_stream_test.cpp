#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_leaf_wire.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include "../runtime/gx_state.hpp"
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/os.h>
#include <cassert>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
// No static geometry cache or state reset observer in this test.
thread_local dusk::native::gx::StaticGeometryHint dusk::native::gx::staticGeometryHint{};
void dusk::native::gx::setStateResetObserver(void (*)()){}
void testOriginalCommandAbi(CommandProcessor&);
namespace {
// Every draw the processor hands to the sink, in order, with its own copy of
// the vertex bytes (the sink may not keep pointers into scratch).
struct Drawn {unsigned primitive,count;std::vector<u8> bytes;};
struct Recorder {std::vector<Drawn> draws;};
bool record(const CommandDraw& draw,void* context){
    static_cast<Recorder*>(context)->draws.push_back({unsigned(draw.primitive),draw.vertexCount,{draw.vertices.begin(),draw.vertices.end()}});
    return true;
}
// VAT1: direct F32 XYZ position (12 bytes), optionally a direct RGBA8 colour
// (16 bytes: the night-star vertex of dKyr_drawStar).
void positionFormat(CommandStream& s,bool color){
    assert(s.writeCp(0x50,512|(color?1u<<13:0))&&s.writeCp(0x60,0)&&s.writeCp(0x71,9|(color?(1u<<13)|(5u<<14):0)));
}
void projection(CommandStream& s,u32 type){assert(s.writeXf(0x1026,{&type,1}));}
std::vector<u8> vertexBytes(size_t count,size_t stride,unsigned seed){
    std::vector<u8> bytes(count*stride);
    for(size_t i=0;i<bytes.size();++i)bytes[i]=u8((i*131+i/7+seed*29)&255);
    return bytes;
}
// One fixed-count primitive written as GXBegin/GXPosition/GXEnd would, in
// chunks of `partition` bytes (1 = the scalar ABI byte by byte).
bool primitive(CommandStream& s,u8 opcode,const std::vector<u8>& payload,size_t stride,size_t partition){
    const size_t count=payload.size()/stride;
    const u8 header[3]{opcode,u8(count>>8),u8(count)};
    for(size_t at=0;at<3;at+=partition)if(!s.write(std::span<const u8>(header).subspan(at,std::min(partition,3-at))))return false;
    for(size_t at=0;at<payload.size();at+=partition)
        if(!s.write(std::span<const u8>(payload).subspan(at,std::min(partition,payload.size()-at))))return false;
    return s.endAuto();
}
std::vector<u8> join(std::initializer_list<std::span<const u8>> parts){
    std::vector<u8> out;for(auto part:parts)out.insert(out.end(),part.begin(),part.end());return out;
}
// Vertex `index` of a payload with the given stride.
std::span<const u8> at(const std::vector<u8>& bytes,size_t stride,size_t index){return std::span<const u8>(bytes).subspan(index*stride,stride);}
void testSplitPrimitives(){
    // Zone 84: the mirrored night stars are one GX_TRIANGLES of 695 x 6
    // vertices x 16 bytes + 3 = 66,723 bytes, over the 64 KiB scratch; at
    // full density 7,200 vertices (115,203 bytes). Independent primitives
    // larger than scratch reach the sink in whole-primitive chunks with the
    // same bytes in the same order as one in-capacity draw.
    for(unsigned kind:{GX_TRIANGLES,GX_QUADS,GX_LINES,GX_POINTS})
    for(size_t requested:{size_t(4170),size_t(7200)})
    for(size_t capacity:{size_t(65536),size_t(1000),size_t(67)})
    for(size_t partition:{size_t(1),size_t(4),size_t(13),size_t(65536)}){
        if(capacity==67&&partition==1&&requested==7200)continue; // same path as 4170, keeps the run short
        const size_t group=kind==GX_TRIANGLES?3:kind==GX_QUADS?4:kind==GX_LINES?2:1,count=requested/group*group;
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(capacity);CommandStream s(p,scratch);
        positionFormat(s,true);Recorder r;p.setSink(record,&r);
        const auto payload=vertexBytes(count,16,unsigned(kind+requested));
        assert(primitive(s,u8(kind|1),payload,16,partition));
        assert(s.idle()&&s.finish()&&s.peakBytes()<=capacity&&s.splitDraws()==1&&s.splitChunks()==r.draws.size());
        assert(r.draws.size()>1&&s.largestSplitBytes()==3+count*16);
        std::vector<u8> drawn;
        for(const auto& d:r.draws){assert(d.primitive==kind&&d.count&&d.count%group==0&&d.bytes.size()==d.count*16u);drawn.insert(drawn.end(),d.bytes.begin(),d.bytes.end());}
        assert(drawn==payload);
        // The stream is reusable: a small draw afterwards is one ordinary draw.
        const auto small=vertexBytes(group,16,7);r.draws.clear();
        assert(primitive(s,u8(kind|1),small,16,partition)&&r.draws.size()==1&&r.draws[0].bytes==small);
    }
    // Strips and fans need boundary vertices: still a capacity error, as is
    // a list whose count is not a whole number of primitives.
    for(unsigned kind:{GX_TRIANGLESTRIP,GX_TRIANGLEFAN,GX_LINESTRIP,GX_TRIANGLES}){
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(1000);CommandStream s(p,scratch);
        positionFormat(s,true);Recorder r;p.setSink(record,&r);
        const size_t count=kind==GX_TRIANGLES?4171:5000;
        const u8 header[3]{u8(kind|1),u8(count>>8),u8(count)};
        assert(!s.write(header)&&s.error()==StreamError::capacity&&r.draws.empty()&&s.splitDraws()==0);
    }
    // Merge scope: a held draw is drawn first, then the oversized one splits.
    {
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(65536);CommandStream s(p,scratch);
        positionFormat(s,true);Recorder r;p.setSink(record,&r);assert(s.setImmediateMerge(true));
        const auto first=vertexBytes(6,16,1),stars=vertexBytes(4170,16,2);
        assert(primitive(s,GX_TRIANGLES|1,first,16,4)&&r.draws.empty());
        assert(primitive(s,GX_TRIANGLES|1,stars,16,4));
        assert(s.setImmediateMerge(false)&&s.idle()&&r.draws.size()==3&&r.draws[0].bytes==first);
        assert(join({r.draws[1].bytes,r.draws[2].bytes})==stars&&s.splitDraws()==1);
    }
    puts("PASS oversized independent primitives: whole-primitive chunks, same bytes and order, strips/fans/partial counts still refused");
}
void testLinePointMerge(){
    for(size_t partition:{size_t(1),size_t(5),size_t(4096)}){
        // Orthographic (minimap): consecutive line strips, line lists and
        // point lists become one line list and one point list.
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(65536);CommandStream s(p,scratch);
        positionFormat(s,false);projection(s,1);Recorder r;p.setSink(record,&r);assert(s.setImmediateMerge(true));
        const auto a=vertexBytes(4,12,1),b=vertexBytes(3,12,2),c=vertexBytes(2,12,3),d=vertexBytes(1,12,4),e=vertexBytes(3,12,5),f=vertexBytes(2,12,6);
        assert(primitive(s,GX_LINESTRIP|1,a,12,partition)&&primitive(s,GX_LINESTRIP|1,b,12,partition)&&primitive(s,GX_LINES|1,c,12,partition));
        assert(primitive(s,GX_LINESTRIP|1,d,12,partition)); // one vertex: no segment, drawn on its own as before
        assert(primitive(s,GX_POINTS|1,e,12,partition)&&primitive(s,GX_POINTS|1,f,12,partition));
        assert(s.setImmediateMerge(false)&&s.idle()&&r.draws.size()==3&&s.mergedLineDraws()==2); // b and c joined a; points are not counted
        assert(r.draws[0].primitive==GX_LINES&&r.draws[0].count==12);
        assert(r.draws[0].bytes==join({at(a,12,0),at(a,12,1),at(a,12,1),at(a,12,2),at(a,12,2),at(a,12,3),
                                       at(b,12,0),at(b,12,1),at(b,12,1),at(b,12,2),c}));
        assert(r.draws[1].primitive==GX_LINESTRIP&&r.draws[1].bytes==d);
        assert(r.draws[2].primitive==GX_POINTS&&r.draws[2].bytes==join({e,f}));
    }
    {
        // Perspective: lines and points are never merged or rewritten.
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(65536);CommandStream s(p,scratch);
        positionFormat(s,false);projection(s,0);Recorder r;p.setSink(record,&r);assert(s.setImmediateMerge(true));
        const auto a=vertexBytes(4,12,1),b=vertexBytes(3,12,2),e=vertexBytes(3,12,5),f=vertexBytes(2,12,6);
        assert(primitive(s,GX_LINESTRIP|1,a,12,1)&&primitive(s,GX_LINESTRIP|1,b,12,1));
        assert(primitive(s,GX_POINTS|1,e,12,1)&&primitive(s,GX_POINTS|1,f,12,1));
        assert(s.setImmediateMerge(false)&&r.draws.size()==4&&s.mergedLineDraws()==0);
        assert(r.draws[0].primitive==GX_LINESTRIP&&r.draws[0].bytes==a&&r.draws[1].bytes==b&&r.draws[2].bytes==e&&r.draws[3].bytes==f);
    }
    {
        // Orthographic, small scratch (10 vertices): a strip nothing joins is
        // drawn as the strip it is, byte for byte; a strip whose line pairs
        // with the next one do not fit is drawn first. A state write ends
        // the merge.
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(128);CommandStream s(p,scratch);
        positionFormat(s,false);projection(s,1);Recorder r;p.setSink(record,&r);assert(s.setImmediateMerge(true));
        const auto big=vertexBytes(8,12,1),a=vertexBytes(3,12,2),b=vertexBytes(5,12,3),c=vertexBytes(2,12,4),e=vertexBytes(2,12,5);
        assert(primitive(s,GX_LINESTRIP|1,big,12,1)&&r.draws.empty()); // held as a strip
        assert(primitive(s,GX_LINESTRIP|1,a,12,1)&&r.draws.size()==1);  // 14+4 pairs do not fit
        assert(r.draws[0].primitive==GX_LINESTRIP&&r.draws[0].bytes==big);
        assert(primitive(s,GX_LINESTRIP|1,e,12,1)&&r.draws.size()==1);  // 4+2 pairs held
        assert(primitive(s,GX_LINESTRIP|1,b,12,1)&&r.draws.size()==2);  // 6+8 pairs do not fit
        assert(r.draws[1].primitive==GX_LINES&&r.draws[1].count==6&&
               r.draws[1].bytes==join({at(a,12,0),at(a,12,1),at(a,12,1),at(a,12,2),e}));
        assert(s.writeBp(0x22,0x1234)&&r.draws.size()==3&&r.draws[2].primitive==GX_LINESTRIP&&r.draws[2].bytes==b);
        assert(primitive(s,GX_LINESTRIP|1,c,12,1)&&s.setImmediateMerge(false)&&r.draws.size()==4&&
               r.draws[3].primitive==GX_LINESTRIP&&r.draws[3].bytes==c);
        assert(s.idle()&&s.peakBytes()<=128);
    }
    {
        // A typed setter that would rewrite the line size or a TEV colour
        // with its current value may skip the write while a draw is held:
        // the next strip still joins. Other values, other registers, a
        // different TEV bank or no held draw keep the normal path.
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(65536);CommandStream s(p,scratch);
        positionFormat(s,false);projection(s,1);
        assert(s.writeBp(0x22,0x10030)&&s.writeBp(0xe0,0x800123)&&s.writeBp(0x41,0x4bc));
        Recorder r;p.setSink(record,&r);
        assert(!s.keepsHeldDraw(0x22,0x700ff,0x10030));
        assert(s.setImmediateMerge(true));
        const auto a=vertexBytes(3,12,1),b=vertexBytes(4,12,2);
        assert(primitive(s,GX_LINESTRIP|1,a,12,1)&&r.draws.empty());
        assert(s.keepsHeldDraw(0x22,0x700ff,0x10030)&&s.keepsHeldDraw(0x22,0xff,0x30)&&s.keepsHeldDraw(0xe0,0xffffff,0x800123));
        assert(!s.keepsHeldDraw(0x22,0x700ff,0x31)&&!s.keepsHeldDraw(0x22,0x700ff,0x20030));
        assert(!s.keepsHeldDraw(0xe0,0xffffff,0x000123)&&!s.keepsHeldDraw(0xe2,0xffffff,0x800123)&&!s.keepsHeldDraw(0x41,0xffffff,0x4bc));
        const auto epoch=s.registers().stateEpoch;
        assert(primitive(s,GX_LINESTRIP|1,b,12,1)&&r.draws.empty()&&s.registers().stateEpoch==epoch);
        assert(s.setImmediateMerge(false)&&r.draws.size()==1&&r.draws[0].primitive==GX_LINES&&r.draws[0].count==4+6);
        assert(!s.keepsHeldDraw(0x22,0x700ff,0x10030));
    }
    puts("PASS orthographic line/point merge: strips as segment pairs, same segments and order; perspective and oversize untouched; unchanged line size/TEV colour keep a held draw");
}
// The primitives a draw list rasterises, in order: triangles, quads, line
// segments (a strip's consecutive pairs) and points, with their vertex bytes.
std::vector<std::pair<unsigned,std::vector<u8>>> rasterised(const Recorder& r,size_t stride){
    std::vector<std::pair<unsigned,std::vector<u8>>> out;
    for(const auto& d:r.draws){
        auto push=[&](unsigned kind,size_t first,size_t count){
            out.push_back({kind,std::vector<u8>(d.bytes.begin()+first*stride,d.bytes.begin()+(first+count)*stride)});};
        auto pair=[&](size_t a,size_t b){std::vector<u8> v(d.bytes.begin()+a*stride,d.bytes.begin()+(a+1)*stride);
            v.insert(v.end(),d.bytes.begin()+b*stride,d.bytes.begin()+(b+1)*stride);out.push_back({2,v});};
        switch(d.primitive){
        case GX_TRIANGLES:for(size_t i=0;i+3<=d.count;i+=3)push(0,i,3);break;
        case GX_QUADS:for(size_t i=0;i+4<=d.count;i+=4)push(1,i,4);break;
        case GX_LINES:for(size_t i=0;i+2<=d.count;i+=2)pair(i,i+1);break;
        case GX_LINESTRIP:for(size_t i=0;i+1<d.count;++i)pair(i,i+1);break;
        case GX_POINTS:for(size_t i=0;i<d.count;++i)push(3,i,1);break;
        default:assert(false);
        }
    }
    return out;
}
void testMergeEquivalence(){
    // Random primitive sequences drawn with merging and splitting (small
    // scratch, orthographic merge scope, byte-by-byte writes) rasterise the
    // same primitives in the same order as plain one-draw-per-GXBegin.
    std::uint32_t seed=0x5eed;
    auto next=[&]{seed=seed*1664525u+1013904223u;return seed>>8;};
    unsigned merges=0,splits=0;
    for(unsigned round=0;round<400;++round){
        struct Item{u8 opcode;std::vector<u8> bytes;};
        std::vector<Item> items;
        for(unsigned n=1+next()%12;n--;){
            static const u8 kinds[]{GX_TRIANGLES,GX_QUADS,GX_LINES,GX_LINESTRIP,GX_POINTS};
            const u8 kind=kinds[next()%5];
            const unsigned group=kind==GX_TRIANGLES?3:kind==GX_QUADS?4:kind==GX_LINES?2:1;
            const unsigned count=kind==GX_LINESTRIP?1+next()%12:group*(1+next()%(kind==GX_POINTS?20:8));
            items.push_back({u8(kind|1),vertexBytes(count,12,next())});
        }
        auto run=[&](size_t capacity,bool merge,size_t partition){
            CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(capacity);CommandStream s(p,scratch);
            positionFormat(s,false);projection(s,1);Recorder r;p.setSink(record,&r);assert(s.setImmediateMerge(merge));
            for(const auto& item:items)assert(primitive(s,item.opcode,item.bytes,12,partition));
            assert(s.setImmediateMerge(false)&&s.idle()&&s.peakBytes()<=capacity);
            merges+=unsigned(s.mergedDraws());splits+=unsigned(s.splitDraws());
            return rasterised(r,12);
        };
        const auto plain=run(65536,false,4096);
        for(size_t capacity:{size_t(150),size_t(400),size_t(65536)})for(size_t partition:{size_t(1),size_t(7)})
            assert(run(capacity,true,partition)==plain);
    }
    assert(merges&&splits);
    printf("PASS randomized merge/split equivalence: 400 sequences x 6 scratch/partition cases, %u merges, %u splits\n",merges,splits);
}
}
struct Capture {const u8* pointer=nullptr;unsigned draws=0;};
static bool capture(const CommandDraw& draw,void* context){
    auto& result=*static_cast<Capture*>(context);
    for(u32 i=0;i<draw.vertexCount;++i){DecodedVertex v;if(!draw.vertex(i,v))return false;
        assert(v.position[0]==1&&v.position[1]==2&&v.position[2]==3);}
    result.pointer=draw.vertices.data();++result.draws;return true;
}
int main(){
    // All float bit patterns, including nonfinite payloads, retain their exact
    // wire representation. Semantic validation still belongs to draw decode.
    for(unsigned i=0;i<65536;++i){
        const std::uint32_t bits[]{i*1000003u,i*77999u^0x80000000u,i*7177u^0x7f800000u};
        const std::array<u8,4> color{u8(i),u8(i>>8),u8(i*37),u8(i*71)};
        const auto actual=grassVertexWire(std::bit_cast<float>(bits[0]),std::bit_cast<float>(bits[1]),std::bit_cast<float>(bits[2]),u8(i*13),color,u8(i*17));
        std::vector<u8> expected;
        for(auto value:bits)for(int shift=24;shift>=0;shift-=8)expected.push_back(value>>shift);
        expected.push_back(u8(i*13));expected.insert(expected.end(),color.begin(),color.end());expected.push_back(u8(i*17));
        assert(std::equal(actual.begin(),actual.end(),expected.begin(),expected.end()));
    }
    puts("PASS 65536 original grass wire vertices: exact position bits, normal/UV indices, RGB/alpha and order");

    // Multi-block automatic draws retain every source byte and primitive order,
    // including writes split inside a vertex and buffers not divisible by stride.
    struct AutoCapture {std::vector<u8> bytes;unsigned group,draws=0;};
    for(unsigned primitive:{GX_TRIANGLES,GX_QUADS,GX_LINES,GX_POINTS})
    for(unsigned capacity:{76u,77u,128u,257u,65536u})for(unsigned partition:{1u,4u,11u,8192u,108000u}) for(unsigned stride:{12u,18u}) {
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(capacity);CommandStream s(p,scratch);
        assert(s.writeCp(0x50,512|(stride==18?2048:0))&&s.writeCp(0x60,0)&&s.writeCp(0x71,9|(stride==18?3u<<10:0))); // VAT1 XYZ F32, optional normal S16
        AutoCapture c{{},primitive==GX_TRIANGLES?3u:primitive==GX_QUADS?4u:primitive==GX_LINES?2u:1u};
        p.setSink([](const CommandDraw& d,void* context){auto& c=*static_cast<AutoCapture*>(context);
            assert(d.vertexCount&&d.vertexCount%c.group==0);++c.draws;
            c.bytes.insert(c.bytes.end(),d.vertices.begin(),d.vertices.end());return true;},&c);
        std::vector<u8> input(6000*stride);for(unsigned i=0;i<input.size();++i)input[i]=(i*37+i/17)&255;
        assert(s.beginAuto(primitive|1));
        for(size_t at=0;at<input.size();at+=partition)assert(s.write(std::span<const u8>(input).subspan(at,std::min(size_t(partition),input.size()-at))));
        assert(s.endAuto()&&s.idle()&&c.bytes==input&&c.draws>1&&s.peakBytes()<=capacity);
    }
    for(unsigned failure=0;failure<4;++failure){
        CommandMemory m;CommandProcessor p(m);std::vector<u8> scratch(failure==0?35:52);CommandStream s(p,scratch);
        assert(s.writeCp(0x50,512)&&s.writeCp(0x60,0)&&s.writeCp(0x70,9));
        p.setSink([](const CommandDraw&,void* context){return *static_cast<unsigned*>(context)!=2;},&failure);
        assert(s.beginAuto(failure==1?GX_TRIANGLESTRIP:GX_TRIANGLES));std::array<u8,73> bytes{};
        if(failure<3){assert(!s.write(bytes));assert(s.error()==(failure==2?StreamError::processor:StreamError::capacity));}
        else{assert(s.write(bytes));assert(!s.endAuto()&&s.error()==StreamError::truncated);}
    }
    CommandMemory memory;CommandProcessor processor(memory);u8 scratch[256]{};CommandStream stream(processor,scratch);
    assert(setCommandStream(&stream));testOriginalCommandAbi(processor);assert(stream.finish());
    // Scalars use the original big-endian wire representation, including 64-bit pointers.
    std::array<float,3> position{1,2,3};MemoryHandle handle;
    assert(memory.add({reinterpret_cast<const u8*>(position.data()),sizeof(position)},handle));
    GXCmd1u8(GX_AURORA);GXCmd1u16(GX_AURORA_LOAD_ARRAYBASE);
    GXCmd1u64(reinterpret_cast<std::uintptr_t>(position.data()));GXCmd1u32(sizeof(position));GXCmd1u8(1);
    GXCmd1u8(GX_LOAD_CP_REG);GXCmd1u8(0xb0);GXCmd1u32(12);assert(stream.finish());
    alignas(32) u8 list[256]{};GDLObj gd;GDInitGDLObj(&gd,list,sizeof(list));GDSetCurrent(&gd);
    const GXVtxDescList desc[]{{GX_VA_POS,GX_INDEX8},{GX_VA_NULL,GX_NONE}};
    const GXVtxAttrFmtList fmt[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_NULL,GX_POS_XY,GX_U8,0}};
    GDSetVtxDescv(desc);GDSetVtxAttrFmtv(GX_VTXFMT0,fmt);
    const auto stateSize=GDGetCurrOffset();GDWrite_u8(GX_TRIANGLES);GDWrite_u16(3);
    GDWrite_u8(0);GDWrite_u8(0);GDWrite_u8(0);const auto size=GDGetCurrOffset();GDSetCurrent(nullptr);
    Capture result;processor.setSink(capture,&result);GXCallDisplayList(list,size);
    assert(result.draws==1&&result.pointer==list+stateSize+3);
    // A complete list larger than scratch executes without an intermediate copy.
    std::vector<u8> large(list,list+size);large.resize(65536,GX_NOP);
    GXCallDisplayList(large.data(),large.size());assert(result.pointer==large.data()+stateSize+3);
    // Every partition of the same list has the same result through incremental writes.
    for(u32 split=0;split<=size;++split){
        const unsigned before=result.draws;
        assert(stream.write({list,split})&&stream.write({list+split,size-split})&&stream.finish());
        assert(result.draws==before+1);
    }
    for(u32 i=0;i<size;++i)assert(stream.write({list+i,1}));assert(stream.finish());
    // The optimized indexed packet also survives bytewise framing.
    const u8 strip[]{GX_TRIANGLESTRIP,0,4,0,0,0,0};
    const auto optimized=aurora::gx::dl::optimize(strip,sizeof(strip),desc);assert(optimized);
    for(const auto& byte:*optimized)assert(stream.write({&byte,1}));assert(stream.finish());
    // No state boundary or list may hide an incomplete scalar command.
    GXCmd1u8(GX_LOAD_BP_REG);assert(!setCommandStream(nullptr));
    assert(!stream.call({list,size})&&stream.error()==StreamError::truncated);
    assert(stream.reset());processor.setSink(capture,&result);
    const u8 bad[]{0x7f};assert(!stream.write(bad)&&stream.error()==StreamError::processor);
    assert(stream.reset());const u8 hugeXF[]{GX_LOAD_XF_REG,0xff,0xff,0,0};
    assert(!stream.write(hugeXF)&&stream.error()==StreamError::capacity);
    assert(stream.reset());u8 tiny[4]{};CommandStream small(processor,tiny);
    const u8 bp[]{GX_LOAD_BP_REG,0x41,0,0,1};assert(!small.write(bp)&&small.error()==StreamError::capacity);
    assert(!processor.registers().bpValid.any());
    // Stream recursion cannot overwrite live scratch/registers during a callback.
    assert(stream.reset());processor.setSink([](const CommandDraw&,void* p){return !static_cast<CommandStream*>(p)->write({});},&stream);
    const u8 direct[]{GX_LOAD_CP_REG,0x50,0,0,2,0,GX_LOAD_CP_REG,0x60,0,0,0,0,
                      GX_LOAD_CP_REG,0x70,0,0,0,9,GX_POINTS,0,1,
                      0x3f,0x80,0,0,0x40,0,0,0,0x40,0x40,0,0};
    assert(!stream.call(direct)&&stream.error()==StreamError::reentrant);
    assert(stream.reset());assert(setCommandStream(nullptr));
    bool rejected=false;try{GXCmd1u8(GX_NOP);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    // ABI selection is isolated between original rendering threads.
    std::atomic<unsigned> ready=0;
    auto worker=[&](u32 value){CommandMemory m;CommandProcessor p(m);u8 b[64];CommandStream s(p,b);
        assert(setCommandStream(&s));GXCmd1u8(GX_LOAD_BP_REG);ready.fetch_add(1);
        while(ready.load()!=2)std::this_thread::yield();GXCmd1u32(0x41000000|value);
        assert(s.finish()&&p.registers().bp[0x41]==value);assert(setCommandStream(nullptr));};
    std::thread a(worker,17),b(worker,23);a.join();b.join();
    puts("PASS: native GX scalar/list ABI, original J3D packet and matrix writes, zero-copy lists, fragmented packets, bounds and TLS isolation");
    testSplitPrimitives();
    testLinePointMerge();
    testMergeEquivalence();
}
