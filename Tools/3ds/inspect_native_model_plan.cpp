// Host-only native resource association inspector. Never submits a draw/frame.
#include "../../Source/platforms/3ds/native/model_plan.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include <cstring>
alignas(4) extern const unsigned char native_scene_shader[]={0};extern const unsigned int native_scene_shader_size=1;
alignas(4) extern const unsigned char native_scene_rigid_shader[]={1};extern const unsigned int native_scene_rigid_shader_size=1;
alignas(4) extern const unsigned char native_scene_uv_shader[]={2};extern const unsigned int native_scene_uv_shader_size=1;
alignas(4) extern const unsigned char native_scene_rigid_uv_shader[]={3};extern const unsigned int native_scene_rigid_uv_shader_size=1;
alignas(4) extern const unsigned char native_scene_projective_shader[]={4};extern const unsigned int native_scene_projective_shader_size=1;
alignas(4) extern const unsigned char native_scene_rigid_projective_shader[]={5};extern const unsigned int native_scene_rigid_projective_shader_size=1;
using namespace dusk::pica;
namespace {
std::vector<std::byte> read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return {};const auto size=f.tellg();if(size<0||size>64*1024*1024)return {};f.seekg(0);std::vector<std::byte> data(static_cast<std::size_t>(size));if(!f.read(reinterpret_cast<char*>(data.data()),size))return {};return data;}
}
int main(int argc,char** argv){
    if(argc!=5)return 2;auto planBytes=read(argv[1]),meshBytes=read(argv[2]),materialBytes=read(argv[3]),imageBytes=std::strcmp(argv[4],"-")?read(argv[4]):std::vector<std::byte>{};
    auto plan=std::make_unique<ModelPlan>();MeshPack geometry;MaterialPack fragments;TexturePack pixels;
    if(ModelPlan::open(planBytes,*plan)!=PackError::none||MeshPack::open(meshBytes,geometry)!=PackError::none||MaterialPack::open(materialBytes,fragments)!=PackError::none||(!imageBytes.empty()&&TexturePack::open(imageBytes,pixels)!=PackError::none))return 3;
    if(!plan->requiredInputs()||!plan->bindings(0).empty())return 4;
    std::array<PartBinding,64> parts{};unsigned opaque=0,alpha=0;
    for(unsigned n=0;n<plan->size();++n){unsigned pass;if(!plan->entry(n,parts[n],pass))return 5;opaque+=pass==0;alpha+=pass==1;
        MeshView mesh;GeometryInfo g;const auto* u=plan->uniform(parts[n].uniformSlot);
        if(!u||geometry.mesh(parts[n].mesh,mesh)!=PackError::none||!inspectPackedGeometry(mesh.vertices,mesh.indices,g)||u->paletteCount<=g.highestBone)return 6;
    }
    auto renderer=std::make_unique<Renderer>();auto model=std::make_unique<Model>();if(!renderer->initialize({}))return 7;
    FogParameters disabledFog; // Resource inspection only; never renders this context.
    const bool loaded=model->load(*renderer,geometry,fragments,pixels,{parts.data(),plan->size()},&disabledFog);
    const auto resources=model->resources();const auto stats=renderer->statistics();const auto error=model->error();
    if(!model->unload()||!renderer->releaseAfterContextDestroyed()||!nativeMock::allocations.empty())return 8;
    std::printf("{\"loaded\":%s,\"error\":%u,\"parts\":%u,\"uniform_slots\":%u,\"required_inputs\":%u,\"opaque_entries\":%u,\"alpha_entries\":%u,\"meshes\":%u,\"materials\":%u,\"images\":%u,\"mesh_bytes\":%zu,\"texture_bytes_including_white\":%zu,\"draws\":%llu,\"gpu_executed\":false}\n",
        loaded?"true":"false",unsigned(error),plan->size(),plan->uniformCount(),plan->requiredInputs(),opaque,alpha,resources.meshes,resources.materials,resources.images,stats.meshBytes,stats.textureBytes,static_cast<unsigned long long>(stats.draws));
    return loaded?0:9;
}
