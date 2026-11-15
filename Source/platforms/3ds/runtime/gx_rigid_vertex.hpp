#pragma once
#include "gx_vertex_transform.hpp"
#include "gx_raster_color.hpp"
#include "gx_fog.hpp"
#include "gx_gpu_geometry.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dusk::native::gx {
// Original rigid GX vertices: position/normal matrices, clamped spot lighting,
// identity TEX0 and linear perspective range fog executed by PICA. The shared
// 60-byte stream carries raw position.xyz/normal.z and normal.xy in UV1.
// Unsupported state falls back to the existing CPU adapter before submission.
struct RigidVertexPlan {
    std::array<std::array<float,4>,94> uniforms{};
    std::array<std::int8_t,10> matrixOffsets{};
    // Multi-texture only: uniform row of each matrix's normal 3x3 (v4.y).
    std::array<std::int8_t,10> normalOffsets{};
    unsigned matrices{};
    // Multi-texture palette overflow retry: 3 rows per matrix; the normal
    // reuses the position rows when proportional (the shader renormalizes).
    bool compactPalette{};
    unsigned paletteEnd{};
    bool indexedMatrices{};
    bool compact{};
    bool projectedTexture{};
    // Projected draws: texgen matrix of the unprojected base texture as
    // s'=a*s+b*t+c, t'=d*s+e*t+f (identity when the texgen uses none).
    std::array<float,6> baseTexgen{1,0,0,0,1,0};
    // Native static geometry: GPU-resident compact vertices to bind directly
    // instead of the per-frame arena copy (null when the arena holds them).
    const std::byte* staticVertices{};
    // Native multi-texture path (gx_rigid_mt.v.pica): GPU texgen for up to
    // three units. Unit n keeps its four rows at multiUnitRow(n) (unit 0
    // highest) and its role in a component the fog rows leave free
    // (multiRole); the palette may use every row below the lowest unit that
    // needs rows (multiRowUnits of them, counted from unit 0).
    bool multiTexture{};
    std::uint8_t multiRowUnits{3};
    std::array<std::uint8_t,3> unitSource{}; // 0 TEX0, 1 TEX1 per unit (role 1)
    // Multi-texture: unit 0 is projected from the position (role 2): u[81..83]
    // are its S, T and Q rows on (position,1), u[84].xy the texture scales.
    bool multiProjected{};
    // Compact records store positions relative to this point (decoders set
    // it; finishCompactRigidUniforms folds it into every translation). The
    // console PICA computes with 24-bit floats: world coordinates near 40 000
    // lose half a unit per product, which shows as wobbling polygons.
    std::array<float,3> origin{};
    // The origin the rows currently carry (zero for rows freshly placed from
    // the registers): finishing a plan twice does not fold it twice.
    std::array<float,3> foldedOrigin{};
    bool normalsUnused{}; // unlit: normal matrix rows may be absent (zeroed)
    // Fixed-point scales of the decoded compact records (native shape replay).
    float decodedNormalScale{},decodedTex0Scale{1},decodedTex1Scale{1};
    RasterColorPlan color;
    unsigned lights{};
    unsigned projectionBase() const { return 19+4*lights; }
    // First row after the palette.
    unsigned paletteLimit() const { return multiTexture?81u+4u*(3u-multiRowUnits):93u; }
    // Projected: three projection rows, the texture scales, two base texgen rows.
    unsigned paletteBase() const { return projectionBase()+(projectedTexture?6:0); }
    unsigned uniformCount() const { return compactPalette?std::max(paletteEnd,paletteBase()):paletteBase()+6*(matrices?matrices-1:0); }
};
// A/B switch (SD file mt-wide-palette-off.txt): off keeps the palette below
// row 81 whatever the number of texture units, as before 2026-11-14.
inline bool multiTextureWidePalette{true};
// Multi-texture uniform layout (gx_rigid_mt.v.pica).
constexpr unsigned multiUnitRow(unsigned unit){return 89-4*unit;}
inline float& multiRole(RigidVertexPlan& p,unsigned unit){return unit==0?p.uniforms[13][2]:unit==1?p.uniforms[13][3]:p.uniforms[18][3];}
inline float multiRole(const RigidVertexPlan& p,unsigned unit){return unit==0?p.uniforms[13][2]:unit==1?p.uniforms[13][3]:p.uniforms[18][3];}
// GX clip-disabled draws: the shader keeps clip z inside [-w,0].
inline void setMultiDepthClamp(RigidVertexPlan& p){p.uniforms[10][3]=1;}
// Diagnostics: first failing condition of planRigidMultiVertex (see game_startup).
inline std::array<std::uint32_t,64> gxRigidPlanRejects{};
// Diagnostics: compact multi-texture decode failures (0-7 general, 8+slot*8+type unsupported field).
inline std::array<std::uint32_t,64> gxCompactDecodeRejects{};
inline bool addRigidMatrix(const CommandRegisters& r,unsigned index,RigidVertexPlan& p){
    if(index>27||index%3)return false;
    if(p.matrixOffsets[index/3]>=0)return true;
    if(p.matrices>=10)return false;
    std::array<std::array<float,4>,6> values{};
    for(unsigned row=0;row<3;++row){
        for(unsigned c=0;c<4;++c){const auto at=4*index+4*row+c;if(!r.xfValid[at])return false;values[row][c]=std::bit_cast<float>(r.xf[at]);}
        for(unsigned c=0;c<3;++c){const auto at=0x400+3*index+3*row+c;
            if(!r.xfValid[at]){if(p.normalsUnused){values[3+row][c]=0;continue;}return false;}
            values[3+row][c]=std::bit_cast<float>(r.xf[at]);}
    }
    for(const auto& row:values)for(float value:row)if(!std::isfinite(value)||std::abs(value)>1e8f)return false;
    if(p.compactPalette&&p.matrices){
        // Normal 3x3 == k * position 3x3 (k > 0) gives the same unit normal.
        bool shared=p.normalsUnused;
        if(!shared){
            float num=0,den=0;
            for(unsigned row=0;row<3;++row)for(unsigned c=0;c<3;++c){num+=values[3+row][c]*values[row][c];den+=values[row][c]*values[row][c];}
            if(den>0&&num>0){
                const float k=num/den;float error=0,size=0;
                for(unsigned row=0;row<3;++row)for(unsigned c=0;c<3;++c){
                    const float d=values[3+row][c]-k*values[row][c];error+=d*d;size+=values[3+row][c]*values[3+row][c];}
                shared=error<=size*1e-8f;
            }
        }
        const unsigned base=std::max(p.paletteEnd,p.paletteBase()),rows=shared?3:6;
        if(base+rows>p.paletteLimit())return false;
        for(unsigned row=0;row<rows;++row)p.uniforms[base+row]=values[row];
        p.matrixOffsets[index/3]=std::int8_t(base);p.normalOffsets[index/3]=std::int8_t(shared?base:base+3);
        p.paletteEnd=base+rows;++p.matrices;return true;
    }
    const unsigned base=p.matrices?p.paletteBase()+6*(p.matrices-1):0;
    if(base+6>(p.multiTexture?p.paletteLimit():unsigned(p.uniforms.size())))return false;
    for(unsigned row=0;row<6;++row)p.uniforms[base+row]=values[row];
    p.matrixOffsets[index/3]=base;p.normalOffsets[index/3]=std::int8_t(base+3);++p.matrices;return true;
}
// Restart a multi-texture palette in compact form, keeping the first matrix
// (rows 0..5) that the plan placed.
inline void restartCompactPalette(RigidVertexPlan& p){
    for(unsigned m=0;m<10;++m)if(p.matrixOffsets[m]>0){p.matrixOffsets[m]=-1;p.normalOffsets[m]=-1;}
    p.matrices=0;for(auto offset:p.matrixOffsets)if(offset==0)p.matrices=1;
    p.compactPalette=true;p.paletteEnd=p.paletteBase();
}
inline bool planRigidVertex(const CommandRegisters& r,const DecodedVertex& first,
    const VertexTransformPlan& transform,const RasterColorPlan& color,
    const VertexFogPlan& fog,bool primaryAlpha,unsigned logicalWidth,RigidVertexPlan& out,bool projectedTexture=false){
    out={};
    constexpr unsigned needed=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_TEX0);
    // First failing condition of a projected draw (codes 32-39): these have no
    // multi-texture route to fall back to and stay on the CPU vertex path.
    auto reject=[&](unsigned code){if(projectedTexture)++gxRigidPlanRejects[code];return false;};
    if((first.present&needed)!=needed||(first.present&2)||first.normalCount<1)return reject(32);
    if(!(color.required&1)||!(color.lit&1))return reject(33);
    if((color.ambientVertex&2)||color.channels[1].mask)return reject(34);
    if(color.channels[0].diffuse!=2||color.channels[0].attenuation!=1)return reject(35);
    if(!color.channels[0].mask)return reject(36);
    if(transform.coordinateMask()!=(projectedTexture?3:1)||transform.projectiveMask()!=(projectedTexture?2:0)||!r.matrixIndexValid[0])return reject(37);
    if(!r.xfValid[0x1040]||!r.xfValid[0x1050])return reject(38);
    const auto gen=r.xf[0x1040],post=r.xf[0x1050];
    // The base texture's texgen matrix is carried only by the projected form.
    const unsigned baseMatrix=(r.matrixIndex[0]>>6)&63;
    if(((gen>>4)&7)||((gen>>7)&31)!=5||(gen&2)||(post&0x13f)!=61||
       (baseMatrix!=60&&(!projectedTexture||baseMatrix>57||baseMatrix%3)))return reject(38);
    const unsigned position=first.present&1?first.matrix[0]:r.matrixIndex[0]&63;
    if(position>27||position%3)return reject(38);
    if(fog.enabled&&(fog.function!=2||fog.orthographic||fog.a<0||fog.b<=1))return reject(38);
    // Build in place (out was zeroed above): one 1.5 KiB uniform block per
    // draw instead of zeroing and copying a second one. Failure clears out.
    struct FailureReset {RigidVertexPlan& plan;bool keep=false;~FailureReset(){if(!keep)plan={};}} reset{out};
    auto& p=out;p.color=color;p.indexedMatrices=first.present&1;p.projectedTexture=projectedTexture;p.matrixOffsets.fill(-1);p.normalOffsets.fill(-1);
    if(!addRigidMatrix(r,position,p))return false;
    auto& u=p.uniforms;
    const auto& projection=transform.clipProjection();
    for(unsigned row=0;row<4;++row)for(unsigned c=0;c<4;++c)u[6+row][c]=projection[row*4+c];
    u[10]=color.ambient;
    // RGB ambient can come from the same original color attribute as material
    // RGB/alpha. Keep these sources independent until the light sum is clamped.
    u[93]=color.material;
    u[11]={float(fog.a),float(fog.b),float(fog.c),0};
    u[12]={float(fog.enabled),float(fog.rangeEnabled),float(primaryAlpha),0};
    if(fog.rangeEnabled){
        u[13]={float(-double(logicalWidth)/fog.viewportWidth),float((logicalWidth-2*fog.rangeCenter)/fog.viewportWidth),0,0};
        for(unsigned i=0;i<5;++i)u[14+i]={float(fog.rangeK[2*i]),float(fog.rangeK[2*i+1]),float(fog.rangeK[std::min(2*i+2,9u)]),0};
    }
    for(unsigned i=0;i<8;++i)if(color.channels[0].mask&(1u<<i)){
        const auto& l=color.lights[i];const unsigned n=19+4*p.lights++;
        // Prove the spot denominator is positive for every finite distance.
        if(l.distance[0]<=0||l.distance[1]<0||l.distance[2]<0)return false;
        // Pack the three spot coefficients into otherwise unused vector w.
        for(unsigned c=0;c<3;++c){u[n][c]=l.position[c];u[n+2][c]=l.direction[c];u[n+3][c]=l.distance[c];}
        u[n][3]=l.cosine[0];u[n+2][3]=l.cosine[1];u[n+3][3]=l.cosine[2];
        u[n+1]=l.color;
    }
    u[14][3]=float(color.vertex&1);
    u[17][3]=float(color.ambientVertex&1);
    if(projectedTexture){
        // The original background uses TEX0 unchanged and TEX1 generated from
        // original position by one 3x4 matrix, with an identity post transform.
        // PICA texture0 is the projected binding after material routing.
        if(first.present&(1u<<GX_VA_TEX1MTXIDX))return reject(39);
        if(!r.xfValid[0x1041]||!r.xfValid[0x1051])return reject(39);
        const auto gen1=r.xf[0x1041],post1=r.xf[0x1051];
        const unsigned selected=(r.matrixIndex[0]>>12)&63,base=p.projectionBase();
        if(((gen1>>4)&7)||((gen1>>7)&31)||!(gen1&2)||(post1&0x13f)!=61||selected>=60||selected%3||base+6>93)return reject(39);
        if(baseMatrix!=60){
            std::array<float,8> m;
            for(unsigned k=0;k<8;++k){
                const unsigned at=4*baseMatrix+k;if(!r.xfValid[at])return reject(39);
                m[k]=std::bit_cast<float>(r.xf[at]);if(!std::isfinite(m[k])||std::abs(m[k])>1e6f)return reject(39);
            }
            p.baseTexgen={m[0],m[1],m[2]+m[3],m[4],m[5],m[6]+m[7]};
        }
        for(unsigned row=0;row<3;++row)for(unsigned c=0;c<4;++c){
            const unsigned at=4*selected+4*row+c;if(!r.xfValid[at])return false;
            u[base+row][c]=std::bit_cast<float>(r.xf[at]);
        }
        u[18][3]=float(base);
    }
    // Only this contiguous prefix and the separate material uniform can be
    // uploaded. Unused light/palette slots stay zero and are never consumed;
    // later palette additions validate their own six rows in addRigidMatrix.
    for(unsigned row=0;row<p.uniformCount();++row)
        for(float value:u[row])if(!std::isfinite(value)||std::abs(value)>1e8f)return false;
    for(float value:u[93])if(!std::isfinite(value)||std::abs(value)>1e8f)return false;
    reset.keep=true;return true;
}
// Native PICA engine: rigid draws whose material samples 1-3 textures through
// regular 2x4 GX texgens from TEX0/TEX1 with an identity or palette texture
// matrix and identity post-transform. Lighting/fog/palette match the rigid
// plan; texture rows fold texgen matrix, texture scale and the vertical flip.
// decodeCompactRigidBatchMT later folds the fixed-point TEX fractions in.
inline bool planRigidMultiVertex(const CommandRegisters& r,const DecodedVertex& first,
    const VertexTransformPlan& transform,const RasterColorPlan& color,const VertexFogPlan& fog,bool primaryAlpha,
    unsigned logicalWidth,const PicaMaterialPlan& material,unsigned textureUnits,
    const std::array<std::array<float,2>,3>& scales,int fogUnit,RigidVertexPlan& out,bool allowProjected=false){
    out={};
    // First failing condition per rejected draw (sparse-frame diagnostics).
    auto reject=[](unsigned code){++gxRigidPlanRejects[code];return false;};
    // Unlit draws (2D/UI, emissive meshes) use ambient 1 and a zero-color dummy
    // light: the PICA light loop cannot run zero iterations.
    // Lighting that only feeds an RGB the material never reads (and leaves
    // alpha unlit) is the same as no lighting: any RGB may be output.
    const bool unlit=!color.lit||(!(color.required&1)&&!(color.lit&2));
    // Lit by ambient only: lighting is on but no light is in the channel mask
    // (dark-world scenery). The light sum is zero, so normals and the diffuse
    // and attenuation functions cannot matter; one zero-colour light keeps the
    // shader loop legal, exactly as for unlit draws.
    const bool ambientOnly=!unlit&&!color.channels[0].mask;
    const bool noNormals=unlit||ambientOnly;
    const unsigned needed=(1u<<GX_VA_POS)|(noNormals?0u:(1u<<GX_VA_NRM));
    if(textureUnits>3)return reject(0);
    if((first.present&needed)!=needed)return reject(1);
    if(!noNormals&&first.normalCount<1)return reject(3);
    // Unlit draws whose TEV ignores rasterized RGB (effects, billboards) may
    // output any RGB; only lit draws need the channel-0 colour itself.
    if(!(color.required&1)&&!unlit)return reject(4);
    if(!unlit&&!(color.lit&1))return reject(5);
    if(!unlit&&(color.ambientVertex&2))return reject(6);
    if(color.channels[1].mask)return reject(7);
    if(!noNormals&&color.channels[0].diffuse!=2)return reject(8);
    if(!noNormals&&color.channels[0].attenuation!=1)return reject(9);
    // One texture projected from the position is planned, on unit 0 (where
    // routeProjectiveTexture puts it); the shader divides it per vertex.
    const bool projected=material.projectiveTexture0;
    if(projected){
        if(!allowProjected||!textureUnits||material.textures[0].coordinate>7||
           transform.projectiveMask()!=(1u<<material.textures[0].coordinate))return reject(11);
    }else if(transform.projectiveMask())return reject(11);
    if(!r.matrixIndexValid[0])return reject(12);
    if(fogUnit>=0&&(fogUnit!=int(textureUnits)||fogUnit>2))return reject(13);
    struct Gen {std::array<std::array<float,4>,2> rows{};std::uint8_t source{};};
    std::array<Gen,3> gens{};
    std::array<std::array<float,4>,3> projectedRows{};
    for(unsigned u=0;u<textureUnits;++u){
        const unsigned c=material.textures[u].coordinate;
        if(c>7||!r.xfValid[0x1040+c]||!r.xfValid[0x1050+c])return reject(27);
        const auto gen=r.xf[0x1040+c],post=r.xf[0x1050+c];
        const unsigned source=(gen>>7)&31;
        // A per-vertex texture matrix selector is refused only for a
        // coordinate the material reads; formats often carry unused ones.
        if(first.present&(1u<<(GX_VA_TEX0MTXIDX+c)))return reject(2);
        if((gen>>4)&7)return reject(14);
        if(u==0&&projected){
            // Regular 3x4 texgen from the position (matrix or none), then an
            // optional post matrix without normalisation: as transformVertex,
            // composed into three rows on (position,1).
            if(!(gen&2)||source!=0)return reject(16);
            const unsigned postRow=post&63;
            if((post&0x100)||(postRow!=61&&(postRow>57||postRow%3)))return reject(17);
            if(c>=4&&!r.matrixIndexValid[1])return reject(28);
            const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
            std::array<std::array<double,4>,3> rows{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
            if(m!=60){
                if(m>=60||m%3)return reject(19);
                for(unsigned row=0;row<3;++row)for(unsigned k=0;k<4;++k){
                    const auto at=4*m+4*row+k;if(!r.xfValid[at])return reject(29);
                    const float value=std::bit_cast<float>(r.xf[at]);if(!std::isfinite(value)||std::abs(value)>1e6f)return reject(29);
                    rows[row][k]=value;
                }
            }
            if(postRow!=61){
                std::array<std::array<double,4>,3> composed{};
                for(unsigned row=0;row<3;++row){
                    double pr[4];
                    for(unsigned k=0;k<4;++k){
                        const auto at=0x500+4*(postRow+row)+k;if(!r.xfValid[at])return reject(29);
                        const float value=std::bit_cast<float>(r.xf[at]);if(!std::isfinite(value)||std::abs(value)>1e6f)return reject(29);
                        pr[k]=value;
                    }
                    for(unsigned k=0;k<4;++k)composed[row][k]=pr[0]*rows[0][k]+pr[1]*rows[1][k]+pr[2]*rows[2][k];
                    composed[row][3]+=pr[3];
                }
                rows=composed;
            }
            for(unsigned row=0;row<3;++row)for(unsigned k=0;k<4;++k)projectedRows[row][k]=float(rows[row][k]);
            continue;
        }
        if(gen&6)return reject(15);
        if(source!=5&&source!=6)return reject(16);
        const unsigned postRow=post&63;
        if((post&0x100)||(postRow!=61&&(postRow>57||postRow%3)))return reject(17);
        if(!(first.present&(1u<<(source==5?GX_VA_TEX0:GX_VA_TEX1))))return reject(18);
        if(c>=4&&!r.matrixIndexValid[1])return reject(28);
        const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
        auto& g=gens[u];g.source=source==5?0:1;
        if(m==60){g.rows[0]={1,0,0,0};g.rows[1]={0,1,0,0};}
        else{
            if(m>=60||m%3)return reject(19);
            for(unsigned row=0;row<2;++row)for(unsigned k=0;k<4;++k){
                const auto at=4*m+4*row+k;if(!r.xfValid[at])return reject(29);
                const float value=std::bit_cast<float>(r.xf[at]);if(!std::isfinite(value)||std::abs(value)>1e6f)return reject(29);
                g.rows[row][k]=value;
            }
        }
        // Non-normalized dual texgen: uv = P*(s',t',1,1) is linear in the
        // first-stage rows, so fold P into them (no shader cost).
        if(postRow!=61){
            std::array<std::array<float,4>,2> composed{};
            for(unsigned row=0;row<2;++row){
                float pr[4];
                for(unsigned k=0;k<4;++k){
                    const auto at=0x500+4*(postRow+row)+k;if(!r.xfValid[at])return reject(29);
                    pr[k]=std::bit_cast<float>(r.xf[at]);if(!std::isfinite(pr[k])||std::abs(pr[k])>1e6f)return reject(29);
                }
                for(unsigned k=0;k<4;++k)composed[row][k]=pr[0]*g.rows[0][k]+pr[1]*g.rows[1][k];
                composed[row][3]+=pr[2]+pr[3];
            }
            g.rows=composed;
        }
    }
    if(!r.xfValid[0x1040]||!r.xfValid[0x1050])return reject(30);
    const unsigned position=first.present&1?first.matrix[0]:r.matrixIndex[0]&63;
    if(position>27||position%3)return reject(31);
    if(fog.enabled&&(fog.function!=2||fog.orthographic||fog.a<0||fog.b<=1))return reject(20);
    struct FailureReset {RigidVertexPlan& plan;bool keep=false;~FailureReset(){if(!keep)plan={};}} reset{out};
    auto& p=out;p.color=color;p.indexedMatrices=first.present&1;p.multiTexture=true;p.normalsUnused=noNormals;p.matrixOffsets.fill(-1);p.normalOffsets.fill(-1);
    if(!addRigidMatrix(r,position,p))return reject(21);
    auto& u=p.uniforms;
    const auto& projection=transform.clipProjection();
    for(unsigned row=0;row<4;++row)for(unsigned c=0;c<4;++c)u[6+row][c]=projection[row*4+c];
    // Units that need rows: the textures (the fog ramp unit, after them,
    // only needs its role). Their count sets where the palette may end.
    p.multiRowUnits=std::uint8_t(multiTextureWidePalette?textureUnits:3);
    u[10]=color.ambient;u[93]=color.material;
    u[11]={float(fog.a),float(fog.b),float(fog.c),0};
    u[12]={float(fog.enabled),float(fog.rangeEnabled),float(primaryAlpha),0};
    if(fog.rangeEnabled){
        u[13]={float(-double(logicalWidth)/fog.viewportWidth),float((logicalWidth-2*fog.rangeCenter)/fog.viewportWidth),0,0};
        for(unsigned i=0;i<5;++i)u[14+i]={float(fog.rangeK[2*i]),float(fog.rangeK[2*i+1]),float(fog.rangeK[std::min(2*i+2,9u)]),0};
    }
    if(noNormals){
        if(unlit)u[10]={1,1,1,1};
        const unsigned n=19+4*p.lights++;
        u[n]={0,0,0,0};u[n+1]={0,0,0,0};u[n+2]={0,0,0,0};u[n+3]={1,0,0,0};
    }else for(unsigned i=0;i<8;++i)if(color.channels[0].mask&(1u<<i)){
        const auto& l=color.lights[i];const unsigned n=19+4*p.lights++;
        auto distance=l.distance;auto cosine=l.cosine;
        // All-zero distance coefficients: GX divides by zero and the light is
        // not attenuated (see evaluateRasterColor). With a constant positive
        // angle term that is exactly a factor of one.
        if(distance[0]==0&&distance[1]==0&&distance[2]==0&&cosine[0]>0&&cosine[1]==0&&cosine[2]==0){distance={1,0,0};cosine={1,0,0};}
        if(distance[0]<=0||distance[1]<0||distance[2]<0)return reject(22);
        for(unsigned c=0;c<3;++c){u[n][c]=l.position[c];u[n+2][c]=l.direction[c];u[n+3][c]=distance[c];}
        u[n][3]=cosine[0];u[n+2][3]=cosine[1];u[n+3][3]=cosine[2];
        u[n+1]=l.color;
    }
    if(p.paletteBase()>p.paletteLimit())return reject(23);
    u[14][3]=float(color.vertex&1);
    u[17][3]=unlit?0.f:float(color.ambientVertex&1);
    // u[10].w is the depth clamp switch in this program (the ambient alpha
    // is folded into the material by the decoders).
    u[10][3]=0;
    // Texture units: role 1 = texgen rows, 0.5 = fog ramp coordinate.
    for(unsigned unit=0;unit<3;++unit){
        const unsigned base=multiUnitRow(unit);
        if(unit==0&&projected){
            const float su=scales[0][0],sv=scales[0][1];
            if(!std::isfinite(su)||!std::isfinite(sv)||su<=0||sv<=0)return reject(24);
            for(unsigned row=0;row<3;++row)u[base+row]=projectedRows[row];
            u[base+3]={su,sv,0,0};multiRole(p,0)=2;p.multiProjected=true;
        }else if(unit<textureUnits){
            const auto& g=gens[unit];const float su=scales[unit][0],sv=scales[unit][1];
            if(!std::isfinite(su)||!std::isfinite(sv)||su<=0||sv<=0)return reject(24);
            const auto& m0=g.rows[0];const auto& m1=g.rows[1];
            if(g.source==0){
                u[base+0]={su*m0[0],su*m0[1],su*m0[2],su*m0[3]};u[base+1]={0,0,0,0};
                u[base+2]={-sv*m1[0],-sv*m1[1],-sv*m1[2],1-sv*m1[3]};u[base+3]={0,0,0,0};
            }else{
                u[base+0]={0,0,su*m0[2],su*m0[3]};u[base+1]={su*m0[0],su*m0[1],0,0};
                u[base+2]={0,0,-sv*m1[2],1-sv*m1[3]};u[base+3]={-sv*m1[0],-sv*m1[1],0,0};
            }
            p.unitSource[unit]=g.source;multiRole(p,unit)=1;
        }else if(int(unit)==fogUnit)multiRole(p,unit)=0.5f;
    }
    for(unsigned row=0;row<p.uniformCount();++row)
        for(float value:u[row])if(!std::isfinite(value)||std::abs(value)>1e8f)return reject(25);
    for(unsigned row=p.paletteLimit();row<94;++row)for(float value:u[row])if(!std::isfinite(value)||std::abs(value)>1e8f)return reject(26);
    reset.keep=true;return true;
}
inline bool packRigidVertex(const RigidVertexPlan& p,const DecodedVertex& v,
    const std::array<float,2>& scale,GpuVertex& out){
    constexpr unsigned needed=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_TEX0);
    if((v.present&needed)!=needed||(v.present&2)||v.normalCount<1||
       ((p.color.vertex|p.color.ambientVertex)&&!(v.present&(1u<<GX_VA_CLR0))))return false;
    for(float value:v.position)if(!std::isfinite(value)||std::abs(value)>1e7f)return false;
    for(float value:v.normal[0])if(!std::isfinite(value)||std::abs(value)>1e4f)return false;
    for(float value:v.texcoord[0])if(!std::isfinite(value)||std::abs(value)>1e8f)return false;
    auto color=p.color.material;
    for(unsigned c=0;c<4;++c)if((p.color.vertex|p.color.ambientVertex)&(c==3?2:1)){
        const float value=v.color[0][c];if(!std::isfinite(value)||value<0||value>1)return false;color[c]=value;
    }
    if(p.color.lit&2)color[3]*=std::clamp(p.color.ambient[3],0.f,1.f);
    out={};out.position={v.position[0],v.position[1],v.position[2],v.normal[0][2]};out.color=color;
    out.texcoord[0]={v.texcoord[0][0]*scale[0],1-v.texcoord[0][1]*scale[1]};
    out.texcoord[1]={v.normal[0][0],v.normal[0][1]};
    out.texcoord[2]={0,3};
    if(p.indexedMatrices){
        const unsigned index=v.matrix[0];
        if(!(v.present&1)||index>27||index%3||p.matrixOffsets[index/3]<0)return false;
        out.texcoord[2]={float(p.matrixOffsets[index/3]),float(p.normalOffsets[index/3])};
    }else if(v.present&1)return false;
    return true;
}
// Projected draws: texture scales of the projected unit, and the base texgen
// rows rewritten to act on the final base coordinate (scaled, with the
// vertical flip): x'=X.(x,y,1), y'=Y.(x,y,1).
inline void setProjectedScales(RigidVertexPlan& p,const std::array<float,2>& projected,const std::array<float,2>& base){
    const unsigned at=p.projectionBase();
    p.uniforms[at+3]={projected[0],projected[1],0,0};
    p.uniforms[at+4]={1,0,0,0};p.uniforms[at+5]={0,1,0,0};
    const auto& m=p.baseTexgen;const float su=base[0],sv=base[1];
    if(!(su>0)||!(sv>0)||!std::isfinite(su)||!std::isfinite(sv))return;
    p.uniforms[at+4]={m[0],-su*m[1]/sv,su*(m[1]/sv+m[2]),0};
    p.uniforms[at+5]={-sv*m[3]/su,m[4],1-m[4]-sv*m[5],0};
}
inline bool validateRigidProjection(const RigidVertexPlan& p,std::span<const GpuVertex> vertices){
    if(!p.projectedTexture&&!p.multiProjected)return true;
    // Both compact records start with the position and have one size.
    static_assert(sizeof(CompactRigidVertexMT)==sizeof(CompactRigidVertex)&&offsetof(CompactRigidVertexMT,position)==0&&offsetof(CompactRigidVertex,position)==0);
    const auto& matrix=p.multiProjected?p.uniforms[multiUnitRow(0)+2]:p.uniforms[p.projectionBase()+2];
    const auto bytes=p.staticVertices?std::span<const std::byte>(p.staticVertices,vertices.size()*sizeof(CompactRigidVertex)):std::as_bytes(vertices);
    for(unsigned i=0;i<vertices.size();++i){
        std::array<float,3> pos;
        if(p.compact){CompactRigidVertex v;std::memcpy(&v,bytes.data()+i*sizeof(v),sizeof(v));pos=v.position;}
        else for(unsigned c=0;c<3;++c)pos[c]=vertices[i].position[c];
        float q=0,magnitude=std::abs(matrix[3]);
        for(unsigned c=0;c<3;++c){const float term=matrix[c]*pos[c];q+=term;magnitude+=std::abs(term);}q+=matrix[3];
        // Ambiguous or camera-crossing projective divisors retain the complete
        // CPU transform/culling checks. Never feed PICA a nonpositive q.
        // Leave cancellation-sensitive divisors to the CPU too: float24
        // products and accumulation need headroom from the zero boundary.
        if(!std::isfinite(q)||!std::isfinite(magnitude)||q<=std::max(1e-5f,magnitude*1e-4f))return false;
    }
    return true;
}
}
