#include "../runtime/gx_state.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
namespace gx=dusk::native::gx;
int main(){
    static std::array<std::array<std::uint8_t,64>,1024> images{};
    static std::array<std::array<std::uint8_t,64>,2048> objects{};
    std::array<gx::TextureHandle,1024> handles{};
    for(unsigned i=0;i<images.size();++i){
        auto& b=images[i];b[3]=8;b[5]=8;b[20]=1;b[21]=1;b[24]=1;b[31]=32;b[32]=i;
        assert(gx::registerTexture(b,handles[i]));
    }
    auto init=[&](unsigned n,unsigned image){
        gx::initTextureObject(objects[n].data(),images[image].data()+32,8,8,0,0,0,0,false,0);
    };
    for(unsigned n=0;n<objects.size();++n)init(n,n%images.size());
    gx::Texture actual;
    unsigned checks=0;
    // Full tables exceed both hint capacities; odd strides permute all entries.
    for(unsigned pass=0;pass<8;++pass)for(unsigned i=0;i<objects.size();++i){
        const unsigned n=(i*701+pass*107)%objects.size(),image=n%images.size();
        init(n,image);
        std::uint32_t token{};
        assert(gx::textureCommandToken(images[image].data()+32,token)&&token==(0x800000|handles[image]));
        assert(gx::queryTextureObject(objects[n].data(),actual));
        assert(actual.handle==handles[image]&&actual.image.data()==images[image].data()+32);
        assert(gx::textureFromCommandToken(0x800000|handles[image],actual));
        assert(actual.handle==handles[image]&&actual.bti.data()==images[image].data());
        gx::loadTextureObject(objects[n].data(),pass);
        assert(gx::textureBinding(pass,actual)&&actual.handle==handles[image]);
        ++checks;
    }
    // Destroyed-address hints, slot reuse, and stale binding generations.
    for(unsigned n=0;n<objects.size();n+=3){
        gx::loadTextureObject(objects[n].data(),0);
        gx::destroyTextureObject(objects[n].data());
        assert(!gx::queryTextureObject(objects[n].data(),actual));
        assert(!gx::textureBinding(0,actual));
        init(n,(n+17)%images.size());
        assert(!gx::textureBinding(0,actual));
        assert(gx::queryTextureObject(objects[n].data(),actual));
        assert(actual.handle==handles[(n+17)%images.size()]);++checks;
    }
    // Retired handles must fail even when their former slots are live again.
    for(unsigned i=0;i<images.size();i+=2){
        const auto stale=handles[i];assert(gx::unregisterTexture(stale));
        assert(!gx::textureFromCommandToken(0x800000|stale,actual));
        assert(gx::registerTexture(images[i],handles[i])&&handles[i]!=stale);
        assert(!gx::textureFromCommandToken(0x800000|stale,actual));
        assert(gx::textureFromCommandToken(0x800000|handles[i],actual));
        assert(actual.bti.data()==images[i].data());++checks;
    }
    gx::resetState();
    for(unsigned i=0;i<images.size();++i){
        assert(gx::textureFromCommandToken(0x800000|handles[i],actual));
        assert(actual.bti.data()==images[i].data());
    }
    for(auto& o:objects)gx::destroyTextureObject(o.data());
    for(auto h:handles)assert(gx::unregisterTexture(h));
    // A later registration can introduce an earlier first match for an image
    // identity previously known only as an alias. A hint must not change the
    // registry's established ordering in that case.
    gx::TextureHandle a,b,c;
    assert(gx::registerTexture(images[0],a));
    assert(gx::registerTexture(images[1],b));
    assert(gx::aliasTextureImage(b,images[2].data()+32));
    auto tokenMatches=[&](gx::TextureHandle expected){std::uint32_t token{};
        const bool found=gx::textureCommandToken(images[2].data()+32,token);
        assert(found==bool(expected));assert(token==(expected?0x800000|expected:0));
    };
    tokenMatches(b);
    init(0,2);assert(gx::queryTextureObject(objects[0].data(),actual)&&actual.handle==b);
    assert(gx::unregisterTexture(a));
    assert(gx::registerTexture(images[2],c));
    tokenMatches(c);
    init(0,2);assert(gx::queryTextureObject(objects[0].data(),actual)&&actual.handle==c);
    assert(gx::unregisterTexture(c));
    tokenMatches(b);
    init(0,2);assert(gx::queryTextureObject(objects[0].data(),actual)&&actual.handle==b);
    assert(gx::aliasTextureImage(b,images[3].data()+32));
    tokenMatches(0);
    init(0,2);assert(gx::queryTextureObject(objects[0].data(),actual)&&!actual.handle);
    init(0,3);assert(gx::queryTextureObject(objects[0].data(),actual)&&actual.handle==b);
    gx::initTextureObject(objects[0].data(),images[3].data()+32,4,8,0,0,0,0,false,0);
    assert(gx::queryTextureObject(objects[0].data(),actual)&&!actual.handle&&actual.width==4);
    gx::destroyTextureObject(objects[0].data());assert(gx::unregisterTexture(b));
    assert(!gx::queryTextureObject(nullptr,actual));
    assert(!gx::textureFromCommandToken(0x800000,actual));
    // Forward-only fixture reaches the production token boundary without
    // millions of registrations. Reinitializations cross the old shared limit.
    constexpr gx::TextureHandle lastId=0x7fffff;
    assert(gx::advanceTextureIdentityCountersForTest(lastId-1,lastId-8));
    assert(!gx::advanceTextureIdentityCountersForTest(lastId-2,lastId-8));
    for(unsigned n=0;n<32;++n){init(0,0);gx::destroyTextureObject(objects[0].data());}
    gx::TextureHandle edgeImage{},edgePalette{};
    assert(gx::registerTexture(images[0],edgeImage)&&edgeImage==lastId-1);
    std::array<std::uint8_t,96> indexed{};
    indexed[0]=8;indexed[3]=indexed[5]=8;indexed[8]=1;indexed[9]=1;
    indexed[11]=16;indexed[15]=64;indexed[20]=indexed[21]=1;indexed[24]=1;indexed[31]=32;
    assert(gx::registerTexture(indexed,edgePalette)&&edgePalette==lastId);
    std::uint32_t imageToken{},paletteToken{};
    assert(gx::textureCommandToken(images[0].data()+32,imageToken)&&imageToken==0xfffffe);
    assert(gx::paletteCommandToken(indexed.data()+64,paletteToken)&&paletteToken==0xffffff);
    assert(gx::textureFromCommandToken(imageToken,actual)&&actual.handle==edgeImage);
    assert(gx::textureFromCommandToken(paletteToken,actual)&&actual.handle==edgePalette);
    gx::initTlutObject(objects[1].data(),indexed.data()+64,1,16);
    gx::loadTlutObject(objects[1].data(),0);
    gx::initTextureObject(objects[0].data(),indexed.data()+32,8,8,8,0,0,0,true,0);
    gx::loadTextureObject(objects[0].data(),0);
    assert(gx::textureBinding(0,actual)&&actual.handle==edgePalette);
    // Reinitializing the same address invalidates the old binding generation.
    gx::initTextureObject(objects[0].data(),indexed.data()+32,8,8,8,0,0,0,true,0);
    assert(!gx::textureBinding(0,actual));
    gx::loadTextureObject(objects[0].data(),0);assert(gx::textureBinding(0,actual));
    assert(gx::unregisterTexture(edgePalette));
    assert(!gx::textureBinding(0,actual)&&!gx::textureFromCommandToken(paletteToken,actual));
    gx::resetState();
    gx::TextureHandle refused=123;
    assert(!gx::registerTexture(indexed,refused)&&!refused);
    assert(!gx::textureCommandToken(indexed.data()+32,paletteToken)&&!paletteToken);
    assert(!gx::paletteCommandToken(indexed.data()+64,paletteToken)&&!paletteToken);
    assert(gx::textureFromCommandToken(imageToken,actual)&&actual.handle==edgeImage);
    // The independent 64-bit generation counter also fails closed on wrap.
    assert(gx::advanceTextureIdentityCountersForTest(lastId+1,std::numeric_limits<std::uint64_t>::max()));
    init(0,0);gx::loadTextureObject(objects[0].data(),0);assert(gx::textureBinding(0,actual));
    init(0,0);assert(!gx::queryTextureObject(objects[0].data(),actual)&&!gx::textureBinding(0,actual));
    assert(gx::textureFromCommandToken(imageToken,actual)&&actual.handle==edgeImage);
    assert(gx::unregisterTexture(edgeImage));
    // Free slots/reset do not recycle stale image or TLUT display-list IDs.
    assert(!gx::registerTexture(images[1],refused)&&!refused);
    assert(!gx::textureFromCommandToken(imageToken,actual));
    assert(!gx::textureFromCommandToken(0xffffff,actual));
    for(std::size_t i=0;i<gx::textureRegistrationCapacity();++i)assert(!gx::registeredTextureAt(i,actual));
    gx::destroyTextureObject(objects[0].data());
    std::puts("PASS separate object generations and 23-bit image/TLUT boundary: atomic exhaustion, stale display-list IDs, bindings and slot reuse");
    std::printf("GX live lookup hints: %u collision/reuse/lifetime checks, full 1024/2048 tables passed\n",checks);
}
