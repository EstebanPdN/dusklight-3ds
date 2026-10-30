#pragma once
#include "../native/color_lookup.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

inline void colorLookupInterpolationCases(){
    constexpr std::array images{dusk::pica::colorLookupImage(0),dusk::pica::colorLookupImage(1)};
    auto texel=[&](unsigned plane,int x,int y,unsigned component){
        x=std::clamp(x,0,7);y=7-std::clamp(y,0,7);
        constexpr unsigned spread[]{0,1,4,5,16,17,20,21};
        return double(images[plane][4*(spread[x]+2*spread[y])+3-component])/255.;
    };
    auto sample=[&](unsigned plane,double u,double v,unsigned component){
        const double x=u*8-.5,y=v*8-.5;const int ix=int(std::floor(x)),iy=int(std::floor(y));
        const double fx=x-ix,fy=y-iy;
        return (1-fy)*((1-fx)*texel(plane,ix,iy,component)+fx*texel(plane,ix+1,iy,component))+
               fy*((1-fx)*texel(plane,ix,iy+1,component)+fx*texel(plane,ix+1,iy+1,component));
    };
    constexpr double colors[3][4]{{0,1,.25,.75},{1,.125,.875,0},{.375,.625,0,1}};
    constexpr double inverseW[]{1,.25,2};
    for(unsigned a=0;a<=32;++a)for(unsigned b=0;b<=32-a;++b){
        double weights[]{double(a)/32,double(b)/32,double(32-a-b)/32},sum=0;
        for(unsigned n=0;n<3;++n){weights[n]*=inverseW[n];sum+=weights[n];}
        for(auto& weight:weights)weight/=sum;
        double expected[4]{};double uv[2][2]{};
        for(unsigned n=0;n<3;++n){
            for(unsigned c=0;c<4;++c)expected[c]+=weights[n]*colors[n][c];
            for(unsigned plane=0;plane<2;++plane){const auto coordinate=dusk::pica::colorLookupUv(float(colors[n][plane*2]),float(colors[n][plane*2+1]));
                for(unsigned c=0;c<2;++c)uv[plane][c]+=weights[n]*coordinate[c];}
        }
        for(unsigned c=0;c<4;++c){double actual=0;for(unsigned plane=0;plane<2;++plane)actual+=sample(plane,uv[plane][0],uv[plane][1],c);assert(std::abs(actual-expected[c])<1e-6);}
    }
    // Exact texel-center endpoints and all 256 source color bytes. This also
    // catches reversed V, channel order and accidental nearest filtering.
    for(unsigned byte=0;byte<256;++byte){const double value=double(byte)/255;
        const auto coordinate=dusk::pica::colorLookupUv(float(value),float(1-value));
        assert(std::abs(sample(0,coordinate[0],coordinate[1],0)-value)<1e-6);
        assert(std::abs(sample(0,coordinate[0],coordinate[1],1)-(1-value))<1e-6);
        assert(std::abs(sample(1,coordinate[0],coordinate[1],2)-value)<1e-6);
        assert(std::abs(sample(1,coordinate[0],coordinate[1],3)-(1-value))<1e-6);
    }
}
