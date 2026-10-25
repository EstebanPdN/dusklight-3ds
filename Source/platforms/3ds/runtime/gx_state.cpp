#include "gx_state.hpp"
#include "resource_archive.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>

namespace dusk::native::gx {
namespace {
constexpr unsigned positionAttribute = 9, colorAttribute = 11, textureAttribute = 13;
struct Format { unsigned count, type; std::uint8_t fraction; bool configured; };
struct Tev {
    unsigned operation, coordinate, texture, channel; bool direct;
    unsigned color[4]{}, alpha[4]{}, colorOp[5]{}, alphaOp[5]{};
    unsigned konstColor = 0, konstAlpha = 0;
    bool explicitProgram = false;
};
struct Channel { unsigned enabled, ambient, material; std::uint32_t lights; unsigned diffuse, attenuation; bool configured; };
struct TexGen { unsigned function, source, normalize; std::uint32_t matrix, postMatrix; };
struct Registration { Texture texture{};const void* alias{}; };
struct PaletteAlias {const void* identity{};TextureHandle handle{};};
std::array<PaletteAlias,2048> paletteAliases{};
struct TextureObject {
    void* address = nullptr;
    std::uint64_t generation = 0;
    Texture texture{};
    const void* imageIdentity{};
    bool supported = false, baseSupported = false, metadataValid = false, samplerValid = true;
    bool indexed{};
    unsigned tlut{};
};
struct TlutObject {void* address{};const void* palette{};unsigned format{},entries{};};
std::array<TlutObject,512> tlutObjects{};
std::array<TextureHandle,20> loadedTluts{};
struct Binding { void* address = nullptr; std::uint64_t generation = 0; Texture texture{}; bool supported = false; };
#ifdef DUSK_GAME_STARTUP
std::array<Registration,1024> registrations{};
std::array<TextureObject,2048> textureObjects{};
#else
std::array<Registration,64> registrations{};
std::array<TextureObject,128> textureObjects{};
#endif
// Display-list image IDs are monotonic for the process lifetime. Retired IDs
// must never resolve to another image, even when a registration slot is reused.
constexpr TextureHandle maxTextureTokenId = 0x7fffff;
TextureHandle nextTextureId = 1;
std::uint64_t nextObjectGeneration = 1;
std::array<std::uint16_t,512> imageHints{};
std::array<std::uint16_t,512> tokenHints{};
void clearImageHints(){imageHints.fill(0);tokenHints.fill(0);}
struct State {
    DrawSink sink = nullptr;
    void* sinkUser = nullptr;
    Statistics stats{};
    std::array<unsigned, 26> descriptors{};
    Format formats[8][26]{};
    float projection[16]{}, matrices[10][12]{}, textureMatrices[11][12]{};
    bool projectionReady = false, positionReady[10]{}, viewportReady = false, scissorReady = false;
    unsigned projectionType = 0, currentMatrix = 0;
    Viewport viewport{};
    Scissor scissor{};
    unsigned blendMode = 0, blendSource = 0, blendDestination = 0, blendLogic = 0;
    unsigned depthEnabled = 0, depthCompare = 0, depthWrite = 0;
    unsigned compare0 = 0, compare1 = 0, alphaOp = 0, cull = 0, zLocation = 0;
    std::uint8_t reference0 = 0, reference1 = 0, lineWidth = 0;
    unsigned lineOffsets = 0, indirectCount = 0, tevCount = 0, channelCount = 0, texGenCount = 0;
    std::array<Tev, 16> tev{};
    std::array<Channel, 4> channels{};
    std::array<TexGen, 8> texgens{};
    std::array<Binding,8> bindings{};
    Rgba registers[4]{}, konst[4]{};
    bool active = false, dropped = false, configurationError = false;
    unsigned expected = 0, received = 0, nextAttribute = positionAttribute, format = 0;
    Primitive primitive{};
};
State state;
thread_local void (*stateResetObserver)()=nullptr;
// Keep capture storage separate: resetState must never materialize a 96 KiB
// State temporary on the 3DS thread stack. Completed vertices are overwritten.
std::array<Vertex, maxVertices> vertices{};
void error(Error code, const char* call) {
    ++state.stats.errors;
    if (state.stats.firstError == Error::none) {
        state.stats.firstError = code; state.stats.firstErrorCall = call;
    }
}
void drop(Error code, const char* call) {
    if (!state.dropped) { error(code, call); state.dropped = true; }
}
bool setter(const char* call) {
    if (state.active) { drop(Error::vertexProtocol, call); return false; }
    return true;
}
void invalid(const char* call) { error(Error::invalidParameter, call); state.configurationError = true; }
bool finite(const float* input, std::size_t count) {
    if (!input) return false;
    for (std::size_t i = 0; i < count; ++i) if (!std::isfinite(input[i])) return false;
    return true;
}
Registration* registration(TextureHandle handle) {
    if(!handle)return nullptr;
    // Hints point only into our fixed table. Validate the complete live handle
    // before using a hit: retirement/reuse never dereferences an old resource.
    static std::array<std::uint16_t,512> hints{};
    auto& hint=hints[(handle^(handle>>32))&(hints.size()-1)];
    if(hint&&registrations[hint-1].texture.handle==handle)return &registrations[hint-1];
    for (auto& entry:registrations) if (entry.texture.handle==handle) {
        hint=std::uint16_t(&entry-registrations.data()+1);return &entry;
    }
    return nullptr;
}
TextureObject* textureObject(void* address) {
    if(!address)return nullptr;
    static std::array<std::uint16_t,1024> hints{};
    auto key=reinterpret_cast<std::uintptr_t>(address);
    // GX objects are commonly aligned and embedded at regular model offsets.
    // Fold higher address bits before indexing to avoid discarding those bits.
    key^=key>>11;key^=key>>21;
    auto& hint=hints[(key>>2)&(hints.size()-1)];
    if(hint&&textureObjects[hint-1].address==address)return &textureObjects[hint-1];
    for (auto& entry:textureObjects) if (entry.address==address) {
        hint=std::uint16_t(&entry-textureObjects.data()+1);return &entry;
    }
    return nullptr;
}
const Registration* imageRegistration(const void* image,unsigned width,unsigned height,unsigned format){
    auto key=reinterpret_cast<std::uintptr_t>(image);
    key^=key>>11;key^=key>>21;
    auto& hint=imageHints[((key>>2)^width^(height<<3)^format)&(imageHints.size()-1)];
    auto matches=[&](const Registration& entry){const auto& t=entry.texture;
        return t.handle&&(t.image.data()==image||entry.alias==image)&&
            t.width==width&&t.height==height&&t.format==format;
    };
    // Mutations clear these hints to preserve the original first-match order,
    // including an alias which overlaps a subsequently registered identity.
    if(hint&&matches(registrations[hint-1]))return &registrations[hint-1];
    for(const auto& entry:registrations)if(matches(entry)){
        hint=std::uint16_t(&entry-registrations.data()+1);return &entry;
    }
    return nullptr;
}
bool args(const unsigned* actual, unsigned a, unsigned b, unsigned c, unsigned d) {
    return actual[0]==a && actual[1]==b && actual[2]==c && actual[3]==d;
}
bool basicOperator(const unsigned* op) { return op[0]==0 && op[1]==0 && op[2]==0 && op[3]==1 && op[4]==0; }
bool validateMaterial() {
    if (state.indirectCount || state.channelCount!=1) return false;
    for (unsigned i=0;i<2;++i)
        if (!state.channels[i].configured || state.channels[i].enabled || state.channels[i].material!=1) return false;
    const auto& first=state.tev[0];
    if (state.texGenCount==0) {
        return state.tevCount==1 && first.direct && !first.explicitProgram && first.operation==4 &&
               first.coordinate==255 && first.texture==255 && first.channel==4;
    }
    if (state.texGenCount!=1 || state.tevCount<1 || state.tevCount>2 ||
        state.descriptors[textureAttribute]!=1) return false;
    const auto& generator=state.texgens[0];
    if (generator.function!=1 || generator.source!=4 || generator.normalize ||
        generator.matrix!=60 || generator.postMatrix!=125) return false;
    const auto& bound=state.bindings[0];
    const auto* object=textureObject(bound.address);
    if (!bound.supported || !object || object->generation!=bound.generation || !registration(bound.texture.handle)) return false;
    if (!first.direct || !first.explicitProgram || first.coordinate!=0 || first.texture!=0 || first.channel!=255 ||
        !args(first.color,8,15,15,15) ||
        !(args(first.alpha,4,7,7,7) || args(first.alpha,3,7,7,7)) ||
        !basicOperator(first.colorOp) || !basicOperator(first.alphaOp)) return false;
    for (unsigned i=1;i<state.tevCount;++i) {
        const auto& stage=state.tev[i];
        if (!stage.direct || !stage.explicitProgram || stage.coordinate!=255 || stage.texture!=255 ||
            !basicOperator(stage.colorOp) || !basicOperator(stage.alphaOp)) return false;
        const bool tint=stage.channel==255 && args(stage.color,2,4,0,15) && args(stage.alpha,1,2,0,7);
        const bool raster=stage.channel==4 && args(stage.color,15,0,10,15) && args(stage.alpha,7,0,5,7);
        if (!tint && !raster) return false;
    }
    return true;
}
bool validate() {
    if (state.configurationError || !state.projectionReady || !state.viewportReady || !state.scissorReady ||
        state.currentMatrix > 27 || state.currentMatrix % 3 || !state.positionReady[state.currentMatrix / 3]) {
        drop(Error::invalidParameter, "GXBegin/state"); return false;
    }
    // Validate only the active program; original J2D deliberately leaves unused
    // texture formats and TEV slots configured between draws.
    if (!validateMaterial()) {
        drop(Error::unsupportedState, "GXBegin/material"); return false;
    }
    if (state.blendMode > 3 || state.blendSource > 7 || state.blendDestination > 7 || state.blendLogic > 15 ||
        state.depthEnabled > 1 || state.depthCompare > 7 || state.depthWrite > 1 || state.compare0 > 7 ||
        state.compare1 > 7 || state.alphaOp > 3 || state.cull > 3 || state.zLocation > 1) {
        drop(Error::invalidParameter, "GXBegin/raster"); return false;
    }
    if ((state.primitive == Primitive::lines || state.primitive == Primitive::lineStrip) &&
        (!state.lineWidth || state.lineOffsets)) {
        drop(Error::unsupportedState, "GXBegin/line"); return false;
    }
    for (unsigned attribute = 0; attribute < state.descriptors.size(); ++attribute) {
        if (!state.descriptors[attribute]) continue;
        if (state.descriptors[attribute] != 1 || (attribute != positionAttribute && attribute != colorAttribute &&
                                                 attribute != textureAttribute)) {
            drop(Error::unsupportedState, "GXBegin/descriptor"); return false;
        }
        const auto& format = state.formats[state.format][attribute];
        const bool floatValue=format.type==4 && format.fraction==0;
        const bool shortUv=attribute==textureAttribute && format.type==3 && format.fraction==8;
        const bool colorValue=attribute==colorAttribute && format.type==5 && format.fraction==0;
        if (!format.configured || format.count != 1 ||
            !(attribute==colorAttribute ? colorValue : floatValue || shortUv)) {
            drop(Error::unsupportedState, "GXBegin/format"); return false;
        }
    }
    if (state.descriptors[positionAttribute] != 1 || state.descriptors[colorAttribute] != 1) {
        drop(Error::unsupportedState, "GXBegin/required-attributes"); return false;
    }
    return true;
}
bool vertex(unsigned attribute, const char* call) {
    if (!state.active) { error(Error::vertexProtocol, call); return false; }
    if (state.dropped) return false;
    if (state.received >= state.expected || state.received >= maxVertices) {
        drop(Error::vertexOverflow, call); return false;
    }
    if (state.nextAttribute != attribute) { drop(Error::vertexProtocol, call); return false; }
    return true;
}
void advance(unsigned attribute) {
    if (attribute == positionAttribute) state.nextAttribute = colorAttribute;
    else if (attribute == colorAttribute && state.descriptors[textureAttribute]) state.nextAttribute = textureAttribute;
    else { ++state.received; state.nextAttribute = positionAttribute; }
}
}
void setDrawSink(DrawSink sink, void* user) {
    if (!setter("setDrawSink")) return;
    state.sink = sink; state.sinkUser = user;
}
void setStateResetObserver(void (*observer)()){stateResetObserver=observer;}
void resetState() {
    if(stateResetObserver)stateResetObserver();
    const auto sink = state.sink; auto* user = state.sinkUser;
    // Placement construction resets this trivially destructible object in place,
    // without either heap allocation or a full State temporary on the thread stack.
    new (&state) State{}; state.sink = sink; state.sinkUser = user;
}
#ifdef DUSK_GX_LOOKUP_TEST
// Test-only, forward-only advancement: cannot resurrect a retired token or
// generation. This hook is absent from production builds.
bool advanceTextureIdentityCountersForTest(TextureHandle imageId,std::uint64_t generation){
    if(state.active||imageId<nextTextureId||generation<nextObjectGeneration||
       !nextObjectGeneration||imageId>maxTextureTokenId+1||!generation)return false;
    nextTextureId=imageId;nextObjectGeneration=generation;return true;
}
#endif
bool registerTexture(std::span<const std::uint8_t> bti, TextureHandle& handle,bool immutablePixels) {
    handle=0;
    if (!setter("registerTexture")) return false;
    BtiInfo info{};
    if (inspectBti({bti.data(),bti.size()},1024,info)!=ResourceStatus::ready || info.image.size>4*1024*1024) {
        error(Error::invalidParameter,"registerTexture/BTI"); return false;
    }
    const bool indexed=info.format==8||info.format==9||info.format==10;
    if ((info.format>6&&info.format!=14&&!indexed)||bti[16]>1||
        (indexed?(bti[8]!=1||!info.paletteEntries):(bti[8]||info.paletteEntries))) {
        error(Error::unsupportedState,"registerTexture/format-mips-palette"); return false;
    }
    Registration* available=nullptr;
    for (auto& entry:registrations) {
        if (entry.texture.handle) {
            // An enclosing TEX1 view includes other headers before its pixels.
            // Claim only the actual 32-byte header and the validated pixel span.
            auto overlaps=[](std::span<const std::uint8_t> x,std::span<const std::uint8_t> y){
                const auto a=reinterpret_cast<std::uintptr_t>(x.data()),b=reinterpret_cast<std::uintptr_t>(y.data());
                return (a<=b && b-a<x.size()) || (b<a && a-b<y.size());
            };
            const auto header=bti.first(32),oldHeader=entry.texture.bti.first(32);
            const std::span<const std::uint8_t> image{info.image.data,info.image.size};
            BtiInfo old;inspectBti({entry.texture.bti.data(),entry.texture.bti.size()},1024,old);
            const std::span<const std::uint8_t> palette{info.palette.data,info.palette.size},oldPalette{old.palette.data,old.palette.size};
            if(overlaps(header,oldHeader)||overlaps(image,entry.texture.image)||
               overlaps(header,entry.texture.image)||overlaps(image,oldHeader)||
               (info.paletteEntries&&(overlaps(palette,oldHeader)||overlaps(palette,entry.texture.image)))||
               (old.paletteEntries&&(overlaps(header,oldPalette)||overlaps(image,oldPalette)))){
                error(Error::invalidParameter,"registerTexture/duplicate-overlap"); return false;
            }
        }
        if (!entry.texture.handle && !available) available=&entry;
    }
    if (!available) { error(Error::unsupportedState,"registerTexture/capacity"); return false; }
    if (!nextTextureId || nextTextureId>maxTextureTokenId) { error(Error::unsupportedState,"registerTexture/token-exhausted"); return false; }
    handle=nextTextureId++;
    available->texture={handle,bti,{info.image.data,info.image.size},info.width,info.height,
                        info.format,bti[6],bti[7],bti[20],bti[21],info.mipCount,static_cast<std::uint8_t>(bti[22]/8),static_cast<std::uint8_t>(bti[23]/8),static_cast<std::int16_t>((unsigned(bti[26])<<8)|bti[27])/100.f};
    available->texture.immutablePixels=immutablePixels;
    clearImageHints();
    return true;
}
bool unregisterTexture(TextureHandle handle) {
    if (!setter("unregisterTexture")) return false;
    auto* entry=registration(handle);
    if (!entry) { error(Error::invalidParameter,"unregisterTexture/handle"); return false; }
    *entry={};
    clearImageHints();
    for(auto& alias:paletteAliases)if(alias.handle==handle)alias={};
    for(auto& palette:loadedTluts)if(palette==handle)palette=0;
    // Clear all borrowed resource spans before the owner can free its archive.
    for (auto& object:textureObjects) if (object.texture.handle==handle) {
        // Keep the GX object's initialized dimensions/sampler. Its immutable
        // source may be rebound to a fresh retained image after retirement.
        object.texture.handle=0;object.texture.bti={};object.texture.image={};
        object.supported=false; object.baseSupported=false;
    }
    for (auto& bound:state.bindings) if (bound.texture.handle==handle) { bound.texture={}; bound.supported=false; }
    return true;
}
bool textureBinding(unsigned unit,Texture& output){
    output={};if(unit>=state.bindings.size())return false;
    const auto& bound=state.bindings[unit];const auto* object=textureObject(bound.address);
    if(!bound.supported||!object||object->generation!=bound.generation||!registration(bound.texture.handle))return false;
    output=bound.texture;return true;
}
std::size_t textureRegistrationCapacity(){return registrations.size();}
bool registeredTextureAt(std::size_t slot,Texture& output){
    output={};
    if(state.active||slot>=registrations.size()||!registrations[slot].texture.handle)return false;
    output=registrations[slot].texture;return true;
}
bool textureCommandToken(const void* image,std::uint32_t& token){
    token=0;
    auto key=reinterpret_cast<std::uintptr_t>(image);
    key^=key>>11;key^=key>>21;
    auto& hint=tokenHints[(key>>2)&(tokenHints.size()-1)];
    if(hint){const auto& entry=registrations[hint-1];
        if(entry.texture.handle&&(entry.texture.image.data()==image||entry.alias==image)){
            if(entry.texture.handle>0x7fffff)return false;
            token=0x800000u|static_cast<std::uint32_t>(entry.texture.handle);return true;
        }
    }
    for(const auto& entry:registrations)if(entry.texture.handle&&(entry.texture.image.data()==image||entry.alias==image)){
        if(entry.texture.handle>0x7fffff)return false;
        hint=std::uint16_t(&entry-registrations.data()+1);
        token=0x800000u|static_cast<std::uint32_t>(entry.texture.handle);return true;
    }
    return false;
}
const void* textureAliasImage(TextureHandle handle){auto* entry=registration(handle);return entry?entry->alias:nullptr;}
bool aliasTextureImage(TextureHandle handle,const void* identity){
    if(!identity||!setter("aliasTextureImage"))return false;
    auto* entry=registration(handle);if(!entry)return false;
    for(const auto& other:registrations)if(other.texture.handle&&other.texture.handle!=handle&&
        (other.alias==identity||other.texture.image.data()==identity))return false;
    entry->alias=identity;
    clearImageHints();
    for(auto& object:textureObjects)if(object.imageIdentity==identity&&object.metadataValid&&
        object.texture.width==entry->texture.width&&object.texture.height==entry->texture.height&&object.texture.format==entry->texture.format){
        const auto old=object.texture;
        object.texture=entry->texture;
        object.texture.wrapS=old.wrapS;object.texture.wrapT=old.wrapT;
        object.texture.minFilter=old.minFilter;object.texture.magFilter=old.magFilter;
        object.texture.minLod=old.minLod;object.texture.maxLod=old.maxLod;object.texture.lodBias=old.lodBias;
        object.baseSupported=true;
        object.supported=object.samplerValid&&old.maxLod<entry->texture.mipCount;
    }
    return true;
}
bool aliasTexturePalette(TextureHandle handle,const void* identity){
    if(!identity||!setter("aliasTexturePalette"))return false;
    auto* entry=registration(handle);if(!entry||!entry->texture.bti[8])return false;
    PaletteAlias* available=nullptr;
    for(auto& alias:paletteAliases){
        if(alias.identity==identity){
            if(alias.handle==handle)return true;
            const auto* old=registration(alias.handle);
            if(!old||!texturePaletteMatches(entry->texture,old->texture))return false;
        }
        if(!alias.handle&&!available)available=&alias;
    }
    if(!available)return false;
    *available={identity,handle};return true;
}
bool paletteCommandToken(const void* palette,std::uint32_t& token){
    token=0;if(!palette)return false;
    for(const auto& entry:registrations)if(entry.texture.handle&&entry.texture.bti[8]){
        BtiInfo info;
        if(inspectBti({entry.texture.bti.data(),entry.texture.bti.size()},1024,info)!=ResourceStatus::ready)return false;
        if(info.palette.data==palette){
            if(entry.texture.handle>0x7fffff)return false;
            token=0x800000u|static_cast<std::uint32_t>(entry.texture.handle);return true;
        }
    }
    for(const auto& alias:paletteAliases)if(alias.identity==palette&&registration(alias.handle)){
        if(alias.handle>0x7fffff)return false;
        token=0x800000u|static_cast<std::uint32_t>(alias.handle);return true;
    }
    return false;
}
bool texturePaletteMatches(const Texture& texture,const Texture& palette){
    if(!texture.handle||!palette.handle)return false;
    BtiInfo a,b;
    return inspectBti({texture.bti.data(),texture.bti.size()},1024,a)==ResourceStatus::ready&&
        inspectBti({palette.bti.data(),palette.bti.size()},1024,b)==ResourceStatus::ready&&
        a.paletteEntries&&a.paletteEntries==b.paletteEntries&&a.paletteFormat==b.paletteFormat&&
        std::memcmp(a.palette.data,b.palette.data,a.palette.size)==0;
}
bool textureFromCommandToken(std::uint32_t token,Texture& output){
    output={};if(!(token&0x800000u)||token>0xffffff)return false;
    const auto* entry=registration(token&0x7fffffu);if(!entry)return false;
    output=entry->texture;return true;
}
bool registerTextureBlock(std::span<const std::uint8_t> block,TextureBlockResources& output,bool allowExternal){
    if(!setter("registerTextureBlock"))return false;
    if(output.count||std::any_of(output.handles.begin(),output.handles.end(),[](auto h){return h!=0;})){
        error(Error::invalidParameter,"registerTextureBlock/output-not-empty");return false;
    }
    auto fail=[&](){
        for(unsigned n=0;n<output.count;++n)unregisterTexture(output.handles[n]);
        output={};error(Error::invalidParameter,"registerTextureBlock/TEX1");return false;
    };
    if(block.size()<32||std::memcmp(block.data(),"TEX1",4))return fail();
    auto word=[&](unsigned at){return (std::uint32_t(block[at])<<24)|(std::uint32_t(block[at+1])<<16)|(std::uint32_t(block[at+2])<<8)|block[at+3];};
    const unsigned count=(unsigned(block[8])<<8)|block[9];const std::size_t headers=word(12);
    if(word(4)==block.size()&&count==0&&headers==0)return true;
    if(word(4)!=block.size()||count>output.handles.size()||headers<32||headers>block.size()||
       headers%4||count>(block.size()-headers)/32)return fail();
    // Validate all payload boundaries against the complete header table before
    // any registration. Existing registry conflicts/capacity still roll back.
    for(unsigned i=0;i<count;++i){
        const std::size_t offset=headers+32*i;BtiInfo info{};
        if(allowExternal&&word(offset+28)==0)continue;
        if(inspectBti({block.data()+offset,block.size()-offset},1024,info)!=ResourceStatus::ready||
           std::size_t(info.image.data-block.data())<headers+32*count||
           (info.paletteEntries&&std::size_t(info.palette.data-block.data())<headers+32*count))return fail();
    }
    for(unsigned i=0;i<count;++i){
        const auto offset=headers+32*i;
        if(allowExternal&&word(offset+28)==0)continue;
        BtiInfo info{};inspectBti({block.data()+offset,block.size()-offset},1024,info);
        bool alias=false;
        for(unsigned j=0;j<output.count;++j){
            const auto& texture=registration(output.handles[j])->texture;
            if(texture.image.data()==info.image.data&&texture.image.size()==info.image.size&&
               texture.width==info.width&&texture.height==info.height&&texture.format==info.format&&texture.mipCount==info.mipCount){
                BtiInfo old;inspectBti({texture.bti.data(),texture.bti.size()},1024,old);
                if(old.paletteEntries!=info.paletteEntries||old.paletteFormat!=info.paletteFormat||
                   (info.palette.size&&std::memcmp(old.palette.data,info.palette.data,info.palette.size)))return fail();
                alias=true;break;
            }
        }
        if(alias)continue;
        if(!registerTexture(block.subspan(offset),output.handles[output.count])){
            for(unsigned n=0;n<output.count;++n)unregisterTexture(output.handles[n]);
            output={};return false;
        }
        ++output.count;
    }
    return true;
}
bool releaseTextureBlock(TextureBlockResources& block){
    if(!setter("releaseTextureBlock"))return false;
    if(block.count>block.handles.size())return false;
    for(unsigned i=block.count;i<block.handles.size();++i)if(block.handles[i])return false;
    // Validate before mutation so stale or duplicate caller handles do not cause
    // partial release. Successful release clears objects/bindings via unregister.
    for(unsigned i=0;i<block.count;++i){
        if(!registration(block.handles[i]))return false;
        for(unsigned j=0;j<i;++j)if(block.handles[j]==block.handles[i])return false;
    }
    for(unsigned i=0;i<block.count;++i)unregisterTexture(block.handles[i]);
    block={};return true;
}
Statistics statistics() { return state.stats; }
bool queryViewport(Viewport& output) {
    if (state.active || !state.viewportReady) return false;
    output = state.viewport; return true;
}
bool queryScissor(Scissor& output) {
    if (state.active || !state.scissorReady) return false;
    output = state.scissor; return true;
}
bool queryProjection(float* output) {
    if (!output || state.active || !state.projectionReady) return false;
    const unsigned type=state.projectionType;
    const float values[]{float(type),state.projection[0],state.projection[type?3:2],
        state.projection[5],state.projection[type?7:6],state.projection[10],state.projection[11]};
    std::copy_n(values,7,output); return true;
}
void setViewport(float x, float y, float width, float height, float nearZ, float farZ) {
    if (!setter("GXSetViewport")) return;
    const float values[] = {x,y,width,height,nearZ,farZ};
    state.viewportReady = finite(values, 6) && width > 0 && height > 0 && nearZ >= 0 && nearZ <= 1 && farZ >= 0 && farZ <= 1;
    state.viewport = {x,y,width,height,nearZ,farZ};
}
void setScissor(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
    if (!setter("GXSetScissor")) return;
    state.scissor = {x,y,width,height};
    state.scissorReady = x <= 2048 && y <= 2048 && width <= 2048 - x && height <= 2048 - y;
}
void setProjection(const float* matrix, unsigned type) {
    if (!setter("GXSetProjection")) return;
    state.projectionReady = type <= 1 && finite(matrix, 16); state.projectionType = type;
    if (state.projectionReady) std::memcpy(state.projection, matrix, sizeof(state.projection));
}
void loadPositionMatrix(const float* matrix, std::uint32_t id) {
    if (!setter("GXLoadPosMtxImm")) return;
    if (id > 27 || id % 3) { invalid("GXLoadPosMtxImm/index"); return; }
    state.positionReady[id / 3] = finite(matrix, 12);
    if (state.positionReady[id / 3]) std::memcpy(state.matrices[id / 3], matrix, 12 * sizeof(float));
}
void loadTextureMatrix(const float* matrix, std::uint32_t id, unsigned type) {
    if (!setter("GXLoadTexMtxImm")) return;
    if (id < 30 || id > 60 || id % 3 || type > 1 || !finite(matrix, type == 0 ? 12 : 8)) {
        invalid("GXLoadTexMtxImm"); return;
    }
    auto& destination = state.textureMatrices[(id - 30) / 3];
    std::fill_n(destination, 12, 0); destination[10] = 1;
    std::memcpy(destination, matrix, (type == 0 ? 12 : 8) * sizeof(float));
}
void setCurrentMatrix(std::uint32_t id) { if (setter("GXSetCurrentMtx")) state.currentMatrix = id; }
void setBlend(unsigned mode, unsigned source, unsigned destination, unsigned logic) {
    if (!setter("GXSetBlendMode")) return;
    state.blendMode=mode; state.blendSource=source; state.blendDestination=destination; state.blendLogic=logic;
}
void setDepth(unsigned enabled, unsigned compare, unsigned write) {
    if (!setter("GXSetZMode")) return;
    state.depthEnabled=enabled; state.depthCompare=compare; state.depthWrite=write;
}
void setAlpha(unsigned compare0, std::uint8_t reference0, unsigned op, unsigned compare1, std::uint8_t reference1) {
    if (!setter("GXSetAlphaCompare")) return;
    state.compare0=compare0; state.reference0=reference0; state.alphaOp=op; state.compare1=compare1; state.reference1=reference1;
}
void setCull(unsigned mode) { if (setter("GXSetCullMode")) state.cull=mode; }
void setZCompareLocation(unsigned beforeTexture) { if (setter("GXSetZCompLoc")) state.zLocation=beforeTexture; }
void setLineWidth(std::uint8_t width, unsigned offsets) {
    if (!setter("GXSetLineWidth")) return;
    state.lineWidth=width; state.lineOffsets=offsets;
}
void setNumIndirect(std::uint8_t count) { if (setter("GXSetNumIndStages")) state.indirectCount=count; }
void setTevDirect(unsigned stage) {
    if (!setter("GXSetTevDirect")) return;
    if (stage >= state.tev.size()) { invalid("GXSetTevDirect"); return; }
    state.tev[stage].direct=true;
}
void setTevOperation(unsigned stage, unsigned operation) {
    if (!setter("GXSetTevOp")) return;
    if (stage >= state.tev.size()) { invalid("GXSetTevOp"); return; }
    auto& tev=state.tev[stage]; tev.operation=operation; tev.explicitProgram=false;
    // GXSetTevOp replaces the explicit inputs and operators as GX does. Only
    // PASSCLR is admitted as a preset in this milestone.
    if (operation==4) {
        const unsigned color[]={15,15,15,10}, alpha[]={7,7,7,5}, op[]={0,0,0,1,0};
        std::copy_n(color,4,tev.color); std::copy_n(alpha,4,tev.alpha);
        std::copy_n(op,5,tev.colorOp); std::copy_n(op,5,tev.alphaOp);
    }
}
void setTevOrder(unsigned stage, unsigned coordinate, unsigned texture, unsigned channel) {
    if (!setter("GXSetTevOrder")) return;
    if (stage >= state.tev.size()) { invalid("GXSetTevOrder"); return; }
    auto& tev=state.tev[stage]; tev.coordinate=coordinate; tev.texture=texture; tev.channel=channel;
}
void setTevInputs(unsigned stage, bool alpha, unsigned a, unsigned b, unsigned c, unsigned d) {
    if (!setter("GXSetTevInputs")) return;
    if (stage>=state.tev.size()) { invalid("GXSetTevInputs/index"); return; }
    auto& tev=state.tev[stage]; auto* target=alpha?tev.alpha:tev.color;
    target[0]=a; target[1]=b; target[2]=c; target[3]=d; tev.explicitProgram=true;
}
void setTevOperator(unsigned stage, bool alpha, unsigned operation, unsigned bias, unsigned scale,
                    unsigned clamp, unsigned output) {
    if (!setter("GXSetTevOperator")) return;
    if (stage>=state.tev.size()) { invalid("GXSetTevOperator/index"); return; }
    auto& tev=state.tev[stage]; auto* target=alpha?tev.alphaOp:tev.colorOp;
    target[0]=operation; target[1]=bias; target[2]=scale; target[3]=clamp; target[4]=output; tev.explicitProgram=true;
}
void setTevRegister(unsigned id, Rgba color, bool konst) {
    if (!setter("GXSetTevColor")) return;
    if (id>=4) { invalid("GXSetTevColor/index"); return; }
    (konst?state.konst:state.registers)[id]=color;
}
void setTevKonstSelection(unsigned stage, unsigned selection, bool alpha) {
    if (!setter("GXSetTevKonstSelection")) return;
    if (stage>=state.tev.size()) { invalid("GXSetTevKonstSelection/index"); return; }
    (alpha?state.tev[stage].konstAlpha:state.tev[stage].konstColor)=selection;
}
void setNumTevStages(std::uint8_t count) { if (setter("GXSetNumTevStages")) state.tevCount=count; }
void setNumChannels(std::uint8_t count) { if (setter("GXSetNumChans")) state.channelCount=count; }
void setChannelControl(unsigned channel, unsigned enabled, unsigned ambientSource, unsigned materialSource,
                       std::uint32_t lights, unsigned diffuse, unsigned attenuation) {
    if (!setter("GXSetChanCtrl")) return;
    if (channel > 5) { invalid("GXSetChanCtrl"); return; }
    const Channel control{enabled,ambientSource,materialSource,lights,diffuse,attenuation,true};
    if (channel == 0 || channel == 4) state.channels[0]=control;
    if (channel == 2 || channel == 4) state.channels[1]=control;
    if (channel == 1 || channel == 5) state.channels[2]=control;
    if (channel == 3 || channel == 5) state.channels[3]=control;
}
void setNumTexGens(std::uint8_t count) { if (setter("GXSetNumTexGens")) state.texGenCount=count; }
void setTexCoordGen(unsigned destination, unsigned function, unsigned source, std::uint32_t matrix,
                    unsigned normalize, std::uint32_t postMatrix) {
    if (!setter("GXSetTexCoordGen2")) return;
    if (destination >= state.texgens.size()) { invalid("GXSetTexCoordGen2"); return; }
    state.texgens[destination]={function,source,normalize,matrix,postMatrix};
}
void clearVertexDescriptors() { if (setter("GXClearVtxDesc")) state.descriptors.fill(0); }
void setVertexDescriptor(unsigned attribute, unsigned type) {
    if (!setter("GXSetVtxDesc")) return;
    if (attribute >= state.descriptors.size()) { invalid("GXSetVtxDesc"); return; }
    state.descriptors[attribute]=type;
}
void setVertexFormat(unsigned format, unsigned attribute, unsigned count, unsigned type, std::uint8_t fraction) {
    if (!setter("GXSetVtxAttrFmt")) return;
    if (format >= 8 || attribute >= state.descriptors.size()) { invalid("GXSetVtxAttrFmt"); return; }
    state.formats[format][attribute]={count,type,fraction,true};
}
void begin(unsigned primitive, unsigned format, std::uint16_t count) {
    if (state.active) {
        drop(Error::vertexProtocol, "GXBegin/nested"); ++state.stats.drawsDropped;
    }
    state.active=true; state.dropped=false; state.expected=count; state.received=0;
    state.nextAttribute=positionAttribute; state.format=format;
    bool sizeOkay=true;
    switch (primitive) {
    case 0x80: state.primitive=Primitive::quads; sizeOkay=count>=4 && count%4==0; break;
    case 0x90: state.primitive=Primitive::triangles; sizeOkay=count>=3 && count%3==0; break;
    case 0x98: state.primitive=Primitive::triangleStrip; sizeOkay=count>=3; break;
    case 0xa0: state.primitive=Primitive::triangleFan; sizeOkay=count>=3; break;
    case 0xa8: state.primitive=Primitive::lines; sizeOkay=count>=2 && count%2==0; break;
    case 0xb0: state.primitive=Primitive::lineStrip; sizeOkay=count>=2; break;
    default: drop(Error::unsupportedState, "GXBegin/primitive"); return;
    }
    if (count > maxVertices) { drop(Error::vertexOverflow, "GXBegin/count"); return; }
    if (format >= 8 || !sizeOkay) { drop(Error::invalidParameter, "GXBegin/count-format"); return; }
    validate();
}
void position(float x, float y, float z) {
    if (!vertex(positionAttribute, "GXPosition3f32")) return;
    const float values[]={x,y,z};
    if (!finite(values,3)) { drop(Error::invalidParameter,"GXPosition3f32/nonfinite"); return; }
    vertices[state.received]={x,y,z,0,0,0,0,0,0}; advance(positionAttribute);
}
void color(std::uint32_t rgba) {
    if (!vertex(colorAttribute, "GXColor1u32")) return;
    auto& v=vertices[state.received];
    v.r=rgba>>24; v.g=rgba>>16; v.b=rgba>>8; v.a=rgba; advance(colorAttribute);
}
void textureCoordinate(float u, float v) {
    if (!vertex(textureAttribute, "GXTexCoord2f32")) return;
    if (state.formats[state.format][textureAttribute].type!=4) {
        drop(Error::vertexProtocol,"GXTexCoord2f32/format"); return;
    }
    if (!std::isfinite(u) || !std::isfinite(v)) { drop(Error::invalidParameter,"GXTexCoord2f32/nonfinite"); return; }
    auto& vertex=vertices[state.received]; vertex.u=u; vertex.v=v; advance(textureAttribute);
}
void textureCoordinateShort(std::int16_t u, std::int16_t v) {
    if (!vertex(textureAttribute,"GXTexCoord2s16")) return;
    const auto& format=state.formats[state.format][textureAttribute];
    if (format.type!=3 || format.fraction!=8) { drop(Error::vertexProtocol,"GXTexCoord2s16/format"); return; }
    auto& value=vertices[state.received]; value.u=u/256.0f; value.v=v/256.0f; advance(textureAttribute);
}
void initTextureObject(void* address, const void* image, std::uint16_t width, std::uint16_t height,
                        unsigned format, unsigned wrapS, unsigned wrapT, unsigned mipmap, bool palette,unsigned tlut) {
    if (!setter("GXInitTexObj")) return;
    if (!address) { invalid("GXInitTexObj/null"); return; }
    auto* object=textureObject(address);
    if (!object) for (auto& entry:textureObjects) if (!entry.address) { object=&entry; break; }
    if (!object) { invalid("GXInitTexObj/capacity"); return; }
    if (!nextObjectGeneration) {
        // Exhaustion must not leave a previous descriptor/binding looking like
        // the successful result of this reinitialization. Never wrap/recycle.
        for(auto& bound:state.bindings)if(bound.address==address)bound={};
        *object={};invalid("GXInitTexObj/generation-exhausted");return;
    }
    *object={}; object->address=address; object->generation=nextObjectGeneration++;object->imageIdentity=image;
    object->metadataValid=width>0&&width<=1024&&height>0&&height<=1024&&
        format<=255&&wrapS<=2&&wrapT<=2&&mipmap<=1;
    // Original dMirror_packet_c explicitly constructs this empty descriptor
    // and queries width before its first capture. It has metadata but no image;
    // loading it remains unsupported. Unknown/destroyed objects still reject.
    object->metadataValid|=!image&&!width&&!height&&format==6&&!wrapS&&!wrapT&&!mipmap&&!palette;
    object->texture.width=width;object->texture.height=height;
    object->texture.format=static_cast<std::uint8_t>(format);
    object->texture.wrapS=static_cast<std::uint8_t>(wrapS);
    object->texture.wrapT=static_cast<std::uint8_t>(wrapT);
    object->texture.minFilter=object->texture.magFilter=1;
    object->indexed=palette;object->tlut=tlut;
    if(palette&&((format!=8&&format!=9&&format!=10)||tlut>=loadedTluts.size())){
        invalid("GXInitTexObjCI/format-tlut");return;
    }
    if (const auto* entry=imageRegistration(image,width,height,format)) {
        const auto& registered=entry->texture;
            object->texture=registered;
            if (wrapS>2 || wrapT>2 || mipmap>1) return;
            object->texture.wrapS=wrapS; object->texture.wrapT=wrapT;
            object->texture.minFilter=1; object->texture.magFilter=1;
            object->supported=true; object->baseSupported=true; return;
    }
    // The raw pointer has no size in the GX ABI. No read is attempted until it
    // has an exact match in the explicitly validated registration table.
}
bool queryTextureObject(const void* address,Texture& output) {
    output={};
    const auto* object=textureObject(const_cast<void*>(address));
    if(!object||!object->metadataValid)return false;
    output=object->texture;return true;
}
void setTextureObjectLod(void* address, unsigned minFilter, unsigned magFilter, float minLod, float maxLod,
                         float lodBias, unsigned biasClamp, unsigned edgeLod, unsigned anisotropy) {
    if (!setter("GXInitTexObjLOD")) return;
    auto* object=textureObject(address);
    if (!object) { invalid("GXInitTexObjLOD/object"); return; }
    object->samplerValid=object->metadataValid && minFilter<=5 && magFilter<=1 &&
        std::isfinite(minLod)&&std::isfinite(maxLod)&&std::isfinite(lodBias)&&
        minLod>=0&&maxLod>=minLod&&maxLod<16&&(!object->baseSupported||maxLod<object->texture.mipCount)&&
        std::floor(minLod)==minLod&&std::floor(maxLod)==maxLod&&
        lodBias>=-4&&lodBias<4&&biasClamp<=1&&(!biasClamp||(maxLod==0&&lodBias==0))&&edgeLod<=1&&anisotropy==0;
    object->supported=object->baseSupported&&object->samplerValid;
    if (!object->samplerValid) return;
    object->texture.minFilter=minFilter; object->texture.magFilter=magFilter;
    object->texture.minLod=static_cast<std::uint8_t>(minLod);object->texture.maxLod=static_cast<std::uint8_t>(maxLod);
    object->texture.lodBias=lodBias;
}
void loadTextureObject(void* address, unsigned unit) {
    if (!setter("GXLoadTexObj")) return;
    if (unit>=state.bindings.size()) { invalid("GXLoadTexObj/unit"); return; }
    const auto* object=textureObject(address);
    auto& bound=state.bindings[unit]; bound={};
    if (!object) return; // Reached textured draw reports an unavailable binding.
    bound={address,object->generation,object->texture,object->supported};
    if(object->indexed){
        const auto* palette=registration(loadedTluts[object->tlut]);
        bound.supported=bound.supported&&palette&&texturePaletteMatches(object->texture,palette->texture);
    }
}
void destroyTextureObject(void* address) {
    if (!setter("GXDestroyTexObj")) return;
    auto* object=textureObject(address);
    if (!object) return; // Original RAII reset/destroy of an empty object is valid.
    for (auto& bound:state.bindings) if (bound.address==address) bound={};
    *object={};
}
void setTextureObjectTlut(void* address,unsigned tlut){
    if(!setter("GXInitTexObjTlut"))return;
    auto* object=textureObject(address);
    if(!object||!object->indexed||tlut>=loadedTluts.size()){invalid("GXInitTexObjTlut/object-slot");return;}
    object->tlut=tlut;
}
void initTlutObject(void* address,const void* palette,unsigned format,unsigned entries){
    if(!setter("GXInitTlutObj"))return;
    if(!address||!palette||format>2||!entries||entries>16384){invalid("GXInitTlutObj/metadata");return;}
    TlutObject* target=nullptr;
    for(auto& object:tlutObjects){if(object.address==address){target=&object;break;}if(!object.address&&!target)target=&object;}
    if(!target){invalid("GXInitTlutObj/capacity");return;}
    *target={address,palette,format,entries};
}
void setTlutObjectData(void* address,const void* palette){
    if(!setter("GXInitTlutObjData"))return;
    for(auto& object:tlutObjects)if(object.address==address&&palette){object.palette=palette;return;}
    invalid("GXInitTlutObjData/object");
}
void loadTlutObject(const void* address,unsigned tlut){
    if(!setter("GXLoadTlut"))return;
    if(tlut>=loadedTluts.size()){invalid("GXLoadTlut/slot");return;}
    loadedTluts[tlut]=0;
    for(const auto& object:tlutObjects)if(object.address==address){
        std::uint32_t token{};Texture texture;BtiInfo info;
        if(!paletteCommandToken(object.palette,token)||!textureFromCommandToken(token,texture)||
           inspectBti({texture.bti.data(),texture.bti.size()},1024,info)!=ResourceStatus::ready||
           info.paletteFormat!=object.format||info.paletteEntries!=object.entries||
           (tlut<16&&object.entries>256)){invalid("GXLoadTlut/unregistered-layout");return;}
        loadedTluts[tlut]=texture.handle;return;
    }
    invalid("GXLoadTlut/object");
}
void destroyTlutObject(void* address){
    if(!setter("GXDestroyTlutObj"))return;
    for(auto& object:tlutObjects)if(object.address==address){object={};return;}
}
void unsupportedPalette(const char* call) { unsupportedOperation(call); }
void unsupportedOperation(const char* call) {
    if (!setter(call)) return;
    error(Error::unsupportedState,call); state.configurationError=true;
}
void end() {
    if (!state.active) { error(Error::vertexProtocol, "GXEnd"); return; }
    if (!state.dropped && (state.received != state.expected || state.nextAttribute != positionAttribute))
        drop(Error::incompleteDraw,"GXEnd/count");
    if (!state.dropped) {
        DrawPacket packet{};
        packet.vertices={vertices.data(),state.received}; packet.primitive=state.primitive;
        std::memcpy(packet.projection,state.projection,sizeof(packet.projection));
        std::memcpy(packet.modelView,state.matrices[state.currentMatrix/3],sizeof(packet.modelView));
        packet.viewport=state.viewport; packet.scissor=state.scissor;
        packet.blend={std::uint8_t(state.blendMode),std::uint8_t(state.blendSource),std::uint8_t(state.blendDestination),std::uint8_t(state.blendLogic)};
        packet.depth={state.depthEnabled!=0,std::uint8_t(state.depthCompare),state.depthWrite!=0};
        packet.alpha={std::uint8_t(state.compare0),state.reference0,std::uint8_t(state.alphaOp),std::uint8_t(state.compare1),state.reference1};
        packet.cull=state.cull; packet.lineWidthPixels=state.lineWidth/6.0f; packet.zCompareBeforeTexture=state.zLocation!=0;
        packet.textured=state.texGenCount!=0;
        if (packet.textured) packet.texture=state.bindings[0].texture;
        packet.tevCount=state.tevCount;
        std::copy_n(state.registers,4,packet.registers); std::copy_n(state.konst,4,packet.konst);
        for (unsigned i=0;i<state.tevCount;++i) {
            const auto& source=state.tev[i]; auto& destination=packet.tev[i];
            for (unsigned j=0;j<4;++j) { destination.color[j]=source.color[j]; destination.alpha[j]=source.alpha[j]; }
            destination.colorOp={std::uint8_t(source.colorOp[0]),std::uint8_t(source.colorOp[1]),std::uint8_t(source.colorOp[2]),
                                 std::uint8_t(source.colorOp[3]),std::uint8_t(source.colorOp[4])};
            destination.alphaOp={std::uint8_t(source.alphaOp[0]),std::uint8_t(source.alphaOp[1]),std::uint8_t(source.alphaOp[2]),
                                 std::uint8_t(source.alphaOp[3]),std::uint8_t(source.alphaOp[4])};
            destination.coordinate=source.coordinate; destination.texture=source.texture; destination.channel=source.channel;
            destination.konstColor=source.konstColor; destination.konstAlpha=source.konstAlpha;
        }
        if (!state.sink) drop(Error::missingSink,"GXEnd/sink");
        else if (!state.sink(packet,state.sinkUser)) drop(Error::backendRejected,"GXEnd/backend");
    }
    if (state.dropped) ++state.stats.drawsDropped;
    else { ++state.stats.drawsSubmitted; state.stats.verticesSubmitted+=state.received; }
    state.active=false;
}
const char* errorText(Error error) {
    switch(error) {
    case Error::none:return "none";
    case Error::invalidParameter:return "invalid GX parameter or unset required state";
    case Error::unsupportedState:return "unsupported active GX state";
    case Error::vertexProtocol:return "GX vertex/command ordering error";
    case Error::vertexOverflow:return "GX vertex capacity/count exceeded";
    case Error::incompleteDraw:return "incomplete GX primitive";
    case Error::missingSink:return "GX draw backend unavailable";
    case Error::backendRejected:return "GX draw rejected by backend";
    }
    return "unknown GX error";
}
}
