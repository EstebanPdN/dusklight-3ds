#include "../runtime/gx_commands.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <dolphin/gx/GXAurora.h>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
// Defined by the native draw code in the game; the processor only counts batches in it.
thread_local dusk::native::gx::StaticGeometryHint dusk::native::gx::staticGeometryHint{};
struct Capture { std::vector<DecodedVertex> vertices;std::vector<u16> indices; };
static bool capture(const CommandDraw& draw,void* context) {
    auto& out=*static_cast<Capture*>(context);std::vector<DecodedVertex> next;
    for(u32 i=0;i<draw.vertexCount;++i){DecodedVertex v;if(!draw.vertex(i,v))return false;next.push_back(v);}
    out.vertices=next;out.indices.clear();
    for(u32 i=0;i<draw.indexCount;++i){u16 index;if(!draw.triangleIndex(i,index))return false;out.indices.push_back(index);}
    return true;
}
static void close(float a,float b){assert(std::fabs(a-b)<1e-6f);}

// J2D material memo. The fake setters below follow the native typed setters:
// masked BP writes behind a BP mask, read-modify-write generation mode and PE
// control, XF channel/texgen writes skipped when unchanged, the texgen matrix
// selector rebuilt from the current one, TEV colour banks and texture
// bindings between the two recorded segments. The memo side makes its masked
// writes with writeBpMasked, the reference with the two writes.
namespace memo {
struct Material {u32 matColor[2],chans,cull,texGens,matrix,tevIn,tevColor[4],kColor[4],blend,dither,texMatrix,earlyOut,outsideRead,foreign,textures,
                 sourced,lateTexture,pendingMask,list;};
// A J2DTexMtx stand-in: info, then the matrix a load sends.
struct TexObject {u32 info[9];float matrix[12];};
TexObject texObject{{1,2,3,4,5,6,7,8,9},{1,0,0,0.25f,0,1,0,0.5f,0,0,1,0}};
const CommandProcessor* maskedTarget{};
void masked(CommandProcessor& p,u8 reg,u32 mask,u32 value,bool earlyOut=false){
    const auto& r=p.registers();
    if(earlyOut&&r.bpValid[reg]&&((r.bp[reg]^value)&mask)==0)return; // a setter that skips unchanged values
    if(&p==maskedTarget)assert(p.writeBpMasked(reg,mask,value));
    else assert(p.writeBp(0xfe,mask)&&p.writeBp(reg,value));
}
void readModifyWrite(CommandProcessor& p,u8 reg,u32 mask,u32 value){
    const auto& r=p.registers();const u32 previous=r.bpValid[reg]?r.bp[reg]:0;
    assert(p.writeBp(reg,(previous&~mask)|(value&mask)));
}
void genMode(CommandProcessor& p,u32 mask,u32 value){assert(p.writeBp(0,(p.registers().bp[0]&~mask)|(value&mask)));}
void xf(CommandProcessor& p,u16 address,u32 value){
    const auto& r=p.registers();
    if(!r.xfValid[address]||r.xf[address]!=value)assert(p.writeXf(address,{&value,1}));
}
void texCoordGen(CommandProcessor& p,unsigned destination,u32 matrix){
    xf(p,0x1040+destination,0x280|destination);xf(p,0x1050+destination,61);
    const unsigned slot=destination/4,shift=slot?6*(destination-4):6*(destination+1),cp=slot?0x40:0x30;
    const auto& r=p.registers();const u32 selected=(r.matrixIndex[slot]&~(63u<<shift))|(matrix<<shift);
    if(!r.cpValid[cp]||r.cp[cp]!=selected)assert(p.writeCp(u8(cp),selected));
    xf(p,u16(0x1018+slot),selected);
}
void textures(CommandProcessor& p,const Material& m){
    for(unsigned unit=0;unit<m.textures;++unit){
        const unsigned offset=(unit/4)*32+unit%4;
        assert(p.writeBp(0xfe,0xffffff)&&p.writeBp(u8(0x80+offset),0x11+unit)&&p.writeBp(u8(0x84+offset),0x2200)&&
               p.writeBp(u8(0x88+offset),0x3ff|(unit<<20))&&p.writeBp(u8(0x94+offset),0x1000+unit));
        if(unit==1)assert(p.writeBp(0x64,0x5000)&&p.writeBp(0x65,(0x3c00>>9)|(1u<<10))&&p.writeBp(u8(0x98+offset),0x1e));
    }
}
void colorAndTexGen(CommandProcessor& p,const Material& m){
    xf(p,0x100c,m.matColor[0]);xf(p,0x100d,m.matColor[1]);
    genMode(p,0x70,m.chans<<4);xf(p,0x1009,m.chans);
    for(unsigned c=0;c<2*m.chans;++c)xf(p,u16(0x100e + c),0x42|c); // spaced: 0x100e+c lexes as one number
    readModifyWrite(p,0,0xc000,m.cull<<14);
    genMode(p,15,m.texGens);xf(p,0x103f,m.texGens);
    for(unsigned i=0;i<m.texGens;++i)texCoordGen(p,i,m.matrix);
    if(m.texMatrix){const u32 values[12]{0x3f800000,0,0,0,0,0x3f800000,0,0,0,0,0x3f800000,m.texMatrix};assert(p.writeXf(0x78,values));}
    if(m.sourced){
        // J2DTexGenBlock::setGX: the object is named, then its matrix loaded.
        materialMemoSource(p.registers(),&texObject,sizeof(texObject),texObject.matrix);
        u32 values[12];std::memcpy(values,texObject.matrix,sizeof(values));assert(p.writeXf(0x84,values));
    }
}
void tevAndPe(CommandProcessor& p,const Material& m){
    textures(p,m);
    if(m.foreign){const u8 nop[]{GX_NOP};assert(p.execute(nop));}
    if(m.list){const u8 nops[32]{};assert(p.executeList(nops));}
    masked(p,0x28,0x3ff,0x48);
    for(unsigned i=0;i<4;++i){masked(p,u8(0xe0+2*i),0xffffff,m.tevColor[i]&0x7fffff);masked(p,u8(0xe1+2*i),0xffffff,(m.tevColor[i]>>8)&0x7fffff);}
    masked(p,0,15u<<10,0,m.earlyOut);
    masked(p,0xc0,0xffff,m.outsideRead?p.registers().bp[0x50]&0xffff:m.tevIn);
    masked(p,0xc1,0xfff0,m.tevIn<<4);
    masked(p,0xc0,0xff0000,0x080000);masked(p,0xc1,0xff0000,0x080000);
    for(unsigned i=0;i<4;++i){masked(p,u8(0xe0+2*i),0xffffff,(m.kColor[i]&0x7ff7ff)|(1u<<23));masked(p,u8(0xe1+2*i),0xffffff,((m.kColor[i]>>4)&0x7ff7ff)|(1u<<23));}
    masked(p,0xf6,31u<<4,12u<<4);
    for(unsigned t=0;t<4;++t){masked(p,u8(0xf6+2*t),15,t);masked(p,u8(0xf7+2*t),15,t^3);}
    masked(p,0x10,0xffffff,0);
    readModifyWrite(p,0xf3,0xffffff,0x3f0000|m.blend);
    readModifyWrite(p,0x41,0xffe3,m.blend);readModifyWrite(p,0x41,4,m.dither<<2);
    if(m.lateTexture)assert(p.writeBp(0x94,0x1007));
    if(m.pendingMask)assert(p.writeBp(0xfe,0xff)); // a BP mask left pending at the end
}
void original(CommandProcessor& p,const Material& m){colorAndTexGen(p,m);tevAndPe(p,m);}
MaterialMemoVerdict lastVerdict;
// The J2DMaterial::setGX flow. Returns the action taken.
MaterialMemoAction memoized(CommandProcessor& p,const Material& m,const void* identity){
    if(materialMemoGivenUp(identity)){original(p,m);return MaterialMemoAction::plain;}
    MaterialMemoSum sum;materialMemoChecksum(sum,&identity,sizeof(identity));materialMemoChecksum(sum,&m,sizeof(m));
    const auto ticket=materialMemoLookup(p.registers(),identity,sum,1,m.texGens);
    if(ticket.action==MaterialMemoAction::replay){
        materialMemoReplay(ticket,0);textures(p,m);materialMemoReplay(ticket,1);
    }else if(ticket.action==MaterialMemoAction::record){
        colorAndTexGen(p,m);materialMemoSplit();tevAndPe(p,m);lastVerdict=materialMemoFinish(ticket);
    }else original(p,m);
    return ticket.action;
}
void same(const CommandProcessor& a,const CommandProcessor& b){
    const auto& x=a.registers();const auto& y=b.registers();
    assert(x.cp==y.cp&&x.bp==y.bp&&x.xf==y.xf&&x.cpValid==y.cpValid&&x.bpValid==y.bpValid&&x.xfValid==y.xfValid);
    assert(x.tevColor==y.tevColor&&x.tevColorValid==y.tevColorValid&&x.matrixIndex==y.matrixIndex&&x.matrixIndexValid==y.matrixIndexValid);
    for(unsigned i=0;i<x.tluts.size();++i)assert(x.tluts[i].token==y.tluts[i].token&&x.tluts[i].offset==y.tluts[i].offset&&x.tluts[i].entries==y.tluts[i].entries);
    assert(x.tlutOverflow==y.tlutOverflow&&x.bpMask==y.bpMask&&x.stateEpoch==y.stateEpoch&&
           x.vertexSetupEpoch==y.vertexSetupEpoch&&x.vertexCacheEpoch==y.vertexCacheEpoch);
    assert(a.statistics().commands==b.statistics().commands);
}
// Runs a material on both processors (memo and original setters only) and
// requires identical banks, epochs and command counts.
MaterialMemoAction both(CommandProcessor& p,CommandProcessor& reference,const Material& m,const void* identity){
    const auto action=memoized(p,m,identity);original(reference,m);same(p,reference);return action;
}
void noise(CommandProcessor& p,CommandProcessor& reference,u8 reg,u32 value){assert(p.writeBp(reg,value)&&reference.writeBp(reg,value));}
void noiseXf(CommandProcessor& p,CommandProcessor& reference,u16 address,u32 value){assert(p.writeXf(address,{&value,1})&&reference.writeXf(address,{&value,1}));}
// Texture matrices from a declared object, refusal causes and giving up.
void sourcedMatrixTests(CommandProcessor& p,CommandProcessor& reference){
    using A=MaterialMemoAction;MaterialMemoStatistics s;materialMemoStatistics(s,true);
    alignas(8) static std::uint64_t id[8]{};
    Material sourced{{0xff8040ff,0x204060ff},2,0,1,30,0x8f8f,{0x10,0x20,0x30,0x40},{0x55,0x66,0x77,0x88},0x2a0,1,0,0,0,0,2,0,0,0,0};
    sourced.sourced=1;
    // Recorded with its matrix, replayed while the object is unchanged.
    assert(both(p,reference,sourced,&id[0])==A::record&&both(p,reference,sourced,&id[0])!=A::plain);
    assert(both(p,reference,sourced,&id[0])==A::replay&&both(p,reference,sourced,&id[0])==A::replay);
    // The object changes (an SRT animation): recorded again with the new
    // matrix, then replayed; the reference sends the new matrix too.
    texObject.matrix[3]=0.75f;
    assert(both(p,reference,sourced,&id[0])==A::record&&both(p,reference,sourced,&id[0])==A::replay);
    assert(p.registers().xf[0x84+3]==std::bit_cast<u32>(0.75f));
    texObject.info[0]=7; // any byte of the object, not only the matrix
    assert(both(p,reference,sourced,&id[0])==A::record&&both(p,reference,sourced,&id[0])==A::replay);
    materialMemoStatistics(s,true);assert(s.sourceChanges==2&&s.mismatches==0&&s.refused==0);
    // A matrix load that sends something else than the named object: refused.
    Material wrong=sourced;wrong.texMatrix=0x3e800000;wrong.sourced=0;
    materialMemoSource(p.registers(),&texObject,sizeof(texObject),texObject.matrix); // not recording: ignored
    assert(both(p,reference,wrong,&id[1])==A::record);
    materialMemoStatistics(s,true);assert(s.refusedBy[unsigned(MaterialMemoRefusal::matrix)]==1);
    // Each refusal cause, and giving up after three refused recordings.
    Material late=sourced;late.lateTexture=1;
    Material list=sourced;list.list=1;
    Material mask=sourced;mask.pendingMask=1;
    assert(both(p,reference,late,&id[2])==A::record&&both(p,reference,list,&id[3])==A::record);
    assert(both(p,reference,mask,&id[4])==A::record);
    assert(p.writeBp(0x42,0)&&reference.writeBp(0x42,0)); // consume the pending mask
    materialMemoStatistics(s,true);
    assert(s.refusedBy[unsigned(MaterialMemoRefusal::textureOrder)]==1&&s.refusedBy[unsigned(MaterialMemoRefusal::list)]==1&&
           s.refusedBy[unsigned(MaterialMemoRefusal::mask)]==1&&s.refused==3&&s.stores==0);
    // The same material refused under three footprints: then given up, and
    // examined again after 255 calls.
    for(unsigned n=0;n<3;++n){noise(p,reference,0x41,0x10+n);assert(both(p,reference,late,&id[5])==A::record);}
    materialMemoStatistics(s,true);assert(s.refused==3&&s.givenUp==0);
    for(unsigned n=0;n<254;++n)assert(both(p,reference,late,&id[5])==A::plain);
    materialMemoStatistics(s,true);assert(s.givenUp==254&&s.calls==254&&s.records==0);
    assert(both(p,reference,late,&id[5])==A::plain); // the 255th: given up for the last time
    noise(p,reference,0x41,0x30);assert(both(p,reference,late,&id[5])==A::record);
    assert(both(p,reference,late,&id[5])==A::plain);
    materialMemoStatistics(s,true);assert(s.givenUp==2&&s.records==1&&s.mismatches==0);
    // Forgotten materials start over.
    materialMemoForget(&id[5]);assert(both(p,reference,late,&id[5])==A::record);
    texObject.matrix[3]=0.25f;texObject.info[0]=1;
}
}
static void materialMemoTests(CommandMemory& memory){
    using namespace memo;
    CommandProcessor p(memory),reference(memory);maskedTarget=&p;
    MaterialMemoStatistics s;materialMemoStatistics(s,true);
    // Material identities: distinct 8-byte cells, as real objects are.
    alignas(8) static std::uint64_t id[8]{};
    const void* const ia=&id[0];const void* const ib=&id[1];const void* const ie=&id[2];const void* const im=&id[3];
    const void* const iff=&id[4];const void* const iv=&id[5];const void* const io=&id[6];const void* const ir=&id[7];
    const Material a{{0xff8040ff,0x204060ff},2,0,1,30,0x8f8f,{0x10,0x20,0x30,0x40},{0x55,0x66,0x77,0x88},0x2a0,1,0,0,0,0,2,0,0,0,0};
    Material b=a;b.tevColor[2]=0x321;b.texGens=2;b.matrix=33;b.textures=1;b.chans=1;
    using A=MaterialMemoAction;
    // The key holds the state the setters start from. From the empty bank,
    // then from its own end state: two recordings, then replays.
    assert(both(p,reference,a,ia)==A::record&&both(p,reference,a,ia)==A::record&&both(p,reference,a,ia)==A::replay);
    // Two panes drawn in turn (a HUD frame) settle within a few frames:
    // each keeps the variant for the state its neighbour leaves.
    auto settle=[&]{
        for(unsigned n=0;n<3;++n){assert(both(p,reference,b,ib)!=A::plain);assert(both(p,reference,a,ia)!=A::plain);}
        assert(both(p,reference,b,ib)==A::replay&&both(p,reference,a,ia)==A::replay);
    };
    settle();
    // Registers outside the footprint (projection, scissor, z mode) do not
    // change what the setters write: still a replay, still identical. Nor do
    // the TEV state and colour banks a previous draw left: the TEV setters
    // write fixed fields through the BP mask and read nothing.
    noiseXf(p,reference,0x1020,0x3f000000);noise(p,reference,0x20,0x123456);noise(p,reference,0x40,0x17);
    assert(both(p,reference,b,ib)==A::replay&&both(p,reference,a,ia)==A::replay);
    noise(p,reference,0xe2,0x800123);noise(p,reference,0xc1,0x0000f0);noise(p,reference,0x28,0x3c0);
    noise(p,reference,0xf7,0x5);assert(both(p,reference,b,ib)==A::replay&&both(p,reference,a,ia)==A::replay);
    // Inside the footprint: generation-mode bits the setters keep, a channel
    // colour they skip when equal and a texgen selector.
    noise(p,reference,0,p.registers().bp[0]|(1u<<19));assert(both(p,reference,b,ib)==A::record);
    assert(both(p,reference,a,ia)!=A::plain);settle();
    noiseXf(p,reference,0x100c,0x01020304);assert(both(p,reference,b,ib)==A::record);
    assert(both(p,reference,a,ia)!=A::plain);settle();
    assert(p.writeCp(0x30,0xabcdef)&&reference.writeCp(0x30,0xabcdef));assert(both(p,reference,b,ib)==A::record);
    assert(both(p,reference,a,ia)!=A::plain);settle();
    // One identity drawn with two contents in turn keeps both variants.
    for(unsigned n=0;n<4;++n){assert(both(p,reference,b,ia)!=A::plain);assert(both(p,reference,a,ia)!=A::plain);}
    assert(both(p,reference,b,ia)==A::replay&&both(p,reference,a,ia)==A::replay);
    // A setter that skips an unchanged value of a footprint register (the
    // TEV stage count in generation mode): the footprint keeps it exact.
    Material early=a;early.earlyOut=1;
    assert(both(p,reference,early,ie)==A::record&&both(p,reference,early,ie)==A::replay);
    assert(both(p,reference,b,ib)!=A::plain&&both(p,reference,early,ie)!=A::plain);
    const u32 stageCount=p.registers().bp[0]|(3u<<10);
    noise(p,reference,0,stageCount);assert(both(p,reference,early,ie)==A::record);
    noise(p,reference,0,stageCount);assert(both(p,reference,early,ie)==A::replay);
    // A texture matrix, a foreign command or a pending BP mask: the setters run.
    Material matrix=a;matrix.texMatrix=0x3e800000;
    materialMemoStatistics(s,true);
    assert(both(p,reference,matrix,im)==A::record&&both(p,reference,matrix,im)==A::plain);
    Material foreign=a;foreign.foreign=1;
    assert(both(p,reference,foreign,iff)==A::record&&both(p,reference,foreign,iff)==A::plain);
    materialMemoStatistics(s,true);assert(s.refused==4&&s.stores==0&&s.calls==4);
    assert(s.refusedBy[unsigned(MaterialMemoRefusal::matrix)]==1&&s.refusedBy[unsigned(MaterialMemoRefusal::execute)]==1);
    assert(p.writeBp(0xfe,0xff)&&reference.writeBp(0xfe,0xff));
    assert(both(p,reference,a,ia)==A::plain);
    materialMemoStatistics(s,true);assert(s.bypassed==1&&s.calls==1);
    // A material that changes on every call stops recording.
    Material animated=a;
    for(unsigned n=0;n<16;++n){animated.tevColor[0]=n;both(p,reference,animated,iv);}
    materialMemoStatistics(s,true);assert(s.volatileSkips>0&&s.records<16);
    // Forgetting a material records it again.
    materialMemoForget(ia);assert(both(p,reference,a,ia)==A::record);
    sourcedMatrixTests(p,reference);
    // Pool exhaustion: every recording is dropped and made again.
    std::vector<std::uint64_t> identities(3000);
    for(auto& identity:identities)both(p,reference,a,&identity);
    for(auto& identity:identities)both(p,reference,a,&identity);
    materialMemoStatistics(s,true);assert(s.poolResets>0&&s.mismatches==0&&!s.storageFailed);
    // Verification: replays and recordings are checked against the setters.
    setMaterialMemo(true,true);assert(materialMemoVerifying());
    assert(both(p,reference,a,ia)==A::record&&!lastVerdict.mismatch&&both(p,reference,a,ia)==A::record);
    assert(!lastVerdict.mismatch);
    materialMemoStatistics(s,true);assert(s.verified==2&&s.mismatches==0&&s.stores==1&&s.replays==1);
    // A refused recording is not a mismatch with verification on.
    assert(both(p,reference,matrix,ir)==A::record&&!lastVerdict.mismatch&&both(p,reference,matrix,ir)==A::plain);
    materialMemoStatistics(s,true);assert(s.mismatches==0&&s.verified==0&&s.refused==2);
    // A setter that reads a register outside the footprint (BP 0x50) is
    // caught when that register changes under the same key; the entry is
    // then retired. Material a restores the start state in between.
    Material outside=a;outside.outsideRead=1;
    noise(p,reference,0x50,0x1111);
    assert(both(p,reference,outside,io)==A::record&&!lastVerdict.mismatch);
    both(p,reference,a,ia);noise(p,reference,0x50,0x2222);
    assert(both(p,reference,outside,io)==A::record&&lastVerdict.mismatch&&lastVerdict.space=='B'&&lastVerdict.index==0xc0);
    both(p,reference,a,ia);assert(both(p,reference,outside,io)==A::plain);
    materialMemoStatistics(s,true);assert(s.mismatches==1);
    // Off: the setters always run.
    setMaterialMemo(false,false);assert(!materialMemoEnabled()&&both(p,reference,a,ia)==A::plain);
    // On again (the off call left a's own end state): record, then replay.
    setMaterialMemo(true,false);assert(both(p,reference,a,ia)==A::record&&both(p,reference,a,ia)==A::replay);
    // A processor reset behind a recording is a foreign command: refused.
    materialMemoForget(ia);materialMemoStatistics(s,true);
    MaterialMemoSum sum;materialMemoChecksum(sum,&ia,sizeof(ia));materialMemoChecksum(sum,&a,sizeof(a));
    auto ticket=materialMemoLookup(p.registers(),ia,sum,1,a.texGens);assert(ticket.action==A::record);
    colorAndTexGen(p,a);materialMemoSplit();p.reset();tevAndPe(p,a);materialMemoFinish(ticket);
    materialMemoStatistics(s,true);assert(s.refused==1&&s.stores==0&&s.refusedBy[unsigned(MaterialMemoRefusal::foreign)]==1);
    puts("PASS material memo: replayed banks, epochs and command counts equal the original setters across footprint changes, skipped writes, texture loads between segments, neighbours, refusals, volatility, pool resets and verification");
}
// Bank equality, epochs, TLUT loads, mask and command counts.
static bool sameBank(const CommandProcessor& a,const CommandProcessor& b){
    const auto& x=a.registers();const auto& y=b.registers();
    bool ok=x.cp==y.cp&&x.bp==y.bp&&x.xf==y.xf&&x.cpValid==y.cpValid&&x.bpValid==y.bpValid&&x.xfValid==y.xfValid&&
        x.tevColor==y.tevColor&&x.tevColorValid==y.tevColorValid&&x.matrixIndex==y.matrixIndex&&x.matrixIndexValid==y.matrixIndexValid&&
        x.tlutOverflow==y.tlutOverflow&&x.bpMask==y.bpMask&&x.stateEpoch==y.stateEpoch&&x.vertexSetupEpoch==y.vertexSetupEpoch&&
        x.vertexCacheEpoch==y.vertexCacheEpoch&&a.statistics().commands==b.statistics().commands&&a.error()==b.error();
    for(unsigned i=0;i<x.tluts.size();++i)ok=ok&&x.tluts[i].token==y.tluts[i].token&&x.tluts[i].offset==y.tluts[i].offset&&x.tluts[i].entries==y.tluts[i].entries;
    return ok;
}
static bool hashCurrent(const CommandRegisters& r){
    const auto h=computeStateHash(r);
    return r.stateHash.tracked==1&&h.bp==r.stateHash.bp&&h.texture==r.stateHash.texture&&h.cp==r.stateHash.cp&&h.xf==r.stateHash.xf;
}
// writeBpMasked against writeBp(0xfe,mask) then writeBp(reg,value): random
// registers (TEV colour banks, TLUT loads, the mask register and BP52
// included), masks, values and pending masks, on known and unknown words.
static void maskedWriteTests(CommandMemory& memory){
    CommandProcessor one(memory),two(memory);
    std::uint32_t seed=0x1234567;auto next=[&]{seed=seed*1664525u+1013904223u;return seed>>8;};
    const u8 interesting[]{0x00,0x0f,0x10,0x28,0x41,0x52,0x64,0x65,0xc0,0xc1,0xe0,0xe1,0xe6,0xe7,0xf3,0xf6,0xfe,0xff,0x94};
    TypedWriteStatistics t;typedWriteStatistics(t,true);
    for(unsigned n=0;n<20000;++n){
        const u8 reg=next()%3?interesting[next()%sizeof(interesting)]:u8(next());
        const u32 mask=next()%4==0?0xffffff:next()%2?(0xffu<<(next()%3*8)):next();
        u32 value=next();if(next()%3==0&&one.registers().bpValid[reg])value=one.registers().bp[reg]^(next()&~mask);
        if(next()%8==0){const u32 pending=next();assert(one.writeBp(0xfe,pending)==two.writeBp(0xfe,pending));}
        const bool a=one.writeBpMasked(reg,mask,value);
        const bool b=two.writeBp(0xfe,mask)&&two.writeBp(reg,value);
        assert(a==b&&sameBank(one,two)&&hashCurrent(one.registers())&&hashCurrent(two.registers()));
        if(!a){assert(reg==0x52);one.reset();two.reset();assert(sameBank(one,two));}
    }
    typedWriteStatistics(t,true);assert(t.masked==20000&&t.unchanged>1000);
    puts("PASS typed masked BP writes: one call equals the mask write and the register write (bank, TEV colour banks, TLUT loads, epochs, hash, commands, errors)");
}
// The incremental state key components against a full recomputation after
// every kind of write, and order independence.
static void bpReplayTests(CommandMemory& memory){
    CommandProcessor p(memory);
    u32 log[16]{};
    p.attachBankLog(log);
    const auto& bank=p.registers();

    // A zero write still makes a previously invalid register visible.
    const u32 epoch=bank.stateEpoch;
    assert(p.writeBp(0x41,0));
    assert(bank.bpValid[0x41]&&bank.stateEpoch==epoch+1);
    assert(p.bankStores().grewValid&&p.bankStores().used==0);
    p.restartBankLog();

    // The unchanged ordinary BP command consumes its mask on every replay.
    const u8 list[32]{GX_LOAD_BP_REG,0xfe,0,0,1,GX_LOAD_BP_REG,0x41,0,0,2};
    const auto commands=p.statistics().commands;
    assert(p.executeList(list)&&p.executeList(list));
    assert(p.pendingBpMask()==0xffffff&&bank.stateEpoch==epoch+1);
    assert(!p.bankStores().grewValid&&p.bankStores().used==0);
    assert(p.statistics().commands==commands+2*(2+sizeof(list)-10)); // commands and NOPs

    assert(p.writeBp(0x41,2));
    assert(bank.bp[0x41]==2&&bank.stateEpoch==epoch+2);
    assert(!p.bankStores().grewValid&&p.bankStores().used==2&&log[1]==2);

    // Alternating the selected TEV latch changes the raw word even when both
    // latches already hold the requested value.
    assert(p.writeBp(0xe0,3)&&p.writeBp(0xe0,0x800004));
    const auto colorEpoch=bank.stateEpoch;
    assert(p.writeBp(0xe0,3));
    assert(bank.stateEpoch==colorEpoch&&bank.bp[0xe0]==3);
    assert(bank.tevColor[0][0]==3&&bank.tevColor[1][0]==0x800004);

    // A palette command loads the current token even if its own word repeats.
    assert(p.writeBp(0x64,0x100)&&p.writeBp(0x65,1u<<10));
    assert(p.writeBp(0x64,0x200));
    const auto paletteEpoch=bank.stateEpoch;
    assert(p.writeBp(0x65,1u<<10)&&bank.stateEpoch==paletteEpoch+1);
    assert(bank.tluts[0].token==0x200&&bank.tluts[0].entries==16);
    puts("PASS BP replay: mask consumption, validity-only writes, TEV latch and repeated TLUT trigger");
}

