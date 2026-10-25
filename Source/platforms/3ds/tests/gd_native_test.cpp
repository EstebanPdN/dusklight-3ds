#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <dolphin/gx/GXAurora.h>
#include <aurora/dl.hpp>
#include "JSystem/J3DGraphBase/J3DGD.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

struct Panic : std::runtime_error { using std::runtime_error::runtime_error; };
extern "C" void OSPanic(const char*, int, const char* msg, ...) { throw Panic(msg); }
static thread_local void* flushed; static thread_local u32 flushBytes;
void testOriginalPackets();
extern "C" void DCFlushRange(void* ptr, u32 size) { flushed=ptr;flushBytes=size; }
static GDLObj grown; alignas(32) static u8 storage[128];
static void grow() { GDInitGDLObj(&grown,storage,sizeof(storage));GDSetCurrent(&grown); }
static void ineffective() {}
template<class F> static void panics(F&& f) { bool hit=false;try { f(); } catch(const Panic&) {hit=true;} assert(hit); }
int main() {
  GDSetCurrent(nullptr); panics([]{GDWrite_u32(1);}); panics([]{GDGetCurrOffset();});
  panics([]{GDGetCurrPointer();});panics([]{GDGetCurrPointer2();});
  alignas(32) std::array<u8,96> data; data.fill(0xcd); GDLObj dl{};
  GDInitGDLObj(&dl,data.data()+32,32);GDSetCurrent(&dl);
  GDWrite_u8(0x12);GDWrite_u16(0x3456);GDWrite_u24(0x789abc);GDWrite_u32(0xdef01234);
  GDWrite_u64(0x0123456789abcdefULL);GDWrite_f32(1.0f);
  const u8 expected[]{0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,0x12,0x34,
    1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0x3f,0x80,0,0};
  assert(!memcmp(data.data()+32,expected,sizeof(expected)));
  assert(GDGetCurrOffset()==sizeof(expected));GDPadCurr32();assert(GDGetCurrOffset()==32);
  for(size_t i=32+sizeof(expected);i<64;++i) assert(data[i]==0);
  for(size_t i=0;i<32;++i) assert(data[i]==0xcd && data[i+64]==0xcd);
  GDFlushCurrToMem();assert(flushed==data.data()+32 && flushBytes==32);
  panics([]{__GDWrite(0xff);});panics([]{GDOverflowCheck(0xffffffffu);});
  assert(GDGetCurrOffset()==32);GDSetOverflowCallback(ineffective);
  panics([]{GDWrite_u16(0xffff);});assert(GDGetCurrOffset()==32);
  GDSetOverflowCallback(grow);GDWrite_u32(0x12345678);assert(GDGetCurrOffset()==4);
  assert(!memcmp(storage,"\x12\x34\x56\x78",4));
  GDSetOverflowCallback(nullptr);GDSetCurrent(&dl);GDSetCurrOffset(31);
  panics([]{GDWrite_u16(0xffff);});assert(GDGetCurrOffset()==31 && data[63]==0);
  panics([]{GDSetCurrOffset(0xffffffffu);});assert(GDGetCurrOffset()==31);
  GDSetCurrOffset(4);GDWrite_u32(0x11223344);assert(GDGetCurrOffset()==8);
  assert(!memcmp(data.data()+36,"\x11\x22\x33\x44",4));
  panics([&]{GDInitGDLObj(&dl,data.data()+1,32);});
  panics([&]{GDInitGDLObj(&dl,data.data(),31);});
  auto saved=dl.top;dl.top=dl.start;panics([]{GDGetCurrOffset();});dl.top=saved;
  // Real sized-array encoder retains address, bound, endian flag and stride.
  GDInitGDLObj(&grown,storage,sizeof(storage));GDSetCurrent(&grown);
  GDSetArraySized(GX_VA_POS,data.data(),data.size(),12,true);
  assert(GDGetCurrOffset()==22 && storage[0]==GX_AURORA && storage[1]==0 && storage[2]==0x10);
  u64 address=0;for(int i=3;i<11;++i) address=(address<<8)|storage[i];
  assert(address==reinterpret_cast<uintptr_t>(data.data()));
  assert(storage[14]==96 && storage[15]==1 && storage[16]==GX_LOAD_CP_REG && storage[17]==0xb0 && storage[21]==12);
  aurora::gx::dl::Reader reader(storage,22,u8(1));
  auto parsed=reader.next();assert(parsed && parsed->size==16);
  parsed=reader.next();assert(parsed && parsed->size==6 && !reader.next() && !reader.failed());
  for(u32 length=1;length<16;++length) {
    aurora::gx::dl::Reader truncated(storage,length,u8(1));
    assert(!truncated.next() && truncated.failed());
  }
  GDSetCurrOffset(0);
  J3DGDSetGenMode(1,1,2,0,GX_CULL_BACK);
  const u8 gen[]{0x61,0xfe,0x07,0xfc,0x3f,0x61,0,0,0x44,0x11};
  assert(GDGetCurrOffset()==sizeof(gen) && !memcmp(storage,gen,sizeof(gen)));
  GDSetCurrOffset(0);J3DGDSetAlphaCompare(GX_GREATER,0x12,GX_AOP_AND,GX_ALWAYS,0x34);
  const u8 alpha[]{0x61,0xf3,0x3c,0x34,0x12};
  assert(GDGetCurrOffset()==sizeof(alpha) && !memcmp(storage,alpha,sizeof(alpha)));
  const auto offset=GDGetCurrOffset();
  panics([]{J3DGDSetTexImgPtr(GX_TEXMAP0,storage);});
  panics([]{J3DGDLoadTlut(storage,0x80000,GX_TLUT_16);});
  panics([]{GDSetArraySized(GX_VA_POS,nullptr,1,12,true);});
  panics([]{GDSetArraySized(GX_VA_PNMTXIDX,storage,1,1,false);});
  assert(GDGetCurrOffset()==offset);
  // Separate callers cannot clobber the parent's current list or callback.
  auto* current=__GDCurrentDL;GDSetOverflowCallback(ineffective);std::atomic<int> ready{0};
  auto worker=[&]{alignas(32) u8 bytes[32]{};GDLObj own;
    assert(!__GDCurrentDL && !GDGetOverflowCallback());GDInitGDLObj(&own,bytes,32);GDSetCurrent(&own);
    ready.fetch_add(1);while(ready.load()<2) std::this_thread::yield();
    for(unsigned i=0;i<8;++i)GDWrite_u32(0xaabbccdd);
    assert(GDGetCurrOffset()==32 && bytes[0]==0xaa && bytes[31]==0xdd);GDSetCurrent(nullptr);};
  std::thread a(worker),b(worker);a.join();b.join();
  assert(__GDCurrentDL==current && GDGetOverflowCallback()==ineffective);
  GDSetOverflowCallback(nullptr);GDSetCurrent(nullptr);
  testOriginalPackets();
  puts("PASS: original J3D packets, GD endian writes, alignment/padding, bounds, callback revalidation, patch offsets, array command, thread isolation");
}
