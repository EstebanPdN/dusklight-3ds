#include "../gx_uniform_pica.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
extern "C" {
C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];
bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
}
using namespace dusk::native;
int main(){
    unsigned checks=0;
    const std::uint32_t patterns[]{0,0x80000000,0x3f800000,0xbf800000,1,0x7f800000,0x7fc00001,0x7fc00002};
    for(unsigned id=0;id<96;++id)for(auto bits:patterns){
        const float x=std::bit_cast<float>(bits);
        const auto expected=FVec4_New(x,.25f,-2.f,1.f);
        C3D_FVUnif[0][id]=FVec4_New(3,3,3,3);C3D_FVUnifDirty[0][id]=false;
        setGxVertexUniform(id,expected);assert(C3D_FVUnifDirty[0][id]);
        assert(!std::memcmp(&C3D_FVUnif[0][id],&expected,sizeof(expected)));
        C3D_FVUnifDirty[0][id]=false;
        setGxVertexUniform(id,expected);assert(!C3D_FVUnifDirty[0][id]);
        // Invalidation must survive an equal write; another shader's intervening
        // value must be replaced even when this draw repeats its previous data.
        C3D_FVUnifDirty[0][id]=true;setGxVertexUniform(id,expected);assert(C3D_FVUnifDirty[0][id]);
        C3D_FVUnif[0][id].z=17;C3D_FVUnifDirty[0][id]=false;
        setGxVertexUniform(id,expected);assert(C3D_FVUnifDirty[0][id]);
        assert(!std::memcmp(&C3D_FVUnif[0][id],&expected,sizeof(expected)));++checks;
    }
    C3D_Mtx matrix{};for(unsigned row=0;row<4;++row)matrix.r[row]=FVec4_New(row,2,3,4);
    setGxVertexMatrix(8,matrix);for(unsigned row=0;row<4;++row){
        assert(!std::memcmp(&C3D_FVUnif[0][8+row],&matrix.r[row],sizeof(C3D_FVec)));
        C3D_FVUnifDirty[0][8+row]=false;
    }
    matrix.r[2].y=19;setGxVertexMatrix(8,matrix);
    for(unsigned row=0;row<4;++row)assert(C3D_FVUnifDirty[0][8+row]==(row==2));
    for(unsigned slot=0;slot<4;++slot)for(int lights=1;lights<=8;++lights){
        C3D_IVUnif[0][slot]=IVec_Pack(123,123,123,123);C3D_IVUnifDirty[0][slot]=false;
        setGxVertexLoop(0x60+slot,lights-1,0,4,0);assert(C3D_IVUnifDirty[0][slot]);
        const auto expected=IVec_Pack(lights-1,0,4,0);
        assert(!std::memcmp(&C3D_IVUnif[0][slot],&expected,sizeof(expected)));
        C3D_IVUnifDirty[0][slot]=false;setGxVertexLoop(0x60+slot,lights-1,0,4,0);assert(!C3D_IVUnifDirty[0][slot]);
        C3D_IVUnifDirty[0][slot]=true;setGxVertexLoop(0x60+slot,lights-1,0,4,0);assert(C3D_IVUnifDirty[0][slot]);
    }
    std::printf("PASS %u float uniform cases, exact signed-zero/NaN bits, dirty preservation, intervening writers, changed matrix rows and all light-loop slots\n",checks);
}
