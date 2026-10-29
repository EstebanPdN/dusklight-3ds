#include "pose.hpp"
#include <bit>
#include <cmath>
#include <cstring>
namespace dusk::pica {
namespace {
std::uint32_t word(std::span<const std::byte> d,unsigned at){std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::to_integer<std::uint32_t>(d[at+n])<<(8*n);return v;}
float scalar(std::span<const std::byte> d,unsigned at){return std::bit_cast<float>(word(d,at));}
Affine matrix(std::span<const std::byte> d,unsigned at){Affine m;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)m[r][c]=scalar(d,at+16*r+4*c);return m;}
bool bounded(float x,float limit){const auto b=std::bit_cast<std::uint32_t>(x)&0x7fffffffu;return b<=std::bit_cast<std::uint32_t>(limit)&&(!b||b>=0x20800000u);}
bool valid(const Affine& m){for(auto& row:m)for(unsigned c=0;c<4;++c)if(!bounded(row[c],c==3?0x1p20f:32768.f))return false;return true;}
bool store(double v,float limit,float& out){if(!std::isfinite(v)||std::abs(v)>limit||(v!=0&&std::abs(v)<0x1p-62))return false;out=float(v);return bounded(out,limit);}
bool compose(const Affine& a,const Affine& b,Affine& out){for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c){double sum=c==3?a[r][3]:0.;for(unsigned k=0;k<3;++k)sum+=double(a[r][k])*b[k][c];if(!store(sum,c==3?0x1p20f:32768.f,out[r][c]))return false;}return true;}
bool normal(const Affine& m,Affine& out){
    const double a=m[0][0],b=m[0][1],c=m[0][2],d=m[1][0],e=m[1][1],f=m[1][2],g=m[2][0],h=m[2][1],i=m[2][2];
    const double cofactor[3][3]={{e*i-f*h,f*g-d*i,d*h-e*g},{c*h-b*i,a*i-c*g,b*g-a*h},{b*f-c*e,c*d-a*f,a*e-b*d}};
    const double determinant=a*cofactor[0][0]+b*cofactor[0][1]+c*cofactor[0][2];if(!std::isfinite(determinant)||determinant==0)return false;
    out={};for(unsigned r=0;r<3;++r)for(unsigned col=0;col<3;++col)if(!store(cofactor[r][col]/determinant,256,out[r][col]))return false;return true;
}
bool overlaps(const void* a,std::size_t an,const void* b,std::size_t bn){if(!an||!bn)return false;const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);return x<=y?y-x<an:x-y<bn;}
}
Affine PosePack::identity(){return {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};}
PackError PosePack::open(std::span<const std::byte> d,PosePack& out){
    out={};if(d.size()<48||d.size()>8u<<20||std::memcmp(d.data(),"DUSKPS01",8)||word(d,8)!=1)return PackError::header;
    const auto n=word(d,16),m=word(d,20),t=word(d,24),ma=word(d,32),ta=word(d,36);
    if(!n||n>8192||!m||m>65535||t>65536||word(d,12)!=d.size()||word(d,28)!=48||ma!=48+n*64||ta!=ma+m*16||d.size()!=ta+t*56||word(d,40))return PackError::layout;
    if(crc32(d.first(44),d.subspan(48))!=word(d,44))return PackError::checksum;
    std::array<std::uint64_t,64> anchors{};
    for(unsigned k=0;k<n;++k){const auto at=48+64*k;const auto parent=word(d,at),anchor=word(d,at+4);
        if(parent>k||anchor>4096||word(d,at+56)||word(d,at+60)||!valid(matrix(d,at+8)))return PackError::layout;
        if(anchor){auto& used=anchors[(anchor-1)/64];const auto bit=std::uint64_t(1)<<((anchor-1)%64);if(used&bit)return PackError::layout;used|=bit;}
    }
    unsigned used=0;
    for(unsigned k=0;k<m;++k){const auto at=ma+k*16,kind=word(d,at),first=word(d,at+4),size=word(d,at+8);
        if(word(d,at+12)>1)return PackError::layout;
        if(kind==0){if(!first||first>n||size)return PackError::layout;}
        else if(kind==1){if(first!=used||!size||size>255||size>t-used)return PackError::layout;used+=size;}
        else return PackError::layout;
    }
    if(used!=t)return PackError::layout;
    for(unsigned k=0;k<t;++k){const auto at=ta+k*56;if(!word(d,at)||word(d,at)>n||!bounded(scalar(d,at+4),16)||!valid(matrix(d,at+8)))return PackError::layout;}
    out.bytes=d;out.count=n;out.draws=m;out.terms=t;out.drawAt=ma;out.termAt=ta;return PackError::none;
}
unsigned PosePack::anchorNode(std::uint32_t id)const{if(!id)return 0;for(unsigned n=0;n<count;++n)if(word(bytes,48+n*64+4)==id)return n+1;return 0;}
bool PosePack::restLocal(unsigned n,Affine& out)const{out={};if(n>=count)return false;out=matrix(bytes,48+64*n+8);return true;}
bool PosePack::evaluate(const Affine& root,std::span<const Affine> locals,std::span<Affine> world,std::span<PoseMatrix> result,PoseView& out,std::span<const NormalMode> modes)const{
    out={};if(!count||world.size()<count||result.size()<draws||(!locals.empty()&&(locals.size()!=count||modes.size()!=draws))||(!modes.empty()&&modes.size()!=draws)||!valid(root))return false;
    world=world.first(count);result=result.first(draws);
    for(const auto& m:locals)if(!valid(m))return false;
    for(auto mode:modes)if(unsigned(mode)>1)return false;
    const auto conflict=[&](const void* p,std::size_t n){return overlaps(world.data(),world.size_bytes(),p,n)||overlaps(result.data(),result.size_bytes(),p,n);};
    if(conflict(bytes.data(),bytes.size())||conflict(&root,sizeof(root))||conflict(locals.data(),locals.size_bytes())||conflict(modes.data(),modes.size_bytes())||overlaps(world.data(),world.size_bytes(),result.data(),result.size_bytes()))return false;
    for(unsigned n=0;n<count;++n){const auto parent=word(bytes,48+n*64);const auto local=locals.empty()?matrix(bytes,48+n*64+8):locals[n];if(!compose(parent?world[parent-1]:root,local,world[n]))return false;}
    for(unsigned n=0;n<draws;++n){const auto at=drawAt+n*16;const auto kind=word(bytes,at),first=word(bytes,at+4),size=word(bytes,at+8);auto& m=result[n];
        if(kind==0)m.position=world[first-1];
        else {double sum[3][4]{};for(unsigned k=0;k<size;++k){const auto term=termAt+(first+k)*56;Affine product;const auto inverse=matrix(bytes,term+8);
                if(!compose(world[word(bytes,term)-1],inverse,product))return false;
                const double weight=scalar(bytes,term+4);for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)sum[r][c]+=double(product[r][c])*weight;
            }
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)if(!store(sum[r][c],c==3?0x1p20f:32768.f,m.position[r][c]))return false;
        }
        const auto mode=modes.empty()?NormalMode(word(bytes,at+12)):modes[n];
        if(mode==NormalMode::inverseTranspose){if(!normal(m.position,m.normal))return false;}
        else {m.normal={};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){if(!bounded(m.position[r][c],256))return false;m.normal[r][c]=m.position[r][c];}}
    }
    out.world=world;out.draws=result;return true;
}
bool PoseView::palette(std::span<const std::uint32_t> ids,Palette& out)const{
    out={};out.count=0;if(ids.empty()||ids.size()>paletteSize||draws.empty())return false;
    for(auto id:ids)if(!id||id>draws.size())return false;
    auto value=Palette::identity();value.count=unsigned(ids.size());
    for(unsigned n=0;n<ids.size();++n)for(unsigned r=0;r<3;++r){value.position[3*n+r]=draws[ids[n]-1].position[r];value.normal[3*n+r]=draws[ids[n]-1].normal[r];}
    out=value;return true;
}
}
