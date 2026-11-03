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
    assert(argc==5);std::ifstream file(argv[1],std::ios::binary);assert(file.good());std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});
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
    for(auto [at,value]:std::vector<std::pair<unsigned,unsigned>>{{first,0},{first+1,0},{first+2,255},{first+6,64},{first+10,1},{first+20,255},{first+26,1},{first+32,255},{first+52,33}}){
        auto bad=bytes;bad[at]=std::byte(value);payloadRepair(bad,32);MaterialPack p;assert(MaterialPack::open(bad,p)==PackError::none);assert(p.material(3,view)!=PackError::none&&view.id==0);++rejected;
    }
    {auto bad=bytes;write(bad,second+64,0x7fc00000);payloadRepair(bad,64);MaterialPack p;assert(MaterialPack::open(bad,p)==PackError::none&&p.material(11,view)==PackError::layout);++rejected;}
    {auto edited=bytes;MaterialPack p;assert(MaterialPack::open(edited,p)==PackError::none);write(edited,36,0xffffffff);write(edited,48,0xffffffff);write(edited,60,0);assert(p.material(3,view)==PackError::none&&view.imageIds[0]==17&&view.requiresFog);}
    {auto bad=bytes;bad[first]^=std::byte{1};MaterialPack p;assert(MaterialPack::open(bad,p)==PackError::none&&p.material(3,view)==PackError::checksum);++rejected;}
    for(unsigned input=2;input<4;++input){
        std::ifstream lookupFile(argv[input],std::ios::binary);assert(lookupFile.good());
        std::vector<char> lookupRaw((std::istreambuf_iterator<char>(lookupFile)),{});
        std::vector<std::byte> lookupBytes(lookupRaw.size());std::memcpy(lookupBytes.data(),lookupRaw.data(),lookupRaw.size());
        MaterialPack lookupPack;MaterialView lookupView;
        assert(MaterialPack::open(lookupBytes,lookupPack)==PackError::none&&lookupPack.material(1,lookupView)==PackError::none);
        const auto& state=lookupView.state;
        assert(state.rasterLookup&&state.lookupBase==1&&state.textureMask==7&&state.bufferRgbMask==1&&state.bufferAlphaMask==1);
        assert(instantiateMaterial(lookupView,nullptr,resolved)&&resolved.rasterLookup);
        if(input==3){
            assert(lookupView.requiresFog&&state.fogCarrier==FogCarrier::primaryAlpha);
            assert(resolved.stages[resolved.stageCount-1].alphaFunction==Combine::replace);
            assert(!instantiateMaterial(lookupView,&fog,resolved)); // Never substitute a depth LUT for source range fog.
        }else assert(!lookupView.requiresFog&&state.fogCarrier==FogCarrier::none);
        for(auto [offset,value]:std::vector<std::pair<unsigned,unsigned>>{{10,2},{11,2},{26,8},{27,1}}){
            auto bad=lookupBytes;const auto payload=read(bad,36);bad[payload+offset]=std::byte(value);payloadRepair(bad,32);
            MaterialPack invalid;assert(MaterialPack::open(bad,invalid)==PackError::none&&invalid.material(1,lookupView)==PackError::layout);
            ++rejected;
        }
    }
    {
        std::ifstream input(argv[4],std::ios::binary);assert(input.good());
        std::vector<char> rawFog((std::istreambuf_iterator<char>(input)),{});
        std::vector<std::byte> fogBytes(rawFog.size());std::memcpy(fogBytes.data(),rawFog.data(),rawFog.size());
        MaterialPack fogPack;assert(MaterialPack::open(fogBytes,fogPack)==PackError::none);
        for(unsigned id=1;id<=4;++id){
            MaterialView carrier;assert(fogPack.material(id,carrier)==PackError::none);
            const unsigned unit=id<=3?id-1:2;
            assert(unsigned(carrier.state.fogCarrier)==5+unit&&carrier.requiresFog&&!carrier.state.fog);
            assert(carrier.imageIds[unit]==15&&(carrier.state.textureMask&(1u<<unit)));
            assert(instantiateMaterial(carrier,nullptr,resolved)&&!resolved.fog);
            assert(!instantiateMaterial(carrier,&fog,resolved));
            const auto& final=carrier.state.stages[carrier.state.stageCount-1];
            assert(final.rgb[2]==Source(unsigned(Source::texture0)+unit)&&final.alpha[0]==Source::previous);
            for(unsigned fault=0;fault<4;++fault){
                auto bad=fogBytes;const unsigned entry=32+(id-1)*32,payload=read(bad,entry+4);
                const unsigned last=payload+32+32*(carrier.state.stageCount-1);
                if(fault==0)bad[payload+26]=std::byte{8};
                if(fault==1){bad[payload+1]&=std::byte(7u^(1u<<unit));write(bad,entry+16+4*unit,0);}
                if(fault==2)bad[payload+32]=std::byte(unsigned(Source::texture0)+unit); // Reuses a live source/lookup UV.
                if(fault==3)bad[last+3]=std::byte(unsigned(Source::primary)); // Loses source alpha.
                payloadRepair(bad,entry);MaterialPack invalid;MaterialView rejectedView;
                assert(MaterialPack::open(bad,invalid)==PackError::none&&invalid.material(id,rejectedView)==PackError::layout);
                ++rejected;
            }
        }
    }
    std::printf("PASS native material parsing, custom blend, required fog binding, owned metadata and %u malformed/truncated refusals\n",rejected);
}
