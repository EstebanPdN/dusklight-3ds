#include "native/material_pack.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace dusk::pica;
namespace {
std::uint32_t word(const std::vector<std::byte>& data,unsigned at){std::uint32_t value=0;for(unsigned n=0;n<4;++n)value|=std::to_integer<unsigned>(data[at+n])<<(8*n);return value;}
void put(std::vector<std::byte>& data,unsigned at,std::uint32_t value){for(unsigned n=0;n<4;++n)data[at+n]=std::byte(value>>(8*n));}
void fix(std::vector<std::byte>& data){put(data,44,crc32(std::span(data).subspan(64)));put(data,28,crc32(std::span(data).first(28),std::span(data).subspan(32,32)));}
}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file.good());std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});std::vector<std::byte> data(raw.size());std::memcpy(data.data(),raw.data(),raw.size());
    assert(word(data,8)==2&&word(data,16)==1&&word(data,36)==64);MaterialPack pack;MaterialView view;
    assert(MaterialPack::open(data,pack)==PackError::none&&pack.material(7,view)==PackError::none&&!view.initialSnapshotOnly);
    assert((view.state.stages[0].constantBindings==std::array<std::uint8_t,4>{1,6,0,32}));Material state;assert(instantiateMaterial(view,nullptr,state)&&state.stages[0].constantBindings[3]==32);
    unsigned cases=0;
    for(unsigned lane=0;lane<4;++lane)for(unsigned selector=0;selector<256;++selector){
        auto changed=data;changed[116+lane]=std::byte(selector);fix(changed);MaterialPack p;assert(MaterialPack::open(changed,p)==PackError::none);
        if(selector<=32){assert(p.material(7,view)==PackError::none);assert(view.state.stages[0].constantBindings[lane]==selector);}
        else assert(p.material(7,view)==PackError::layout&&view.id==0);
        ++cases;
    }
    // v1 never admits v2 bytes; future versions and remaining reserved fields
    // must not become implicit bindings.
    {auto changed=data;put(changed,8,1);fix(changed);MaterialPack p;assert(MaterialPack::open(changed,p)==PackError::none&&p.material(7,view)==PackError::layout);}
    {auto changed=data;put(changed,8,3);fix(changed);MaterialPack p;assert(MaterialPack::open(changed,p)==PackError::header);}
    {auto changed=data;MaterialPack p;assert(MaterialPack::open(changed,p)==PackError::none);put(changed,8,1);
        assert(p.material(7,view)==PackError::none&&view.state.stages[0].constantBindings[3]==32);}
    for(unsigned at=120;at<128;++at){auto changed=data;changed[at]=std::byte(1);fix(changed);MaterialPack p;assert(MaterialPack::open(changed,p)==PackError::none&&p.material(7,view)==PackError::layout);}
    std::printf("PASS %u native parameter-pack selector domains and version/reserved-field refusal\n",cases);
}
