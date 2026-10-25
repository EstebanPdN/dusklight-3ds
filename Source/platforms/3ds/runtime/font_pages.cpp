#include "font_pages.hpp"
#include "gx_state.hpp"
#include "gpu_sync.hpp"
#include "../gx_texture_pica.hpp"
#include "texture_gx.hpp"
#include <dolphin/os.h>
#include <array>
#include <cstdlib>
#include <cstring>
namespace dusk::native {
namespace {
struct Page {const void* image{};std::uint8_t* bti{};std::size_t bytes{};gx::TextureHandle handle{};unsigned references{};};
struct Borrower {const void* font{};Page* page{};};
std::array<Page,64> pages{};
std::array<Borrower,128> borrowers{};
std::size_t used{};
constexpr std::size_t budget=2*1024*1024;
unsigned half(const std::uint8_t* p){return (unsigned(p[0])<<8)|p[1];}
std::uint32_t word(const std::uint8_t* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
bool owner(){return platformGpuOwner()&&platformGpuOwner()==OSGetCurrentThread();}
}
bool registerFontPage(const void* font,std::span<const std::uint8_t> gly1,unsigned page){
    if(!font||!owner()||gly1.size()<32||word(gly1.data())!=0x474c5931||word(gly1.data()+4)!=gly1.size())return false;
    const auto width=half(gly1.data()+26),height=half(gly1.data()+28),format=half(gly1.data()+20);
    const std::size_t size=word(gly1.data()+16);
    if(!size||!width||!height||width>1024||height>1024||format>6||size>gly1.size()-32||page>=(gly1.size()-32)/size)return false;
    const auto* image=gly1.data()+32+page*size;
    Borrower* borrower=nullptr;
    for(auto& entry:borrowers){
        if(entry.font==font&&entry.page->image==image)return true;
        if(!entry.font&&!borrower)borrower=&entry;
    }
    if(!borrower)return false;
    Page* slot=nullptr;
    for(auto& entry:pages){
        if(entry.image==image){
            if(entry.bytes!=size+32||half(entry.bti+2)!=width||half(entry.bti+4)!=height||entry.bti[0]!=format||std::memcmp(entry.bti+32,image,size))return false;
            *borrower={font,&entry};++entry.references;return true;
        }
        if(!entry.image&&!slot)slot=&entry;
    }
    if(!slot||size+32>budget-used)return false;
    auto* bti=static_cast<std::uint8_t*>(std::calloc(1,size+32));if(!bti)return false;
    bti[0]=format;bti[2]=width>>8;bti[3]=width;bti[4]=height>>8;bti[5]=height;
    bti[20]=bti[21]=1;bti[24]=1;bti[31]=32;
    std::memcpy(bti+32,image,size);
    NativeTexturePlan plan;gx::TextureHandle handle{};
    // This owns a private copy, never a mutable cache-font backing page.
    // Replacement retires the handle before its copied bytes are released.
    if(!inspectNativeTexture({bti,size+32},plan)||plan.sourceBytes!=size||!gx::registerTexture({bti,size+32},handle,true)){
        std::free(bti);return false;
    }
    if(!gx::aliasTextureImage(handle,image)){gx::unregisterTexture(handle);std::free(bti);return false;}
    *slot={image,bti,size+32,handle,1};*borrower={font,slot};used+=size+32;return true;
}
bool releaseFontPages(const void* font){
    bool found=false;for(const auto& borrower:borrowers)found|=borrower.font==font;
    if(!found)return true;
    if(!owner()||platformCompleteGpu(500000000)!=GpuCompletion::complete)return false;
    for(auto& borrower:borrowers)if(borrower.font==font){
        auto& page=*borrower.page;borrower={};
        if(--page.references)continue;
        if(!releaseGxTexture(page.handle))return false;
        if(!gx::unregisterTexture(page.handle))return false;
        used-=page.bytes;std::free(page.bti);page={};
    }
    return true;
}
std::size_t fontPageBytes(){return used;}
}
