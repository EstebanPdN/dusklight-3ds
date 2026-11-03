#include "game_draw.hpp"
#include "game_assets.hpp"
#include "camera.hpp"
#include "game_draw_api.hpp"
#include "game_limits.hpp"
#if defined(__3DS__)
#include "../runtime/gpu_sync.hpp"
#elif defined(DUSK_NATIVE_GAME_DIAGNOSTIC_TEST)
#include "runtime/gpu_sync.hpp"
#endif
#include <utility>
#if defined(__3DS__) || defined(DUSK_NATIVE_GAME_DIAGNOSTIC_TEST)
extern "C" void OSReport(const char*,...);
#endif
namespace dusk::pica {
namespace {
#if defined(__3DS__) || defined(DUSK_NATIVE_GAME_DIAGNOSTIC_TEST)
void reportInvalidDraw(const char* stage,unsigned shape,int part,int slot){
    OSReport("Native draw rejected stage=%s shape=%u part=%d slot=%d\n",stage,shape,part,slot);
}
void reportInvalidTarget(){
    OSReport("Native draw rejected stage=target-scissor shape=unavailable part=-1 slot=-1\n");
}
void reportInvalidSource(const SourceIdentity& source){
    constexpr char digits[]="0123456789abcdef";std::array<char,65> hash{},context{};
    for(unsigned n=0;n<32;++n){
        hash[n*2]=digits[source.sha256[n]>>4];hash[n*2+1]=digits[source.sha256[n]&15];
        context[n*2]=digits[source.imageContext[n]>>4];context[n*2+1]=digits[source.imageContext[n]&15];
    }
    OSReport("Native draw source_sha256=%s image_context_sha256=%s\n",hash.data(),source.hasImageContext?context.data():"unavailable");
}
#else
void reportInvalidDraw(const char*,unsigned,int,int){}
void reportInvalidTarget(){}
void reportInvalidSource(const SourceIdentity&){}
#endif
}
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
    const auto bytes=shapes.size()*sizeof(std::uint16_t);
    if(bytes>maxBindingBytes-bindingBytes)return fail(Error::budget);
    unsigned required=0;
    // Cold binding only: count DISTINCT native slots for each source shape.
    // The actor-wide uniform ID remains independent of the dense workspace.
    for(unsigned first=0;first<shapes.size();++first){
        unsigned previous=0;while(previous<first&&shapes[previous]!=shapes[first])++previous;
        if(previous!=first)continue;
        denseSlots.fill(std::uint16_t(maxModelParts));unsigned distinct=0;
        for(unsigned n=first;n<shapes.size();++n)if(shapes[n]==shapes[first]){
            auto& slot=denseSlots[model.sequence[n].uniform];
            if(slot==maxModelParts){slot=0;++distinct;}
        }
        if(distinct>required)required=distinct;
    }
    if(required>renderer->limits.uniformBytes/sizeof(DrawUniforms))return fail(Error::budget);
    auto copied=allocateStorage<std::uint16_t>(shapes.size());if(!copied)return fail(Error::allocation);
    Storage<DrawUniforms> grown;
    if(required>uniformCapacity){grown=allocateStorage<DrawUniforms>(required);if(!grown)return fail(Error::allocation);}
    for(unsigned n=0;n<shapes.size();++n)copied[n]=std::uint16_t(shapes[n]);
    if(grown){uniforms=std::move(grown);uniformCapacity=required;}
    vacant->instance=instance;vacant->model=&model;vacant->producer=producer;vacant->user=user;vacant->revision=model.revision;
    vacant->shapes=std::move(copied);vacant->count=unsigned(shapes.size());bindingBytes+=bytes;
    failure=Error::none;return true;
}
bool GameDraw::forget(const void* instance){
    if(!renderer)return true;
    if(submitting){invalidated=true;return fail(Error::protocol);}
    if(!renderer->owner())return fail(renderer->error());
    for(auto& slot:bindings)if(slot.instance==instance){bindingBytes-=std::size_t(slot.count)*sizeof(std::uint16_t);slot={};}
    if(empty()){uniforms.reset();uniformCapacity=0;}
    failure=Error::none;return true;
}
bool GameDraw::empty()const{for(const auto& slot:bindings)if(slot.instance)return false;return true;}
Submission GameDraw::draw(const void* instance,unsigned shape){
    const auto reject=[&](Error e,unsigned submitted=0){failure=e;return Submission{submitted,e};};
    if(submitting){invalidated=true;return reject(Error::protocol);}
    if(!renderer)return reject(Error::protocol);
    auto& r=*renderer;if(!r.owner())return reject(r.error());if(!r.open||!externalOpen)return reject(Error::protocol);
    Binding* binding=nullptr;for(auto& slot:bindings)if(slot.instance==instance){binding=&slot;break;}
    if(!instance||!binding){reportInvalidDraw("binding",shape,-1,-1);return reject(Error::invalidAsset);}
    auto& model=*binding->model;
    if(!model.loaded||model.renderer!=renderer||model.revision!=binding->revision)return reject(Error::invalidHandle);
    unsigned count=0;denseSlots.fill(std::uint16_t(maxModelParts));
    for(unsigned n=0;n<model.partCount;++n)if(binding->shapes[n]==shape){selection[count++]=std::uint16_t(n);denseSlots[model.sequence[n].uniform]=0;}
    if(!count){reportInvalidDraw("source-shape",shape,-1,-1);return reject(Error::invalidAsset);}
    if(count>r.limits.drawsPerFrame-r.stats.frameDraws){reportInvalidDraw("frame-draw-budget",shape,int(count),int(r.stats.frameDraws));return reject(Error::budget);}
    submitting=true;invalidated=false;
    struct Scope {bool& active;~Scope(){active=false;}} scope{submitting};
    unsigned dense=0;
    for(unsigned native=0;native<maxModelParts;++native)if(denseSlots[native]!=maxModelParts){
        if(dense>=uniformCapacity){reportInvalidDraw("uniform-capacity",shape,int(dense),int(uniformCapacity));return reject(Error::budget);}
        denseSlots[native]=std::uint16_t(dense);
        uniforms[dense]={};if(!binding->producer(instance,native,uniforms[dense],binding->user)){
            reportInvalidDraw("producer",shape,-1,int(native));return reject(Error::invalidAsset);
        }
        if(invalidated)return reject(Error::protocol);
        if(!model.loaded||model.renderer!=renderer||model.revision!=binding->revision)return reject(Error::invalidHandle);
        ++dense;
    }
    // A producer cannot silently unload/reload the resident asset under the draw.
    if(invalidated)return reject(Error::protocol);
    if(!model.loaded||model.renderer!=renderer||model.revision!=binding->revision)return reject(Error::invalidHandle);
    if(!r.owner())return reject(r.error());
    if(!r.open||!externalOpen)return reject(Error::protocol);
    if(count>r.limits.drawsPerFrame-r.stats.frameDraws)return reject(Error::budget);
    for(unsigned n=0;n<count;++n){
        const auto part=selection[n];const auto error=model.preflightPart(part,uniforms[denseSlots[model.sequence[part].uniform]]);
        if(error!=Error::none){
            if(error==Error::invalidAsset)reportInvalidDraw("part-preflight",shape,int(part),int(model.sequence[part].uniform));
            model.failure=error;return reject(error);
        }
    }
    unsigned submitted=0;
    for(unsigned n=0;n<count;++n){
        const auto part=selection[n];
        if(!model.submitPart(part,uniforms[denseSlots[model.sequence[part].uniform]])){
            const auto error=model.error();
            if(error==Error::invalidAsset){
                reportInvalidDraw("part-submit",shape,int(part),int(model.sequence[part].uniform));
#if defined(__3DS__)
                if(const auto* detail=r.errorDetail())OSReport("Native draw submit reason=%.128s\n",detail);
#endif
            }
            return reject(error,submitted);
        }
        ++submitted;
    }
    model.failure=Error::none;failure=Error::none;return {submitted,Error::none};
}
namespace game {
namespace {Renderer nativeRenderer;GameDraw nativeDraw;GameAssets nativeAssets;C3D_RenderTarget* mainOutput{};C3D_RenderTarget* currentOutput{};CameraProjection camera;CameraRegion cameraRegion;bool cameraValid{},cameraFailure{};bool initialized{},lifetimeFailure{},attempted{};Error lifecycleError=Error::none;GameAssetError assetError=GameAssetError::none;unsigned lastSubmitted{};}
namespace {
const void* mirrorModel{};
unsigned mirrorShape{},mirrorMaterial{};
std::array<std::uint8_t,4> mirrorAmbient{};
}
GameDraw& bindings(){return nativeDraw;}
Renderer& renderer(){return nativeRenderer;}
bool initialize(void* nativeTarget){
#if defined(__3DS__) || defined(DUSK_NATIVE_GAME_DIAGNOSTIC_TEST)
    const auto prepareDraw=&dusk::native::platformPrepareGpuDraw;
#else
    constexpr bool(*prepareDraw)()=nullptr;
#endif
#if defined(__3DS__)
    const auto uploadBarrier=+[]{
        using namespace dusk::native;
        return platformCompleteGpu(500000000)==GpuCompletion::complete&&
            platformReserveGpuEntries(2,500000000)==GpuCompletion::complete;
    };
#endif
    if(attempted||!nativeTarget){lifecycleError=Error::protocol;return false;}
    attempted=true;
    if(!nativeRenderer.initialize(nativeGameLimits,prepareDraw)
#if defined(__3DS__)
       ||!nativeRenderer.setTextureUploadBarrier(uploadBarrier)
#endif
       ||!nativeDraw.initialize(nativeRenderer)||!nativeAssets.initialize(nativeRenderer,nativeDraw,"sdmc:/3ds/DuskLight/native")){lifecycleError=Error::backend;return false;}
    mainOutput=static_cast<C3D_RenderTarget*>(nativeTarget);initialized=true;lifecycleError=Error::none;return true;
}
bool setFramebufferProvider(bool(*provider)(void*&,unsigned&,unsigned&)){
    if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}
    const bool okay=nativeRenderer.setFramebufferProvider(provider);lifecycleError=nativeRenderer.error();return okay;
}
void invalidateCamera(){cameraValid=false;nativeDraw.invalidateInputs();}
bool beginFrame(){invalidateCamera();if(!initialized||lifetimeFailure||mirrorModel){lifecycleError=Error::protocol;return false;}const bool okay=nativeDraw.beginExternalFrame(mainOutput);if(okay)currentOutput=mainOutput;lifecycleError=nativeDraw.error();return okay;}
bool endFrame(){invalidateCamera();if(!initialized||lifetimeFailure||mirrorModel){lifecycleError=Error::protocol;return false;}const bool okay=nativeDraw.endExternalFrame();if(okay)currentOutput=nullptr;lifecycleError=nativeDraw.error();return okay;}
unsigned submittedParts(){return lastSubmitted;}
bool frameActive(){return initialized&&nativeDraw.frameActive();}
bool target(void* output){
    lastSubmitted=0;assetError=GameAssetError::none;cameraFailure=false;
    if(!initialized||lifetimeFailure){lifecycleError=Error::protocol;return false;}
    if(output!=mainOutput)invalidateCamera(); // A fresh main publication may follow an offscreen pass.
    if(!nativeRenderer.target(static_cast<C3D_RenderTarget*>(output))){lifecycleError=nativeRenderer.error();return false;}
    currentOutput=static_cast<C3D_RenderTarget*>(output);
    const bool okay=!cameraValid||nativeRenderer.scissor(camera.left,camera.top,camera.right,camera.bottom);
    lifecycleError=nativeRenderer.error();
    if(!okay&&lifecycleError==Error::invalidAsset)reportInvalidTarget();
    return okay;
}
bool publishCamera(void* output,const Matrix& projection,const CameraRegion& region){
    invalidateCamera();
    if(!initialized||lifetimeFailure||!nativeDraw.frameActive()||!nativeDraw.onOwnerThread()||output!=mainOutput){cameraFailure=true;return false;}
    if(!prepareCamera(projection,region,mainOutput->frameBuf.width,mainOutput->frameBuf.height,camera)){cameraFailure=true;return false;}
    cameraRegion=region;cameraValid=true;cameraFailure=false;return true;
}
bool publishCamera(void* output,const Matrix& projection,const CameraRegion& region,const CameraScissor& scissor){
    if(!publishCamera(output,projection,region))return false;
    if(!restrictCameraScissor(camera,region,scissor,mainOutput->frameBuf.width,mainOutput->frameBuf.height)){
        invalidateCamera();cameraFailure=true;return false;
    }
    return true;
}
bool beginMirrorDraw(const void* model,unsigned shape,unsigned material,const std::array<std::uint8_t,4>& ambient){
    if(!initialized||lifetimeFailure||!nativeDraw.frameActive()||!nativeDraw.onOwnerThread())return false;
    nativeDraw.invalidateInputs();
    // The packet boundary binds the main target after this scope begins.
    if(mirrorModel||!cameraValid||!model||shape>=4096||!material||material>4096)return false;
    mirrorModel=model;mirrorShape=shape;mirrorMaterial=material;mirrorAmbient=ambient;return true;
}
bool applyMirrorDraw(const void* model,const UniformBinding& binding,DrawUniforms& out){
    if(!mirrorModel)return true;
    if(model!=mirrorModel||binding.sourceShape!=mirrorShape||binding.material!=mirrorMaterial)return false;
    // GFSetChanAmbColor writes the entire XF ambient register, including alpha.
    for(unsigned i=0;i<4;++i)out.ambientColors[0][i]=mirrorAmbient[i]/255.0f;
    out.cullOverride=CullOverride::back; // GX_CULL_FRONT has the opposite native winding.
    return true;
}
bool endMirrorDraw(){
    if(!initialized||!nativeDraw.frameActive()||!nativeDraw.onOwnerThread()||!mirrorModel)return false;
    nativeDraw.invalidateInputs();mirrorModel=nullptr;return true;
}
bool applyCamera(DrawUniforms& uniforms,unsigned fogCenter){
    if(!initialized||lifetimeFailure||!nativeDraw.frameActive()||!cameraValid||currentOutput!=mainOutput)return false;
    uniforms.viewProjection=camera.rows;
    auto& fog=uniforms.fog;
    if(fog.rangeEnabled){
        if(!fog.provided)return false;
        // Native clip Y is -logical clip X, including the published viewport
        // translation/scale. Undo the canvas mapping, normalize to viewport
        // half-width, then let the shader measure distance from declared center.
        fog.rangeScale=-cameraRegion.canvasWidth/cameraRegion.width;
        fog.rangeBias=(cameraRegion.canvasWidth-2*float(fogCenter))/cameraRegion.width;
    }
    if(fog.enabled){
        if(!fog.provided)return false;
        const float span=cameraRegion.farDepth-cameraRegion.nearDepth;
        if(span==0)return false;
        // The shader sees native depth after viewport mapping. Express the
        // original source fog function in that coordinate, without rebuilding
        // a projection or guessing the game's near/far planes.
        if(fog.orthographic){
            fog.a/=span;
            fog.c+=fog.a*cameraRegion.nearDepth;
        }else{
            fog.a*=span;
            fog.b=fog.b*span+cameraRegion.nearDepth;
        }
    }
    return true;
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
    if(!nativeAssets.bind(instance,identity,producer,user,inputs)){
        assetError=nativeAssets.error();
        if(assetError==GameAssetError::textureContext)reportInvalidSource(identity);
        lifecycleError=Error::invalidAsset;return false;
    }
    const bool okay=drawShape(instance,shape);
    if(!okay&&lifecycleError==Error::invalidAsset)reportInvalidSource(identity);
    return okay;
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
