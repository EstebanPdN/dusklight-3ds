#include "texture_resources.hpp"
#include "resource_archive.hpp"
#include "texture_gx.hpp"
#include "gx_state.hpp"
#include "gpu_sync.hpp"
#include "../gx_texture_pica.hpp"
#include <dolphin/os.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
namespace dusk::native {
namespace {
struct Exclusion {BOOL previous=OSDisableInterrupts();~Exclusion(){OSRestoreInterrupts(previous);}};
struct Archive {const void* owner{};std::span<const std::uint8_t> bytes{};};
struct Image {const void* identity{};std::uint8_t* bti{};std::size_t bytes{};gx::TextureHandle handle{};unsigned references{};};
struct Borrower {const void* owner{};Image* image{};bool released{};};
std::array<Archive,128> archives{};
std::array<Image,512> images{};
std::array<Borrower,2048> borrowers{};
std::size_t used{};
// Protected by Exclusion along with the borrowers. Most per-draw retain
// calls have no retirement work; avoid scanning all 2048 records each time.
bool pendingReleases{};
constexpr std::size_t budget=4*1024*1024;
std::atomic<const char*> failure{"none"};
bool reject(const char* reason){failure.store(reason,std::memory_order_relaxed);return false;}
bool renderOwner(){return platformGpuOwner()&&platformGpuOwner()==OSGetCurrentThread();}
std::uint32_t word(const std::uint8_t* p){return(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
bool collect(){
    if(!pendingReleases)return true;
    if(!renderOwner()||platformCompleteGpu(500000000)!=GpuCompletion::complete)return reject("texture retirement GPU completion");
    for(auto& borrower:borrowers)if(borrower.released){
        auto& image=*borrower.image;
        if(image.references>1){--image.references;borrower={};continue;}
        if(!releaseGxTexture(image.handle)||!gx::unregisterTexture(image.handle))return reject("texture retirement handle");
        borrower={};used-=image.bytes;std::free(image.bti);image={};
    }
    pendingReleases=false;return true;
}
bool retain(const void* owner,std::span<const std::uint8_t> bti,const void* identity){
    if(!owner||!renderOwner())return reject("texture resource render owner");
    if(!collect())return false;
    BtiInfo info{};NativeTexturePlan plan{};
    if(inspectBti({bti.data(),bti.size()},1024,info)!=ResourceStatus::ready||
       !inspectNativeTexture(bti,plan)||plan.sourceBytes!=info.image.size)return reject("texture resource BTI bounds/format");
    if(!identity)identity=info.image.data;
    Borrower* borrower=nullptr;Image* slot=nullptr;
    for(auto& entry:borrowers){
        if(entry.owner==owner&&entry.image->identity==identity)return true;
        if(!entry.owner&&!borrower)borrower=&entry;
    }
    if(!borrower)return reject("texture resource owners exhausted");
    for(auto& entry:images){
        if(entry.identity==identity){
            // Original TEX1 headers can share texels with different wrapping,
            // filtering and LOD options. Those belong to each GX object; the
            // immutable image layout is format, extent and mip count only.
            BtiInfo old;
            if(inspectBti({entry.bti,entry.bytes},1024,old)!=ResourceStatus::ready||
               entry.bytes!=32+info.image.size+info.palette.size||entry.bti[0]!=bti[0]||
               std::memcmp(entry.bti+2,bti.data()+2,4)||entry.bti[24]!=bti[24]||
               std::memcmp(entry.bti+32,info.image.data,info.image.size)||
               old.paletteEntries!=info.paletteEntries||old.paletteFormat!=info.paletteFormat||
               (info.palette.size&&std::memcmp(old.palette.data,info.palette.data,info.palette.size))){
                OSReport("Native immutable texture conflict: old fmt=%u %ux%u mips=%u bytes=%lu; new fmt=%u %ux%u mips=%u bytes=%lu\n",
                    unsigned(entry.bti[0]),unsigned(entry.bti[2])*256+entry.bti[3],unsigned(entry.bti[4])*256+entry.bti[5],unsigned(entry.bti[24]),static_cast<unsigned long>(entry.bytes-32),
                    unsigned(bti[0]),unsigned(info.width),unsigned(info.height),unsigned(info.mipCount),static_cast<unsigned long>(info.image.size));
                return reject("texture identity reused while retained");
            }
            if(info.palette.size&&!gx::aliasTexturePalette(entry.handle,info.palette.data))return reject("texture palette alias");
            *borrower={owner,&entry,false};++entry.references;return true;
        }
        if(!entry.identity&&!slot)slot=&entry;
    }
    const auto bytes=32+info.image.size+info.palette.size;
    if(!slot||bytes>budget-used){
        unsigned imageCount{},ownerCount{};
        for(const auto& value:images)imageCount+=value.identity!=nullptr;
        for(const auto& value:borrowers)ownerCount+=value.owner!=nullptr;
        OSReport("Native texture retention limit: images=%u/%u owners=%u/%u bytes=%lu/%lu required=%lu\n",
            imageCount,unsigned(images.size()),ownerCount,unsigned(borrowers.size()),
            static_cast<unsigned long>(used),static_cast<unsigned long>(budget),static_cast<unsigned long>(bytes));
        return reject(!slot?"texture resource image slots exhausted":"texture resource byte budget exhausted");
    }
    auto* copy=static_cast<std::uint8_t*>(std::malloc(bytes));if(!copy)return reject("texture resource allocation");
    std::memcpy(copy,bti.data(),32);copy[28]=copy[29]=copy[30]=0;copy[31]=32;
    std::memcpy(copy+32,info.image.data,info.image.size);
    if(info.palette.size){
        const auto offset=static_cast<std::uint32_t>(32+info.image.size);
        for(unsigned n=0;n<4;++n)copy[12+n]=offset>>(24-8*n);
        std::memcpy(copy+offset,info.palette.data,info.palette.size);
    }
    gx::TextureHandle handle{};
    if(!gx::registerTexture({copy,bytes},handle,true)){std::free(copy);return reject("texture resource registration");}
    if(!gx::aliasTextureImage(handle,identity)){gx::unregisterTexture(handle);std::free(copy);return reject("texture resource alias");}
    if(info.palette.size&&!gx::aliasTexturePalette(handle,info.palette.data)){gx::unregisterTexture(handle);std::free(copy);return reject("texture palette alias");}
    *slot={identity,copy,bytes,handle,1};*borrower={owner,slot,false};used+=bytes;return true;
}
}
bool registerTextureArchive(const void* owner,std::span<const std::uint8_t> bytes){
    if(!owner)return false;
    RarcArchive archive;
    const auto status=archive.open(bytes.data(),bytes.size(),{64*1024*1024,4096,65536,255});
    if(status!=ResourceStatus::ready){OSReport("Native texture archive rejected: %s (%lu bytes)\n",resourceStatusText(status),static_cast<unsigned long>(bytes.size()));return false;}
    bytes=bytes.first(word(bytes.data()+4));
    Exclusion guard;Archive* available=nullptr;
    for(auto& entry:archives){
        if(entry.owner==owner)return entry.bytes.data()==bytes.data()&&entry.bytes.size()==bytes.size();
        if(!entry.owner&&!available)available=&entry;
    }
    if(!available)return false;*available={owner,bytes};return true;
}
void unregisterTextureArchive(const void* owner){
    if(!owner)return;
    Exclusion guard;
    for(auto& entry:archives)if(entry.owner==owner){
        for(auto& borrower:borrowers)if(borrower.owner==owner){borrower.released=true;pendingReleases=true;}
        entry={};
    }
}
bool retainTextureResource(const void* owner,std::span<const std::uint8_t> bti,const void* identity){Exclusion guard;return retain(owner,bti,identity);}
bool retainRawTextureResource(const void* owner,std::span<const std::uint8_t> image,unsigned width,unsigned height,unsigned format){
    if(!owner||image.empty()||image.size()>budget-32||!width||!height||width>1024||height>1024||
        (format>6&&format!=14))return reject("raw texture extent/format");
    auto* bti=static_cast<std::uint8_t*>(std::calloc(1,32+image.size()));
    if(!bti)return reject("raw texture metadata allocation");
    bti[0]=format;bti[1]=1;bti[2]=width>>8;bti[3]=width;bti[4]=height>>8;bti[5]=height;
    bti[20]=bti[21]=1;bti[24]=1;bti[31]=32;
    std::memcpy(bti+32,image.data(),image.size());
    NativeTexturePlan plan;
    const bool valid=inspectNativeTexture({bti,32+image.size()},plan)&&plan.sourceBytes==image.size();
    const bool retained=valid?retainTextureResource(owner,{bti,32+image.size()},image.data()):reject("raw texture pixel extent mismatch");
    std::free(bti);return retained;
}
bool retainArchiveTexture(const void* owner,const void* bti,std::uint64_t externalHandle){
    Exclusion guard;
    if(externalHandle){
        bool owned=false;
        for(const auto& image:images)owned|=image.handle==externalHandle;
        // A separately managed GX-copy/TEX1 registration already supplies its
        // immutable extent and lifetime. Do not duplicate its image identity.
        if(!owned&&externalHandle<=0x7fffff){
            gx::Texture external;
            if(gx::textureFromCommandToken(0x800000u|static_cast<std::uint32_t>(externalHandle),external))return true;
        }
    }
    const auto address=reinterpret_cast<std::uintptr_t>(bti);
    for(const auto& archive:archives)if(archive.owner){
        const auto begin=reinterpret_cast<std::uintptr_t>(archive.bytes.data());
        if(address>=begin&&address-begin<archive.bytes.size())
            return retain(owner,archive.bytes.subspan(address-begin),nullptr);
    }
    // GX framebuffer-copy images have their own retained execution provider.
    // A bank image whose archive disappeared cannot silently reuse old pixels.
    if(externalHandle){
        for(const auto& image:images)if(image.handle==externalHandle)
            return reject("retained texture archive is no longer live");
        gx::Texture external;
        if(externalHandle<=0x7fffff&&gx::textureFromCommandToken(0x800000u|static_cast<std::uint32_t>(externalHandle),external))return true;
    }
    return reject("texture is outside a live validated archive");
}
bool retainArchiveTextureForLifetime(const void* bti,std::uint64_t externalHandle){
    Exclusion guard;
    const auto address=reinterpret_cast<std::uintptr_t>(bti);
    for(const auto& archive:archives)if(archive.owner){
        const auto begin=reinterpret_cast<std::uintptr_t>(archive.bytes.data());
        if(address>=begin&&address-begin<archive.bytes.size())
            return retain(archive.owner,archive.bytes.subspan(address-begin),nullptr);
    }
    // Original framebuffer captures have a separately validated provider and
    // are not immutable archive images. Never extend a retired archive lease.
    if(externalHandle&&externalHandle<=0x7fffff){
        for(const auto& image:images)if(image.handle==externalHandle)
            return reject("archive-lifetime texture is no longer live");
        gx::Texture external;
        if(gx::textureFromCommandToken(0x800000u|static_cast<std::uint32_t>(externalHandle),external))return true;
    }
    return reject("archive-lifetime texture is outside a live archive");
}
void releaseTextureResources(const void* owner){
    if(!owner)return;
    Exclusion guard;
    for(auto& borrower:borrowers)if(borrower.owner==owner){borrower.released=true;pendingReleases=true;}
}
bool collectTextureResources(){Exclusion guard;return collect();}
std::size_t textureResourceBytes(){Exclusion guard;return used;}
const char* textureResourceFailure(){return failure.load(std::memory_order_relaxed);}
}
