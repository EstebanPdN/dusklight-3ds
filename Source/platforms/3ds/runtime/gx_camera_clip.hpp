// Private root113 derivative of sealed109: normal-only interval endpoints.
// Original implementation; no emulator-derived code. Keep runtime OFF pending evidence.
// Dusklight contributions: CC0. Requires immutable, exclusively pinned inputs.
#pragma once
#include "gx_gpu_geometry.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#if defined(__FAST_MATH__)
#error camera clipping requires strict floating-point semantics
#endif

namespace dusk::native::gx::camera_clip109 {
static_assert(sizeof(GpuVertex)==60 && std::is_trivially_copyable_v<GpuVertex>);
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits==24);
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits==53);
inline constexpr std::size_t maxVertices=65535, maxIndices=196605;
inline constexpr std::size_t noDetail=std::numeric_limits<std::size_t>::max();
inline constexpr double minTransport=0x1p-48, maxTransport=0x1p20;
inline constexpr double minW=0x1p-10, maxRatio=0x1p12;
enum class Error { none, environment, inputCounts, originalAttribute, originalQ,
    inputDomain, index, alias, ambiguous, degenerate, polygonOverflow,
    outputDomain, budget, capacity, changed };
struct Counts {
    std::size_t vertices{},indices{},rawBytes{},arenaBytes{},emptyTriangles{},duplicateTriangles{};
    bool operator==(const Counts&) const = default;
};
struct Budget {
    std::size_t vertices=maxVertices,indices=maxIndices,geometryBytes=512*1024;
    bool operator==(const Budget&) const = default;
};
struct Input { std::span<const GpuVertex> vertices; std::span<const std::uint16_t> indices; };
struct Plan {
    Error error=Error::changed;
    Counts counts{};
    Budget budget{};
    std::size_t failedTriangle=noDetail,failedVertex=noDetail,failedElement=noDetail;
    const GpuVertex* source{};
    const std::uint16_t* elements{};
    std::size_t sourceCount{},elementCount{};
    std::array<std::uint32_t,8> digest{}; // SHA256 of ALL input bytes, not a concurrency lock.
};
struct Result { Error error=Error::changed; Counts counts{}; };
namespace detail {
struct Interval { double lo{},hi{}; };
struct Vertex {
    std::array<double,15> a{};
    std::array<Interval,4> p{};
    unsigned boundary{};
    std::uint16_t original=65535;
};
// Integer next-normal bounds: never create a denormal interval endpoint.
// A flushed result lies between -min_normal and +min_normal. Clamp outward
// across that entire gap, in both modes; never use an epsilon clipping plane.
inline double outward(double x,bool upper){
    constexpr std::uint64_t sign=0x8000000000000000ull,normal=0x0010000000000000ull;
    constexpr std::uint64_t inf=0x7ff0000000000000ull;
    const auto bits=std::bit_cast<std::uint64_t>(x),magnitude=bits&~sign;
    if(magnitude>inf)return std::bit_cast<double>((upper?0:sign)|inf); // invalid arithmetic widens
    if(magnitude<=normal)return std::bit_cast<double>((upper?0:sign)|normal);
    if(magnitude==inf){
        if(bool(bits&sign)==upper)return std::bit_cast<double>(bits-1);
        return x;
    }
    return std::bit_cast<double>(bits+((bool(bits&sign)==upper)?-std::uint64_t(1):std::uint64_t(1)));
}
inline double down(double x){return outward(x,false);}
inline double up(double x){return outward(x,true);}
// Bit-based domain checks happen BEFORE floating conversion or comparisons:
// an input-denormal flush must never turn an unsupported nonzero into zero.
inline bool scalar32(float x){
    const auto magnitude=std::bit_cast<std::uint32_t>(x)&0x7fffffffu;
    return magnitude==0||(magnitude>=0x27800000u&&magnitude<=0x49800000u);
}
inline bool scalar64(double x){
    const auto magnitude=std::bit_cast<std::uint64_t>(x)&0x7fffffffffffffffull;
    return magnitude==0||(magnitude>=0x3cf0000000000000ull&&magnitude<=0x4130000000000000ull);
}
inline Interval add(Interval a,Interval b){return {down(a.lo+b.lo),up(a.hi+b.hi)};}
inline Interval neg(Interval a){return {-a.hi,-a.lo};}
inline Interval sub(Interval a,Interval b){return add(a,neg(b));}
inline Interval mul(Interval a,Interval b){
    const double x[]{a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi};
    return {down(*std::min_element(x,x+4)),up(*std::max_element(x,x+4))};
}
inline Interval div(Interval a,Interval b){
    // Bound reciprocal operands before any division. The interval may widen
    // to uncertain on a numerical limit; never execute an overflowing quotient.
    auto safe=[](double x){const auto m=std::bit_cast<std::uint64_t>(x)&0x7fffffffffffffffull;
        return m>=0x2ff0000000000000ull&&m<=0x4ff0000000000000ull;};
    if(!safe(b.lo)||!safe(b.hi)||!(b.lo>0||b.hi<0))
        return {-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()};
    return mul(a,{down(1/b.hi),up(1/b.lo)});
}
inline std::array<float,15> fields(const GpuVertex& v){
    return {v.position[0],v.position[1],v.position[2],v.position[3],
        v.color[0],v.color[1],v.color[2],v.color[3],v.texcoord[0][0],v.texcoord[0][1],
        v.texcoord[1][0],v.texcoord[1][1],v.texcoord[2][0],v.texcoord[2][1],v.textureQ};
}
inline double affine(double outside,double inside,double t){
    // Canonical endpoint order plus separate operations: no orientation- or
    // implicit-FMA-dependent rounding at a shared crossing.
    const volatile double delta=inside-outside;
    const volatile double product=t*delta;
    return outside+product;
}
inline GpuVertex unpack(const Vertex& v){
    GpuVertex r;
    // Refuse unsupported central values before narrowing can flush them to
    // binary32 zero. NaN sentinel is rejected by the unchanged output gates.
    for(double x:v.a)if(!scalar64(x)){
        r.textureQ=std::bit_cast<float>(std::uint32_t(0x7fc00000));return r;
    }
    for(unsigned i=0;i<4;++i){r.position[i]=float(v.a[i]);r.color[i]=float(v.a[4+i]);}
    for(unsigned i=0;i<6;++i)r.texcoord[i/2][i%2]=float(v.a[8+i]);
    r.textureQ=float(v.a[14]);return r;
}
inline double distance(const Vertex& v,unsigned plane){
    switch(plane){case 0:return v.a[3]+v.a[0];case 1:return v.a[3]-v.a[0];
    case 2:return v.a[3]+v.a[1];case 3:return v.a[3]-v.a[1];
    case 4:return -v.a[2];default:return v.a[3]+v.a[2];}
}
inline Interval distanceRange(const Vertex& v,unsigned plane){
    if(v.boundary&(1u<<plane))return {0,0};
    switch(plane){case 0:return add(v.p[3],v.p[0]);case 1:return sub(v.p[3],v.p[0]);
    case 2:return add(v.p[3],v.p[1]);case 3:return sub(v.p[3],v.p[1]);
    case 4:return neg(v.p[2]);default:return add(v.p[3],v.p[2]);}
}
inline int sign(Interval x){return x.lo>0?1:x.hi<0?-1:x.lo==0&&x.hi==0?0:2;}
inline Vertex load(const GpuVertex& v,std::uint16_t index){
    Vertex r;const auto f=fields(v);
    for(unsigned i=0;i<15;++i)r.a[i]=f[i];
    for(unsigned i=0;i<4;++i)r.p[i]={f[i],f[i]};
    // A zero sum/difference of binary32 endpoints certifies exact equality.
    for(unsigned p=0;p<6;++p)if(distance(r,p)==0)r.boundary|=1u<<p;
    r.original=index;return r;
}
inline Interval det(const Vertex& a,const Vertex& b,const Vertex& c){
    const auto minor=[&](unsigned x,unsigned y){return sub(mul(b.p[x],c.p[y]),mul(b.p[y],c.p[x]));};
    return add(sub(mul(a.p[0],minor(1,3)),mul(a.p[1],minor(0,3))),mul(a.p[3],minor(0,1)));
}
inline bool transport(float x){return scalar32(x);}
inline Error finalVertex(const Vertex& v,const GpuVertex& f){
    if(v.p[3].lo<minW||v.p[3].hi>maxTransport||f.position[3]<minW||
       std::bit_cast<std::uint32_t>(f.textureQ)!=std::bit_cast<std::uint32_t>(f.position[3]))return Error::outputDomain;
    for(float x:fields(f))if(!transport(x))return Error::outputDomain;
    for(float x:f.color)if(x<0||x>1)return Error::outputDomain;
    for(unsigned p=0;p<6;++p){
        if(sign(distanceRange(v,p))==2||sign(distanceRange(v,p))<0)return Error::ambiguous;
        // Final binary32 coordinates themselves must be in the canonical volume.
        if(distance(load(f,0),p)<0)return Error::outputDomain;
    }
    for(float x:f.texcoord[0])if(std::abs(double(x)/f.textureQ)>maxRatio)return Error::outputDomain;
    return Error::none;
}
// Authored SHA256 implementation, used only as an immutable-input identity check.
struct Hash {
    std::array<std::uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<std::uint8_t,64> block{};
    std::uint64_t bytes{};unsigned used{};
    void compress(){
        constexpr std::uint32_t k[]{0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::array<std::uint32_t,64> w{};
        for(unsigned i=0;i<16;++i)for(unsigned j=0;j<4;++j)w[i]=(w[i]<<8)|block[4*i+j];
        for(unsigned i=16;i<64;++i){const auto a=w[i-15],b=w[i-2];
            w[i]=w[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+w[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));}
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(unsigned i=0;i<64;++i){const auto t=z+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            const auto u=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
            z=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    void put(std::uint8_t b){block[used++]=b;++bytes;if(used==64){compress();used=0;}}
    void append(std::span<const std::byte> s){for(auto b:s)put(std::to_integer<std::uint8_t>(b));}
    std::array<std::uint32_t,8> finish(){const auto bits=bytes*8;put(0x80);while(used!=56)put(0);
        for(unsigned i=0;i<8;++i)put(std::uint8_t(bits>>(56-8*i)));
        return h;}
};
inline bool overlap(const void* a,std::size_t na,const void* b,std::size_t nb){
    if(!na||!nb)return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    if(x>std::numeric_limits<std::uintptr_t>::max()-na||y>std::numeric_limits<std::uintptr_t>::max()-nb)return true;
    return x<y+nb&&y<x+na;
}
inline Error inputShape(Input in){
    if(in.vertices.size()<3||in.vertices.size()>maxVertices||in.indices.empty()||
        in.indices.size()%3||in.indices.size()>maxIndices||!in.vertices.data()||!in.indices.data())return Error::inputCounts;
    if(overlap(in.vertices.data(),in.vertices.size_bytes(),in.indices.data(),in.indices.size_bytes()))return Error::alias;
    return Error::none;
}
} // detail
// Caller-owned, constant size; never instantiate a mesh-sized temporary.
struct Scratch { std::array<detail::Vertex,9> a{},b{}; bool duplicate{}; };
static_assert(sizeof(Scratch)<=4096);
namespace detail {
inline Error scratchCheck(Input in,Scratch& s){
    if(auto e=inputShape(in);e!=Error::none)return e;
    if(overlap(&s,sizeof s,in.vertices.data(),in.vertices.size_bytes())||
       overlap(&s,sizeof s,in.indices.data(),in.indices.size_bytes()))return Error::alias;
    return Error::none;
}
inline Error originals(Input in,Plan& p){
    for(std::size_t originalIndex=0;originalIndex<in.vertices.size();++originalIndex){
        p.failedVertex=originalIndex;const auto& v=in.vertices[originalIndex];
        const auto f=fields(v);
        for(float x:f)if(!std::isfinite(x))return Error::originalAttribute;
        for(float x:v.color)if(x<0||x>1)return Error::originalAttribute;
        if(v.textureQ==0||std::bit_cast<std::uint32_t>(v.textureQ)!=std::bit_cast<std::uint32_t>(v.position[3]))return Error::originalQ;
        for(float x:f)if(!transport(x))return Error::inputDomain;
    }
    p.failedVertex=noDetail;
    for(std::size_t i=0;i<in.indices.size();++i)if(in.indices[i]>=in.vertices.size()){
        p.failedElement=i;p.failedTriangle=i/3;return Error::index;}
    return Error::none;
}
inline Error triangle(Input in,std::size_t start,Scratch& s,unsigned& count){
    count=3;s.duplicate=false;
    // Only identical position WORDS prove a transport-stable zero-area connector.
    // Complete original attributes/Q/indices have already passed in scan().
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){
        bool identical=true;
        for(unsigned k=0;k<4;++k)identical&=std::bit_cast<std::uint32_t>(in.vertices[in.indices[start+a]].position[k])==
            std::bit_cast<std::uint32_t>(in.vertices[in.indices[start+b]].position[k]);
        if(identical){count=0;s.duplicate=true;return Error::none;}
    }
    for(unsigned i=0;i<3;++i)s.a[i]=load(in.vertices[in.indices[start+i]],in.indices[start+i]);
    const auto determinant=sign(det(s.a[0],s.a[1],s.a[2]));
    if(determinant==0||determinant==2)return Error::degenerate;
    for(unsigned plane=0;plane<6;++plane){
        unsigned n=0;
        const auto append=[&](const Vertex& v){if(n==9)return false;s.b[n++]=v;return true;};
        for(unsigned i=0;i<count;++i){
            const auto& f=s.a[i];const auto& t=s.a[(i+1)%count];
            const auto fr=distanceRange(f,plane),tr=distanceRange(t,plane);
            const int sf=sign(fr),st=sign(tr);
            if(sf==2||st==2)return Error::ambiguous;
            if(sf>=0&&!append(f))return Error::polygonOverflow;
            if(sf*st==-1){
                const auto& outside=sf<0?f:t;const auto& inside=sf<0?t:f;
                const auto outer=sf<0?fr:tr,inner=sf<0?tr:fr;
                const auto ur=div(outer,sub(outer,inner));
                if(ur.lo<=0||ur.hi>=1)return Error::ambiguous;
                // Certified, bounded nonzero denominator precedes central math.
                const double df=distance(outside,plane),dt=distance(inside,plane),u=df/(df-dt);
                if(!std::isfinite(u)||u<=0||u>=1)return Error::ambiguous;
                Vertex v;v.boundary=(f.boundary&t.boundary)|(1u<<plane);
                for(unsigned k=0;k<15;++k)v.a[k]=affine(outside.a[k],inside.a[k],u);
                for(unsigned k=0;k<4;++k)v.p[k]=add(outside.p[k],mul(ur,sub(inside.p[k],outside.p[k])));
                if(!append(v))return Error::polygonOverflow;
            }
        }
        count=n;if(!count)return Error::none; // Strictly certified canonical noncoverage.
        for(unsigned i=0;i<count;++i)s.a[i]=s.b[i];
    }
    if(count<3)return Error::degenerate;
    for(unsigned i=0;i<count;++i){
        const auto f=s.a[i].original==65535?unpack(s.a[i]):in.vertices[s.a[i].original];
        if(auto e=finalVertex(s.a[i],f);e!=Error::none)return e;
    }
    for(unsigned i=1;i+1<count;++i){
        const auto d=sign(det(s.a[0],s.a[i],s.a[i+1]));
        if(d!=determinant)return Error::degenerate;
        // Binary32 emission must also retain nonzero winding.
        const auto a=load(unpack(s.a[0]),0),b=load(unpack(s.a[i]),0),c=load(unpack(s.a[i+1]),0);
        if(sign(det(a,b,c))!=determinant)return Error::degenerate;
    }
    return Error::none;
}
inline Plan scan(Input in,Scratch& scratch,Budget budget){
    Plan p;p.budget=budget;p.source=in.vertices.data();p.elements=in.indices.data();
    p.sourceCount=in.vertices.size();p.elementCount=in.indices.size();
    // Normal-only interval endpoints enclose gradual or flushed underflow.
    // Original/transport scalars are checked by their bits before conversion.
    if(std::fegetround()!=FE_TONEAREST){p.error=Error::environment;return p;}
#if defined(__arm__) && !defined(__SOFTFP__)
    std::uint32_t fpscr;asm volatile("vmrs %0, fpscr":"=r"(fpscr)::"memory");
    // ARM11 must retain FZ and scalar/no-user-traps mode. No register writes.
    if(!(fpscr&0x01000000u)||(fpscr&0x00f79f00u)){p.error=Error::environment;return p;}
#endif
    if((p.error=scratchCheck(in,scratch))!=Error::none)return p;
    if((p.error=originals(in,p))!=Error::none)return p;
    Hash hash;hash.append(std::as_bytes(in.vertices));hash.append(std::as_bytes(in.indices));p.digest=hash.finish();
    for(std::size_t t=0;t<in.indices.size();t+=3){
        unsigned n=0;if((p.error=triangle(in,t,scratch,n))!=Error::none){p.failedTriangle=t/3;p.counts={};return p;}
        if(!n){++p.counts.emptyTriangles;if(scratch.duplicate)++p.counts.duplicateTriangles;continue;}
        p.counts.vertices+=n;p.counts.indices+=3*(n-2);
        // Bounded before multiplication. Counts can never wrap.
        if(p.counts.vertices>std::min(maxVertices,budget.vertices)||p.counts.indices>std::min(maxIndices,budget.indices)){
            p.error=Error::budget;p.failedTriangle=t/3;p.counts={};return p;}
    }
    p.counts.rawBytes=60*p.counts.vertices+2*p.counts.indices;
    p.counts.arenaBytes=((60*p.counts.vertices+15)&~std::size_t(15))+2*p.counts.indices;
    if(p.counts.arenaBytes>std::min<std::size_t>(512*1024,budget.geometryBytes)){p.error=Error::budget;p.counts={};return p;}
    p.error=Error::none;return p;
}
} // detail
inline Plan plan(Input in,Scratch& scratch,Budget budget={}){return detail::scan(in,scratch,budget);}
inline Result emit(Input in,const Plan& expected,Scratch& scratch,std::span<GpuVertex> vertices,
                   std::span<std::uint16_t> indices){
    if(auto e=detail::scratchCheck(in,scratch);e!=Error::none)return {e,{}};
    if(vertices.size()>maxVertices||indices.size()>maxIndices||
       (!vertices.empty()&&!vertices.data())||(!indices.empty()&&!indices.data()))return {Error::capacity,{}};
    const void* ptr[]{in.vertices.data(),in.indices.data(),&scratch,&expected,vertices.data(),indices.data()};
    const std::size_t size[]{in.vertices.size_bytes(),in.indices.size_bytes(),sizeof scratch,sizeof expected,vertices.size_bytes(),indices.size_bytes()};
    for(unsigned a=0;a<6;++a)for(unsigned b=a+1;b<6;++b)
        if(detail::overlap(ptr[a],size[a],ptr[b],size[b]))return {Error::alias,{}};
    if(expected.error!=Error::none)return {Error::changed,{}};
    const auto current=detail::scan(in,scratch,expected.budget);
    if(current.error!=Error::none)return {current.error,{}};
    if(current.source!=expected.source||current.elements!=expected.elements||current.sourceCount!=expected.sourceCount||
       current.elementCount!=expected.elementCount||current.digest!=expected.digest||current.counts!=expected.counts)return {Error::changed,{}};
    if(vertices.size()<current.counts.vertices||indices.size()<current.counts.indices)return {Error::capacity,{}};
    // All fallible validation precedes the first write. The parent must exclude
    // mutation/concurrency for the entire call; SHA256 is not a synchronization primitive.
    std::size_t nv=0,ni=0;
    for(std::size_t t=0;t<in.indices.size();t+=3){
        unsigned n=0;const auto e=detail::triangle(in,t,scratch,n);
        if(e!=Error::none)return {e,{}}; // Impossible under the immutable/environment contract; never partial success.
        for(unsigned i=0;i<n;++i)vertices[nv+i]=scratch.a[i].original==65535?detail::unpack(scratch.a[i]):in.vertices[scratch.a[i].original];
        for(unsigned i=1;i+1<n;++i){indices[ni++]=std::uint16_t(nv);indices[ni++]=std::uint16_t(nv+i);indices[ni++]=std::uint16_t(nv+i+1);}
        nv+=n;
    }
    return {Error::none,current.counts};
}
} // dusk::native::gx::camera_clip109
