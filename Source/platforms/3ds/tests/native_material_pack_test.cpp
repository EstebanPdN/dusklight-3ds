#include "native/material_pack.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
using namespace dusk::pica;
namespace {
std::uint32_t read(const std::vector<std::byte>& b,unsigned at){std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::uint32_t(std::to_integer<unsigned>(b[at+n]))<<(8*n);return v;}
void write(std::vector<std::byte>& b,unsigned at,std::uint32_t v){for(unsigned n=0;n<4;++n)b[at+n]=std::byte(v>>(8*n));}
void repair(std::vector<std::byte>& b){write(b,28,crc32(std::span(b).first(28),std::span(b).subspan(32,read(b,24))));}
void payloadRepair(std::vector<std::byte>& b,unsigned entry){write(b,entry+12,crc32(std::span(b).subspan(read(b,entry+4),read(b,entry+8))));repair(b);}
}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file.good());std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});
    std::vector<std::byte> bytes(raw.size());std::memcpy(bytes.data(),raw.data(),raw.size());
    MaterialPack pack;assert(MaterialPack::open(bytes,pack)==PackError::none&&pack.size()==2);MaterialView view;
    assert(pack.material(3,view)==PackError::none&&view.requiresFog&&view.imageIds[0]==17&&view.state.blend==Blend::custom);
    assert(view.state.customBlend.alphaSource==BlendFactor::sourceAlpha);
    Material resolved;assert(!instantiateMaterial(view,nullptr,resolved));FogParameters fog;assert(instantiateMaterial(view,&fog,resolved)&&!resolved.fog);
    view.initialSnapshotOnly=true;assert(!instantiateMaterial(view,&fog,resolved));assert(instantiateMaterial(view,&fog,resolved,true));view.initialSnapshotOnly=false;
    fog.enabled=true;fog.values.fill(.5f);assert(instantiateMaterial(view,&fog,resolved)&&resolved.fog&&resolved.fogValues[255]==.5f);
    fog.values[7]=-1;assert(!instantiateMaterial(view,&fog,resolved));
    assert(pack.material(11,view)==PackError::none&&view.state.fog&&view.state.fogColor==0x12345678&&view.state.fogValues[255]==1);
    assert(instantiateMaterial(view,nullptr,resolved));assert(pack.material(99,view)==PackError::missing&&view.id==0);
    std::vector<std::byte> unaligned(bytes.size()+1);std::memcpy(unaligned.data()+1,bytes.data(),bytes.size());
    assert(MaterialPack::open(std::span(unaligned).subspan(1),pack)==PackError::none&&pack.material(3,view)==PackError::none);
    unsigned rejected=0;
    for(unsigned n=0;n<bytes.size();++n){MaterialPack p;assert(MaterialPack::open(std::span(bytes).first(n),p)!=PackError::none&&p.size()==0);++rejected;}
    for(unsigned n=0;n<96;++n){auto bad=bytes;bad[n]^=std::byte{1};MaterialPack p;assert(MaterialPack::open(bad,p)!=PackError::none);++rejected;}
    for(auto [at,value]:std::vector<std::pair<unsigned,std::uint32_t>>{{32,0},{64,3},{36,0xffffffff},{40,0xffffffff},{60,4}}){
        auto bad=bytes;write(bad,at,value);repair(bad);MaterialPack p;assert(MaterialPack::open(bad,p)!=PackError::none);++rejected;
    }
    const auto first=read(bytes,36),second=read(bytes,68);
    for(auto [at,value]:std::vector<std::pair<unsigned,unsigned>>{{first,0},{first+1,0},{first+2,255},{first+6,64},{first+10,1},{first+20,255},{first+26,1},{first+32,255},{first+52,1}}){
        auto bad=bytes;bad[at]=std::byte(value);payloadRepair(bad,32);MaterialPack p;assert(MaterialPack::open(bad,p)==PackError::none);assert(p.material(3,view)!=PackError::none&&view.id==0);++rejected;
    }
    {auto bad=bytes;write(bad,second+64,0x7fc00000);payloadRepair(bad,64);MaterialPack p;assert(MaterialPack::open(bad,p)==PackError::none&&p.material(11,view)==PackError::layout);++rejected;}
    {auto edited=bytes;MaterialPack p;assert(MaterialPack::open(edited,p)==PackError::none);write(edited,36,0xffffffff);write(edited,48,0xffffffff);write(edited,60,0);assert(p.material(3,view)==PackError::none&&view.imageIds[0]==17&&view.requiresFog);}
    {auto bad=bytes;bad[first]^=std::byte{1};MaterialPack p;assert(MaterialPack::open(bad,p)==PackError::none&&p.material(3,view)==PackError::checksum);++rejected;}
    std::printf("PASS native material parsing, custom blend, required fog binding, owned metadata and %u malformed/truncated refusals\n",rejected);
}
