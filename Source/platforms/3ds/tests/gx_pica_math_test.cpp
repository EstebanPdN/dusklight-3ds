#include "../runtime/gx_pica_math.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdint>
#include <unordered_set>
#include <vector>
using namespace dusk::native::gx;
namespace {
bool compare(unsigned op, unsigned a, unsigned r) {
    switch (op) {
        case 0: return false; case 1: return a<r; case 2:return a==r;
        case 3:return a<=r; case 4:return a>r; case 5:return a!=r;
        case 6:return a>=r; default:return true;
    }
}
void point(const std::array<float,16>& m, float x,float y,float z,float ex,float ey,float ez) {
    const float values[]{x,y,z,1}; float result[4]{};
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) result[r]+=m[r*4+c]*values[c];
    assert(std::fabs(result[0]/result[3]-ex)<0.0001f);
    assert(std::fabs(result[1]/result[3]-ey)<0.0001f);
    assert(std::fabs(result[2]/result[3]-ez)<0.0001f);
}
void exhaustiveAlpha() {
    using Truth=std::array<std::uint64_t,4>;
    struct Hash{std::size_t operator()(const Truth& t)const{
        std::uint64_t hash=14695981039346656037ull;
        for(auto word:t){hash^=word;hash*=1099511628211ull;}
        return hash;
    }};
    // Independent oracle: explicitly evaluate all 256 alpha values for each
    // comparator/reference, then combine their full bit vectors. The set of
    // every single-comparison truth table proves both acceptance and refusal.
    static std::array<std::array<Truth,256>,8> truth{};
    std::unordered_set<Truth,Hash> representable;
    for(unsigned c=0;c<8;++c)for(unsigned ref=0;ref<256;++ref){
        for(unsigned v=0;v<256;++v)if(compare(c,v,ref))truth[c][ref][v/64]|=std::uint64_t(1)<<(v%64);
        representable.insert(truth[c][ref]);
    }
    unsigned accepted=0,refused=0,split=0;
    for(unsigned c0=0;c0<8;++c0)for(unsigned c1=0;c1<8;++c1)
    for(unsigned r0=0;r0<256;++r0)for(unsigned r1=0;r1<256;++r1)for(unsigned op=0;op<4;++op){
        Truth expected{};
        for(unsigned word=0;word<4;++word){const auto l=truth[c0][r0][word],r=truth[c1][r1][word];
            expected[word]=op==0?l&r:op==1?l|r:op==2?l^r:~(l^r);}
        AlphaPlan plan;const AlphaCompare a{std::uint8_t(c0),std::uint8_t(r0),std::uint8_t(op),std::uint8_t(c1),std::uint8_t(r1)};
        if(reduceAlpha(a,plan)){assert(plan.comparison<8&&truth[plan.comparison][plan.reference]==expected);++accepted;}
        else{assert(!representable.contains(expected));++refused;}
        std::array<AlphaPlan,2> pair;
        if(splitAlphaUnion(a,pair)){
            const auto& l=truth[pair[0].comparison][pair[0].reference];
            const auto& r=truth[pair[1].comparison][pair[1].reference];
            for(unsigned word=0;word<4;++word){assert(!(l[word]&r[word]));assert((l[word]|r[word])==expected[word]);}
            ++split;
        }
    }
    assert(accepted+refused==16777216);
    assert(split>0);
    std::printf("PASS %u disjoint alpha-union plans against independent full truth sets\n",split);
    std::printf("PASS all 16,777,216 alpha comparator/reference/boolean configurations against full 256-value truth sets: %u exact / %u correctly refused\n",accepted,refused);
}
}
int main() {
    // Captured Kakariko triangle 2 (vertices 6..8): mixed w, beyond one clip
    // side. Keep homogeneous coordinates; no division or q epsilon is valid.
    const std::array<std::array<float,4>,3> kakariko{{
        {45448.183594f,-60900.781250f,-3.253011f,55906.914062f},
        {-26200.509766f,-56679.859375f,-6.125771f,-36018.519531f},
        {16449.490234f,-78357.789062f,-5.021552f,-684.641602f}}};
    assert(picaClipSideMask(kakariko[0])&picaClipSideMask(kakariko[1])&picaClipSideMask(kakariko[2]));
    for(unsigned axis=0;axis<2;++axis)for(float sign:{-1.f,1.f}){
        for(float w:{-1.f,0.f,1.f}){
            std::array<float,4> p{0,0,-.5f,w};p[axis]=sign*4;
            assert(picaClipSideMask(p)&(unsigned(sign>0?1:2)<<(axis*2)));
        }
        std::array<float,4> edge{0,0,-.5f,1};edge[axis]=sign*1.01f;
        assert(!picaClipSideMask(edge));edge[axis]=sign;
        assert(!picaClipSideMask(edge));
    }
    for(unsigned component=0;component<4;++component)for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        auto p=kakariko[0];p[component]=bad;assert(!picaClipSideMask(p));
    }
    for(unsigned component=0;component<4;++component)
        for(float bad:{0x1p-63f,0x1p63f,std::numeric_limits<float>::denorm_min(),std::numeric_limits<float>::max()})
            for(float sign:{-1.f,1.f}){
                auto p=kakariko[0];p[component]=sign*bad;assert(!picaClipSideMask(p));
            }
    // Ordon spring is an interior interval, not the union of its two tails.
    const AlphaCompare spring{6,185,3,3,100};AlphaPlan springPlan;std::array<AlphaPlan,2> springPair;
    assert(!reduceAlpha(spring,springPlan)&&!splitAlphaUnion(spring,springPair));
    unsigned passing=0;
    for(unsigned alpha=0;alpha<256;++alpha){const bool actual=compare(6,alpha,185)==compare(3,alpha,100);
        assert(actual==(alpha>=101&&alpha<=184));passing+=actual;}
    assert(passing==84);
    std::puts("PASS Kakariko homogeneous clip-side evidence, zero/mixed w, conservative boundary/nonfinite refusal; exact 256-byte Ordon XNOR truth table remains unsupported");
    // Independent polygon-clipping oracle: an accepted outcode intersection
    // must leave no polygon after four homogeneous Sutherland-Hodgman clips.
    std::uint32_t random=0x44c11f3u;unsigned proved=0;
    auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;
        return float(int(random%20001)-10000)/100;};
    for(unsigned trial=0;trial<16384;++trial){
        std::vector<std::array<double,4>> polygon;unsigned mask=15;
        for(unsigned n=0;n<3;++n){std::array<float,4> p{next(),next(),next(),next()};
            mask&=picaClipSideMask(p);polygon.push_back({p[0],p[1],p[2],p[3]});}
        if(!mask)continue;
        for(unsigned plane=0;plane<4&&!polygon.empty();++plane){
            const auto distance=[&](const auto& p){return p[3]+(plane&1?-p[plane/2]:p[plane/2]);};
            std::vector<std::array<double,4>> clipped;
            auto a=polygon.back();double da=distance(a);
            for(const auto& b:polygon){const double db=distance(b);
                if((da>=0)!=(db>=0)){std::array<double,4> intersection{};const double t=da/(da-db);
                    for(unsigned c=0;c<4;++c)intersection[c]=a[c]+t*(b[c]-a[c]);
                    clipped.push_back(intersection);}
                if(db>=0)clipped.push_back(b);
                a=b;da=db;
            }
            polygon=std::move(clipped);
        }
        assert(polygon.empty());++proved;
    }
    assert(proved>1000);std::printf("PASS %u exact lateral rejections against independent polygon-clipping oracle (16,384 random triangles)\n",proved);
    // Independent float24 precision check over many exponents: a proved bit
    // must survive both mantissa truncation (libctru) and nearest rounding.
    unsigned quantizedProofs=0;
    for(unsigned trial=0;trial<16384;++trial){
        std::array<float,4> p;
        for(auto& value:p)value=std::ldexp(next(),int(random%125)-62);
        const auto mask=picaClipSideMask(p);if(!mask)continue;
        for(bool nearest:{false,true}){
            std::array<double,4> q;
            for(unsigned c=0;c<4;++c){int exponent;const double fraction=std::frexp(double(p[c]),&exponent);
                const auto mantissa=nearest?std::round(fraction*131072):std::trunc(fraction*131072);
                q[c]=std::ldexp(mantissa,exponent-17);}
            for(unsigned axis=0;axis<2;++axis){
                if(mask&(1u<<(axis*2)))assert(q[axis]>q[3]);
                if(mask&(2u<<(axis*2)))assert(-q[axis]>q[3]);
            }
        }
        ++quantizedProofs;
    }
    assert(quantizedProofs>1000);std::printf("PASS %u clip outcodes survive float24 truncation/nearest rounding across exponent range\n",quantizedProofs);
    // Original front-face convention is CW. The rotated PICA clip transform
    // preserves orientation: check both triangle windings against keep-CW/CCW.
    {
        const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        std::array<float,16> rotation;
        assert(picaProjection(identity,{0,0,400,240,0,1},rotation));
        for(bool clockwise:{false,true}) {
            const std::array<std::array<float,2>,3> tri{{{-.5f,-.5f},{-.5f,.5f},{.5f,-.5f}}};
            std::array<std::array<float,2>,3> transformed{};
            for(unsigned i=0;i<3;++i) {
                const auto& point=tri[clockwise?i:(i==1?2:i==2?1:0)];
                for(unsigned axis=0;axis<2;++axis)transformed[i][axis]=rotation[axis*4]*point[0]+rotation[axis*4+1]*point[1]+rotation[axis*4+3];
            }
            const float area=(transformed[1][0]-transformed[0][0])*(transformed[2][1]-transformed[0][1])-(transformed[1][1]-transformed[0][1])*(transformed[2][0]-transformed[0][0]);
            assert((area<0)==clockwise);
            for(unsigned cull=0;cull<3;++cull) {
                const bool expected=cull==0||(cull==1?!clockwise:clockwise);
                const unsigned mode=picaCull(cull);
                const bool actual=mode==0||(mode==1?area<0:area>0);
                assert(actual==expected);
            }
        }
    }
    DrawPacket p{};
    p.modelView[0]=p.modelView[5]=p.modelView[10]=1;
    // Logical coordinates (0,0)..(400,240), depth z=-1 near / +1 far.
    p.projection[0]=2.0f/400; p.projection[3]=-1;
    p.projection[5]=-2.0f/240; p.projection[7]=1;
    p.projection[10]=0.5f; p.projection[11]=-0.5f; p.projection[15]=1;
    p.viewport={0,0,400,240,0,1};
    std::array<float,16> m;
    assert(picaTransform(p,m));
    // Projection conversion permits in-place use by the indexed draw planner.
    auto alias=std::to_array(p.projection);
    assert(picaProjection(alias,p.viewport,alias)&&alias==m);
    point(m,0,0,0,1,1,-0.5f); point(m,400,0,0,1,-1,-0.5f);
    point(m,0,240,0,-1,1,-0.5f); point(m,400,240,0,-1,-1,-0.5f);
    point(m,200,120,-1,0,0,-1); point(m,200,120,1,0,0,0);
    p.viewport={100,60,200,120,0.25f,0.75f};
    assert(picaTransform(p,m));
    alias=std::to_array(p.projection);
    assert(picaProjection(alias,p.viewport,alias)&&alias==m);
    point(m,0,0,-1,0.5f,0.5f,-0.75f); point(m,400,240,1,-0.5f,-0.5f,-0.25f);
    p.modelView[3]=400; assert(picaTransform(p,m)); point(m,0,0,1,0.5f,-0.5f,-0.25f);
    p.projection[0]=std::numeric_limits<float>::quiet_NaN(); assert(!picaTransform(p,m));
    auto s=picaScissor({16,24,40,80});
    assert(!s.empty && s.left==136 && s.top==344 && s.right==216 && s.bottom==384);
    s=picaScissor({390,230,0xffffffff,0xffffffff});
    assert(!s.empty && s.left==0 && s.top==0 && s.right==10 && s.bottom==10);
    assert(picaScissor({400,0,100,100}).empty); assert(picaScissor({0,0,0,0}).empty);
    // Exhaustive alpha values for all comparator/boolean forms at representative
    // boundary references. An unsupported two-sided interval must be refused.
    const unsigned refs[]{0,1,127,128,254,255}; unsigned accepted=0, rejected=0;
    for(unsigned c0=0;c0<8;++c0) for(unsigned c1=0;c1<8;++c1)
    for(unsigned r0:refs) for(unsigned r1:refs) for(unsigned op=0;op<4;++op) {
        AlphaCompare a{std::uint8_t(c0),std::uint8_t(r0),std::uint8_t(op),std::uint8_t(c1),std::uint8_t(r1)};
        AlphaPlan plan;
        if(!reduceAlpha(a,plan)) { ++rejected; continue; }
        ++accepted;
        for(unsigned v=0;v<256;++v) {
            bool l=compare(c0,v,r0),r=compare(c1,v,r1);
            bool expected=op==0?l&&r:op==1?l||r:op==2?l!=r:l==r;
            assert(compare(plan.comparison,v,plan.reference)==expected);
        }
    }
    AlphaPlan plan; assert(!reduceAlpha({4,50,0,1,200},plan));
    assert(reduceAlpha({4,0,1,4,0},plan));
    assert(accepted>0 && rejected>0);
    std::printf("PASS GX/PICA transform corners, viewport/depth, scissor overflow; alpha %u exact / %u refused\n",accepted,rejected);
    exhaustiveAlpha();
}
