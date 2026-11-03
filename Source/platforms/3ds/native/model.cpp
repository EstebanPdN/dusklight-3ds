#include "model.hpp"
namespace dusk::pica {
std::size_t Model::metadataBytesForParts(unsigned count){return std::size_t(count)*(sizeof(Part)+sizeof(MeshResource));}
bool Model::cancel(Error e){unload();failure=e;return false;}
bool Model::load(Renderer& r,const MeshPack& geometry,const MaterialPack& fragments,const TexturePack& pixels,std::span<const PartBinding> bindings,const FogParameters* environment){
    if(renderer)return fail(Error::protocol);
    if(revision==~std::uint64_t{})return fail(Error::budget);
    if(!r.writable())return fail(r.error());
    if(bindings.empty()||bindings.size()>maxModelParts)return fail(Error::invalidAsset);
    renderer=&r;partCount=unsigned(bindings.size());
    sequence=allocateStorage<Part>(partCount);meshes=allocateStorage<MeshResource>(partCount);
    auto meshViews=allocateStorage<MeshView>(partCount);
    if(!sequence||!meshes||!meshViews)return cancel(Error::allocation);
    // Validate every dependency and sampler before allocating any GPU resource.
    // Pack payloads remain immutable/alive for this synchronous load only.
    if(pixels.size()>images.size())return cancel(Error::budget);
    std::array<TextureView,64> bank;
    std::size_t bankBytes=0;imageCount=pixels.size();
    for(unsigned n=0;n<imageCount;++n){
        auto& image=bank[n];if(pixels.imageAt(n,image)!=PackError::none)return cancel(Error::invalidAsset);
        images[n].id=image.id;imageLevels[n]=std::uint8_t(image.image.mipLevels);
        framebufferImages[n]=image.image.framebufferColor;bankBytes+=image.payload.size();
    }
    for(unsigned n=0;n<partCount;++n){const auto& b=bindings[n];auto& p=sequence[n];
        if(!b.mesh||!b.material||b.uniformSlot>=maxModelParts)return cancel(Error::invalidAsset);
        for(const auto& sampler:b.samplers)if(!validateSampler(sampler,8))return cancel(Error::invalidAsset);
        unsigned mi=0;while(mi<meshCount&&meshes[mi].id!=b.mesh)++mi;
        if(mi==meshCount){
            GeometryInfo info;
            if(geometry.mesh(b.mesh,meshViews[mi])!=PackError::none||
               !inspectPackedGeometry(meshViews[mi].vertices,meshViews[mi].indices,info))return cancel(Error::invalidAsset);
            meshes[mi]={b.mesh,std::uint8_t(info.highestBone)};++meshCount;
        }
        MaterialView material;Material resolved;
        if(fragments.material(b.material,material)!=PackError::none||!instantiateMaterial(material,environment,resolved))return cancel(Error::invalidAsset);
        if(b.samplerMask!=resolved.textureMask)return cancel(Error::invalidAsset);
        const int ma=index(materials,materialCount,b.material);
        if(ma<0)return cancel(Error::budget);
        p.mesh=std::uint16_t(mi);p.material=std::uint8_t(ma);p.uniform=std::uint16_t(b.uniformSlot);p.textureMask=std::uint8_t(resolved.textureMask);p.samplers=b.samplers;
        p.highestBone=meshes[mi].highestBone;p.depth=resolved.depthTest||resolved.depthWrite;
        p.lookupMask=resolved.rasterLookup?std::uint8_t(3u<<resolved.lookupBase):0;p.fogCarrier=resolved.fogCarrier!=FogCarrier::none;
        p.fogMask=resolved.fogCarrier>=FogCarrier::texture0?std::uint8_t(1u<<(unsigned(resolved.fogCarrier)-unsigned(FogCarrier::texture0))):0;
        if((p.fogMask&p.lookupMask)||(p.fogMask&~p.textureMask))return cancel(Error::invalidAsset);
        for(unsigned stage=0;stage<resolved.stageCount;++stage)for(auto selected:resolved.stages[stage].constantBindings)if(selected)p.parameters|=1u<<((selected-1)/4);
        for(unsigned unit=0;unit<3;++unit)if(p.textureMask&(1u<<unit)){
            unsigned im=0;while(im<imageCount&&images[im].id!=material.imageIds[unit])++im;
            if(im==imageCount)return cancel(Error::invalidAsset);
            const auto& image=bank[im];if(!validateSampler(b.samplers[unit],image.image.mipLevels))return cancel(Error::invalidAsset);
            if((p.lookupMask&(1u<<unit))&&(image.image.width!=8||image.image.height!=8||image.image.mipLevels!=1))return cancel(Error::invalidAsset);
            if((p.fogMask&(1u<<unit))&&(image.image.width!=8||image.image.height!=256||image.image.mipLevels!=1))return cancel(Error::invalidAsset);
            p.images[unit]=std::uint8_t(im);
        }
    }
    if(r.stats.textureBytes>r.limits.textureBytes||bankBytes>r.limits.textureBytes-r.stats.textureBytes)return cancel(Error::budget);
    meshBank=r.createMeshBank({meshViews.get(),meshCount});if(!meshBank)return cancel(r.error());
    for(unsigned n=0;n<materialCount;++n){MaterialView v;Material m;
        if(fragments.material(materials[n].id,v)!=PackError::none||!instantiateMaterial(v,environment,m))return cancel(Error::invalidAsset);
        materials[n].handle=r.createMaterial(m);if(!materials[n].handle)return cancel(r.error());}
    for(unsigned n=0;n<imageCount;++n){const auto& v=bank[n];
        if(v.image.framebufferColor)continue;
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
    if(meshBank){if(renderer->release(meshBank))meshBank={};else first=renderer->error();}
    release(materials,materialCount);release(images,imageCount);
    if(first!=Error::none)return fail(first); // Retain remaining ownership for diagnosis/retry.
    renderer=nullptr;meshes={};materials={};images={};sequence={};imageLevels={};framebufferImages={};partCount=meshCount=materialCount=imageCount=0;
    failure=Error::none;return true;
}
Submission Model::draw(std::span<const DrawUniforms> parameters){
    const auto reject=[&](Error e,unsigned submitted=0){failure=e;return Submission{submitted,e};};
    if(!renderer||!loaded)return reject(Error::protocol);
    auto& r=*renderer;if(!r.owner())return reject(r.error());
    if(!r.open)return reject(Error::protocol);
    if(partCount>r.limits.drawsPerFrame-r.stats.frameDraws)return reject(Error::budget);
    for(unsigned n=0;n<partCount;++n){
        if(sequence[n].uniform>=parameters.size())return reject(Error::invalidAsset);
        const auto error=preflightPart(n,parameters[sequence[n].uniform]);if(error!=Error::none)return reject(error);
    }
    unsigned submitted=0;
    for(unsigned n=0;n<partCount;++n){if(!submitPart(n,parameters[sequence[n].uniform]))return reject(r.error(),submitted);++submitted;}
    failure=Error::none;return {submitted,Error::none};
}
Error Model::preflightPart(unsigned index,const DrawUniforms& u)const{
    if(!renderer||!loaded)return Error::protocol;
    if(index>=partCount)return Error::invalidAsset;
    const auto& p=sequence[index];
    if((p.depth&&!renderer->hasDepth)||((u.samplerMask|u.imageOverrideMask)&~p.textureMask)||
       ((u.samplerMask|u.imageOverrideMask)&(p.lookupMask|p.fogMask))||!validateUniforms(u,p.highestBone)||
       (u.material.suppliedMask&p.parameters)!=p.parameters)return Error::invalidAsset;
    if(p.fogCarrier&&(!u.fog.provided||!(u.material.suppliedMask&128)||u.material.colors[7]!=u.fog.color))return Error::invalidAsset;
    if(u.fog.enabled&&!p.fogCarrier)return Error::invalidAsset;
    if(u.projective.enabled&&(p.textureMask&1)&&(((p.lookupMask|p.fogMask)&1)||!u.uvTransforms[0].passthrough(0)))return Error::invalidAsset;
    for(unsigned unit=0;unit<3;++unit){
        if(((p.lookupMask|p.fogMask)&(1u<<unit))&&!u.uvTransforms[unit].passthrough(unit))return Error::invalidAsset;
        if(p.textureMask&(1u<<unit)){
            const int im=selectedImage(p,unit,u);
            if(im<0)return Error::invalidAsset;
            const auto& sampler=(u.samplerMask&(1u<<unit))?u.samplers[unit]:p.samplers[unit];
            if((!framebufferImages[im]&&!images[im].handle)||!validateSampler(sampler,imageLevels[im])||
               (framebufferImages[im]&&(sampler.wrapS!=Wrap::clamp||sampler.wrapT!=Wrap::clamp)))return Error::invalidAsset;
        }
    }
    return Error::none;
}
int Model::selectedImage(const Part& p,unsigned unit,const DrawUniforms& u)const{
    if(!(u.imageOverrideMask&(1u<<unit)))return p.images[unit];
    const auto id=u.imageOverrides[unit];if(!id)return -1;
    unsigned lo=0,hi=imageCount;
    while(lo<hi){const auto mid=lo+(hi-lo)/2;if(images[mid].id<id)lo=mid+1;else hi=mid;}
    return lo<imageCount&&images[lo].id==id?int(lo):-1;
}
bool Model::submitPart(unsigned index,const DrawUniforms& parameters){
    const auto& p=sequence[index];auto u=parameters;
    std::array<TextureHandle,3> textures{};TextureHandle capture{};float scaleS=1,scaleT=1;
    for(unsigned unit=0;unit<3;++unit)if(p.textureMask&(1u<<unit)){
        const auto im=selectedImage(p,unit,parameters);
        if(framebufferImages[im]){
            if(!capture){if(!renderer->borrowFramebuffer(images[im].handle,scaleS,scaleT))return fail(renderer->error());capture=images[im].handle;}
            textures[unit]=capture;
            if(unit==0&&u.projective.enabled){
                for(unsigned c=0;c<4;++c){
                    u.projective.rows[0][c]*=scaleS;
                    u.projective.rows[1][c]=u.projective.rows[2][c]-scaleT*u.projective.rows[1][c];
                }
            }else{
                auto& transform=u.uvTransforms[unit];
                for(float& x:transform.rows[0])x*=scaleS;
                for(float& x:transform.rows[1])x*=-scaleT;
                transform.translation[0]*=scaleS;transform.translation[1]=1-scaleT*transform.translation[1];
            }
        }else textures[unit]=images[im].handle;
        if(!(u.samplerMask&(1u<<unit)))u.samplers[unit]=p.samplers[unit];
    }
    u.samplerMask=p.textureMask&~(p.lookupMask|p.fogMask);
    u.imageOverrides={};u.imageOverrideMask=0; // Only Model resolves bank IDs.
    if(!renderer->draw(meshBank,p.mesh,materials[p.material].handle,textures,u))return fail(renderer->error());
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
