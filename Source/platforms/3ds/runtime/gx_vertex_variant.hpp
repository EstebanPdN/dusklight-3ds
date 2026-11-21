#pragma once
// What the multi-texture rigid vertex program (gx_rigid_mt.v.pica) decides
// per vertex, read from the uniform rows it reads.
//
// The program is one general program: every vertex tests uniform rows
// (cmp + ifc) for the depth clamp, the colour sources, the light loop, three
// texture-coordinate generators and the fog. The PICA's vertex work follows
// the instructions each vertex runs (console, Alpha 20: about 0.26 ms per
// 1000 indices with this program, 0.15 with the 16-instruction caster
// program), so the key below names the path a draw's vertices take; the
// census in gx_pica.cpp counts draws and indices per key.
#include <array>
#include <bit>
#include <cstdint>

namespace dusk::native::gx {
namespace vertexKey {
// Unit roles (u[13].z, u[13].w, u[18].w), three bits per unit.
enum Role : std::uint32_t {roleNone=0,roleCoordinate=1,roleNormal=2,roleFogRamp=3,roleProjectedHere=4,roleProjectedPica=5,roleOther=6};
constexpr unsigned unitShift(unsigned unit){return 3*unit;}
constexpr std::uint32_t depthClamp=1u<<9,paired=1u<<10,ambientVertex=1u<<11,materialVertex=1u<<12,alphaVertex=1u<<13;
// Fog: u[12].x 0 off, 1 linear, else constant ramp coordinate; u[12].y range; u[12].z carrier.
constexpr unsigned fogShift=14;
enum Fog : std::uint32_t {fogOff=0,fogLinear=1,fogConstant=2};
constexpr std::uint32_t fogRange=1u<<16;
constexpr unsigned carrierShift=17; // 0 ramp unit, 1 alpha, 2 red
constexpr unsigned lightShift=19;   // the light loop's count, 1..8
// Every light of the loop has a zero colour and the ambient sum cannot be
// -0: the loop adds +-0 to it, which changes no bit (see darkLights).
constexpr std::uint32_t darkLights=1u<<23;
constexpr std::uint32_t fogOnly=1u<<24; // b0: the fog pass of a rigid fog pair
constexpr std::uint32_t role(std::uint32_t key,unsigned unit){return key>>unitShift(unit)&7;}
constexpr std::uint32_t fog(std::uint32_t key){return key>>fogShift&3;}
constexpr std::uint32_t carrier(std::uint32_t key){return key>>carrierShift&3;}
constexpr unsigned lights(std::uint32_t key){return key>>lightShift&15;}
}
using UniformRows=std::array<std::array<float,4>,94>;
inline bool signBit(float value){return std::bit_cast<std::uint32_t>(value)>>31;}
// The tests of the program, in its words: unit 0 "cmp u[13].z, eq, lt, (1,0)"
// (== 1, < 0), "cmp u[13].z, gt, gt, 1" (projected) and "== 0.5" (fog ramp);
// units 1 and 2 the same without the projected form.
inline std::uint32_t vertexUnitRole(float role,unsigned unit,float projectionMode){
    using namespace vertexKey;
    if(role==1)return roleCoordinate;
    if(role<0)return roleNormal;
    if(role==0.5f)return roleFogRamp;
    if(role==0)return roleNone;
    if(unit==0&&role>1)return projectionMode>0?roleProjectedPica:roleProjectedHere;
    return roleOther;
}
// `lights`: the loop count the draw uploads (RigidVertexPlan::lights, 1..8).
inline std::uint32_t vertexPlanKey(const UniformRows& u,unsigned lights,bool fogOnly){
    using namespace vertexKey;
    std::uint32_t key=vertexUnitRole(u[13][2],0,u[92][2])<<unitShift(0)|vertexUnitRole(u[13][3],1,0)<<unitShift(1)|
        vertexUnitRole(u[18][3],2,0)<<unitShift(2);
    if(u[10][3]>0)key|=depthClamp;
    if(u[12][3]>0)key|=paired;
    if(u[17][3]>0)key|=ambientVertex;
    if(u[14][3]>0)key|=materialVertex;
    if(u[15][3]>=0)key|=alphaVertex;
    if(u[12][0]>0){
        key|=(u[12][0]==1?fogLinear:fogConstant)<<fogShift;
        if(u[12][1]>0)key|=fogRange;
    }
    // The carrier is read only with the fog on; keep it out of the key otherwise.
    if(u[12][0]>0&&u[12][2]>0)key|=(u[12][2]>1?2u:1u)<<carrierShift;
    key|=std::uint32_t(lights&15)<<lightShift;
    // The loop adds colour[aL+20].xyz * (a finite factor) to the ambient sum:
    // +-0 for a zero colour. x + (+-0) == x for every x but -0, and the sum
    // starts at u[10].xyz (non-negative colours) or at u[16].w * vertex colour.
    bool dark=lights>=1&&lights<=8&&!signBit(u[10][0])&&!signBit(u[10][1])&&!signBit(u[10][2])&&
        (!(u[17][3]>0)||!signBit(u[16][3]));
    for(unsigned n=0;dark&&n<lights;++n){
        const auto& c=u[20+4*n];
        dark=c[0]==0&&c[1]==0&&c[2]==0;
    }
    if(dark)key|=darkLights;
    if(fogOnly)key|=vertexKey::fogOnly;
    return key;
}
// Variant programs (features::Id::vertexVariants). Program n of
// gx_rigid_mt.shbin is entry n here (CMakeLists.txt assembles them in this
// order, the general program first). A variant runs the general program's
// arithmetic for the plans whose key has `value` in the bits of `mask`, minus
// the tests those bits decide; the bits outside the mask (light count, the
// material's vertex colour and alpha) it still tests or loops over. The
// census of the owner's scenes chose them (Alpha 21, emulator, five scenes):
// lit plans with linear fog and its range adjustment carry most indices.
struct VertexVariant {std::uint32_t mask,value;const char* name;};
namespace vertexKey {
constexpr std::uint32_t roles=0x1ffu;
// What every variant fixes: the roles, the fog form, the carrier, and no
// depth clamp, paired texgen, ambient vertex colour or fog-only pass.
constexpr std::uint32_t variantMask=roles|depthClamp|paired|ambientVertex|3u<<fogShift|fogRange|3u<<carrierShift|fogOnly;
constexpr std::uint32_t units(Role u0,Role u1,Role u2){return u0<<unitShift(0)|u1<<unitShift(1)|u2<<unitShift(2);}
constexpr std::uint32_t rangeFog=fogLinear<<fogShift|fogRange;
}
inline constexpr VertexVariant vertexVariants[]{
    {0,0,"general"},
    {vertexKey::variantMask,vertexKey::units(vertexKey::roleCoordinate,vertexKey::roleNone,vertexKey::roleNone)|vertexKey::rangeFog|1u<<vertexKey::carrierShift,
     "texture, fog in alpha"},
    {vertexKey::variantMask,vertexKey::units(vertexKey::roleCoordinate,vertexKey::roleFogRamp,vertexKey::roleNone)|vertexKey::rangeFog,
     "texture, fog ramp"},
    {vertexKey::variantMask,vertexKey::units(vertexKey::roleProjectedHere,vertexKey::roleCoordinate,vertexKey::roleFogRamp)|vertexKey::rangeFog,
     "projected, texture, fog ramp"},
    {vertexKey::variantMask,vertexKey::units(vertexKey::roleProjectedHere,vertexKey::roleCoordinate,vertexKey::roleCoordinate),
     "projected, two textures, no fog"},
};
inline constexpr unsigned vertexVariantCount=sizeof(vertexVariants)/sizeof(vertexVariants[0]);
// The program for a key: a variant whose bits match, else 0 (the general program).
inline unsigned vertexVariantFor(std::uint32_t key){
    for(unsigned n=1;n<vertexVariantCount;++n)if((key&vertexVariants[n].mask)==vertexVariants[n].value)return n;
    return 0;
}
}
