#include "gx_pica_math.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace dusk::native::gx {
unsigned picaClipSideMask(const std::array<float,4>& p) {
    // PICA float24 has a 7-bit exponent (bias 63) and 16 fraction bits.
    // Refuse underflow/overflow regions, including a spare upper exponent;
    // an identity shader must preserve finite clip coordinates for this proof.
    for(float value:p)if(!std::isfinite(value)||
        (value!=0&&(std::abs(value)<0x1p-62f||std::abs(value)>=0x1p63f)))return 0;
    unsigned mask=0;
    for(unsigned axis=0;axis<2;++axis){
        // Do not divide by w: each clip half-space is linear even across the
        // camera plane. A convex combination of points strictly outside one
        // plane stays outside. Double arithmetic avoids float overflow, and
        // the 1/16 relative margin is conservative for PICA float24 rounding.
        const double value=p[axis],w=p[3];
        const double margin=std::max(std::abs(value),std::abs(w))/16;
        if(value-w>margin)mask|=1u<<(axis*2);
        if(-value-w>margin)mask|=2u<<(axis*2);
    }
    return mask;
}
bool picaTriangleOutsideSides(const std::array<float,4>& a,const std::array<float,4>& b,
                              const std::array<float,4>& c) {
    const std::array<float,4>* points[]{&a,&b,&c};
    double scale=0;
    for(const auto* point:points)for(float value:*point){
        if(!std::isfinite(value)||(value!=0&&(std::abs(value)<0x1p-62f||std::abs(value)>=0x1p63f)))return false;
        scale=std::max(scale,std::abs(double(value)));
    }
    if(scale==0)return false;
    using Distances=std::array<double,4>;
    std::array<Distances,12> polygon{},next{};
    unsigned count=3;
    for(unsigned vertex=0;vertex<3;++vertex)for(unsigned axis=0;axis<2;++axis){
        const double value=(*points[vertex])[axis],w=(*points[vertex])[3];
        const double margin=std::max(std::abs(value),std::abs(w))/16;
        polygon[vertex][axis*2]=(w-value+margin)/scale;
        polygon[vertex][axis*2+1]=(w+value+margin)/scale;
    }
    // Each distance is affine in barycentric coordinates. Interpolating the
    // positive per-vertex margins bounds coordinate rounding everywhere in
    // the original triangle (triangle inequality); it enlarges the accepted
    // volume. Empty intersection therefore proves original non-coverage.
    constexpr double uncertain=1e-12;
    for(unsigned plane=0;plane<4;++plane){
        unsigned written=0;
        for(unsigned i=0;i<count;++i){
            const auto& from=polygon[i];const auto& to=polygon[(i+1)%count];
            const double df=from[plane],dt=to[plane];
            if(std::abs(df)<uncertain||std::abs(dt)<uncertain)return false;
            const bool insideFrom=df>0,insideTo=dt>0;
            if(insideFrom){if(written==next.size())return false;next[written++]=from;}
            if(insideFrom!=insideTo){
                const double t=df/(df-dt);
                if(!std::isfinite(t)||t<0||t>1||written==next.size())return false;
                auto& intersection=next[written++];
                for(unsigned p=0;p<4;++p)intersection[p]=from[p]+t*(to[p]-from[p]);
            }
        }
        if(!written)return true;
        polygon=next;count=written;
    }
    return false;
}
bool picaTransform(const DrawPacket& p, std::array<float, 16>& out, const TargetExtent& target) {
    for (float f : p.projection) if (!std::isfinite(f)) return false;
    for (float f : p.modelView) if (!std::isfinite(f)) return false;
    std::array<float,16> clip{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
        for (unsigned k = 0; k < 3; ++k)
            clip[r * 4 + c] += p.projection[r * 4 + k] * p.modelView[k * 4 + c];
        if (c == 3) clip[r * 4 + c] += p.projection[r * 4 + 3];
    }
    return picaProjection(clip,p.viewport,out,target);
}
bool picaProjection(const std::array<float,16>& clip,const Viewport& v,std::array<float,16>& out,const TargetExtent& target){
    if(!target.valid())return false;
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.width) ||
        !std::isfinite(v.height) || v.width <= 0 || v.height <= 0 ||
        !std::isfinite(v.nearZ) || !std::isfinite(v.farZ) ||
        v.nearZ < 0 || v.nearZ > 1 || v.farZ < 0 || v.farZ > 1) return false;
    for(float f:clip)if(!std::isfinite(f))return false;
    // GX viewport is in upright top-left screen coordinates. PICA's target
    // is 240x400 and rotated: logical top-left maps to clip (+w,+w).
    const float xScale = v.width / target.logicalWidth, yScale = v.height / target.logicalHeight;
    const float xOffset = 1 - (2 * v.x + v.width) / target.logicalWidth;
    const float yOffset = 1 - (2 * v.y + v.height) / target.logicalHeight;
    for (unsigned c = 0; c < 4; ++c) {
        const float x=clip[c],y=clip[4+c],z=clip[8+c],w=clip[12+c];
        out[c] = yScale * y + yOffset * w;
        out[4 + c] = -xScale * x + xOffset * w;
        out[8 + c] = (v.farZ - v.nearZ) * z + (v.farZ - 1) * w;
        out[12 + c] = w;
    }
    for (float f : out) if (!std::isfinite(f)) return false;
    return true;
}
PicaScissor picaScissor(const Scissor& s,const TargetExtent& target) {
    if(!target.valid())return {0,0,0,0,true};
    // Saturate in wide arithmetic before subtracting to avoid unsigned wrap.
    const auto x0 = std::min<std::uint64_t>(s.x, target.logicalWidth);
    const auto y0 = std::min<std::uint64_t>(s.y, target.logicalHeight);
    const auto x1 = std::min<std::uint64_t>(std::uint64_t(s.x) + s.width, target.logicalWidth);
    const auto y1 = std::min<std::uint64_t>(std::uint64_t(s.y) + s.height, target.logicalHeight);
    const unsigned px0=x0*target.pixelWidth/target.logicalWidth;
    const unsigned py0=y0*target.pixelHeight/target.logicalHeight;
    const unsigned px1=(x1*target.pixelWidth+target.logicalWidth-1)/target.logicalWidth;
    const unsigned py1=(y1*target.pixelHeight+target.logicalHeight-1)/target.logicalHeight;
    return {target.pixelHeight-py1,target.pixelWidth-px1,target.pixelHeight-py0,target.pixelWidth-px0,x1<=x0||y1<=y0};
}
namespace {
bool compare(unsigned operation, unsigned value, unsigned reference) {
    switch (operation) {
        case 0: return false; case 1: return value < reference;
        case 2: return value == reference; case 3: return value <= reference;
        case 4: return value > reference; case 5: return value != reference;
        case 6: return value >= reference; case 7: return true;
    }
    return false;
}
}
bool reduceAlpha(const AlphaCompare& a, AlphaPlan& out) {
    if (a.compare0 > 7 || a.compare1 > 7 || a.op > 3) return false;
    unsigned count = 0, first = 256, last = 0, missing = 0;
    // A byte comparison only changes at its reference and reference+1. The
    // combined truth table is constant between these sorted boundaries, so
    // inspect at most five intervals instead of every alpha byte per draw.
    const unsigned low=std::min(a.reference0,a.reference1),high=std::max(a.reference0,a.reference1);
    const unsigned boundaries[]{0,low,std::min(low+1,high),high,high+1,256};
    for (unsigned interval = 0; interval < 5; ++interval) {
        const unsigned value=boundaries[interval],end=boundaries[interval+1];
        if(value==end)continue;
        bool l = compare(a.compare0, value, a.reference0), r = compare(a.compare1, value, a.reference1);
        bool pass = a.op == 0 ? l && r : a.op == 1 ? l || r : a.op == 2 ? l != r : l == r;
        if (pass) { count+=end-value; first = std::min(first, value); last = end-1; }
        else missing = end-1;
    }
    if (!count) out = {0, 0};
    else if (count == 256) out = {7, 0};
    else if (count == 1) out = {2, std::uint8_t(first)};
    else if (count == 255) out = {5, std::uint8_t(missing)};
    else if (first == 0 && last + 1 == count) out = {3, std::uint8_t(last)};
    else if (last == 255 && 256 - first == count) out = {6, std::uint8_t(first)};
    else return false;
    return true;
}
bool splitAlphaUnion(const AlphaCompare& a,std::array<AlphaPlan,2>& out){
    out={};
    if(a.op!=1)return false;
    auto low=[](unsigned c,unsigned r){return c==3?int(r):c==1&&r?int(r)-1:-1;};
    auto high=[](unsigned c,unsigned r){return c==6?int(r):c==4&&r<255?int(r)+1:256;};
    int left=low(a.compare0,a.reference0),right=high(a.compare1,a.reference1);
    if(left<0||right>255){left=low(a.compare1,a.reference1);right=high(a.compare0,a.reference0);}
    if(left<0||right>255||left+1>=right)return false;
    out={AlphaPlan{3,std::uint8_t(left)},AlphaPlan{6,std::uint8_t(right)}};
    return true;
}
}
