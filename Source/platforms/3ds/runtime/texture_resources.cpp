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
// Protected by Exclusion as well.
unsigned long conflicts{},rangeReleases{},rangeOwners{};
// Owner index, protected by Exclusion: the records of each owner hash are
// chained (index + 1, 0 ends a chain). A JUTTexture releases its records at
// every image change (storeTIMG), and J2D counters change several images per
// logic tick: the walk of all 2,048 records under the global exclusion cost
// 58 us per release in Azahar (Alpha 13, the goat minigame timer). A record is
// chained when retain fills a free one and stays chained while it holds an
// owner; collect, the only place that empties records, rebuilds the chains.
std::array<std::uint16_t,1024> ownerHeads{};
std::array<std::uint16_t,2048> ownerNext{};
static_assert(std::tuple_size<decltype(ownerNext)>::value==std::tuple_size<decltype(borrowers)>::value);
std::size_t ownerBucket(const void* owner){
    const auto a=reinterpret_cast<std::uintptr_t>(owner);
    return ((a>>3)^(a>>13))&(ownerHeads.size()-1);
}
void chainBorrower(Borrower& borrower){
    const auto index=std::size_t(&borrower-borrowers.data());
    auto& head=ownerHeads[ownerBucket(borrower.owner)];
    ownerNext[index]=head;head=std::uint16_t(index+1);
}
void rebuildOwnerChains(){
    ownerHeads.fill(0);
    for(auto& borrower:borrowers)if(borrower.owner)chainBorrower(borrower);
}
// A J2D pane or a J3D material loads its textures in every picture, and each
// load asks for the same retention again: a scan of the image table, the
// archives and up to 2,048 owner records (10 % of the file select picture in
// the Alpha 8 profile). The memo answers a repeated question at once. An
// entry stands for "this owner holds a live record of the image this BTI
// header names": it is valid while no record, image or archive was released
// (retainEpoch) and the header fields that name the image are unchanged.
struct RetainMemo {const void* owner{};const void* bti{};std::uint64_t handle{};std::uint32_t head{},image{},epoch{};};
std::array<RetainMemo,1024> retainMemos{};
std::uint32_t retainEpoch{1};
unsigned long retainMemoHits{};
std::uint32_t headerWord(const void* bti,unsigned at){std::uint32_t w;std::memcpy(&w,static_cast<const std::uint8_t*>(bti)+at,4);return w;}
RetainMemo& retainMemoSlot(const void* owner,const void* bti){
    const auto a=reinterpret_cast<std::uintptr_t>(owner),b=reinterpret_cast<std::uintptr_t>(bti);
    return retainMemos[((a>>3)^(a>>13)^(b>>2)^(b>>12))&(retainMemos.size()-1)];
}
bool retainMemoHit(const void* owner,const void* bti,std::uint64_t handle){
    const auto& m=retainMemoSlot(owner,bti);
    if(pendingReleases||m.epoch!=retainEpoch||m.owner!=owner||m.bti!=bti||m.handle!=handle||
       m.head!=headerWord(bti,0)||m.image!=headerWord(bti,28))return false;
    ++retainMemoHits;return true;
}
void retainMemoStore(const void* owner,const void* bti,std::uint64_t handle){
    retainMemoSlot(owner,bti)={owner,bti,handle,headerWord(bti,0),headerWord(bti,28),retainEpoch};
}
constexpr std::size_t budget=4*1024*1024;
std::atomic<const char*> failure{"none"};
bool reject(const char* reason){failure.store(reason,std::memory_order_relaxed);return false;}
bool renderOwner(){return platformGpuOwner()&&platformGpuOwner()==OSGetCurrentThread();}
std::uint32_t word(const std::uint8_t* p){return(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
bool collect(){
    if(!pendingReleases)return true;
    if(!renderOwner()||platformCompleteGpu(500000000)!=GpuCompletion::complete)return reject("texture retirement GPU completion");
    ++retainEpoch;
    for(auto& borrower:borrowers)if(borrower.released){
        auto& image=*borrower.image;
        if(image.references>1){--image.references;borrower={};continue;}
        if(!releaseGxTexture(image.handle)||!gx::unregisterTexture(image.handle)){
            rebuildOwnerChains();return reject("texture retirement handle");
        }
        borrower={};used-=image.bytes;std::free(image.bti);image={};
    }
    rebuildOwnerChains();
    pendingReleases=false;return true;
}
// Alpha 15 (gx-overlap.txt 1 and 2): an owner that already holds the image
// is answered before the render owner is asked. With the second-core worker
// on, asking it means drawing everything queued first: a J2D or J3D texture
// outside a mounted archive (no memo) asked in every picture and handed 37 to
// 41 queued shapes a picture to the game thread (Azahar village, Alpha 14 m1,
// sync site retain, 12-17 ms a picture). The tables read before the question
// are this thread's own; collect(), a new record and a registration still
// need the render owner.
bool heldWithoutOwner{};
bool retain(const void* owner,std::span<const std::uint8_t> bti,const void* identity,const void* paletteIdentity=nullptr){
    if(!owner)return reject("texture resource render owner");
    if(!heldWithoutOwner||pendingReleases){
        if(!renderOwner())return reject("texture resource render owner");
        if(!collect())return false;
    }
    BtiInfo info{};NativeTexturePlan plan{};
    if(inspectBti({bti.data(),bti.size()},1024,info)!=ResourceStatus::ready||
       !inspectNativeTexture(bti,plan)||plan.sourceBytes!=info.image.size)return reject("texture resource BTI bounds/format");
    if(!identity)identity=info.image.data;
    if(!paletteIdentity)paletteIdentity=info.palette.data;
    Borrower* borrower=nullptr;Image* slot=nullptr;
    for(auto& entry:borrowers){
        if(entry.owner==owner&&entry.image->identity==identity)return true;
        if(!entry.owner&&!borrower)borrower=&entry;
    }
    if(heldWithoutOwner&&!renderOwner())return reject("texture resource render owner");
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
                // The caller retries every frame: 1,578 lines in one Alpha 6
                // zone. The first few and then one in 256, with the total.
                ++conflicts;
                if(conflicts<=8||conflicts%256==0)
                    OSReport("Native immutable texture conflict #%lu: old fmt=%u %ux%u mips=%u bytes=%lu; new fmt=%u %ux%u mips=%u bytes=%lu\n",conflicts,
                        unsigned(entry.bti[0]),unsigned(entry.bti[2])*256+entry.bti[3],unsigned(entry.bti[4])*256+entry.bti[5],unsigned(entry.bti[24]),static_cast<unsigned long>(entry.bytes-32),
                        unsigned(bti[0]),unsigned(info.width),unsigned(info.height),unsigned(info.mipCount),static_cast<unsigned long>(info.image.size));
                return reject("texture identity reused while retained");
            }
            if(info.palette.size&&!gx::aliasTexturePalette(entry.handle,paletteIdentity))return reject("texture palette alias");
            *borrower={owner,&entry,false};chainBorrower(*borrower);++entry.references;return true;
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
    if(info.palette.size&&!gx::aliasTexturePalette(handle,paletteIdentity)){gx::unregisterTexture(handle);std::free(copy);return reject("texture palette alias");}
    *slot={identity,copy,bytes,handle,1};*borrower={owner,slot,false};chainBorrower(*borrower);used+=bytes;return true;
}
bool retainRaw(const void* owner,std::span<const std::uint8_t> image,unsigned width,unsigned height,unsigned format,
               std::span<const std::uint8_t> palette={},unsigned paletteFormat=0){
    if(!owner||image.empty()||image.size()>budget-32-palette.size()||!width||!height||width>1024||height>1024)
        return reject("raw texture extent/format");
    const auto paletteOffset=32+image.size();
    const auto bytes=paletteOffset+palette.size();
    auto* bti=static_cast<std::uint8_t*>(std::calloc(1,bytes));
    if(!bti)return reject("raw texture metadata allocation");
    bti[0]=format;bti[1]=1;bti[2]=width>>8;bti[3]=width;bti[4]=height>>8;bti[5]=height;
    bti[20]=bti[21]=1;bti[24]=1;bti[31]=32;
    std::memcpy(bti+32,image.data(),image.size());
    if(!palette.empty()){
        const unsigned entries=palette.size()/2;
        bti[8]=1;bti[9]=paletteFormat;bti[10]=entries>>8;bti[11]=entries;
        for(unsigned n=0;n<4;++n)bti[12+n]=paletteOffset>>(24-8*n);
        std::memcpy(bti+paletteOffset,palette.data(),palette.size());
    }
    NativeTexturePlan plan;
    bool retained;
    if(inspectNativeTexture({bti,bytes},plan)&&plan.sourceBytes==image.size()){
        Exclusion guard;
        retained=retain(owner,{bti,bytes},image.data(),palette.data());
    }else retained=reject("raw texture pixel extent mismatch");
    std::free(bti);return retained;
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
        entry={};++retainEpoch;
    }
}
bool retainTextureResource(const void* owner,std::span<const std::uint8_t> bti,const void* identity){Exclusion guard;return retain(owner,bti,identity);}
bool retainRawTextureResource(const void* owner,std::span<const std::uint8_t> image,unsigned width,unsigned height,unsigned format){
    if(format>6&&format!=14)return reject("raw texture extent/format");
    return retainRaw(owner,image,width,height,format);
}
bool retainRawCi8TextureResource(const void* owner,std::span<const std::uint8_t> image,unsigned width,unsigned height,
                                std::span<const std::uint8_t> palette,unsigned paletteFormat){
    if(palette.empty()||palette.size()>512||palette.size()%2||paletteFormat>2)
        return reject("raw CI8 texture palette extent/format");
    for(const auto index:image)if(index>=palette.size()/2)return reject("raw CI8 texture palette index");
    return retainRaw(owner,image,width,height,9,palette,paletteFormat);
}
bool retainArchiveTexture(const void* owner,const void* bti,std::uint64_t externalHandle){
    Exclusion guard;
    if(owner&&bti&&retainMemoHit(owner,bti,externalHandle))return true;
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
        if(address>=begin&&address-begin<archive.bytes.size()){
            // Only this answer is kept: the others above and below depend on
            // texture registrations that end without moving retainEpoch.
            if(!retain(owner,archive.bytes.subspan(address-begin),nullptr))return false;
            if(address-begin+32<=archive.bytes.size())retainMemoStore(owner,bti,externalHandle);
            return true;
        }
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
    // The same memo as retainArchiveTexture, under an owner of its own: the
    // archive that holds this header keeps the image for as long as it is
    // registered. Weather and effect packets ask in every picture; without
    // the memo each call needs the render owner, which with the second-core
    // worker on means drawing everything queued first (gx_worker.hpp).
    static const char lifetimeOwner{};
    if(bti&&retainMemoHit(&lifetimeOwner,bti,externalHandle))return true;
    const auto address=reinterpret_cast<std::uintptr_t>(bti);
    for(const auto& archive:archives)if(archive.owner){
        const auto begin=reinterpret_cast<std::uintptr_t>(archive.bytes.data());
        if(address>=begin&&address-begin<archive.bytes.size()){
            if(!retain(archive.owner,archive.bytes.subspan(address-begin),nullptr))return false;
            if(address-begin+32<=archive.bytes.size())retainMemoStore(&lifetimeOwner,bti,externalHandle);
            return true;
        }
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
    // Only this owner's chain. An owner without a live record changes
    // nothing: no memo entry of it can be valid (a record ends only through a
    // release, which moved the epoch), so the epoch stays and the other
    // owners' memo entries stay usable.
    bool released=false;
    for(auto at=ownerHeads[ownerBucket(owner)];at;at=ownerNext[at-1]){
        auto& borrower=borrowers[at-1];
        if(borrower.owner==owner&&!borrower.released){borrower.released=true;released=true;}
    }
    if(released){pendingReleases=true;++retainEpoch;}
}
std::size_t releaseTextureResourcesInRange(const void* begin,const void* end){
    const auto low=reinterpret_cast<std::uintptr_t>(begin),high=reinterpret_cast<std::uintptr_t>(end);
    if(!begin||high<=low)return 0;
    const auto inside=[low,high](const void* owner){
        const auto at=reinterpret_cast<std::uintptr_t>(owner);return at>=low&&at<high;};
    Exclusion guard;
    // An archive whose owner object dies with the range is unregistered as
    // its destructor would have done; its lifetime records are in the range
    // too and are released below.
    for(auto& entry:archives)if(entry.owner&&inside(entry.owner))entry={};
    std::size_t count{};
    for(auto& borrower:borrowers)if(borrower.owner&&!borrower.released&&inside(borrower.owner)){
        borrower.released=true;pendingReleases=true;++count;
    }
    ++rangeReleases;rangeOwners+=count;++retainEpoch;
    return count;
}
bool collectTextureResources(){Exclusion guard;return collect();}
void setTextureRetainWithoutOwner(bool on){Exclusion guard;heldWithoutOwner=on;}
std::size_t textureResourceBytes(){Exclusion guard;return used;}
const char* textureResourceFailure(){return failure.load(std::memory_order_relaxed);}
TextureResourceStatistics textureResourceStatistics(){
    Exclusion guard;TextureResourceStatistics s{conflicts,rangeReleases,rangeOwners,0,0,used,retainMemoHits};
    for(const auto& value:images)s.images+=value.identity!=nullptr;
    for(const auto& value:borrowers)s.owners+=value.owner!=nullptr;
    return s;
}
}
