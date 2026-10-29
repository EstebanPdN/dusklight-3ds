#include "model.hpp"
#include <algorithm>
namespace dusk::pica {
bool Model::cancel(Error e){unload();failure=e;return false;}
bool Model::load(Renderer& r,const MeshPack& geometry,const MaterialPack& fragments,const TexturePack& pixels,std::span<const PartBinding> bindings,const FogParameters* environment){
    if(renderer)return fail(Error::protocol);
    if(revision==~std::uint64_t{})return fail(Error::budget);
    if(!r.writable())return fail(r.error());
    if(bindings.empty()||bindings.size()>64)return fail(Error::invalidAsset);
    renderer=&r;partCount=unsigned(bindings.size());
    // Validate every dependency and sampler before allocating any GPU resource.
    // Pack payloads remain immutable/alive for this synchronous load only.
    for(unsigned n=0;n<partCount;++n){const auto& b=bindings[n];auto& p=sequence[n];
        if(!b.mesh||!b.material||b.uniformSlot>=64)return cancel(Error::invalidAsset);
        for(const auto& sampler:b.samplers)if(!validateSampler(sampler,8))return cancel(Error::invalidAsset);
        MeshView mesh;GeometryInfo info;MaterialView material;Material resolved;
        if(geometry.mesh(b.mesh,mesh)!=PackError::none||!inspectPackedGeometry(mesh.vertices,mesh.indices,info)||
           fragments.material(b.material,material)!=PackError::none||!instantiateMaterial(material,environment,resolved))return cancel(Error::invalidAsset);
        if(b.samplerMask!=resolved.textureMask)return cancel(Error::invalidAsset);
        const int mi=index(meshes,meshCount,b.mesh),ma=index(materials,materialCount,b.material);
        if(mi<0||ma<0)return cancel(Error::budget);
        p.mesh=std::uint8_t(mi);p.material=std::uint8_t(ma);p.uniform=std::uint8_t(b.uniformSlot);p.textureMask=std::uint8_t(resolved.textureMask);p.samplers=b.samplers;
        p.highestBone=std::uint8_t(info.highestBone);p.depth=resolved.depthTest||resolved.depthWrite;
        for(unsigned stage=0;stage<resolved.stageCount;++stage)for(auto selected:resolved.stages[stage].constantBindings)if(selected)p.parameters|=1u<<((selected-1)/4);
        uniformUsed[p.uniform]=true;uniformBones[p.uniform]=std::max(uniformBones[p.uniform],std::uint8_t(info.highestBone));
        uniformTextures[p.uniform]|=p.textureMask;needsDepth|=resolved.depthTest||resolved.depthWrite;
        for(unsigned stage=0;stage<resolved.stageCount;++stage)for(auto selected:resolved.stages[stage].constantBindings)if(selected)uniformParameters[p.uniform]|=1u<<((selected-1)/4);
        for(unsigned unit=0;unit<3;++unit)if(p.textureMask&(1u<<unit)){
            TextureView image;if(pixels.image(material.imageIds[unit],image)!=PackError::none||!validateSampler(b.samplers[unit],image.image.mipLevels))return cancel(Error::invalidAsset);
            const int im=index(images,imageCount,image.id);if(im<0)return cancel(Error::budget);p.images[unit]=std::uint8_t(im);
        }
    }
    for(unsigned n=0;n<meshCount;++n){MeshView v;if(geometry.mesh(meshes[n].id,v)!=PackError::none)return cancel(Error::invalidAsset);
        meshes[n].handle=r.createMeshBytes(v.vertices,v.indices);if(!meshes[n].handle)return cancel(r.error());}
    for(unsigned n=0;n<materialCount;++n){MaterialView v;Material m;
        if(fragments.material(materials[n].id,v)!=PackError::none||!instantiateMaterial(v,environment,m))return cancel(Error::invalidAsset);
        materials[n].handle=r.createMaterial(m);if(!materials[n].handle)return cancel(r.error());}
    for(unsigned n=0;n<imageCount;++n){TextureView v;if(pixels.image(images[n].id,v)!=PackError::none)return cancel(Error::invalidAsset);
        TextureInfo info{v.image.width,v.image.height,v.image.format};info.mipLevels=v.image.mipLevels;
        images[n].handle=r.createTexture(info,v.payload);if(!images[n].handle)return cancel(r.error());}
    loaded=true;++revision;failure=Error::none;return true;
}
bool Model::unload(){
    if(!renderer){failure=Error::none;return true;}
    if(!renderer->owner())return fail(renderer->error());
    loaded=false;Error first=Error::none;
    const auto release=[&](auto& resources,unsigned count){for(unsigned n=0;n<count;++n)if(resources[n].handle){
        if(renderer->release(resources[n].handle))resources[n].handle={};else if(first==Error::none)first=renderer->error();
    }};
    release(meshes,meshCount);release(materials,materialCount);release(images,imageCount);
    if(first!=Error::none)return fail(first); // Retain remaining ownership for diagnosis/retry.
    renderer=nullptr;meshes={};materials={};images={};sequence={};uniformBones={};uniformParameters={};uniformTextures={};uniformUsed={};needsDepth=false;partCount=meshCount=materialCount=imageCount=0;
    failure=Error::none;return true;
}
Submission Model::draw(std::span<const DrawUniforms> parameters){
    const auto reject=[&](Error e,unsigned submitted=0){failure=e;return Submission{submitted,e};};
    if(!renderer||!loaded)return reject(Error::protocol);
    auto& r=*renderer;if(!r.owner())return reject(r.error());
    if(!r.open)return reject(Error::protocol);
    if(needsDepth&&!r.hasDepth)return reject(Error::invalidAsset);
    if(partCount>r.limits.drawsPerFrame-r.stats.frameDraws)return reject(Error::budget);
    // One preflight per used pose/parameter slot, before any model submission.
    for(unsigned n=0;n<64;++n)if(uniformUsed[n]){
        if(n>=parameters.size())return reject(Error::invalidAsset);
        const auto& u=parameters[n];
        if(u.samplerMask||!validateUniforms(u,uniformBones[n])||(u.material.suppliedMask&uniformParameters[n])!=uniformParameters[n])return reject(Error::invalidAsset);
        if(u.projective.enabled&&(uniformTextures[n]&1)&&!u.uvTransforms[0].passthrough(0))return reject(Error::invalidAsset);
    }
    unsigned submitted=0;
    for(unsigned n=0;n<partCount;++n){const auto& p=sequence[n];auto u=parameters[p.uniform];u.samplers=p.samplers;u.samplerMask=p.textureMask;
        std::array<TextureHandle,3> textures{};for(unsigned unit=0;unit<3;++unit)if(p.textureMask&(1u<<unit))textures[unit]=images[p.images[unit]].handle;
        if(!r.draw(meshes[p.mesh].handle,materials[p.material].handle,textures,u))return reject(r.error(),submitted);
        ++submitted;
    }
    failure=Error::none;return {submitted,Error::none};
}
Error Model::preflightPart(unsigned index,const DrawUniforms& u)const{
    if(!renderer||!loaded)return Error::protocol;
    if(index>=partCount)return Error::invalidAsset;
    const auto& p=sequence[index];
    if((p.depth&&!renderer->hasDepth)||u.samplerMask||!validateUniforms(u,p.highestBone)||
       (u.material.suppliedMask&p.parameters)!=p.parameters)return Error::invalidAsset;
    if(u.projective.enabled&&(p.textureMask&1)&&!u.uvTransforms[0].passthrough(0))return Error::invalidAsset;
    return Error::none;
}
bool Model::submitPart(unsigned index,const DrawUniforms& parameters){
    const auto& p=sequence[index];auto u=parameters;u.samplers=p.samplers;u.samplerMask=p.textureMask;
    std::array<TextureHandle,3> textures{};
    for(unsigned unit=0;unit<3;++unit)if(p.textureMask&(1u<<unit))textures[unit]=images[p.images[unit]].handle;
    if(!renderer->draw(meshes[p.mesh].handle,materials[p.material].handle,textures,u))return fail(renderer->error());
    failure=Error::none;return true;
}
Submission Model::drawParts(std::span<const unsigned> indices,std::span<const DrawUniforms> parameters){
    const auto reject=[&](Error e,unsigned submitted=0){failure=e;return Submission{submitted,e};};
    if(!renderer||!loaded)return reject(Error::protocol);
    auto& r=*renderer;if(!r.owner())return reject(r.error());
    if(!r.open)return reject(Error::protocol);
    if(indices.size()>r.limits.drawsPerFrame-r.stats.frameDraws)return reject(Error::budget);
    for(unsigned index:indices){
        if(index>=partCount||sequence[index].uniform>=parameters.size())return reject(Error::invalidAsset);
        const auto error=preflightPart(index,parameters[sequence[index].uniform]);if(error!=Error::none)return reject(error);
    }
    unsigned submitted=0;
    for(unsigned index:indices){if(!submitPart(index,parameters[sequence[index].uniform]))return reject(failure,submitted);++submitted;}
    failure=Error::none;return {submitted,Error::none};
}
Submission Model::drawPart(unsigned index,const DrawUniforms& parameters){
    const auto reject=[&](Error e){failure=e;return Submission{0,e};};
    if(!renderer||!loaded)return reject(Error::protocol);
    auto& r=*renderer;if(!r.owner())return reject(r.error());
    if(!r.open)return reject(Error::protocol);
    if(r.stats.frameDraws>=r.limits.drawsPerFrame)return reject(Error::budget);
    const auto error=preflightPart(index,parameters);if(error!=Error::none)return reject(error);
    if(!submitPart(index,parameters))return reject(failure);
    return {1,Error::none};
}
}
