#include "game_assets.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#if defined(__3DS__)
extern "C" void OSReport(const char*,...);
#endif
namespace dusk::pica {
namespace {
struct File {std::FILE* value{};~File(){if(value)std::fclose(value);}};
struct Bytes {std::byte* value{};~Bytes(){std::free(value);}};
#if defined(__3DS__)
void reportInvalidProducer(const char* stage,unsigned slot,const UniformBinding* binding){
    OSReport("Native asset producer rejected stage=%s slot=%u source_shape=%d\n",
             stage,slot,binding?int(binding->sourceShape):-1);
}
#else
void reportInvalidProducer(const char*,unsigned,const UniformBinding*){}
#endif
}
bool GameAssets::initialize(Renderer& r,GameDraw& d,const char* root){
    if(renderer||d.renderer!=&r||!root||!root[0]||std::strlen(root)>=directory.size())return fail(GameAssetError::protocol);
    if(!r.owner())return fail(GameAssetError::owner);
    renderer=&r;draw=&d;std::memcpy(directory.data(),root,std::strlen(root)+1);failure=GameAssetError::none;return true;
}
std::size_t GameAssets::metadataBytes()const{std::size_t bytes=0;for(const auto* asset:assets)if(asset)bytes+=asset->metadataBytes();return bytes;}
bool GameAssets::empty()const{for(const auto& i:instances)if(i.identity)return false;return resident==0;}
bool GameAssets::produce(const void* instance,unsigned slot,DrawUniforms& out,void* user) noexcept {
    auto& i=*static_cast<Instance*>(user);
    if(instance!=i.identity||!i.asset||!i.producer){reportInvalidProducer("binding",slot,nullptr);return false;}
    auto& owner=*i.owner;if(owner.busy){owner.invalidated=true;reportInvalidProducer("reentrant",slot,nullptr);return false;}
    owner.busy=true;owner.invalidated=false;struct Scope{bool& value;~Scope(){value=false;}} scope{owner.busy};
    const auto* binding=i.asset->plan.uniform(slot);
    if(!binding){reportInvalidProducer("uniform-plan",slot,nullptr);return false;}
    if(!i.producer(instance,*binding,out,i.user)){reportInvalidProducer("callback",slot,binding);return false;}
    if(owner.invalidated){reportInvalidProducer("invalidated",slot,binding);return false;}
    return true;
}
bool GameAssets::bind(const void* instance,const SourceIdentity& source,GameSourceProducer producer,void* user,unsigned inputs,const FogParameters* environment){
    if(busy){invalidated=true;return fail(GameAssetError::protocol);}
    if(!renderer)return fail(GameAssetError::protocol);
    if(!renderer->owner())return fail(GameAssetError::owner); // Before any file/GPU operation.
    if(!instance||!producer||!source.bytes||inputs>63)return fail(GameAssetError::identity);
    if(!source.hasImageContext)return fail(GameAssetError::textureContext);
    Instance* vacant=nullptr;
    const auto sameEnvironment=[&](const Asset& a){return a.hasEnvironment==bool(environment)&&(!environment||
        (a.environment.enabled==environment->enabled&&a.environment.depthFlip==environment->depthFlip&&a.environment.color==environment->color&&a.environment.values==environment->values));};
    for(auto& i:instances){if(i.identity==instance){
        if(i.producer!=producer||i.user!=user||i.inputs!=inputs)return fail(GameAssetError::identity);
        if(i.asset->source!=source){
            if(i.asset->source.bytes!=source.bytes||i.asset->source.sha256!=source.sha256)return fail(GameAssetError::identity);
            // An actual texture-table assignment selects another bank. Retire
            // the old binding, never draw it as a fallback for a missing variant.
            if(!forget(instance))return false;
            vacant=&i;break;
        }
        if(!sameEnvironment(*i.asset))return fail(GameAssetError::inputs);
        failure=GameAssetError::none;return true;
    }if(!i.identity&&!vacant)vacant=&i;}
    if(!vacant)return fail(GameAssetError::budget);
    busy=true;struct Scope{bool& value;~Scope(){value=false;}} scope{busy};
    Asset* asset=nullptr;Asset** destination=nullptr;
    for(auto& p:assets){if(p&&p->source==source)asset=p;if(!p&&!destination)destination=&p;}
    bool created=false;
    if(!asset){
        if(!destination||metadataBytes()+sizeof(Asset)>256u<<10)return fail(GameAssetError::budget);
        // Admit only new immutable storage; queued and retired resources remain owned.
        if(!renderer->writable())return fail(GameAssetError::protocol);
        constexpr char digits[]="0123456789abcdef";std::array<char,328> path{};
        const auto root=std::strlen(directory.data());std::memcpy(path.data(),directory.data(),root);unsigned at=unsigned(root);
        if(path[at-1]!='/')path[at++]='/';
        for(auto b:source.sha256){path[at++]=digits[b>>4];path[at++]=digits[b&15];}
        path[at++]='-';
        for(auto b:source.imageContext){path[at++]=digits[b>>4];path[at++]=digits[b&15];}
        std::memcpy(path.data()+at,".dna",5);
        File file{std::fopen(path.data(),"rb")};
        if(!file.value){
            if(errno==ENOENT){
#if defined(__3DS__)
                const auto* name=path.data()+root+(directory[root-1]!='/'?1:0);
                // OSReport has a 256-byte buffer; keep each complete line below it.
                OSReport("Native missing asset directory=%.191s\n",directory.data());
                OSReport("Native missing asset filename(source_sha256-image_context_sha256)=%.133s\n",name);
                errno=ENOENT;
#endif
                return fail(GameAssetError::missing);
            }
            return fail(GameAssetError::io);
        }
        if(std::fseek(file.value,0,SEEK_END))return fail(GameAssetError::io);
        const auto size=std::ftell(file.value);
        if(size<160||std::uint64_t(size)>AssetPack::maxBytes)return fail(GameAssetError::pack);
        if(std::fseek(file.value,0,SEEK_SET))return fail(GameAssetError::io);
        Bytes bytes{static_cast<std::byte*>(std::malloc(std::size_t(size)))};if(!bytes.value)return fail(GameAssetError::allocation);
        if(std::fread(bytes.value,1,std::size_t(size),file.value)!=std::size_t(size)||std::fgetc(file.value)!=EOF||std::ferror(file.value))return fail(GameAssetError::io);
        AssetPack pack;const auto packError=AssetPack::open({bytes.value,std::size_t(size)},source,pack);
        if(packError!=PackError::none)return fail(packError==PackError::allocation?GameAssetError::allocation:GameAssetError::pack);
        const auto bindings=pack.plan.bindings(inputs);if(bindings.empty())return fail(GameAssetError::inputs);
        const auto metadata=sizeof(Asset)+pack.plan.metadataBytes()+Model::metadataBytesForParts(pack.plan.size())+std::size_t(pack.plan.size())*sizeof(unsigned);
        if(metadata>(256u<<10)-metadataBytes())return fail(GameAssetError::budget);
        // malloc bypasses the game's current temporary JKR solid heap. Asset
        // ownership must survive that heap and is bounded independently above.
        asset=static_cast<Asset*>(std::malloc(sizeof(Asset)));if(!asset)return fail(GameAssetError::allocation);
        std::construct_at(asset);asset->source=source;asset->plan=std::move(pack.plan);asset->shapes=std::move(pack.shapes);asset->hasEnvironment=bool(environment);if(environment)asset->environment=*environment;
        if(!asset->model.load(*renderer,pack.meshes,pack.materials,pack.images,bindings,environment)){
            // Model::load cancels its own partial allocations. Unload remains
            // explicit to retain ownership if an unexpected release fails.
#if defined(__3DS__)
            {const auto* detail=renderer->errorDetail();
            OSReport("Native asset load failed error=%u detail=%.160s\n",unsigned(asset->model.error()),detail?detail:"none");}
#endif
            if(!asset->model.unload()){*destination=asset;++resident;return fail(GameAssetError::backend);}
            std::destroy_at(asset);std::free(asset);return fail(GameAssetError::backend);
        }
        *destination=asset;++resident;created=true;
    }else if(asset->plan.bindings(inputs).empty()||!sameEnvironment(*asset))return fail(GameAssetError::inputs);
    *vacant={instance,asset,producer,user,inputs,this};
    if(!draw->bind(instance,asset->model,{asset->shapes.get(),asset->plan.size()},produce,vacant)){
        const auto bindingError=draw->error();*vacant={};
        if(created){
            if(!asset->model.unload())return fail(GameAssetError::backend);
            *destination=nullptr;--resident;std::destroy_at(asset);std::free(asset);
        }
        return fail(bindingError==Error::budget?GameAssetError::budget:bindingError==Error::allocation?GameAssetError::allocation:GameAssetError::backend);
    }
    ++asset->references;failure=GameAssetError::none;return true;
}
bool GameAssets::forget(const void* instance){
    if(!renderer)return true;
    if(busy){invalidated=true;return fail(GameAssetError::protocol);}
    if(!renderer->owner())return fail(GameAssetError::owner);
    for(auto& i:instances)if(i.identity==instance){
        if(!draw->forget(instance))return fail(GameAssetError::backend);
        auto* asset=i.asset;i={};--asset->references;
        if(!asset->references){
            if(!asset->model.unload())return fail(GameAssetError::backend);
            for(auto& p:assets)if(p==asset)p=nullptr;
            --resident;std::destroy_at(asset);std::free(asset);
        }
        break;
    }
    failure=GameAssetError::none;return true;
}
}
