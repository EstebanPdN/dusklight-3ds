#include "renderer.hpp"
#include <algorithm>
#include <cstring>

extern const unsigned char native_scene_shader[];
extern const unsigned int native_scene_shader_size;
extern const unsigned char native_scene_rigid_shader[];
extern const unsigned int native_scene_rigid_shader_size;
extern const unsigned char native_scene_uv_shader[];
extern const unsigned int native_scene_uv_shader_size;
extern const unsigned char native_scene_rigid_uv_shader[];
extern const unsigned int native_scene_rigid_uv_shader_size;
extern const unsigned char native_scene_projective_shader[];
extern const unsigned int native_scene_projective_shader_size;
extern const unsigned char native_scene_rigid_projective_shader[];
extern const unsigned int native_scene_rigid_projective_shader_size;
namespace dusk::pica {
namespace {
constexpr GPU_TEVSRC sources[]{GPU_PRIMARY_COLOR,GPU_TEXTURE0,GPU_TEXTURE1,GPU_TEXTURE2,GPU_CONSTANT,GPU_PREVIOUS,GPU_PREVIOUS_BUFFER};
constexpr GPU_COMBINEFUNC functions[]{GPU_REPLACE,GPU_MODULATE,GPU_ADD,GPU_ADD_SIGNED,GPU_INTERPOLATE,GPU_SUBTRACT,GPU_DOT3_RGB,GPU_MULTIPLY_ADD,GPU_ADD_MULTIPLY};
constexpr GPU_TEVOP_RGB rgbOps[]{GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR,GPU_TEVOP_RGB_SRC_ALPHA,GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,GPU_TEVOP_RGB_SRC_R,GPU_TEVOP_RGB_ONE_MINUS_SRC_R,GPU_TEVOP_RGB_SRC_G,GPU_TEVOP_RGB_ONE_MINUS_SRC_G,GPU_TEVOP_RGB_SRC_B,GPU_TEVOP_RGB_ONE_MINUS_SRC_B};
constexpr GPU_TESTFUNC tests[]{GPU_NEVER,GPU_ALWAYS,GPU_EQUAL,GPU_NOTEQUAL,GPU_LESS,GPU_LEQUAL,GPU_GREATER,GPU_GEQUAL};
constexpr GPU_TEXTURE_WRAP_PARAM wraps[]{GPU_CLAMP_TO_EDGE,GPU_REPEAT,GPU_MIRRORED_REPEAT};
constexpr GPU_TEXTURE_FILTER_PARAM filters[]{GPU_NEAREST,GPU_LINEAR};
constexpr GPU_TEXCOLOR formats[]{GPU_RGBA8,GPU_RGB565,GPU_ETC1,GPU_ETC1A4};
constexpr GPU_BLENDEQUATION blendEquations[]{GPU_BLEND_ADD,GPU_BLEND_SUBTRACT,GPU_BLEND_REVERSE_SUBTRACT,GPU_BLEND_MIN,GPU_BLEND_MAX};
constexpr GPU_BLENDFACTOR blendFactors[]{GPU_ZERO,GPU_ONE,GPU_SRC_COLOR,GPU_ONE_MINUS_SRC_COLOR,GPU_DST_COLOR,GPU_ONE_MINUS_DST_COLOR,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_DST_ALPHA,GPU_ONE_MINUS_DST_ALPHA};
}
bool Renderer::owner(){return ready&&getThreadLocalStorage()==ownerTls?true:fail(ready?Error::wrongThread:Error::protocol);}
bool Renderer::writable(){if(!owner())return false;return !open||!emitted?true:fail(Error::protocol);}
void Renderer::discardShaders(){
    for(unsigned i=0;i<programCount;++i){
        if(programInitialized[i]){shaderProgramFree(&programs[i]);programInitialized[i]=false;}
        if(binaries[i]){DVLB_Free(binaries[i]);binaries[i]=nullptr;}
        uniformLocations[i]=-1;
    }
}
bool Renderer::initialize(const Limits& cfg){
    if(ready)return fail(Error::protocol);
    // Budgets include retired resources, until a real completed frame boundary.
    if(!cfg.meshBytes||cfg.textureBytes<256||cfg.transientBytes<256||cfg.meshBytes>64u<<20||cfg.textureBytes>64u<<20||cfg.transientBytes>4u<<20||
       !cfg.drawsPerFrame||cfg.drawsPerFrame>1024||cfg.commandBytes<32768+std::size_t(cfg.drawsPerFrame)*4096)return fail(Error::budget);
    const unsigned char* blobs[]{native_scene_shader,native_scene_rigid_shader,native_scene_uv_shader,native_scene_rigid_uv_shader,native_scene_projective_shader,native_scene_rigid_projective_shader};
    const unsigned sizes[]{native_scene_shader_size,native_scene_rigid_shader_size,native_scene_uv_shader_size,native_scene_rigid_uv_shader_size,native_scene_projective_shader_size,native_scene_rigid_projective_shader_size};
    for(unsigned i=0;i<programCount;++i){
        binaries[i]=DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(blobs[i])),sizes[i]);
        if(!binaries[i]){discardShaders();return fail(Error::allocation);}
        if(R_FAILED(shaderProgramInit(&programs[i]))){discardShaders();return fail(Error::backend);}
        programInitialized[i]=true;
        if(R_FAILED(shaderProgramSetVsh(&programs[i],&binaries[i]->DVLE[0]))){discardShaders();return fail(Error::backend);}
        uniformLocations[i]=shaderInstanceGetUniformLocation(programs[i].vertexShader,"u");
        if(uniformLocations[i]<0||uniformLocations[i]+(i<2?57:i<4?64:68)>96){discardShaders();return fail(Error::backend);}
    }
    transient=static_cast<std::byte*>(linearMemAlign(cfg.transientBytes,16));
    if(!transient){discardShaders();return fail(Error::allocation);}
    if(!C3D_TexInit(&unusedSampler,8,8,GPU_RGBA8)){
        linearFree(transient);transient=nullptr;discardShaders();return fail(Error::allocation);
    }
    std::memset(unusedSampler.data,255,256);
    if(R_FAILED(GSPGPU_FlushDataCache(unusedSampler.data,256))){
        C3D_TexDelete(&unusedSampler);linearFree(transient);transient=nullptr;discardShaders();return fail(Error::publication);
    }
    C3D_TexSetFilter(&unusedSampler,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&unusedSampler,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    limits=cfg;stats={};stats.textureBytes=256;ownerTls=getThreadLocalStorage();ready=true;failure=Error::none;return true;
}
void Renderer::collect(){
    // Called only AFTER a successful NONBLOCK FrameBegin has drained the old
    // queue. Retired pointers are never freed on a busy/failed begin.
    for(auto& s:meshes)if(s.retired){stats.meshBytes-=s.payload.info.bytes;linearFree(s.payload.data);recycle(s);}
    for(auto& s:textures)if(s.retired){stats.textureBytes-=s.payload.bytes;C3D_TexDelete(&s.payload.value);recycle(s);}
    for(auto& s:materials)if(s.retired)recycle(s);
}
bool Renderer::begin(C3D_RenderTarget* output){
    if(!owner())return false;
    if(open||!output||!output->frameBuf.colorBuf||!output->frameBuf.width||!output->frameBuf.height)return fail(Error::protocol);
    if(!C3D_FrameBegin(C3D_FRAME_NONBLOCK))return fail(Error::busy);
    collect();open=true;emitted=false;stats.frameDraws=0;stats.transientBytes=0;
    stats.previousGpuMilliseconds=C3D_GetDrawingTime();stats.previousCpuMilliseconds=C3D_GetProcessingTime();
    if(!target(output)){C3D_FrameEnd(0);open=false;return false;}
    ++stats.frames;failure=Error::none;return true;
}
bool Renderer::target(C3D_RenderTarget* output){
    if(!owner())return false;
    if(!open||!output||!output->frameBuf.colorBuf||!output->frameBuf.width||!output->frameBuf.height)return fail(Error::protocol);
    if(!C3D_FrameDrawOn(output))return fail(Error::backend);
    width=output->frameBuf.width;height=output->frameBuf.height;activeTarget=output;passEmitted=false;
    hasDepth=output->frameBuf.depthBuf!=nullptr;
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,width,height);++stateSerial;return true;
}
bool Renderer::clear(std::uint32_t color,std::uint32_t depth,bool colorBuffer,bool depthBuffer){
    if(!owner())return false;
    if(!open||passEmitted||(!colorBuffer&&!depthBuffer)||(depthBuffer&&!hasDepth))return fail(Error::protocol);
    // SDK clears enqueue a GSP fill immediately; prior draw commands otherwise
    // remain in the CPU command list until FrameEnd. Publish that list first
    // so a later pass clear cannot execute before the earlier pass's draws.
    // Native queue ordering only: no synchronous copy or blocking GPU wait.
    if(emitted)C3D_FrameSplit(GX_CMDLIST_FLUSH);
    C3D_RenderTargetClear(activeTarget,C3D_ClearBits((colorBuffer?C3D_CLEAR_COLOR:0)|(depthBuffer?C3D_CLEAR_DEPTH:0)),color,depth);
    ++stateSerial;
    return true;
}
bool Renderer::scissor(unsigned left,unsigned top,unsigned right,unsigned bottom){
    if(!owner())return false;
    if(!open||left>=right||top>=bottom||right>width||bottom>height)return fail(Error::invalidAsset);
    C3D_SetScissor(GPU_SCISSOR_NORMAL,left,top,right,bottom);++stateSerial;return true;
}
bool Renderer::end(){
    if(!owner())return false;
    if(!open)return fail(Error::protocol);
    // This is the native 3DS queue flag, not a GameCube API. All vertex/texture
    // uploads were checked/flushed individually; flush only the command list.
    C3D_FrameEnd(GX_CMDLIST_FLUSH);open=false;failure=Error::none;return true;
}
MeshHandle Renderer::createMesh(std::span<const Vertex> v,std::span<const std::uint16_t> ix){
    if(!writable())return {};
    GeometryInfo info;if(!inspectGeometry(v,ix,info)){fail(Error::invalidAsset);return {};}
    return uploadMesh(std::as_bytes(v),std::as_bytes(ix),info);
}
MeshHandle Renderer::createMeshBytes(std::span<const std::byte> v,std::span<const std::byte> ix){
    if(!writable())return {};
    GeometryInfo info;if(!inspectPackedGeometry(v,ix,info)){fail(Error::invalidAsset);return {};}
    return uploadMesh(v,ix,info);
}
MeshHandle Renderer::uploadMesh(std::span<const std::byte> v,std::span<const std::byte> ix,const GeometryInfo& info){
    int i=vacant(meshes);if(i<0||info.bytes>limits.meshBytes-stats.meshBytes){fail(Error::budget);return {};}
    void* data=linearMemAlign(info.bytes,16);if(!data){fail(Error::allocation);return {};}
    std::memcpy(data,v.data(),v.size());std::memcpy(static_cast<std::byte*>(data)+align16(v.size()),ix.data(),ix.size());
    if(R_FAILED(GSPGPU_FlushDataCache(data,info.bytes))){linearFree(data);fail(Error::publication);return {};}
    auto& s=meshes[i];s.payload={data,info};s.live=true;stats.meshBytes+=info.bytes;failure=Error::none;return {token(s,unsigned(i))};
}
TextureHandle Renderer::createTexture(const TextureInfo& info,std::span<const std::byte> bytes){
    if(!writable())return {};
    const auto size=textureBytes(info);if(!size||size!=bytes.size()){fail(Error::invalidAsset);return {};}
    int i=vacant(textures);if(i<0||size>limits.textureBytes-stats.textureBytes){fail(Error::budget);return {};}
    C3D_Tex tex{};
    C3D_TexInitParams params{info.width,info.height,u8(info.mipLevels-1),formats[unsigned(info.format)],GPU_TEX_2D,false};
    if(!C3D_TexInitWithParams(&tex,nullptr,params)){fail(Error::allocation);return {};}
    const auto actual=C3D_TexCalcTotalSize(tex.size,tex.maxLevel);
    if(actual!=size){C3D_TexDelete(&tex);fail(Error::backend);return {};}
    std::memcpy(tex.data,bytes.data(),size);
    if(R_FAILED(GSPGPU_FlushDataCache(tex.data,size))){C3D_TexDelete(&tex);fail(Error::publication);return {};}
    C3D_TexSetFilter(&tex,info.linear?GPU_LINEAR:GPU_NEAREST,info.linear?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetFilterMipmap(&tex,info.linear?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetWrap(&tex,wraps[unsigned(info.wrapS)],wraps[unsigned(info.wrapT)]);
    auto& s=textures[i];s.payload={tex,size};s.live=true;stats.textureBytes+=size;failure=Error::none;return {token(s,unsigned(i))};
}
MaterialHandle Renderer::createMaterial(const Material& state){
    if(!writable())return {};
    if(!validateMaterial(state)){fail(Error::invalidAsset);return {};}
    int i=vacant(materials);if(i<0){fail(Error::budget);return {};}
    auto& slot=materials[i];auto& p=slot.payload;p.state=state;
    p.parameterMask=p.parameterStages=0;
    for(unsigned n=0;n<6;++n){auto& env=p.env[n];C3D_TexEnvInit(&env);if(n>=state.stageCount)continue;
        const auto& s=state.stages[n];
        for(auto binding:s.constantBindings)if(binding){p.parameterMask|=1u<<((binding-1)/4);p.parameterStages|=1u<<n;}
        C3D_TexEnvSrc(&env,C3D_RGB,sources[unsigned(s.rgb[0])],sources[unsigned(s.rgb[1])],sources[unsigned(s.rgb[2])]);
        C3D_TexEnvSrc(&env,C3D_Alpha,sources[unsigned(s.alpha[0])],sources[unsigned(s.alpha[1])],sources[unsigned(s.alpha[2])]);
        C3D_TexEnvOpRgb(&env,rgbOps[unsigned(s.rgbOperand[0])],rgbOps[unsigned(s.rgbOperand[1])],rgbOps[unsigned(s.rgbOperand[2])]);
        C3D_TexEnvOpAlpha(&env,GPU_TEVOP_A(s.alphaOperand[0]),GPU_TEVOP_A(s.alphaOperand[1]),GPU_TEVOP_A(s.alphaOperand[2]));
        C3D_TexEnvFunc(&env,C3D_RGB,functions[unsigned(s.rgbFunction)]);C3D_TexEnvFunc(&env,C3D_Alpha,functions[unsigned(s.alphaFunction)]);
        C3D_TexEnvScale(&env,C3D_RGB,GPU_TEVSCALE(s.rgbScale));C3D_TexEnvScale(&env,C3D_Alpha,GPU_TEVSCALE(s.alphaScale));C3D_TexEnvColor(&env,s.constant);
    }
    if(state.fog)FogLut_FromArray(&p.fog,state.fogValues.data()); // Stable slot storage; never a local LUT pointer.
    slot.live=true;failure=Error::none;return {token(slot,unsigned(i))};
}
bool Renderer::release(MeshHandle h){return retire(meshes,h.value);}
bool Renderer::release(TextureHandle h){return retire(textures,h.value);}
bool Renderer::release(MaterialHandle h){return retire(materials,h.value);}
bool Renderer::draw(MeshHandle mh,MaterialHandle ph,const std::array<TextureHandle,3>& tx,const DrawUniforms& u){
    if(!owner())return false;
    auto* mesh=lookup(meshes,mh.value);auto* material=lookup(materials,ph.value);
    if(!mesh||!material)return fail(Error::invalidHandle);
    const auto& m=mesh->payload;return emit(static_cast<const Vertex*>(m.data),reinterpret_cast<const std::uint16_t*>(static_cast<const std::byte*>(m.data)+align16(m.info.vertices*sizeof(Vertex))),m.info,material->payload,tx,u);
}
bool Renderer::drawTransient(std::span<const Vertex> v,std::span<const std::uint16_t> ix,MaterialHandle ph,const std::array<TextureHandle,3>& tx,const DrawUniforms& u){
    if(!owner())return false;
    if(!open)return fail(Error::protocol);
    auto* material=lookup(materials,ph.value);if(!material)return fail(Error::invalidHandle);
    GeometryInfo info;if(!inspectGeometry(v,ix,info)||!validateUniforms(u,info.highestBone))return fail(Error::invalidAsset);
    if(info.bytes>limits.transientBytes-stats.transientBytes)return fail(Error::budget);
    auto* data=transient+stats.transientBytes;
    std::memcpy(data,v.data(),v.size_bytes());std::memcpy(data+align16(v.size_bytes()),ix.data(),ix.size_bytes());
    if(R_FAILED(GSPGPU_FlushDataCache(data,info.bytes)))return fail(Error::publication);
    // Keep copied bytes until the next completed begin, including failed emits.
    stats.transientBytes+=info.bytes;
    return emit(reinterpret_cast<const Vertex*>(data),reinterpret_cast<const std::uint16_t*>(data+align16(v.size_bytes())),info,material->payload,tx,u);
}
bool Renderer::emit(const Vertex* v,const std::uint16_t* ix,const GeometryInfo& info,Prepared& p,const std::array<TextureHandle,3>& tx,const DrawUniforms& u){
    if(!open)return fail(Error::protocol);
    if(stats.frameDraws>=limits.drawsPerFrame)return fail(Error::budget);
    if((p.state.depthTest||p.state.depthWrite)&&!hasDepth)return fail(Error::invalidAsset);
    if(!validateUniforms(u,info.highestBone))return fail(Error::invalidAsset);
    if((u.material.suppliedMask&p.parameterMask)!=p.parameterMask)return fail(Error::invalidAsset);
    if(u.samplerMask&~p.state.textureMask)return fail(Error::invalidAsset);
    const bool projected=u.projective.enabled&&(p.state.textureMask&1);
    // The complete projective matrix owns unit0 coordinates. Do not silently
    // ignore a conflicting independent unit0 affine/source transform.
    if(projected&&!u.uvTransforms[0].passthrough(0))return fail(Error::invalidAsset);
    std::array<C3D_Tex*,3> bound{};
    for(unsigned n=0;n<3;++n){bound[n]=&unusedSampler;if(p.state.textureMask&(1u<<n)){
        auto* t=lookup(textures,tx[n].value);if(!t)return fail(Error::invalidHandle);bound[n]=&t->payload.value;
        if((u.samplerMask&(1u<<n))&&!validateSampler(u.samplers[n],bound[n]->maxLevel+1))return fail(Error::invalidAsset);
    }}
    auto* attr=C3D_GetAttrInfo();AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr,0,GPU_FLOAT,3);AttrInfo_AddLoader(attr,1,GPU_FLOAT,3);
    for(unsigned n=2;n<5;++n)AttrInfo_AddLoader(attr,n,GPU_FLOAT,2);
    for(unsigned n=5;n<8;++n)AttrInfo_AddLoader(attr,n,GPU_UNSIGNED_BYTE,4);
    auto* buffers=C3D_GetBufInfo();BufInfo_Init(buffers);
    if(BufInfo_Add(buffers,v,sizeof(Vertex),8,0x76543210)<0)return fail(Error::backend);
    Row uvEnabled{};bool uvNeeded=false;
    for(unsigned n=0;n<3;++n)if((p.state.textureMask&(1u<<n))&&!u.uvTransforms[n].passthrough(n)){uvEnabled[n]=1;uvNeeded=true;}
    const unsigned variant=(info.rigidFirst?1:0)+(projected?4:uvNeeded?2:0);C3D_BindProgram(&programs[variant]);
    auto put=[&](unsigned row,const Row& x){C3D_FVUnifSet(GPU_VERTEX_SHADER,uniformLocations[variant]+row,x[0],x[1],x[2],x[3]);};
    // Only paletteCount rows are fetched by validated nonzero-weight vertices.
    for(unsigned n=0;n<u.palette.count*3;++n){put(n,u.palette.position[n]);put(24+n,u.palette.normal[n]);}
    for(unsigned n=0;n<4;++n)put(48+n,u.viewProjection[n]);
    put(52,{u.ambient[0],u.ambient[1],u.ambient[2],u.lit?1.f:0.f});
    for(unsigned n=0;n<2;++n){const auto& l=u.lights[n];put(53+2*n,{l.toward[0],l.toward[1],l.toward[2],0});put(54+2*n,{l.color[0],l.color[1],l.color[2],0});}
    if(uvNeeded||projected){
        for(unsigned n=0;n<3;++n){
            const auto& transform=u.uvTransforms[n];
            for(unsigned r=0;r<2;++r){const auto& x=transform.rows[r];put(57+2*n+r,{x[0],x[1],x[2],r==0?float(transform.source):0.f});}
        }
        put(63,uvEnabled);
    }
    if(projected){for(unsigned n=0;n<3;++n)put(64+n,u.projective.rows[n]);put(67,{u.projective.source==PositionSource::posed?1.f:0.f,0,0,0});}
    for(unsigned n=0;n<3;++n){
        // One image may be projected in unit0 and ordinary2D in unit1/2.
        // Citro3D rejects a non2D descriptor on those latter units. Retain
        // separate stable views without duplicating or mutating owned images.
        auto* sampler=bound[n];
        if((u.samplerMask&(1u<<n))||(n==0&&projected)){
            auto& view=samplerViews[n];view=*sampler;
            if(u.samplerMask&(1u<<n)){
                const auto& sampling=u.samplers[n];
                C3D_TexSetFilter(&view,filters[unsigned(sampling.magnify)],filters[unsigned(sampling.minify)]);
                C3D_TexSetFilterMipmap(&view,filters[unsigned(sampling.mipmap)]);
                C3D_TexSetWrap(&view,wraps[unsigned(sampling.wrapS)],wraps[unsigned(sampling.wrapT)]);
                view.lodBias=std::uint16_t(sampling.lodBias)&0x1fff;
                view.minLevel=sampling.firstLevel;view.maxLevel=sampling.lastLevel;
            }
            if(n==0&&projected)view.param=(view.param&~GPU_TEXTURE_MODE(7))|GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
            sampler=&view;
        }
        C3D_TexBind(n,sampler);
    }
    for(unsigned n=0;n<6;++n){
        if(!(p.parameterStages&(1u<<n))){C3D_SetTexEnv(n,&p.env[n]);continue;}
        // SetTexEnv copies the native 20-byte SDK object into its context.
        // Keep the immutable prepared template unchanged between instances.
        auto env=p.env[n];auto color=p.state.stages[n].constant;
        for(unsigned c=0;c<4;++c)if(auto binding=p.state.stages[n].constantBindings[c]){
            const unsigned source=binding-1;
            const auto value=(u.material.colors[source/4]>>(8*(source%4)))&255u;
            color=(color&~(255u<<(8*c)))|(value<<(8*c));
        }
        C3D_TexEnvColor(&env,color);C3D_SetTexEnv(n,&env);
    }
    const auto& s=p.state;
    C3D_TexEnvBufUpdate(C3D_RGB,s.bufferRgbMask);C3D_TexEnvBufUpdate(C3D_Alpha,s.bufferAlphaMask);C3D_TexEnvBufColor(s.bufferColor);
    C3D_LightEnvBind(nullptr); // Vertex shader owns this native lighting model.
    C3D_FragOpMode(GPU_FRAGOPMODE_GL);C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
    C3D_DepthMap(true,-1,0);
    C3D_CullFace(s.cull==Cull::none?GPU_CULL_NONE:s.cull==Cull::front?GPU_CULL_FRONT_CCW:GPU_CULL_BACK_CCW);
    C3D_StencilTest(false,GPU_ALWAYS,0,255,0);C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
    C3D_DepthTest(s.depthTest,tests[unsigned(s.depth)],GPU_WRITEMASK((s.colorWrite?7:0)|(s.alphaWrite?8:0)|(s.depthWrite?16:0)));
    C3D_AlphaTest(s.alpha!=Compare::always,tests[unsigned(s.alpha)],s.alphaReference);
    if(s.blend==Blend::custom){const auto& b=s.customBlend;C3D_AlphaBlend(blendEquations[unsigned(b.color)],blendEquations[unsigned(b.alpha)],blendFactors[unsigned(b.colorSource)],blendFactors[unsigned(b.colorDestination)],blendFactors[unsigned(b.alphaSource)],blendFactors[unsigned(b.alphaDestination)]);}
    else if(s.blend==Blend::opaque)C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    else C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,s.blend==Blend::alpha?GPU_ONE_MINUS_SRC_ALPHA:GPU_ONE,GPU_ONE,s.blend==Blend::alpha?GPU_ONE_MINUS_SRC_ALPHA:GPU_ONE);
    C3D_FogGasMode(s.fog?GPU_FOG:GPU_NO_FOG,GPU_PLAIN_DENSITY,s.fogDepthFlip);
    if(s.fog){C3D_FogColor(s.fogColor);C3D_FogLutBind(&p.fog);}
    C3D_DrawElements(GPU_TRIANGLES,info.indices,C3D_UNSIGNED_SHORT,ix);
    if(info.rigidFirst)++stats.rigidDraws;else ++stats.weightedDraws;
    if(uvNeeded)++stats.uvDraws;
    if(projected)++stats.projectiveDraws;
    emitted=passEmitted=true;++stats.draws;++stats.frameDraws;stats.triangles+=info.indices/3;failure=Error::none;return true;
}
bool Renderer::releaseAfterContextDestroyed(){
    // Citro3D retains program/LUT/texture pointers. The caller must have
    // destroyed its exclusive C3D context (and completed its queue) FIRST.
    if(!owner())return false;
    if(open)return fail(Error::protocol);
    for(auto& s:meshes)if(s.live||s.retired){linearFree(s.payload.data);s.live=false;s.retired=true;recycle(s);}
    for(auto& s:textures)if(s.live||s.retired){C3D_TexDelete(&s.payload.value);s.live=false;s.retired=true;recycle(s);}
    for(auto& s:materials)if(s.live||s.retired){s.live=false;s.retired=true;recycle(s);}
    C3D_TexDelete(&unusedSampler);linearFree(transient);discardShaders();
    samplerViews={};transient=nullptr;ready=false;ownerTls=nullptr;stats={};failure=Error::none;return true;
}
}
