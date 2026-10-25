#include "../runtime/font_batch.hpp"
#include <dolphin/gx.h>
#include <bit>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace dusk::native;
struct Vertex {std::uint32_t x,y,z,color;std::uint16_t u,v;const void* texture;bool operator==(const Vertex&)const=default;};
static std::vector<Vertex> actual,expected;
static std::vector<unsigned> draws;
static const void* bound;
static Vertex pending{};
static unsigned remaining,phase;
extern "C" {
void GXBegin(GXPrimitive primitive,GXVtxFmt format,u16 count){
    assert(!remaining&&phase==0&&primitive==GX_QUADS&&format==GX_VTXFMT0&&count&&count<=128&&count%4==0);
    remaining=count;draws.push_back(count);
}
void GXPosition3f32(f32 x,f32 y,f32 z){assert(remaining&&phase==0);pending={std::bit_cast<u32>(x),std::bit_cast<u32>(y),std::bit_cast<u32>(z),0,0,0,bound};phase=1;}
void GXColor1u32(u32 color){assert(phase==1);pending.color=color;phase=2;}
void GXTexCoord2u16(u16 u,u16 v){assert(phase==2);pending.u=u;pending.v=v;actual.push_back(pending);phase=0;--remaining;}
void GXEnd(){assert(!remaining&&phase==0);}
}
static void reference(const FontQuad& q){
    const float x[]{q.left,q.right,q.right,q.left},y[]{q.top,q.top,q.bottom,q.bottom};
    const unsigned color[]{0,1,3,2},u[]{0,2,2,0},v[]{1,1,3,3};
    for(unsigned i=0;i<4;++i)expected.push_back({std::bit_cast<u32>(x[i]),std::bit_cast<u32>(y[i]),0,q.colors[color[i]],q.uv[u[i]],q.uv[v[i]],bound});
}
int main(){
    unsigned glyphs=0;char images[3]{};
    for(unsigned length:{0u,1u,31u,32u,33u,64u,65u,257u})for(unsigned pageEvery:{1u,7u,32u,300u}){
        actual.clear();expected.clear();draws.clear();bound=nullptr;
        {
            FontBatch batch;
            for(unsigned i=0;i<length;++i){
                const void* page=&images[(i/pageEvery)%3];
                const bool changed=page!=bound;
                assert(batch.selectTexture(page)==changed);bound=page;
                // Distinct per-corner gradients, UV boundaries, negative/fractional
                // coordinates and overlapping glyphs must survive grouping exactly.
                FontQuad q{float(i%9)-0.5f,-float(i%13),float(i%9)+17.25f,5.5f-float(i%13),
                    {i*0x17321u,0xff00ff00u+i,0x01020300u+i,0xaabbcc00u+i},
                    {u16(i*997),u16(i*179),u16(65535-i),u16(i*31)}};
                reference(q);batch.append(q);q.colors.fill(0);q.left=0; // caller reuse
                ++glyphs;
            }
            batch.flush();const auto count=draws.size();batch.flush();assert(count==draws.size());
        }
        assert(actual==expected);
        if(pageEvery==300)assert(draws.size()==(length+31)/32);
    }
    actual.clear();expected.clear();draws.clear();bound=&images[0];
    {
        FontBatch batch;assert(batch.selectTexture(bound));
        FontQuad q{0,1,2,3,{1,2,3,4},{5,6,7,8}};reference(q);batch.append(q);
        batch.boundary(); // flush before overwrite at the identical page address
        assert(actual==expected&&draws.size()==1);assert(batch.selectTexture(bound));
        q.colors[0]=99;reference(q);batch.append(q); // destructor consumes final glyph
    }
    assert(actual==expected&&draws.size()==2);
    std::printf("PASS %u glyphs: exact original quad vertices/colors/UV order, 32-glyph capacity, page changes, same-address overwrite barrier, explicit and scope-end flush\n",glyphs+2);
}
