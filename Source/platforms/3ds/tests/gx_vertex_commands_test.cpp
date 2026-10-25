#include "../runtime/gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
static std::span<const u8> liveBytes;
static bool resolveLive(std::uint64_t address,u32 size){
    const auto base=reinterpret_cast<std::uintptr_t>(liveBytes.data());
    return address>=base&&address-base<=liveBytes.size()&&size<=liveBytes.size()-(address-base);
}
int main(){
    CommandMemory memory;CommandProcessor native(memory),reference(memory);
    u8 scratch[256];CommandStream stream(native,scratch);assert(setCommandStream(&stream));
    alignas(32) u8 commands[2048];GDLObj gd;
    std::vector<GXVtxDescList> desc{{GX_VA_PNMTXIDX,GX_DIRECT},{GX_VA_TEX7MTXIDX,GX_DIRECT},
        {GX_VA_POS,GX_INDEX16},{GX_VA_NBT,GX_INDEX8},{GX_VA_CLR0,GX_INDEX8},{GX_VA_CLR1,GX_INDEX16}};
    std::vector<GXVtxAttrFmtList> formats{{GX_VA_POS,GX_POS_XYZ,GX_S16,7},{GX_VA_NBT,GX_NRM_NBT3,GX_S8,0},
        {GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA6,255},{GX_VA_CLR1,GX_CLR_RGB,GX_RGB565,255}};
    for(unsigned i=0;i<8;++i){desc.push_back({static_cast<GXAttr>(GX_VA_TEX0+i),GX_INDEX16});
        formats.push_back({static_cast<GXAttr>(GX_VA_TEX0+i),static_cast<GXCompCnt>(i%2),static_cast<GXCompType>(i%5),static_cast<u8>(31-i)});}
    desc.push_back({GX_VA_NULL,GX_NONE});formats.push_back({GX_VA_NULL,GX_POS_XY,GX_U8,0});
    GXClearVtxDesc();for(auto d:desc)if(d.attr!=GX_VA_NULL)GXSetVtxDesc(d.attr,d.type);
    GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);GDSetVtxDescv(desc.data());
    assert(reference.execute({commands,GDGetCurrOffset()}));GDSetCurrent(nullptr);
    assert(native.registers().cp[0x50]==reference.registers().cp[0x50]);
    assert(native.registers().cp[0x60]==reference.registers().cp[0x60]);
    assert(native.registers().xf[0x1008]==reference.registers().xf[0x1008]);
    // Compare all VAT bitfields and all eight formats to actual original GD output.
    for(unsigned vat=0;vat<8;++vat){
        for(const auto f:formats)if(f.attr!=GX_VA_NULL)GXSetVtxAttrFmt(static_cast<GXVtxFmt>(vat),f.attr,f.cnt,f.type,f.frac);
        GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);GDSetVtxAttrFmtv(static_cast<GXVtxFmt>(vat),formats.data());
        assert(reference.execute({commands,GDGetCurrOffset()}));GDSetCurrent(nullptr);
        for(unsigned group=0;group<3;++group)assert(native.registers().cp[0x70+16*group+vat]==reference.registers().cp[0x70+16*group+vat]);
    }
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_NRM,GX_INDEX8);GXSetVtxDesc(GX_VA_NBT,GX_INDEX16);
    assert(((native.registers().xf[0x1008]>>2)&3)==2);
    GXSetVtxDesc(GX_VA_NBT,GX_NONE);
    assert(((native.registers().xf[0x1008]>>2)&3)==1&&((native.registers().cp[0x50]>>11)&3)==GX_INDEX16);
    GXSetVtxDesc(GX_VA_NBT,GX_INDEX8);
    const GXVtxDescList normal[]{{GX_VA_NRM,GX_INDEX16},{GX_VA_NULL,GX_NONE}};
    GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);GDSetVtxDescv(normal);
    GXCallDisplayList(commands,GDGetCurrOffset());GDSetCurrent(nullptr);GXSetVtxDesc(GX_VA_NBT,GX_NONE);
    assert(((native.registers().cp[0x50]>>11)&3)==GX_INDEX16&&((native.registers().xf[0x1008]>>2)&3)==1);
    assert(stream.reset());
    std::array<float,3> position{1,2,3};MemoryHandle handle;
    assert(memory.add({reinterpret_cast<const u8*>(position.data()),sizeof(position)},handle));
    unsigned draws=0;native.setSink([](const CommandDraw& draw,void* context){
        for(unsigned i=0;i<draw.vertexCount;++i){DecodedVertex vertex;if(!draw.vertex(i,vertex))return false;
            assert(vertex.position[0]==1&&vertex.position[1]==2&&vertex.position[2]==3);}
        ++*static_cast<unsigned*>(context);return true;},&draws);
    const u8 triangle[]{GX_TRIANGLES,0,3,0,0,0};
    for(unsigned i=0;i<20;++i){GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_INDEX8);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
        GXSetArray(GX_VA_POS,position.data(),sizeof(position),12,true);GXCallDisplayList(triangle,sizeof(triangle));}
    assert(draws==20&&native.statistics().plans==1);
    // Raw GD state also reaches the restored plan, without changing array ownership.
    GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
    const GXVtxDescList pos[]{{GX_VA_POS,GX_INDEX8},{GX_VA_NULL,GX_NONE}};
    GDSetVtxDescv(pos);GXCallDisplayList(commands,GDGetCurrOffset());GDSetCurrent(nullptr);
    GXCallDisplayList(triangle,sizeof(triangle));assert(native.statistics().plans==1&&draws==21);
    // Even an unchanged typed setter cannot enter an active processor callback.
    native.setSink([](const CommandDraw&,void*){
        try{GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);}
        catch(const std::runtime_error&){return false;}return true;});
    assert(!native.execute(triangle)&&stream.error()==StreamError::reentrant);assert(stream.reset());
    assert(memory.remove(handle));bool rejected=false;
    try{GXSetArray(GX_VA_POS,position.data(),sizeof(position),12,true);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected&&native.error()==CommandError::unregisteredMemory);assert(stream.reset());
    // Original startup may resolve currently live arrays without accumulating
    // registrations. Both typed setters and original GD commands use it.
    native.setLiveArrayResolver(resolveLive);
    liveBytes={reinterpret_cast<const u8*>(position.data()),sizeof(position)};
    native.setSink([](const CommandDraw& draw,void* context){
        DecodedVertex v;assert(draw.vertex(0,v)&&v.position[2]==3);
        ++*static_cast<unsigned*>(context);return true;},&draws);
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    GXSetArray(GX_VA_POS,position.data(),sizeof(position),12,true);
    GXCallDisplayList(triangle,sizeof(triangle));
    GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
    GDSetArraySized(GX_VA_POS,position.data(),sizeof(position),12,true);
    GXCallDisplayList(commands,GDGetCurrOffset());GDSetCurrent(nullptr);
    GXCallDisplayList(triangle,sizeof(triangle));assert(draws==23);
    // Expired allocations must fail again at consumption, not only on bind.
    liveBytes={};assert(!native.execute(triangle)&&native.error()==CommandError::staleMemory);
    assert(stream.reset());
    liveBytes={reinterpret_cast<const u8*>(position.data()),sizeof(position)};
    rejected=false;try{GXSetArray(GX_VA_POS,position.data(),sizeof(position)+1,12,true);}
    catch(const std::runtime_error&){rejected=true;}
    assert(rejected&&native.error()==CommandError::unregisteredMemory);assert(stream.reset());
    liveBytes={};native.setLiveArrayResolver(nullptr);
    rejected=false;try{GXSetVtxDesc(GX_VA_PNMTXIDX,GX_INDEX8);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected&&native.error()==CommandError::format);assert(stream.reset());
    assert(setCommandStream(nullptr));
    puts("PASS: typed VCD/VAT exact original-GD comparison, NRM/NBT aliases, registered arrays and clear/restore plan reuse across 21 draws");
}
