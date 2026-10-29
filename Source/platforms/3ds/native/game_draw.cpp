#include "game_draw.hpp"
#include "game_assets.hpp"
#include "camera.hpp"
#include "game_draw_api.hpp"
namespace dusk::pica {
bool GameDraw::onOwnerThread(){if(!renderer)return fail(Error::protocol);if(!renderer->owner())return fail(renderer->error());return true;}
bool GameDraw::initialize(Renderer& r){
    if(renderer)return fail(Error::protocol);
    if(!r.owner())return fail(r.error());
    if(r.open)return fail(Error::protocol);
    renderer=&r;failure=Error::none;return true;
}
bool GameDraw::beginExternalFrame(C3D_RenderTarget* target){
    if(submitting){invalidated=true;return fail(Error::protocol);}
    if(!renderer)return fail(Error::protocol);
    auto& r=*renderer;if(!r.owner())return fail(r.error());
    if(r.open||externalOpen||!target||!target->frameBuf.colorBuf||!target->frameBuf.width||!target->frameBuf.height)return fail(Error::protocol);
    // Caller has completed the previous entire shared SDK/GSP frame. Never
    // call this from a mere draw-buffer flush or an unfinished queue callback.
    r.collect();r.open=true;r.emitted=false;r.stats.frameDraws=0;r.stats.transientBytes=0;
    if(!r.target(target)){r.open=false;return fail(r.error());}
    externalOpen=true;++r.stats.frames;failure=Error::none;return true;
}
bool GameDraw::endExternalFrame(){
    if(submitting){invalidated=true;return fail(Error::protocol);}
    if(!renderer)return fail(Error::protocol);
    auto& r=*renderer;if(!r.owner())return fail(r.error());
    if(!r.open||!externalOpen)return fail(Error::protocol);
    externalOpen=false;r.open=false;failure=Error::none;return true; // External owner still submits FrameEnd.
}
bool GameDraw::bind(const void* instance,Model& model,std::span<const unsigned> shapes,GameUniformProducer producer,void* user){
    if(submitting){invalidated=true;return fail(Error::protocol);}
    if(!renderer)return fail(Error::protocol);
    if(!renderer->owner())return fail(renderer->error());
    if(!instance||!producer||!model.loaded||model.renderer!=renderer||shapes.size()!=model.partCount)return fail(Error::invalidAsset);
    Binding* vacant=nullptr;
    for(auto& slot:bindings){if(slot.instance==instance)return fail(Error::protocol);if(!slot.instance&&!vacant)vacant=&slot;}
    if(!vacant)return fail(Error::budget);
    for(unsigned shape:shapes)if(shape>=4096)return fail(Error::invalidAsset);
    vacant->instance=instance;vacant->model=&model;vacant->producer=producer;vacant->user=user;vacant->revision=model.revision;
    for(unsigned n=0;n<shapes.size();++n)vacant->shapes[n]=std::uint16_t(shapes[n]);
    failure=Error::none;return true;
}
bool GameDraw::forget(const void* instance){
    if(!renderer)return true;
    if(submitting){invalidated=true;return fail(Error::protocol);}
    if(!renderer->owner())return fail(renderer->error());
    for(auto& slot:bindings)if(slot.instance==instance)slot={};
    failure=Error::none;return true;
}
bool GameDraw::empty()const{for(const auto& slot:bindings)if(slot.instance)return false;return true;}
Submission GameDraw::draw(const void* instance,unsigned shape){
    const auto reject=[&](Error e){failure=e;return Submission{0,e};};
    if(submitting){invalidated=true;return reject(Error::protocol);}
    if(!renderer)return reject(Error::protocol);
    auto& r=*renderer;if(!r.owner())return reject(r.error());if(!r.open||!externalOpen)return reject(Error::protocol);
    Binding* binding=nullptr;for(auto& slot:bindings)if(slot.instance==instance){binding=&slot;break;}
    if(!instance||!binding)return reject(Error::invalidAsset);
    auto& model=*binding->model;
    if(!model.loaded||model.renderer!=renderer||model.revision!=binding->revision)return reject(Error::invalidHandle);
    std::array<unsigned,64> selection{};unsigned count=0;std::uint64_t used=0;
    for(unsigned n=0;n<model.partCount;++n)if(binding->shapes[n]==shape){selection[count++]=n;used|=std::uint64_t(1)<<model.sequence[n].uniform;}
    if(!count)return reject(Error::invalidAsset);
    if(count>r.limits.drawsPerFrame-r.stats.frameDraws)return reject(Error::budget);
    submitting=true;invalidated=false;
    struct Scope {bool& active;~Scope(){active=false;}} scope{submitting};
    for(unsigned n=0;n<64;++n)if(used&(std::uint64_t(1)<<n)){
        uniforms[n]={};if(!binding->producer(instance,n,uniforms[n],binding->user))return reject(Error::invalidAsset);
        if(invalidated)return reject(Error::protocol);
    }
    // A producer cannot silently unload/reload the resident asset under the draw.
    if(invalidated)return reject(Error::protocol);
    if(!model.loaded||model.revision!=binding->revision)return reject(Error::invalidHandle);
    auto result=model.drawParts(std::span(selection).first(count),uniforms);failure=result.error;return result;
}
namespace game {
namespace {Renderer nativeRenderer;GameDraw nativeDraw;GameAssets nativeAssets;C3D_RenderTarget* mainOutput{};C3D_RenderTarget* currentOutput{};CameraProjection camera;bool cameraValid{},cameraFailure{};bool initialized{},lifetimeFailure{},attempted{};Error lifecycleError=Error::none;GameAssetError assetError=GameAssetError::none;unsigned lastSubmitted{};}
GameDraw& bindings(){return nativeDraw;}
Renderer& renderer(){return nativeRenderer;}
bool initialize(void* nativeTarget){
    if(attempted||!nativeTarget){lifecycleError=Error::protocol;return false;}
    attempted=true;
    if(!nativeRenderer.initialize({})||!nativeDraw.initialize(nativeRenderer)||!nativeAssets.initialize(nativeRenderer,nativeDraw,"sdmc:/3ds/DuskLight/native")){lifecycleError=Error::backend;return false;}
    mainOutput=static_cast<C3D_RenderTarget*>(nativeTarget);initialized=true;lifecycleError=Error::none;return true;
}
void invalidateCamera(){cameraValid=false;nativeDraw.invalidateInputs();}
bool beginFrame(){invalidateCamera();if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}const bool okay=nativeDraw.beginExternalFrame(mainOutput);if(okay)currentOutput=mainOutput;lifecycleError=nativeDraw.error();return okay;}
bool endFrame(){invalidateCamera();if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}const bool okay=nativeDraw.endExternalFrame();if(okay)currentOutput=nullptr;lifecycleError=nativeDraw.error();return okay;}
unsigned submittedParts(){return lastSubmitted;}
bool frameActive(){return initialized&&nativeDraw.frameActive();}
bool target(void* output){
    lastSubmitted=0;assetError=GameAssetError::none;cameraFailure=false;
    if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}
    if(output!=mainOutput)invalidateCamera(); // A fresh main publication may follow an offscreen pass.
    if(!nativeRenderer.target(static_cast<C3D_RenderTarget*>(output))){lifecycleError=nativeRenderer.error();return false;}
    currentOutput=static_cast<C3D_RenderTarget*>(output);
    const bool okay=!cameraValid||nativeRenderer.scissor(camera.left,camera.top,camera.right,camera.bottom);lifecycleError=nativeRenderer.error();return okay;
}
bool publishCamera(void* output,const Matrix& projection,const CameraRegion& region){
    invalidateCamera();
    if(!initialized||lifetimeFailure||!nativeDraw.frameActive()||!nativeDraw.onOwnerThread()||output!=mainOutput){cameraFailure=true;return false;}
    if(!prepareCamera(projection,region,mainOutput->frameBuf.width,mainOutput->frameBuf.height,camera)){cameraFailure=true;return false;}
    cameraValid=true;cameraFailure=false;return true;
}
bool applyCamera(DrawUniforms& uniforms){
    if(!initialized||lifetimeFailure||!nativeDraw.frameActive()||!cameraValid||currentOutput!=mainOutput)return false;
    uniforms.viewProjection=camera.rows;return true;
}
bool clear(unsigned char r,unsigned char g,unsigned char b,unsigned char a){
    if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}
    const bool okay=nativeRenderer.clear((unsigned(r)<<24)|(unsigned(g)<<16)|(unsigned(b)<<8)|a,0);lifecycleError=nativeRenderer.error();return okay;
}
bool drawShape(const void* instance,unsigned shape){if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}auto result=nativeDraw.draw(instance,shape);lastSubmitted=result.submitted;lifecycleError=result.error;return bool(result);}
bool drawSourceShape(const void* instance,unsigned shape,const SourceIdentity& identity,GameSourceProducer producer,void* user,unsigned inputs){
    lastSubmitted=0;assetError=GameAssetError::none;
    if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}
    if(!cameraValid||currentOutput!=mainOutput){cameraFailure=true;lifecycleError=Error::invalidAsset;return false;}
    cameraFailure=false;
    if(!nativeAssets.bind(instance,identity,producer,user,inputs)){assetError=nativeAssets.error();lifecycleError=Error::invalidAsset;return false;}
    return drawShape(instance,shape);
}
void forget(const void* instance){if(initialized&&(!nativeAssets.forget(instance)||!nativeDraw.forget(instance)))lifetimeFailure=true;}
const char* failure(){
    if(lifetimeFailure)return "Native game model lifetime violation";
    if(cameraFailure)return "Native game camera/pass binding missing or unsupported";
    if(assetError==GameAssetError::textureContext)return "Native external-image context/variant proof missing";
    switch(assetError){case GameAssetError::identity:return "Native game source identity missing or changed";case GameAssetError::missing:return "Source-identified native .dna asset missing";case GameAssetError::io:return "Native .dna asset read failed";case GameAssetError::pack:return "Native .dna asset identity or integrity rejected";case GameAssetError::inputs:return "Native game camera/material/environment producers incomplete";case GameAssetError::allocation:return "Native asset staging allocation failed";case GameAssetError::budget:return "Native asset/instance metadata budget exhausted";case GameAssetError::owner:return "Native asset load attempted outside render owner";case GameAssetError::protocol:return "Native asset admission outside writable render boundary";case GameAssetError::backend:return "Native asset resource admission failed";case GameAssetError::none:case GameAssetError::textureContext:break;}
    switch(lifecycleError){case Error::invalidAsset:return "Native game asset, shape or parameter producer missing";case Error::invalidHandle:return "Native game resident model binding expired";case Error::budget:return "Native game frame/resource budget exhausted";case Error::wrongThread:return "Native game renderer owner mismatch";case Error::none:return "none";default:return "Native game renderer protocol/backend failure";}
}
bool releaseAfterContextDestroyed(){
    if(!initialized)return true;
    if(!nativeDraw.empty()||!nativeAssets.empty()||lifetimeFailure){lifecycleError=Error::protocol;return false;}
    const bool okay=nativeRenderer.releaseAfterContextDestroyed();if(okay){initialized=false;mainOutput=nullptr;}return okay;
}
}
}