// Display list templates (executeList) against the interpreter (execute):
// lists rewritten in place, with new values only (the template stays) or a
// new structure (it is built again), with commands a template does not take
// (indexed loads: interpreter-only), and a BP mask pending from outside.
// Both banks, epochs and command counts must stay equal after every call.
// packetPaint: c replays every template hit through runTemplateFast. Its
// bank (hash included), bindings, command count and bank log must equal a's
// after every call.
// The bindings as the worker reads them: per slot handle, offset, size,
// byte order and live address (32 bytes; bytes 17-23 are padding).
static bool sameBindings(const CommandProcessor& a,const CommandProcessor& c){
    const auto& x=a.currentArrayBindings().bytes;const auto& y=c.currentArrayBindings().bytes;
    for(unsigned slot=0;slot<16;++slot){
        const u8* p=x.data()+32*slot;const u8* q=y.data()+32*slot;
        if(std::memcmp(p,q,17)||std::memcmp(p+24,q+24,8))return false;
    }
    return true;
}
static void sameFast(const CommandProcessor& a,const CommandProcessor& c){
    memo::same(a,c);
    assert(!std::memcmp(&a.registers(),&c.registers(),sizeof(CommandRegisters)));
    assert(sameBindings(a,c));
    const auto& x=a.bankStores();const auto& y=c.bankStores();
    assert(x.used==y.used&&x.lost==y.lost&&x.grewValid==y.grewValid&&!std::memcmp(x.words,y.words,x.used*sizeof(std::uint32_t)));
}
static void templateTests(CommandMemory& memory){
    CommandProcessor a(memory),b(memory),c(memory);
    static std::uint32_t logA[8192],logC[8192];
    a.attachBankLog(logA);c.attachBankLog(logC);
    std::uint32_t seed=0x2545f491u;auto next=[&]{seed=seed*1664525u+1013904223u;return seed>>8;};
    alignas(32) static u8 arrays[1024];for(unsigned i=0;i<sizeof(arrays);++i)arrays[i]=u8(i*13+5);
    MemoryHandle handle;assert(memory.add({arrays,sizeof(arrays)},handle));
    for(unsigned slot=12;slot<16;++slot)assert(a.bindArray(slot,arrays,sizeof(arrays),16,false)&&b.bindArray(slot,arrays,sizeof(arrays),16,false)&&
                                               c.bindArray(slot,arrays,sizeof(arrays),16,false));
    const u8 cps[]{0x30,0x40,0x50,0x60,0x70,0x73,0x77,0x80,0x85,0x90,0x97,0xb0,0xb5,0xbf};
    constexpr unsigned lists=6,capacity=512;
    alignas(32) static u8 storage[lists][capacity];static unsigned sizes[lists];
    auto put32=[](u8* at,u32 v){at[0]=u8(v>>24);at[1]=u8(v>>16);at[2]=u8(v>>8);at[3]=u8(v);};
    // New values for every command of a list, its structure kept.
    auto revalue=[&](u8* l,unsigned size){
        for(unsigned at=0;at<size;){
            const u8 code=l[at];
            if(code==GX_LOAD_BP_REG){const u8 reg=l[at+1];put32(l+at+1,(u32(reg)<<24)|(next()&0xffffff));at+=5;}
            else if(code==GX_LOAD_CP_REG){const u8 reg=l[at+1];put32(l+at+2,reg==0x50||reg==0x60?next()&0x3ff:next());at+=6;}
            else if(code==GX_LOAD_XF_REG){const unsigned n=((l[at+1]<<8)|l[at+2])+1;for(unsigned i=0;i<n;++i)put32(l+at+5+4*i,next());at+=5+4*n;}
            else if(code==GX_AURORA){const unsigned off=next()%512;const auto address=std::uint64_t(reinterpret_cast<std::uintptr_t>(arrays+off));
                put32(l+at+3,u32(address>>32));put32(l+at+7,u32(address));put32(l+at+11,1+next()%(512-off));at+=16;}
            else if(code==GX_LOAD_INDX_A){l[at+2]=u8(next()%16);at+=5;}
            else at+=1;
        }
    };
    auto rebuild=[&](unsigned which){
        u8* l=storage[which];unsigned at=0;
        const unsigned commands=4+next()%40;const bool interpreterOnly=next()%8==0;
        for(unsigned c=0;c<commands&&at+40<capacity;++c){
            switch(next()%9){
            case 0:{const unsigned n=1+next()%3;for(unsigned i=0;i<n;++i)l[at++]=GX_NOP;break;}
            case 1:l[at++]=GX_CMD_INVL_VC;break;
            case 2:{u8 reg=u8(next());if(reg==0x52)reg=0x53;l[at]=GX_LOAD_BP_REG;l[at+1]=reg;at+=5;break;}
            case 3:l[at]=GX_LOAD_BP_REG;l[at+1]=0xfe;l[at+5]=GX_LOAD_BP_REG;l[at+6]=u8(0xe0+next()%8);at+=10;break;
            case 4:l[at]=GX_LOAD_BP_REG;l[at+1]=0x64;l[at+5]=GX_LOAD_BP_REG;l[at+6]=0x65;at+=10;break;
            case 5:l[at]=GX_LOAD_CP_REG;l[at+1]=cps[next()%sizeof(cps)];at+=6;break;
            case 6:{const unsigned n=1+next()%4;const u16 address=next()%3?u16(0x1000+next()%(0x58-n)):u16(next()%(0x400-n));
                l[at]=GX_LOAD_XF_REG;l[at+1]=0;l[at+2]=u8(n-1);l[at+3]=u8(address>>8);l[at+4]=u8(address);at+=5+4*n;break;}
            case 7:{const unsigned slot=next()%12;l[at]=GX_AURORA;l[at+1]=u8((GX_AURORA_LOAD_ARRAYBASE+slot)>>8);l[at+2]=u8(GX_AURORA_LOAD_ARRAYBASE+slot);
                l[at+15]=u8(next()%2);at+=16;break;}
            default:l[at]=GX_LOAD_BP_REG;l[at+1]=u8(0x28+next()%8);at+=5;break;
            }
        }
        if(interpreterOnly){l[at]=GX_LOAD_INDX_A;l[at+1]=0;l[at+3]=0x10;l[at+4]=0x40;at+=5;}
        while(at<16||at%32)l[at++]=GX_NOP;
        sizes[which]=at;revalue(l,at);
    };
    for(unsigned i=0;i<lists;++i)rebuild(i);
    std::uint64_t h0,c0,i0,m0;compiledListStatistics(h0,c0,i0,m0);
    for(unsigned n=0;n<40000;++n){
        const unsigned which=next()%lists;
        const unsigned change=next()%10;
        if(change<6)revalue(storage[which],sizes[which]);else if(change==6)rebuild(which);
        if(next()%50==0){const u32 mask=next()&0xffffff;assert(a.writeBp(0xfe,mask)&&b.writeBp(0xfe,mask)&&c.writeBp(0xfe,mask));}
        const std::span<const u8> list{storage[which],sizes[which]};
        a.restartBankLog();c.restartBankLog();
        assert(a.executeList(list));assert(b.execute(list));memo::same(a,b);
        assert(c.executeList(list,true));sameFast(a,c);
        if(n==30000)setCompiledLists(true,true); // the rest with verification
    }
    std::uint64_t h1,c1,i1,m1;compiledListStatistics(h1,c1,i1,m1);
    assert(h1>h0&&c1>c0&&i1>i0&&m1==m0);
    std::uint64_t fast,held,heldBad;paintListStatistics(fast,held,heldBad);assert(fast>0);
    setCompiledLists(true,false);
    std::printf("templates: %llu replays, %llu built, %llu interpreted, 0 mismatches; %llu fast replays equal to the template's, bank log included\n",
        static_cast<unsigned long long>(h1-h0),static_cast<unsigned long long>(c1-c0),static_cast<unsigned long long>(i1-i0),
        static_cast<unsigned long long>(fast));
}
// packetPaint: a J3D vertex setup list (J3DShape::makeVcdVatCmd's encoders)
// is held only when running it would leave the processor as it is. Held or
// run, two processors must end equal: bank, bindings, command count and the
// normal aliases a typed descriptor query reads.
static void heldListTests(CommandMemory& memory){
    alignas(32) static u8 positions[960],normals[480],texcoords[240];
    MemoryHandle handle;assert(memory.add({positions,sizeof(positions)},handle));
    static u8 liveArray[64];
    static const auto live=+[](std::uint64_t address,u32 size){
        const auto at=reinterpret_cast<std::uintptr_t>(liveArray);return address>=at&&address+size<=at+sizeof(liveArray);};
    alignas(32) static u8 list[0x180];
    auto build=[&](bool color,const void* uv){
        std::memset(list,0,sizeof(list));
        GDLObj gd;GDInitGDLObj(&gd,list,sizeof(list));GDSetCurrent(&gd);
        const GXVtxDescList plain[]{{GX_VA_POS,GX_INDEX16},{GX_VA_NRM,GX_INDEX16},{GX_VA_TEX0,GX_INDEX16},{GX_VA_NULL,GX_NONE}};
        const GXVtxDescList colored[]{{GX_VA_POS,GX_INDEX16},{GX_VA_NRM,GX_INDEX16},{GX_VA_CLR0,GX_DIRECT},{GX_VA_TEX0,GX_INDEX16},
                                      {GX_VA_NULL,GX_NONE}};
        GDSetVtxDescv(color?colored:plain);
        for(unsigned i=0;i<12;++i){
            const auto attr=GXAttr(GX_VA_POS+i);
            if(i==0)GDSetArraySized(attr,positions,sizeof(positions),12,true);
            else if(i==1)GDSetArraySized(attr,normals,sizeof(normals),6,true);
            else if(i==4)GDSetArraySized(attr,const_cast<void*>(uv),uv==liveArray?sizeof(liveArray):sizeof(texcoords),4,true);
            else GDSetArraySized(attr,nullptr,0,0,true);
        }
        const GXVtxAttrFmtList formats[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_NRM,GX_NRM_XYZ,GX_S16,14},
                                         {GX_VA_TEX0,GX_TEX_ST,GX_S16,8},{GX_VA_NULL,GX_POS_XY,GX_U8,0}};
        GDSetVtxAttrFmtv(GX_VTXFMT0,formats);GDPadCurr32();GDSetCurrent(nullptr);
        return std::span<const u8>(list,sizeof(list));
    };
    MemoryHandle normalHandle,uvHandle;
    assert(memory.add({normals,sizeof(normals)},normalHandle)&&memory.add({texcoords,sizeof(texcoords)},uvHandle));
    CommandProcessor held(memory),ran(memory);held.setLiveArrayResolver(live);ran.setLiveArrayResolver(live);
    auto bothRun=[&](std::span<const u8> l){assert(held.executeList(l,true)&&ran.executeList(l));};
    auto equal=[&]{
        memo::same(held,ran);
        assert(!std::memcmp(&held.registers(),&ran.registers(),sizeof(CommandRegisters)));
        assert(sameBindings(held,ran));
        for(const unsigned attribute:{unsigned(GX_VA_NRM),unsigned(GX_VA_NBT),unsigned(GX_VA_POS),unsigned(GX_VA_TEX0)}){
            unsigned x=99,y=99;assert(held.queryDescriptor(attribute,x)==ran.queryDescriptor(attribute,y)&&x==y);
        }
    };
    std::uint64_t f0,h0,m0;paintListStatistics(f0,h0,m0);
    auto first=build(false,texcoords);
    assert(!held.listHeld(first)); // nothing valid yet
    bothRun(first);equal();
    // The same list again: held, and equal to running it.
    assert(held.listHeld(first));assert(ran.executeList(first));equal();
    // A matrix array binding (slot 12, as J3D binds per shape) moves the
    // setup epoch but no value the list sets: still held.
    assert(held.bindArray(12,liveArray,48,48,true)&&ran.bindArray(12,liveArray,48,48,true));
    assert(held.listHeld(first));assert(ran.executeList(first));equal();
    // A typed descriptor call that changes no value still sets the normal
    // aliases; the list's CP 0x50 resets them, held or run.
    assert(held.setDescriptor(GX_VA_POS,GX_INDEX16)&&ran.setDescriptor(GX_VA_POS,GX_INDEX16));
    assert(held.listHeld(first));assert(ran.executeList(first));equal();
    // One that changes a descriptor: not held.
    assert(held.setDescriptor(GX_VA_NBT,GX_INDEX16)&&ran.setDescriptor(GX_VA_NBT,GX_INDEX16));
    assert(!held.listHeld(first));bothRun(first);equal();
    // One binding, one descriptor or one format different: run.
    auto colored=build(true,texcoords);assert(!held.listHeld(colored));bothRun(colored);equal();
    assert(held.listHeld(colored));assert(ran.executeList(colored));equal();
    auto liveUv=build(false,liveArray);assert(!held.listHeld(liveUv));bothRun(liveUv);equal();
    assert(held.listHeld(liveUv));assert(ran.executeList(liveUv));equal();
    // A held live array that is no longer live is not held (running it fails as before).
    static bool alive=true;
    held.setLiveArrayResolver(+[](std::uint64_t,u32){return alive;});
    alive=false;assert(!held.listHeld(liveUv));alive=true;held.setLiveArrayResolver(live);
    // Anything but CP/XF loads, array bases and NOPs is never held.
    alignas(32) u8 withBp[32]{};withBp[0]=GX_LOAD_BP_REG;withBp[1]=0x28;assert(!held.listHeld(withBp));
    std::uint64_t f1,h1,m1;paintListStatistics(f1,h1,m1);assert(h1-h0==5&&m1==m0);
    assert(memory.remove(handle)&&memory.remove(normalHandle)&&memory.remove(uvHandle));
    puts("PASS held vertex setup lists: held only when running them changes nothing; bank, bindings, counts and normal aliases equal to running them");
}
// packetPaint: J3DCurrentMtx::load's bytes (CP 0x30, CP 0x40, XF 0x1018-0x1019)
// executed, against the typed writes duskNativeLoadCurrentMtx makes instead:
// same bank (hash, epochs, matrix index mirror), log and command count, also
// after an XF 0x1018 write made the mirror differ from CP 0x30.
static void currentMatrixTests(CommandMemory& memory){
    CommandProcessor bytes(memory),typed(memory);
    static std::uint32_t logBytes[256],logTyped[256];
    bytes.attachBankLog(logBytes);typed.attachBankLog(logTyped);
    std::uint32_t seed=0x51ed27u;auto next=[&]{seed=seed*1664525u+1013904223u;return seed;};
    for(unsigned n=0;n<2000;++n){
        bytes.restartBankLog();typed.restartBankLog();
        if(n%7==3){const u32 v=next();assert(bytes.writeXf(0x1018,{&v,1})&&typed.writeXf(0x1018,{&v,1}));}
        const u32 a=n%5?next()&0xffffff:0x3cf3cf00u,b=n%3?next()&0xffffff:0x00f3cf3cu;
        const u8 list[]{GX_LOAD_CP_REG,0x30,u8(a>>24),u8(a>>16),u8(a>>8),u8(a),
                        GX_LOAD_CP_REG,0x40,u8(b>>24),u8(b>>16),u8(b>>8),u8(b),
                        GX_LOAD_XF_REG,0,1,0x10,0x18,u8(a>>24),u8(a>>16),u8(a>>8),u8(a),u8(b>>24),u8(b>>16),u8(b>>8),u8(b)};
        assert(bytes.execute(std::span<const u8>(list,6))&&bytes.execute(std::span<const u8>(list+6,6))&&bytes.execute(std::span<const u8>(list+12,13)));
        const u32 both[2]{a,b};
        assert(typed.writeCp(0x30,a)&&typed.writeCp(0x40,b)&&typed.writeXf(0x1018,both));
        sameFast(bytes,typed);
    }
    puts("PASS current matrix: typed writes equal the executed bytes (bank, hash, epochs, matrix index mirror, log, counts)");
}
static void stateHashTests(CommandMemory& memory){
    CommandProcessor p(memory);
    std::uint32_t seed=0x9e3779b9u;auto next=[&]{seed=seed*1664525u+1013904223u;return seed>>8;};
    assert(hashCurrent(p.registers())&&p.registers().stateHash.bp==0&&p.registers().stateHash.xf==0);
    // Indexed XF loads need a bound array: big- and little-endian words.
    alignas(4) static u8 matrices[256];for(unsigned i=0;i<sizeof(matrices);++i)matrices[i]=u8(i*37+11);
    MemoryHandle handle;assert(memory.add({matrices,sizeof(matrices)},handle));
    const u8 cps[]{0x30,0x40,0x50,0x60,0x70,0x73,0x77,0x80,0x85,0x90,0x97,0xb0,0xb5,0xbf};
    for(unsigned n=0;n<30000;++n){
        const unsigned kind=next()%10;
        if(kind==0){const u8 reg=u8(next());if(reg!=0x52)assert(p.writeBp(reg,next()));}
        else if(kind==1){const u8 reg=u8(next());if(reg!=0x52)assert(p.writeBpMasked(reg,next(),next()));}
        else if(kind==2)assert(p.writeCp(cps[next()%sizeof(cps)],next()%4?next():p.registers().cp[0x50]));
        else if(kind==3){
            const unsigned count=1+next()%4;const u16 address=next()%4?u16(0x1000+next()%(0x58-count+1)):u16(0x78+next()%60);
            u32 values[4];for(auto& v:values)v=next()%3?next():0;assert(p.writeXf(address,{values,count}));
        }else if(kind==4){
            // A TLUT load: token, then the load (overlapping ones replace;
            // more than 32 overflow).
            assert(p.writeBp(0x64,next()));assert(p.writeBp(0x65,(next()%1024)|((1+next()%16)<<10)));
        }else if(kind==5){
            // Raw commands: BP, CP and XF through execute.
            const u32 bp=next();const u8 reg=u8(next());const u32 xf=next();const u16 at=u16(0x1000+next()%0x58);
            const u8 cp=cps[next()%sizeof(cps)];const u32 cv=next();
            const u8 bytes[]{GX_LOAD_BP_REG,reg==0x52?u8(0x53):reg,u8(bp>>16),u8(bp>>8),u8(bp),
                GX_LOAD_CP_REG,cp,u8(cv>>24),u8(cv>>16),u8(cv>>8),u8(cv),
                GX_LOAD_XF_REG,0,0,u8(at>>8),u8(at),u8(xf>>24),u8(xf>>16),u8(xf>>8),u8(xf)};
            assert(p.execute(bytes));
        }else if(kind==6){
            // A display list called twice: compiled, then replayed compiled.
            static u8 list[3][32];auto& l=list[next()%3];
            const u32 v=next()%4?0x123456:next();const u16 at=u16(0x1040+next()%8);
            const u8 bytes[]{GX_LOAD_BP_REG,0xfe,0x00,0xff,0xff,GX_LOAD_BP_REG,u8(0xc0+next()%32),u8(v>>16),u8(v>>8),u8(v),
                GX_LOAD_BP_REG,u8(0xe0+next()%8),u8(next()),u8(next()),u8(next()),
                GX_LOAD_XF_REG,0,0,u8(at>>8),u8(at),0,0,u8(v>>8),u8(v),GX_LOAD_CP_REG,0x50,0,0,0x2,0x0,GX_NOP,GX_NOP};
            std::memcpy(l,bytes,sizeof(bytes));assert(p.executeList(l));assert(hashCurrent(p.registers()));assert(p.executeList(l));
        }else if(kind==7){
            // Indexed XF loads into channel/texgen state and texture matrices.
            const bool little=next()%2;const u8 code=GX_LOAD_INDX_A+8*(next()%4);const unsigned slot=12+(code-GX_LOAD_INDX_A)/8;
            assert(p.bindArray(slot,matrices,sizeof(matrices),16,little));
            const u16 address=next()%2?u16(0x1000+next()%0x50):u16(0x78+next()%40);
            assert(p.loadIndexed(code,u16(next()%16),u16(address|((next()%4)<<12))));
        }else if(kind==8&&next()%64==0){p.reset();}
        else{const u8 reg=u8(0xe0+next()%8);assert(p.writeBp(reg,next()));}
        assert(hashCurrent(p.registers()));
    }
    // Order independence: the same words written in another order.
    CommandProcessor a(memory),b(memory);
    const u32 words[]{0x1009,0x100e,0x1040,0x1041,0x1050};
    for(unsigned i=0;i<5;++i){const u32 v=0x100+i;assert(a.writeXf(u16(words[i]),{&v,1}));}
    for(unsigned i=5;i-->0;){const u32 v=0x100+i;assert(b.writeXf(u16(words[i]),{&v,1}));}
    assert(a.writeBp(0x28,0x48)&&a.writeBp(0x80,0x11)&&a.writeCp(0x50,0x200));
    assert(b.writeCp(0x50,0x200)&&b.writeBp(0x80,0x99)&&b.writeBp(0x80,0x11)&&b.writeBp(0x28,0x48));
    assert(a.registers().stateHash.bp==b.registers().stateHash.bp&&a.registers().stateHash.texture==b.registers().stateHash.texture&&
           a.registers().stateHash.cp==b.registers().stateHash.cp&&a.registers().stateHash.xf==b.registers().stateHash.xf);
    // Words outside the key (TEV colour banks, fog colour, matrix indices,
    // viewport) leave it; every keyed word moves its own component.
    const auto before=a.registers().stateHash;
    assert(a.writeBp(0xe0,0x123)&&a.writeBp(0xf2,0x456)&&a.writeCp(0x30,7));
    {const u32 v=0x3f800000;assert(a.writeXf(0x101a,{&v,1})&&a.writeXf(0x1018,{&v,1})&&a.writeXf(0x100a,{&v,1}));}
    assert(a.registers().stateHash.bp==before.bp&&a.registers().stateHash.texture==before.texture&&
           a.registers().stateHash.cp==before.cp&&a.registers().stateHash.xf==before.xf);
    assert(a.writeBp(0xf1,0x0fffff)&&a.registers().stateHash.bp==before.bp); // only bits 20+ of BP 0xf1 count
    assert(a.writeBp(0xf1,0x100000)&&a.registers().stateHash.bp!=before.bp);
    assert(a.writeBp(0x84,1)&&a.registers().stateHash.texture!=before.texture);
    assert(a.writeCp(0xb3,9)&&a.registers().stateHash.cp!=before.cp);
    {const u32 v=1;assert(a.writeXf(0x1030,{&v,1})&&a.registers().stateHash.xf!=before.xf);}
    // The switch and the verification.
    std::uint32_t c[4];StateHashStatistics st;stateHashStatistics(st,true);
    setStateHash(true,true);
    assert(stateHashComponents(p.registers(),c[0],c[1],c[2],c[3])&&c[0]==p.registers().stateHash.bp&&c[3]==p.registers().stateHash.xf);
    assert(stateHashComponents(a.registers(),c[0],c[1],c[2],c[3])&&stateHashComponents(a.registers(),c[0],c[1],c[2],c[3]));
    stateHashStatistics(st,true);assert(st.keys==3&&st.checks==2&&st.mismatches==0);
    CommandRegisters handBuilt{};assert(!stateHashComponents(handBuilt,c[0],c[1],c[2],c[3])); // no processor keeps it
    CommandRegisters copied=a.registers();copied.bp[0x28]^=1; // a stale hash is caught and replaced
    assert(stateHashComponents(copied,c[0],c[1],c[2],c[3])&&c[0]==computeStateHash(copied).bp);
    stateHashStatistics(st,true);assert(st.mismatches==1&&st.firstComponent=='B');
    setStateHash(false,false);assert(!stateHashComponents(a.registers(),c[0],c[1],c[2],c[3]));
    setStateHash(true,false);
    assert(memory.remove(handle));
    puts("PASS incremental state key: the four components equal a full recomputation after BP/CP/XF, masked, raw, compiled-list, indexed, TLUT and reset writes; order independent; keyed words only; verification and switch");
}
int main(){
    // Full registry, slot reuse and stale handles, including the last slot.
    CommandMemory full;std::array<u8,CommandMemory::capacity+1> regions{};
    std::array<MemoryHandle,CommandMemory::capacity> handles{};std::span<const u8> resolved;
    for(unsigned i=0;i<full.capacity;++i)assert(full.add({&regions[i],1},handles[i]));
    MemoryHandle extra;assert(!full.add({&regions.back(),1},extra)&&extra==0);
    for(unsigned i=0;i<full.capacity;++i){
        assert(full.resolve(handles[i],0,1,resolved)&&resolved.data()==&regions[i]);
        assert(!full.resolve(handles[i],1,1,resolved)&&resolved.empty());
    }
    const auto expired=handles.back();assert(full.remove(expired));
    assert(full.add({&regions.back(),1},extra)&&extra!=expired);
    assert(!full.resolve(expired,0,1,resolved)&&!full.remove(expired));
    assert(full.resolve(extra,0,1,resolved)&&resolved.data()==&regions.back());
    assert(!full.resolve(0,0,0,resolved)&&!full.remove(0));
    CommandMemory memory;CommandProcessor processor(memory);Capture result;processor.setSink(capture,&result);
    std::array<float,9> positions{1,2,3,4,5,6,7,8,9};std::array<s16,6> uv{0,256,128,64,256,0};
    auto bytes=[](const auto& a){return std::span<const u8>(reinterpret_cast<const u8*>(a.data()),sizeof(a));};
    MemoryHandle posHandle,uvHandle,duplicate;
    assert(memory.add(bytes(positions),posHandle)&&memory.add(bytes(uv),uvHandle));
    assert(!memory.add(bytes(positions).subspan(4),duplicate)&&duplicate==0);
    alignas(32) u8 storage[4096]{};GDLObj gd;GDInitGDLObj(&gd,storage,sizeof(storage));GDSetCurrent(&gd);
    const GXVtxDescList desc[]{{GX_VA_POS,GX_INDEX16},{GX_VA_TEX0,GX_INDEX8},{GX_VA_NULL,GX_NONE}};
    const GXVtxAttrFmtList formats[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_TEX0,GX_TEX_ST,GX_S16,8},{GX_VA_NULL,GX_POS_XY,GX_U8,0}};
    GDSetVtxDescv(desc);GDSetVtxAttrFmtv(GX_VTXFMT0,formats);
    GDSetArraySized(GX_VA_POS,positions.data(),sizeof(positions),12,true);
    GDSetArraySized(GX_VA_TEX0,uv.data(),sizeof(uv),4,true);
    GDWriteBPCmd(0x410000ff);GDWriteBPCmd(0xfe00000f);GDWriteBPCmd(0x41001230);GDWriteBPCmd(0x42000123);
    const u32 geometryOffset=GDGetCurrOffset();
    GDWrite_u8(GX_TRIANGLES);GDWrite_u16(3);
    for(u16 i=0;i<3;++i){GDWrite_u16(i);GDWrite_u8(2-i);}
    const auto all=std::span<const u8>(storage,GDGetCurrOffset()),geometry=all.subspan(geometryOffset);
    const std::vector<u8> original(all.begin(),all.end());
    assert(processor.execute(all));assert(result.vertices.size()==3);close(result.vertices[2].position[1],8);
    close(result.vertices[0].texcoord[0][0],1);close(result.vertices[1].texcoord[0][1],0.25f);
    assert(processor.registers().bp[0x41]==0xf0&&processor.registers().bp[0x42]==0x123);
    assert(processor.statistics().plans==1&&processor.statistics().draws==1);
    assert(processor.execute(all)&&processor.execute(geometry));assert(processor.statistics().plans==1&&processor.statistics().draws==3);
    // All prefixes shorter than a complete geometry packet fail, without a draw.
    for(size_t i=1;i<geometry.size();++i){CommandProcessor p(memory);p.setSink(capture,&result);assert(p.execute(all.first(geometryOffset)));
      assert(!p.execute(geometry.first(i))&&p.error()==CommandError::truncated&&p.statistics().draws==0);}
    assert(memory.remove(posHandle));MemoryHandle replacement;assert(memory.add(bytes(positions),replacement)&&replacement!=posHandle);
    assert(!processor.execute(geometry)&&processor.error()==CommandError::staleMemory);
    processor.reset();assert(processor.execute(all));
    // An oversized vertex index reaches the bounded decoder, never a raw pointer read.
    std::vector<u8> bad(geometry.begin(),geometry.end());bad[3]=0xff;bad[4]=0xff;
    assert(!processor.execute(bad)&&processor.error()==CommandError::sinkRejected);
    processor.reset();assert(processor.execute(all));
    // Strips optimized by the original Aurora routine preserve prebuilt indices.
    std::vector<u8> strip{GX_TRIANGLESTRIP,0,4,0,0,0,0,1,1,0,2,2,0,0,0};
    auto optimized=aurora::gx::dl::optimize(strip.data(),strip.size(),desc);assert(optimized);
    assert(processor.execute(*optimized));assert((result.indices==std::vector<u16>{0,1,2,2,1,3}));
    auto wrong=*optimized;wrong[10]=0xff;wrong[11]=0xff;assert(!processor.execute(wrong)&&processor.error()==CommandError::invalidIndex);
    // Every texture VAT field, including TEX4 spanning VAT B/C.
    processor.reset();GDInitGDLObj(&gd,storage,sizeof(storage));GDSetCurrent(&gd);
    std::vector<GXVtxDescList> direct{{GX_VA_POS,GX_DIRECT}};std::vector<GXVtxAttrFmtList> fmt{{GX_VA_POS,GX_POS_XYZ,GX_F32,0}};
    for(unsigned i=0;i<8;++i){direct.push_back({static_cast<GXAttr>(GX_VA_TEX0+i),GX_DIRECT});fmt.push_back({static_cast<GXAttr>(GX_VA_TEX0+i),GX_TEX_ST,GX_S16,static_cast<u8>(i+1)});}
    direct.push_back({GX_VA_NULL,GX_NONE});fmt.push_back({GX_VA_NULL,GX_POS_XY,GX_U8,0});
    GDSetVtxDescv(direct.data());GDSetVtxAttrFmtv(GX_VTXFMT3,fmt.data());
    const u32 directOffset=GDGetCurrOffset();
    GDWrite_u8(GX_POINTS|3);GDWrite_u16(1);GDWrite_f32(-1);GDWrite_f32(2);GDWrite_f32(0);
    for(unsigned i=0;i<8;++i){GDWrite_u16(2u<<(i+1));GDWrite_u16(static_cast<u16>(-(3<<(i+1))));}
    assert(processor.execute({storage,GDGetCurrOffset()}));assert(processor.statistics().plans==1);
    for(unsigned i=0;i<8;++i){close(result.vertices[0].texcoord[i][0],2);close(result.vertices[0].texcoord[i][1],-3);}
    // Rewriting a used VAT invalidates exactly its plan; the new fraction is observed.
    const std::vector<u8> directGeometry(storage+directOffset,storage+GDGetCurrOffset());
    GDSetCurrOffset(0);fmt[1].frac=2;GDSetVtxAttrFmtv(GX_VTXFMT3,fmt.data());
    assert(processor.execute({storage,GDGetCurrOffset()}));assert(processor.execute(directGeometry));
    assert(processor.statistics().plans==2);close(result.vertices[0].texcoord[0][0],1);
    close(result.vertices[0].texcoord[1][0],2);
    // Indexed position-matrix upload follows the registered byte bound/stride/endian.
    std::array<float,24> matrices{};for(unsigned i=0;i<24;++i)matrices[i]=static_cast<float>(i)+0.25f;
    MemoryHandle matrixHandle;assert(memory.add(bytes(matrices),matrixHandle));
    GDInitGDLObj(&gd,storage,sizeof(storage));GDSetCurrent(&gd);
    GDSetArraySized(GX_POS_MTX_ARRAY,matrices.data(),sizeof(matrices),48,true);
    GDWriteXFIndxACmd(0,12,1);assert(processor.execute({storage,GDGetCurrOffset()}));
    for(unsigned i=0;i<12;++i)assert(processor.registers().xf[i]==std::bit_cast<u32>(matrices[12+i]));
    GDSetCurrOffset(0);GDWriteXFIndxACmd(0,12,2);
    assert(!processor.execute({storage,GDGetCurrOffset()})&&processor.error()==CommandError::arrayBounds);
    // Direct XF writes are transactional per packet, including every truncated prefix.
    const u8 xf[]{GX_LOAD_XF_REG,0,1,0x10,0x56,0x12,0x34,0x56,0x78,0x90,0xab,0xcd,0xef};
    for(size_t n=1;n<sizeof(xf);++n){processor.reset();assert(!processor.execute({xf,n}));
        assert(processor.error()==CommandError::truncated&&!processor.registers().xfValid.any());}
    processor.reset();assert(processor.execute(xf));
    assert(processor.registers().xf[0x1056]==0x12345678&&processor.registers().xf[0x1057]==0x90abcdef);
    auto invalidXF=std::vector<u8>(std::begin(xf),std::end(xf));invalidXF[4]=0x57;
    assert(!processor.execute(invalidXF)&&processor.error()==CommandError::unsupported);
    // Array records reject unregistered memory and malformed flags before exposing it.
    processor.reset();GDSetCurrOffset(0);
    GDSetArraySized(GX_VA_POS,positions.data(),sizeof(positions),12,true);
    assert(memory.remove(replacement));
    assert(!processor.execute({storage,GDGetCurrOffset()})&&processor.error()==CommandError::unregisteredMemory);
    assert(memory.add(bytes(positions),replacement));processor.reset();storage[15]=2;
    assert(!processor.execute({storage,GDGetCurrOffset()})&&processor.error()==CommandError::format);
    // Reentrancy cannot invalidate a draw's borrowed register/plan state.
    processor.reset();processor.setSink([](const CommandDraw&,void* p){return !static_cast<CommandProcessor*>(p)->execute({});},&processor);
    assert(!processor.execute(original)&&processor.error()==CommandError::reentrant);
    // Reset leaves no stale format/binding admission.
    processor.reset();processor.setSink(capture,&result);const u8 unconfigured[]{GX_TRIANGLES,0,3};
    assert(!processor.execute(unconfigured)&&processor.error()==CommandError::format);
    processor.reset();const u8 nested[]{GX_CMD_CALL_DL,0,0,0,0,0,0,0,0};assert(!processor.execute(nested)&&processor.error()==CommandError::unsupported);
    // Compare actual ordered triangle/UV/position output with and without
    // batching, including mixed primitives, state barriers and storage splits.
    struct Triangles {std::vector<std::array<float,6>> vertices;unsigned calls{};};
    auto triangles=[](const CommandDraw& d,void* context){
        auto& out=*static_cast<Triangles*>(context);++out.calls;
        auto vertex=[&](unsigned i){DecodedVertex v;assert(d.vertex(i,v));
            out.vertices.push_back({v.position[0],v.position[1],v.position[2],v.texcoord[0][0],v.texcoord[0][1],float(d.registers.bp[0x41])});};
        if(d.indexCount)for(unsigned i=0;i<d.indexCount;++i){u16 index;assert(d.triangleIndex(i,index));vertex(index);}
        else assert(aurora::gx::dl::expand_triangles(d.primitive,d.vertexCount,[&](u16 a,u16 b,u16 c){vertex(a);vertex(b);vertex(c);}));
        return true;
    };
    std::vector<u8> mixed;
    for(unsigned i=0;i<3000;++i){
        const unsigned n=i%4==0?4:i%4==1?5:i%4==2?3:4;
        const u8 op=i%4==0?GX_TRIANGLESTRIP:i%4==1?GX_TRIANGLEFAN:i%4==2?GX_TRIANGLES:GX_QUADS;
        mixed.insert(mixed.end(),{op,0,u8(n)});
        for(unsigned v=0;v<n;++v)mixed.insert(mixed.end(),{0,u8(v%3),u8((v+1)%3)});
        if(i%127==126)mixed.insert(mixed.end(),{GX_LOAD_BP_REG,0x41,0,0,u8(i)});
    }
    CommandProcessor plain(memory),batched(memory);Triangles reference,grouped;
    plain.setSink(triangles,&reference);batched.setSink(triangles,&grouped);
    std::array<u8,1024> batchVertices;std::array<u16,384> batchIndices;
    batched.setPrimitiveBatchScratch({batchVertices,batchIndices,128});
    assert(plain.execute(std::span(original).first(geometryOffset))&&batched.execute(std::span(original).first(geometryOffset)));
    assert(plain.execute(mixed)&&batched.execute(mixed));
    assert(reference.vertices==grouped.vertices&&grouped.calls<reference.calls/10);
    assert(plain.statistics().commands==batched.statistics().commands&&batched.statistics().mergedPrimitives>2900);
    // Deduplication compares every packed byte, preserving ordered original
    // triangles/UVs/state even across hash collisions and bounded batch splits.
    CommandProcessor deduplicated(memory);Triangles compact;
    std::array<u16,256> hashSlots;std::array<u16,128> remap;
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,remap});
    deduplicated.setSink(triangles,&compact);
    assert(deduplicated.execute(std::span(original).first(geometryOffset))&&deduplicated.execute(mixed));
    assert(compact.vertices==reference.vertices&&compact.calls==grouped.calls);
    assert(deduplicated.statistics().reusedVertices>11000);

    // Snapshot reuse still resolves live arrays and consumes current state.
    // Mutating a packet at the same address must miss, not reuse old topology.
    CommandMemory cacheMemory;MemoryHandle cachedPosition,cachedUv;
    assert(cacheMemory.add(bytes(positions),cachedPosition)&&cacheMemory.add(bytes(uv),cachedUv));
    CommandProcessor cached(cacheMemory);Triangles retained;
    std::array<u8,128*1024> cacheStorage{};PrimitiveBatchCache topology;
    assert(topology.attach(cacheStorage));cached.setPrimitiveBatchCache(&topology);
    cached.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,remap});
    cached.setSink(triangles,&retained);
    assert(cached.execute(std::span(original).first(geometryOffset)));
    auto compareCached=[&]{
        reference={};retained={};assert(plain.writeBp(0x41,0)&&cached.writeBp(0x41,0));
        assert(plain.execute(mixed)&&cached.execute(mixed));assert(reference.vertices==retained.vertices);
    };
    compareCached();assert(cached.statistics().batchCacheMisses>0&&cached.statistics().batchCacheHits==0);
    compareCached();assert(cached.statistics().batchCacheHits>0);
    const auto cachedHits=cached.statistics().batchCacheHits;
    positions[0]+=12;uv[0]+=32;compareCached();assert(cached.statistics().batchCacheHits>cachedHits);
    positions[0]-=12;uv[0]-=32;
    const auto oldIndex=mixed[4];mixed[4]=(oldIndex+1)%3;
    const auto misses=cached.statistics().batchCacheMisses;compareCached();
    assert(cached.statistics().batchCacheMisses>misses);mixed[4]=oldIndex;
    // Same bytes under a different decode fraction must use the new plan.
    const auto vatA=plain.registers().cp[0x70];
    const auto fraction=(vatA&~(31u<<25))|(9u<<25);
    assert(plain.writeCp(0x70,fraction)&&cached.writeCp(0x70,fraction));compareCached();
    assert(plain.writeCp(0x70,vatA)&&cached.writeCp(0x70,vatA));
    // Force many ring wraps and entry evictions with a small bounded pool.
    assert(topology.attach(std::span(cacheStorage).first(4096)));
    for(unsigned pass=0;pass<3;++pass)compareCached();
    assert(topology.attach(cacheStorage));compareCached();compareCached();
    assert(cacheMemory.remove(cachedPosition));
    assert(!cached.execute(mixed)&&cached.error()==CommandError::staleMemory);
    assert(cacheMemory.add(bytes(positions),cachedPosition));
    cached.reset();assert(cached.execute(std::span(original).first(geometryOffset)));
    compareCached();assert(cached.statistics().batchCacheHits==0);
    // Alias/extent rejection must leave an existing valid snapshot untouched.
    PrimitiveBatchCache::Result fixture{{mixed.data()+3,9},{reinterpret_cast<const u8*>(batchIndices.data()),6},3};
    assert(topology.store(std::span(mixed).first(12),3,fixture));
    assert(topology.find(std::span(mixed).first(12),3)&&!topology.find(std::span(mixed).first(12),4));
    assert(!topology.store(std::span(cacheStorage).first(12),3,fixture));
    auto invalidFixture=fixture;invalidFixture.vertexCount=65536;
    assert(!topology.store(std::span(mixed).first(12),3,invalidFixture));
    puts("PASS packet snapshot cache: exact ordered triangles, same-address mutations, current arrays/VAT/state, stale lifetime rejection, bounded eviction, reset and alias/extent refusal");
    deduplicated.reset();assert(deduplicated.execute(std::span(original).first(geometryOffset)));
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,{hashSlots.data(),128}});
    assert(!deduplicated.execute(mixed)&&deduplicated.error()==CommandError::format);
    deduplicated.reset();assert(deduplicated.execute(std::span(original).first(geometryOffset)));
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,{hashSlots.data(),16},remap});
    assert(!deduplicated.execute(mixed)&&deduplicated.error()==CommandError::format);
    deduplicated.reset();assert(deduplicated.execute(std::span(original).first(geometryOffset)));
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,remap});
    deduplicated.setSink([](const CommandDraw& d,void*){assert(d.vertexCount==3&&d.indexCount==6);
        for(unsigned n=0;n<6;++n){u16 index;assert(d.triangleIndex(n,index)&&index==0);}return true;});
    const u8 degenerate[]{GX_TRIANGLES,0,3,0,0,0,0,0,0,0,0,0,GX_TRIANGLES,0,3,0,0,0,0,0,0,0,0,0};
    assert(deduplicated.execute(degenerate));
    // Malformed tails still fail, and an aliased scratch rejects before copy.
    mixed.push_back(GX_TRIANGLES);assert(!batched.execute(mixed)&&batched.error()==CommandError::truncated);
    batched.reset();assert(batched.execute(std::span(original).first(geometryOffset)));
    batched.setPrimitiveBatchScratch({{mixed.data(),1024},batchIndices,128});
    assert(!batched.execute(mixed)&&batched.error()==CommandError::format);
    GDSetCurrent(nullptr);
    maskedWriteTests(memory);
    stateHashTests(memory);
    {CommandMemory templateMemory;bpReplayTests(templateMemory);templateTests(templateMemory);}
    {CommandMemory heldMemory;heldListTests(heldMemory);currentMatrixTests(heldMemory);}
    materialMemoTests(memory);
    puts("PASS: original GD-to-command execution, CP/VAT caching, registered arrays, generation expiry, BP mask, indexed XF, all UV formats and draw decoding");
}
