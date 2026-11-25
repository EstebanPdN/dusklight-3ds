// Exhaustive occupancy masks for J3D's ten skin slots, through the production
// batching helper, the real GX processor and the worker's bank-delta decoder.
// The small model/math fixture supplies diagonal transforms and singular data.
#include "../runtime/gx_bank_delta.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include <dolphin/gx/GXCommandList.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace dusk::native::gx;

extern "C" void OSPanic(const char*, int, const char* message, ...) { throw std::runtime_error(message); }
extern "C" void DCFlushRange(void*, u32) {}
thread_local StaticGeometryHint dusk::native::gx::staticGeometryHint{};

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); \
} } while (false)

using Mtx = float[3][4];
using Mtx33 = float[3][3];
using MtxP = float (*)[4];
struct BigIndex {
    u8 bytes[2];
    operator u16() const {return u16(bytes[0])*256+bytes[1];}
};
#define BE(T) BigIndex
struct J3DModelData {
    unsigned getDrawMtxIndex(u16 i) const {return i/2;}
    unsigned getDrawMtxFlag(u16 i) const {return i%2;}
    unsigned getWEvlpMtxNum() const {return 8;}
    unsigned getJointNum() const {return 16;}
};
static CommandProcessor* processor;
static unsigned xfCalls;
static struct {
    Mtx* bound{};
    void setModelDrawMtx(Mtx* table,unsigned size) {
        bound=table;
        CHECK(processor->bindArray(12,table,size*sizeof(Mtx),sizeof(Mtx),true));
    }
    Mtx& getModelDrawMtx(unsigned index) {return bound[index];}
} j3dSys;
void GXNativeLoadXf(u16 address,const u32* value,u32 count){
    ++xfCalls;CHECK(processor->writeXf(address,{value,count}));
}
void MTXConcat(MtxP a,MtxP b,MtxP dest){
    for(int i=0;i<3;++i) for(int j=0;j<4;++j)
        dest[i][j]=a[i][0]*b[0][j]+a[i][1]*b[1][j]+a[i][2]*b[2][j]+(j==3?a[i][3]:0);
}
// The branch fixture supplies diagonal matrices and includes singular inputs.
void J3DPSCalcInverseTranspose(MtxP mtx, Mtx33& dst) {
    if(mtx[0][0]*mtx[1][1]*mtx[2][2]==0)return;
    for(int i=0;i<3;++i)dst[i][i]=1.f/mtx[i][i];
}
#include "../../../libs/JSystem/src/J3DGraphBase/J3DNativeSkinMatrixRuns.h"

void reference(const BigIndex* indices,int slots,const u8* scale,MtxP base,const J3DModelData& model,MtxP const (&tables)[2]){
    for(int i=0;i<slots;++i){
        const u16 index=indices[i];if(index==0xffff)continue;
        const unsigned table=model.getDrawMtxFlag(index),draw=model.getDrawMtxIndex(index);
        j3dSys.setModelDrawMtx(reinterpret_cast<Mtx*>(tables[table]),table?8:16);
        Mtx position;MTXConcat(base,j3dSys.getModelDrawMtx(draw),position);
        u32 pos[12];std::memcpy(pos,position,sizeof(pos));
        GXNativeLoadXf(12*i,pos,12);
        Mtx33 normal{};
        if(scale[draw]==1){for(int r=0;r<3;++r)for(int c=0;c<3;++c)normal[r][c]=position[r][c];}
        else J3DPSCalcInverseTranspose(position,normal);
        u32 nrm[9];std::memcpy(nrm,normal,sizeof(nrm));
        GXNativeLoadXf(0x400+9*i,nrm,9);
    }
}
int main(){
    static Mtx joints[16]{},weights[8]{},base{{2,0,0,7},{0,3,0,-0.0f},{0,0,5,11}};
    for(unsigned i=0;i<16;++i)for(unsigned j=0;j<3;++j){
        joints[i][j][j]=float(i+j);
        if(i<8)weights[i][j][j]=float(i+j+4);
    }
    MtxP tables[2]{joints[0],weights[0]};
    u8 scale[16]{};
    for(unsigned i=0;i<16;++i)scale[i]=i%2;
    J3DModelData model;
    static CommandMemory memory;
    MemoryHandle handle;
    CHECK(memory.add({reinterpret_cast<const u8*>(joints),sizeof(joints)},handle));
    CHECK(memory.add({reinterpret_cast<const u8*>(weights),sizeof(weights)},handle));
    static CommandProcessor plain(memory),fast(memory);
    unsigned oldCalls=0,newCalls=0,oldWords=0,newWords=0;
    static u32 oldLog[512],newLog[512],tail[bankTailRunCapacity];
    plain.attachBankLog(oldLog);fast.attachBankLog(newLog);
    static CommandRegisters mirror;
    BigIndex groups[1024][10];
    for(unsigned m=0;m<1024;++m){
        for(unsigned i=0;i<10;++i)groups[m][i]=m>>i&1?BigIndex{{0,u8((i+m)%16)}}:BigIndex{{255,255}};
        for(int size=2;size<=10;++size){
            processor=&plain;xfCalls=0;plain.restartBankLog();
            reference(groups[m],size,scale,base,model,tables);oldCalls+=xfCalls;oldWords+=plain.bankStores().used;
            const auto before=j3dSys.bound;
            processor=&fast;xfCalls=0;fast.restartBankLog();
            loadSkinMatrixRuns(groups[m],size,scale,base,model,tables);newCalls+=xfCalls;newWords+=fast.bankStores().used;
            CHECK(before==j3dSys.bound);
            CHECK(firstBankDifference(plain.registers(),fast.registers())==bank_layout::words);
            // Read through the final binding on both processors. Unlike a
            // raw comparison of private Binding objects this ignores padding.
            if (before) {
                CHECK(plain.loadIndexed(GX_LOAD_INDX_A, 0, 0xb080));
                CHECK(fast.loadIndexed(GX_LOAD_INDX_A, 0, 0xb080));
                CHECK(firstBankDifference(plain.registers(), fast.registers()) == bank_layout::words);
            }
            const auto& log=fast.bankStores();
            CHECK(applyBankDelta(mirror,newLog,log.used));
            CHECK(applyBankDelta(mirror,tail,copyBankTail(fast.registers(),log.grewValid,tail)));
            CHECK(firstBankDifference(mirror,fast.registers())==bank_layout::words);
        }
    }
    std::printf("9216 palette cases: exact full banks, bindings and worker deltas. XF calls %u -> %u; bank-log words %u -> %u\n",oldCalls,newCalls,oldWords,newWords);
}
