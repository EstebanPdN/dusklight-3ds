// Host SDK-substitute inspector; no draws, frames or invented fog environment.
#include "../../Source/platforms/3ds/native/game_assets.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
alignas(4) extern const unsigned char native_scene_shader[]={0};extern const unsigned int native_scene_shader_size=1;
alignas(4) extern const unsigned char native_scene_rigid_shader[]={1};extern const unsigned int native_scene_rigid_shader_size=1;
alignas(4) extern const unsigned char native_scene_uv_shader[]={2};extern const unsigned int native_scene_uv_shader_size=1;
alignas(4) extern const unsigned char native_scene_rigid_uv_shader[]={3};extern const unsigned int native_scene_rigid_uv_shader_size=1;
alignas(4) extern const unsigned char native_scene_projective_shader[]={4};extern const unsigned int native_scene_projective_shader_size=1;
alignas(4) extern const unsigned char native_scene_rigid_projective_shader[]={5};extern const unsigned int native_scene_rigid_projective_shader_size=1;
using namespace dusk::pica;
std::vector<std::byte> read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return {};const auto size=f.tellg();if(size<0||static_cast<std::size_t>(size)>AssetPack::maxBytes)return {};f.seekg(0);std::vector<std::byte> data(static_cast<std::size_t>(size));if(!f.read(reinterpret_cast<char*>(data.data()),size))return {};return data;}
int main(int argc,char** argv){
    if(argc!=4)return 2;auto original=read(argv[1]),bytes=read(argv[2]);SourceIdentity source;auto asset=std::make_unique<AssetPack>();
    if(!identifySource(original,source)||AssetPack::open(bytes,source,*asset)!=PackError::none)return 3;
    for(unsigned n=0;n<asset->plan.size();++n){PartBinding p;unsigned pass;MeshView mesh;GeometryInfo info;
        if(!asset->plan.entry(n,p,pass)||asset->meshes.mesh(p.mesh,mesh)!=PackError::none||!inspectPackedGeometry(mesh.vertices,mesh.indices,info))return 4;
        const auto* binding=asset->plan.uniform(p.uniformSlot);if(!binding||binding->paletteCount<=info.highestBone)return 5;
        MaterialView material;if(asset->materials.material(p.material,material)!=PackError::none)return 6;
        for(unsigned unit=0;unit<3;++unit)if(p.samplerMask&(1u<<unit)){TextureView image;
            if(asset->images.image(material.imageIds[unit],image)!=PackError::none||!validateSampler(p.samplers[unit],image.image.mipLevels))return 7;}
    }
    auto renderer=std::make_unique<Renderer>();auto draw=std::make_unique<GameDraw>();auto factory=std::make_unique<GameAssets>();
    if(!renderer->initialize({})||!draw->initialize(*renderer)||!factory->initialize(*renderer,*draw,argv[3]))return 8;
    const auto allocations=nativeMock::allocationCalls;unsigned calls=0;int instance{};
    const GameSourceProducer producer=+[](const void*,const UniformBinding&,DrawUniforms&,void* p) noexcept{++*static_cast<unsigned*>(p);return false;};
    const bool admitted=factory->bind(&instance,source,producer,&calls,orderingInput);const auto error=factory->error();
    if(admitted||error!=GameAssetError::inputs||calls||nativeMock::allocationCalls!=allocations||!factory->empty()||!draw->empty())return 9;
    if(asset->requiresTextureContext&&(factory->bind(&instance,source,producer,&calls,63)||factory->error()!=GameAssetError::textureContext||nativeMock::allocationCalls!=allocations||calls||!factory->empty()))return 11;
    if(!renderer->releaseAfterContextDestroyed()||!nativeMock::allocations.empty())return 10;
    std::printf("{\"parts\":%u,\"uniform_slots\":%u,\"required_inputs\":%u,\"source_shapes\":[",asset->plan.size(),asset->plan.uniformCount(),asset->plan.requiredInputs());
    for(unsigned n=0;n<asset->plan.size();++n)std::printf("%s%u",n?",":"",asset->shapes[n]);
    std::printf("],\"requires_texture_context\":%s,\"factory_refused_missing_inputs\":true,\"new_gpu_allocations\":0,\"draws\":0,\"gpu_executed\":false}\n",asset->requiresTextureContext?"true":"false");return 0;
}
