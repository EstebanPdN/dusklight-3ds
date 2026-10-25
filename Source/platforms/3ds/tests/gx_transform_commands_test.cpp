#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_state.hpp"
#include <dolphin/gx.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/os.h>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
void testOriginalCommandAbi(CommandProcessor&);
static float value(const CommandProcessor& p,u32 address){assert(p.registers().xfValid[address]);return std::bit_cast<float>(p.registers().xf[address]);}
static void close(float a,float b){assert(std::fabs(a-b)<1e-4f);}
template<class F>static void rejected(F action){bool failed=false;try{action();}catch(const std::runtime_error&){failed=true;}assert(failed);}
static void cp(u8 reg,u32 data){GXCmd1u8(GX_LOAD_CP_REG);GXCmd1u8(reg);GXCmd1u32(data);}
static void xf(u16 reg,u32 data){GXCmd1u8(GX_LOAD_XF_REG);GXCmd1u16(0);GXCmd1u16(reg);GXCmd1u32(data);}
int main(){
    CommandMemory memory;CommandProcessor p(memory);u8 scratch[256];CommandStream stream(p,scratch);
    resetState();assert(setCommandStream(&stream));
    const float projection[]{2,0,0.25f,0,0,3,-0.5f,0,0,0,0.1f,4,0,0,-1,0};
    GXSetProjection(projection,GX_PERSPECTIVE);
    const float parameters[]{2,0.25f,3,-0.5f,0.1f,4};
    for(unsigned i=0;i<6;++i)close(value(p,0x1020+i),parameters[i]);assert(p.registers().xf[0x1026]==0);
    const float orthographic[]{2,0,0,-3,0,4,0,5,0,0,6,7,0,0,0,1};
    GXSetProjection(orthographic,GX_ORTHOGRAPHIC);close(value(p,0x1021),-3);close(value(p,0x1023),5);
    assert(p.registers().xf[0x1026]==1);
    GXSetViewport(10,20,400,240,0.25f,0.75f);
    close(value(p,0x101a),200);close(value(p,0x101b),-120);close(value(p,0x101d),550);close(value(p,0x101e),480);
    close((value(p,0x101f)-value(p,0x101c))/16777215.0f,0.25f);close(value(p,0x101f)/16777215.0f,0.75f);
    float matrix[]{1,2,3,4,5,6,7,8,9,10,11,12};
    GXLoadPosMtxImm(matrix,GX_PNMTX1);for(unsigned i=0;i<12;++i)close(value(p,12+i),matrix[i]);
    // Original J3D list/scalar methods overwrite the same bank, then typed calls overwrite them.
    testOriginalCommandAbi(p);close(value(p,12),1.25f);
    matrix[0]=17;GXLoadPosMtxImm(matrix,GX_PNMTX1);close(value(p,12),17);
    matrix[3]=matrix[7]=matrix[11]=std::numeric_limits<float>::quiet_NaN();
    GXLoadNrmMtxImm(matrix,GX_PNMTX2);
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)close(value(p,0x412+row*3+col),matrix[row*4+col]);
    matrix[3]=4;matrix[7]=8;matrix[11]=12;
    GXLoadTexMtxImm(matrix,GX_TEXMTX0,GX_MTX3x4);
    matrix[0]=23;GXLoadTexMtxImm(matrix,GX_TEXMTX0,GX_MTX2x4);close(value(p,120),23);close(value(p,131),12);
    GXLoadTexMtxImm(matrix,GX_PTTEXMTX1,GX_MTX3x4);close(value(p,0x50c),23);
    // Matrix selection obeys last-write order across CP and XF; typed changes preserve texgen selectors.
    cp(0x30,0x123456c3);xf(0x1018,0x23456786);GXSetCurrentMtx(GX_PNMTX3);
    assert(p.registers().matrixIndex[0]==((0x23456786u&~63u)|9));
    assert(p.registers().cp[0x30]==p.registers().xf[0x1018]);
    xf(0x1018,0x345678c0);cp(0x30,0x15678903);GXSetCurrentMtx(GX_PNMTX9);
    assert(p.registers().matrixIndex[0]==((0x15678903u&~63u)|27));
    cp(0x40,0xabcdef);xf(0x1019,0x123456);assert(p.registers().matrixIndex[1]==0x123456);
    xf(0x1019,0x345678);cp(0x40,0xabcdef);assert(p.registers().matrixIndex[1]==0xabcdef);
    // Raster setters preserve unrelated fields from previous raw J3D writes.
    GXCmd1u8(GX_LOAD_BP_REG);GXCmd1u32(0x4100001c);
    GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_COPY);
    assert((p.registers().bp[0x41]&0x1c)==0x1c);
    assert((p.registers().bp[0x41]&0xffe3)==(1u|(5u<<5)|(4u<<8)|(3u<<12)));
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);assert(p.registers().bp[0x40]==0x17);
    GXSetAlphaCompare(GX_GREATER,37,GX_AOP_OR,GX_LESS,190);
    assert(p.registers().bp[0xf3]==(37u|(190u<<8)|(4u<<16)|(1u<<19)|(1u<<22)));
    GXCmd1u8(GX_LOAD_BP_REG);GXCmd1u32(0x00002abc);GXSetCullMode(GX_CULL_FRONT);
    assert(p.registers().bp[0]==0xaabc);
    GXCmd1u8(GX_LOAD_BP_REG);GXCmd1u32(0x43000015);GXSetZCompLoc(GX_TRUE);
    assert(p.registers().bp[0x43]==0x55);
    GXSetScissor(10,20,400,240);
    assert(p.registers().bp[0x20]==(352u<<12|362u));
    assert(p.registers().bp[0x21]==(751u<<12|601u));
    GXSetLineWidth(12,GX_TO_ZERO);assert((p.registers().bp[0x22]&0x700ff)==12);
    // A pending BP mask retains its one-write hardware semantics across typed calls.
    GXCmd1u8(GX_LOAD_BP_REG);GXCmd1u32(0xfe000001);GXSetZMode(GX_FALSE,GX_NEVER,GX_FALSE);
    assert(p.registers().bp[0x40]==0x16&&p.registers().bpMask==0xffffff);
    GXSetZMode(GX_FALSE,GX_NEVER,GX_FALSE);assert(p.registers().bp[0x40]==0);
    // Invalid typed calls must not fall through and contaminate the legacy diagnostic's state.
    const auto stats=statistics();const auto old=p.registers().xf[0x1020];
    rejected([&]{GXSetTevOp(GX_TEVSTAGE0,GX_PASSCLR);});
    rejected([&]{GXLoadPosMtxImm(matrix,1);});rejected([&]{GXSetProjection(nullptr,GX_PERSPECTIVE);});
    rejected([&]{GXLoadTexMtxImm(matrix,GX_PTTEXMTX0,GX_MTX2x4);});
    rejected([&]{GXSetViewport(0,0,400,0,0,1);});
    assert(p.registers().xf[0x1020]==old&&statistics().errors==stats.errors);
    GXCmd1u8(GX_LOAD_BP_REG);rejected([&]{GXSetProjection(projection,GX_PERSPECTIVE);});
    assert(stream.error()==StreamError::truncated);assert(stream.reset());assert(!p.registers().matrixIndexValid.any());
    const u32 selectors[]{0x120006,0x340012};assert(p.writeXf(0x1018,selectors));
    assert(p.registers().matrixIndex[0]==selectors[0]&&p.registers().matrixIndex[1]==selectors[1]);
    assert(!p.writeXf(0x1057,selectors)&&p.error()==CommandError::unsupported);
    assert(!p.registers().xfValid[0x1057]);assert(stream.reset());
    // Direct host-word writes cannot mutate register state inside a draw callback.
    p.setSink([](const CommandDraw&,void* data){auto& processor=*static_cast<CommandProcessor*>(data);
        const u32 value=123;return !processor.writeXf(0,{&value,1});},&p);
    assert(p.writeCp(0x50,0x200)&&p.writeCp(0x60,0)&&p.writeCp(0x70,9));
    const u8 point[]{GX_POINTS,0,1,0x3f,0x80,0,0,0x40,0,0,0,0x40,0x40,0,0};
    assert(!p.execute(point)&&p.error()==CommandError::reentrant&&!p.registers().xfValid[0]);
    assert(stream.reset());
    assert(setCommandStream(nullptr));
    GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_COPY);assert(statistics().errors==stats.errors);
    puts("PASS: shared typed/list transform registers, original J3D overwrites, CP/XF ordering, viewport and matrix formats, explicit mode isolation");
}
