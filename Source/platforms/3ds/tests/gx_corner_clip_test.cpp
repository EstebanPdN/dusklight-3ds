#include "../runtime/gx_pica_math.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
using namespace dusk::native::gx;
using Point=std::array<float,4>;

// Independent exact integer feasibility oracle: enumerate pair intersections
// of seven half-planes in the triangle's two barycentric variables. No polygon
// clipping/interpolation, floating rounding, or production margins are used.
bool reachesVolume(const std::array<Point,3>& points){
    using I=__int128_t;
    struct Line{I a,b,c;};
    std::array<Line,7> lines{{{1,0,0},{0,1,0},{-1,-1,1}}};
    for(unsigned plane=0;plane<4;++plane){
        I d[3];
        for(unsigned i=0;i<3;++i)d[i]=int(points[i][3])+(plane%2?1:-1)*int(points[i][plane/2]);
        lines[3+plane]={d[1]-d[0],d[2]-d[0],d[0]};
    }
    for(unsigned i=0;i<7;++i)for(unsigned j=i+1;j<7;++j){
        const auto& l=lines[i];const auto& r=lines[j];
        I det=l.a*r.b-r.a*l.b;if(!det)continue;
        I u=l.b*r.c-r.b*l.c,v=l.c*r.a-r.c*l.a;
        if(det<0){det=-det;u=-u;v=-v;}
        bool inside=true;
        for(const auto& p:lines)if(p.a*u+p.b*v+p.c*det<0){inside=false;break;}
        if(inside)return true;
    }
    return false;
}
int main(){
    const std::array<Point,3> live{{{39734.8164f,62017.1328f,-3.6014514f,44757.1562f},
        {79972.2422f,62017.1094f,-3.82726288f,37531.4219f},
        {116692.453f,15338.085f,-2.84875178f,68842.7969f}}};
    assert(!(picaClipSideMask(live[0])&picaClipSideMask(live[1])&picaClipSideMask(live[2])));
    assert(picaTriangleOutsideSides(live[0],live[1],live[2]));
    unsigned checks=0,proved=0,visible=0;
    for(int exponent=-30;exponent<=30;++exponent)for(unsigned mirror=0;mirror<4;++mirror){
        auto p=live;
        for(auto& vertex:p){for(auto& value:vertex)value=std::ldexp(value,exponent);
            if(mirror&1)vertex[0]=-vertex[0];if(mirror&2)vertex[1]=-vertex[1];}
        assert(picaTriangleOutsideSides(p[0],p[1],p[2]));++checks;
        p[0][0]=p[0][1]=0;
        assert(!picaTriangleOutsideSides(p[0],p[1],p[2]));++checks;
    }
    std::uint32_t seed=0x327a44u;
    auto value=[&](){seed=seed*1664525u+1013904223u;return int(seed%2049)-1024;};
    for(unsigned n=0;n<20000;++n){
        std::array<Point,3> p;
        for(auto& vertex:p)vertex={float(value()),float(value()),-1,float(value())};
        const bool reachable=reachesVolume(p);
        const bool rejected=picaTriangleOutsideSides(p[0],p[1],p[2]);
        if(rejected){assert(!reachable);++proved;}
        if(reachable){assert(!rejected);++visible;}
        ++checks;
    }
    assert(proved>1000&&visible>1000);
    for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),0x1p-63f,0x1p63f}){
        auto p=live;p[0][2]=bad;
        assert(!picaTriangleOutsideSides(p[0],p[1],p[2]));++checks;
    }
    const Point zero{};assert(!picaTriangleOutsideSides(zero,zero,zero));++checks;
    std::printf("PASS %u corner checks; %u exact-oracle outside proofs, %u reachable triangles retained\n",checks,proved,visible);
}
