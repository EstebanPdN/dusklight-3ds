#include "game_assets.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
namespace dusk::pica {
namespace {
struct File {std::FILE* value{};~File(){if(value)std::fclose(value);}};
struct Bytes {std::byte* value{};~Bytes(){std::free(value);}};
}
bool GameAssets::initialize(Renderer& r,GameDraw& d,const char* root){
    if(renderer||d.renderer!=&r||!root||!root[0]||std::strlen(root)>=directory.size())return fail(GameAssetError::protocol);
    if(!r.owner())return fail(GameAssetError::owner);
    renderer=&r;draw=&d;std::memcpy(directory.data(),root,std::strlen(root)+1);failure=GameAssetError::none;return true;
}
std::size_t GameAssets::metadataBytes()const{return resident*sizeof(Asset);}
bool GameAssets::empty()const{for(const auto& i:instances)if(i.identity)return false;return resident==0;}
bool GameAssets::produce(const void* instance,unsigned slot,DrawUniforms& out,void* user) noexcept {
    auto& i=*static_cast<Instance*>(user);if(instance!=i.identity||!i.asset||!i.producer)return false;
    auto& owner=*i.owner;if(owner.busy){owner.invalidated=true;return false;}
    owner.busy=true;owner.invalidated=false;struct Scope{bool& value;~Scope(){value=false;}} scope{owner.busy};
    const auto* binding=i.asset->plan.uniform(slot);return binding&&i.producer(instance,*binding,out,i.user)&&!owner.invalidated;
}
bool GameAssets::bind(const void* instance,const SourceIdentity& source,GameSourceProducer producer,void* user,unsigned inputs,const FogParameters* environment){
    if(busy){invalidated=true;return fail(GameAssetError::protocol);}
    if(!renderer)return fail(GameAssetError::protocol);
    if(!renderer->owner())return fail(GameAssetError::owner); // Before any file/GPU operation.
    if(!instance||!producer||!source.bytes||inputs>63)return fail(GameAssetError::identity);
    Instance* vacant=nullptr;
    const auto sameEnvironment=[&](const Asset& a){return a.hasEnvironment==bool(environment)&&(!environment||
        (a.environment.enabled==environment->enabled&&a.environment.depthFlip==environment->depthFlip&&a.environment.color==environment->color&&a.environment.values==environment->values));};
    for(auto& i:instances){if(i.identity==instance){
        if(i.asset->source!=source||i.producer!=producer||i.user!=user||i.inputs!=inputs)return fail(GameAssetError::identity);
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
        // New resource admission cannot modify an already-emitted native frame.
        if(!renderer->writable())return fail(GameAssetError::protocol);
        constexpr char digits[]="0123456789abcdef";std::array<char,272> path{};
        const auto root=std::strlen(directory.data());std::memcpy(path.data(),directory.data(),root);unsigned at=unsigned(root);
        if(path[at-1]!='/')path[at++]='/';
        for(auto b:source.sha256){path[at++]=digits[b>>4];path[at++]=digits[b&15];}
        std::memcpy(path.data()+at,".dna",5);
        File file{std::fopen(path.data(),"rb")};if(!file.value)return fail(errno==ENOENT?GameAssetError::missing:GameAssetError::io);
        if(std::fseek(file.value,0,SEEK_END))return fail(GameAssetError::io);
        const auto size=std::ftell(file.value);
        if(size<128||std::uint64_t(size)>AssetPack::maxBytes)return fail(GameAssetError::pack);
        if(std::fseek(file.value,0,SEEK_SET))return fail(GameAssetError::io);
        Bytes bytes{static_cast<std::byte*>(std::malloc(std::size_t(size)))};if(!bytes.value)return fail(GameAssetError::allocation);
        if(std::fread(bytes.value,1,std::size_t(size),file.value)!=std::size_t(size)||std::fgetc(file.value)!=EOF||std::ferror(file.value))return fail(GameAssetError::io);
        AssetPack pack;if(AssetPack::open({bytes.value,std::size_t(size)},source,pack)!=PackError::none)return fail(GameAssetError::pack);
        const auto bindings=pack.plan.bindings(inputs);if(bindings.empty())return fail(GameAssetError::inputs);
        // Identical source BMDs may resolve different external images in other
        // archives/stages. Until the live variant key is implemented, refuse
        // these assets even if a caller declares every uniform input ready.
        if(pack.requiresTextureContext)return fail(GameAssetError::textureContext);
        // malloc bypasses the game's current temporary JKR solid heap. Asset
        // ownership must survive that heap and is bounded independently above.
        asset=static_cast<Asset*>(std::malloc(sizeof(Asset)));if(!asset)return fail(GameAssetError::allocation);
        std::construct_at(asset);asset->source=source;asset->plan=pack.plan;asset->shapes=pack.shapes;asset->hasEnvironment=bool(environment);if(environment)asset->environment=*environment;
        if(!asset->model.load(*renderer,pack.meshes,pack.materials,pack.images,bindings,environment)){
            // Model::load cancels its own partial allocations. Unload remains
            // explicit to retain ownership if an unexpected release fails.
            if(!asset->model.unload()){*destination=asset;++resident;return fail(GameAssetError::backend);}
            std::destroy_at(asset);std::free(asset);return fail(GameAssetError::backend);
        }
        *destination=asset;++resident;created=true;
    }else if(asset->plan.bindings(inputs).empty()||!sameEnvironment(*asset))return fail(GameAssetError::inputs);
    *vacant={instance,asset,producer,user,inputs,this};
    if(!draw->bind(instance,asset->model,std::span(asset->shapes).first(asset->plan.size()),produce,vacant)){
        *vacant={};
        if(created&&asset->model.unload()){*destination=nullptr;--resident;std::destroy_at(asset);std::free(asset);}
        return fail(GameAssetError::backend);
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
