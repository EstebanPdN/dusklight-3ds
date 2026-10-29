// Host-only native fragment-template inspector; no GPU or game execution.
#include "../../Source/platforms/3ds/native/material_pack.hpp"
#include <cstdio>
#include <charconv>
#include <cstring>
#include <vector>
int main(int argc,char** argv){
    if(argc<2)return 2;FILE* file=std::fopen(argv[1],"rb");if(!file)return 3;
    std::fseek(file,0,SEEK_END);const auto size=std::ftell(file);std::rewind(file);
    if(size<32||size>81952){std::fclose(file);return 4;}
    std::vector<std::byte> bytes(size);const bool read=std::fread(bytes.data(),1,bytes.size(),file)==bytes.size();std::fclose(file);if(!read)return 5;
    dusk::pica::MaterialPack pack;if(dusk::pica::MaterialPack::open(bytes,pack)!=dusk::pica::PackError::none)return 6;
    unsigned stages=0,fog=0,bindings=0,snapshots=0,parameterized=0;
    std::vector<std::uint32_t> ids;
    if(argc==2)for(unsigned id=1;id<=pack.size();++id)ids.push_back(id);
    else for(int n=2;n<argc;++n){std::uint32_t id;auto end=argv[n]+std::strlen(argv[n]);auto parsed=std::from_chars(argv[n],end,id);
        if(parsed.ec!=std::errc{}||parsed.ptr!=end||!id||(!ids.empty()&&ids.back()>=id))return 10;ids.push_back(id);
    }
    if(ids.size()!=pack.size())return 11;
    for(auto id:ids){dusk::pica::MaterialView view;
        if(pack.material(id,view)!=dusk::pica::PackError::none)return 7;
        stages+=view.state.stageCount;fog+=view.requiresFog;
        snapshots+=view.initialSnapshotOnly;
        bool parameters=false;for(unsigned n=0;n<view.state.stageCount;++n)for(auto selected:view.state.stages[n].constantBindings)parameters|=selected!=0;
        parameterized+=parameters;
        for(auto image:view.imageIds)bindings+=image!=0;
        dusk::pica::Material state;
        if(view.requiresFog&&dusk::pica::instantiateMaterial(view,nullptr,state))return 8;
        dusk::pica::FogParameters context;
        if(view.initialSnapshotOnly&&dusk::pica::instantiateMaterial(view,&context,state))return 9;
    }
    std::printf("{\"native_templates\":%u,\"native_stages\":%u,\"required_fog_bindings\":%u,\"image_bindings\":%u,\"initial_snapshots\":%u,\"parameterized_templates\":%u}\n",pack.size(),stages,fog,bindings,snapshots,parameterized);
}
