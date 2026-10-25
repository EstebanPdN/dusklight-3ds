#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_leaf_wire.hpp"
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
void testOriginalCommandAbi(CommandProcessor&);
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
}
