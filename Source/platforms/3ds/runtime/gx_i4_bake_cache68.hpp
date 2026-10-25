#pragma once
#include "gx_i4_palette_bake84.hpp"
#include <cstring>
namespace dusk::native::gx::bake68 {
enum class Error {none,proof,reentrant,capacity,registration,lookup,alias};
enum class Mode { strictQuarter, exactNearestI4 };
// Single command-render owner. Address-stable, process-retained backing. No
// published slot is ever overwritten/evicted and no old source pointer is kept.
// Recompute the complete bounded bake on EVERY request, then compare every
// derived byte. Content sharing across original identities is safe because the
// result owns all bytes and no correctness decision trusts an old source ID/hash.
class Cache {
    struct Slot {std::array<std::uint8_t,bake84::maxBtiBytes> bti{};TextureHandle handle{};unsigned bytes{};bool occupied{},ready{};};
    std::array<Slot,8> slots{};
    bool active{};
    bool resolve(Slot& slot,Texture& output){
        std::uint32_t token{};Texture checked{};
        if(!slot.ready||!textureCommandToken(slot.bti.data()+32,token)||
           !textureFromCommandToken(token,checked)||checked.handle!=slot.handle||!checked.immutablePixels||
           checked.bti.data()!=slot.bti.data()||checked.bti.size()!=slot.bytes||
           checked.image.data()!=slot.bti.data()+32||checked.image.size()!=slot.bytes-32||
           checked.format!=slot.bti[0]||checked.width!=bake67::be16(slot.bti.data()+2)||
           checked.height!=bake67::be16(slot.bti.data()+4)||checked.wrapS||checked.wrapT||
           checked.minFilter||checked.magFilter||checked.mipCount!=1||checked.minLod||
           checked.maxLod||checked.lodBias!=0)return false;
        output=checked;return true;
    }
public:
    Cache()=default;
    Cache(const Cache&)=delete;Cache& operator=(const Cache&)=delete;
    Cache(Cache&&)=delete;Cache& operator=(Cache&&)=delete;
    unsigned count()const{unsigned n=0;for(const auto& s:slots)n+=s.ready;return n;}
    static constexpr unsigned capacity=8,maxGpuTexelBytes=8*16*16*4;
    bool fetch(const MaterialState& original,const Texture& source,Texture& output,
               PicaMaterialPlan& material,Error& error,bake67::Error& proof,Mode mode=Mode::strictQuarter){
        error=Error::none;proof=bake67::Error::none;
        auto bytes=[](const void* p,std::size_t n){return std::span(static_cast<const std::uint8_t*>(p),n);};
        auto conflict=[&](const void* p,std::size_t n){
            return bake67::overlaps(bytes(&original,sizeof(original)),p,n)||
                   bake67::overlaps(bytes(&source,sizeof(source)),p,n)||
                   bake67::overlaps(source.bti,p,n)||bake67::overlaps(source.image,p,n);
        };
        if(conflict(&output,sizeof(output))||conflict(&material,sizeof(material))||
           bake67::overlaps(bytes(&output,sizeof(output)),&material,sizeof(material))){
            error=Error::alias;proof=bake67::Error::alias;return false;
        }
        if(active){error=Error::reentrant;return false;}
        active=true;struct End{bool& a;~End(){a=false;}}end{active};
        bake84::Result result;
        {
            bake67::Result legacy;
            if(bake67::bake(original,source,legacy,bake67::Policy::strictNonzeroI4,&proof)){
                result.bytes=legacy.bytes;result.material=legacy.material;result.intensities=legacy.intensities;
                std::copy_n(legacy.bti.begin(),legacy.bytes,result.bti.begin());
            }else if(mode!=Mode::exactNearestI4||
                     !bake84::bake(original,source,result,bake84::Policy::exactNearestI4,&proof)){
                error=Error::proof;return false;
            }
        }
        Slot* free=nullptr;
        for(auto& slot:slots){
            if(!slot.occupied){if(!free)free=&slot;continue;}
            if(slot.bytes==result.bytes&&!std::memcmp(slot.bti.data(),result.bti.data(),result.bytes)){
                Texture checked;
                if(!resolve(slot,checked)){slot.ready=false;error=Error::lookup;return false;}
                output=checked;material=result.material;return true;
            }
        }
        if(!free){error=Error::capacity;return false;}
        // The FREE slot has no borrowed GPU/registry view. Copy first and only
        // register persistent storage. Registration failures publish no handle.
        std::memcpy(free->bti.data(),result.bti.data(),result.bytes);
        TextureHandle handle{};
        if(!registerTexture({free->bti.data(),result.bytes},handle,true)){
            error=Error::registration;return false;
        }
        free->handle=handle;free->bytes=result.bytes;free->occupied=true;free->ready=true;
        Texture checked;
        if(!resolve(*free,checked)){
            // Retain backing forever even after a post-registration failure.
            // Never overwrite a resource that the registry may still retain.
            free->ready=false;error=Error::lookup;return false;
        }
        output=checked;material=result.material;return true;
    }
};
}
