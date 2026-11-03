#include "native/renderer.hpp"
#include "native/quad_batch.hpp"
#include "native/pack.hpp"
#include "native/texture_pack.hpp"
#include "native/material_pack.hpp"
#include "native/model.hpp"
#include "native/scene.hpp"
#include "native/pose.hpp"
#include "native/game_draw.hpp"
#include "native/game_limits.hpp"
#include "native/game_assets.hpp"
#include "native_game_packet_fixture.hpp"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <memory>
#include <fstream>
#include <iterator>
#include <bit>

static char nativeReport[256];
extern "C" void OSReport(const char* format,...){
    va_list args;va_start(args,format);std::vsnprintf(nativeReport,sizeof(nativeReport),format,args);va_end(args);
}
namespace dusk::native {
unsigned hostPrepareCalls{};
bool failHostPrepare{};
bool platformPrepareGpuDraw(){++hostPrepareCalls;return !failHostPrepare;}
}

#include "native_shader_fixture.hpp"
using namespace dusk::pica;
namespace {
std::array<Vertex,4> vertices(){
    std::array<Vertex,4> v{};
    for(unsigned i=0;i<4;++i){v[i].position={float(i&1),float(i>>1),0};v[i].normal={0,0,1};v[i].uv[0]={float(i&1),float(i>>1)};}
    v[0].color={17,34,51,68};v[0].color1={85,102,119,136};
    return v;
}
constexpr std::array<std::uint16_t,6> indices{0,1,2,2,1,3};
Material material(){Material m;m.stages[0].rgb={Source::texture0,Source::primary,Source::constant};m.stages[0].alpha=m.stages[0].rgb;m.stages[0].rgbFunction=m.stages[0].alphaFunction=Combine::modulate;m.textureMask=1;return m;}
DrawUniforms uniforms(){DrawUniforms u;for(unsigned i=0;i<4;++i)u.viewProjection[i][i]=1;return u;}
void contracts(){
    auto v=vertices();GeometryInfo info;
    assert(inspectGeometry(v,indices,info)&&info.highestBone==0&&info.bytes==272&&info.rigidFirst);
    auto wide=v;wide[0].position[0]=-99305.1953125f;wide[0].normal[0]=1.9517822265625f;
    assert(inspectGeometry(wide,indices,info));
    wide[0].position[0]=0x1p20f;wide[0].normal[0]=-2;assert(inspectGeometry(wide,indices,info));
    wide[0].position[0]=std::nextafter(0x1p20f,INFINITY);assert(!inspectGeometry(wide,indices,info));
    wide=v;wide[0].normal[0]=std::nextafter(2.f,INFINITY);assert(!inspectGeometry(wide,indices,info));
    for(auto bits:{1u,0x80000001u,0x7f800001u,0x7fc00000u,0xff800000u}){
        wide=v;wide[0].position[0]=std::bit_cast<float>(bits);assert(!inspectGeometry(wide,indices,info));
    }
    auto broken=v;broken[0].weights={254,0,0,0};assert(!inspectGeometry(broken,indices,info));
    broken=v;broken[0].boneRows[0]=1;assert(!inspectGeometry(broken,indices,info));
    broken=v;broken[0].boneRows[0]=12;assert(!inspectGeometry(broken,indices,info));
    broken=v;broken[0].position[0]=std::numeric_limits<float>::infinity();assert(!inspectGeometry(broken,indices,info));
    broken=v;broken[0].uv[0][0]=0x1p-80f;assert(!inspectGeometry(broken,indices,info));
    auto badIx=indices;badIx[5]=4;assert(!inspectGeometry(v,badIx,info));assert(!inspectGeometry(v,std::span(indices).first(5),info));
    broken=v;broken[0].weights={64,64,64,63};broken[0].boneRows={0,3,6,9};assert(inspectGeometry(broken,indices,info)&&info.highestBone==3&&!info.rigidFirst);
    auto u=uniforms();assert(!validateUniforms(u,3));u.palette.count=4;assert(validateUniforms(u,3));
    u.palette.count=5;assert(!validateUniforms(u,3));u.palette.count=4;
    u.lightCount=1;u.lights[0].kind=LightKind::directional;u.lights[0].direction={1,1,1};
    assert(!validateUniforms(u,3));u.lights[0].direction={1,0,0};assert(validateUniforms(u,3));
    u.viewProjection[0][0]=std::numeric_limits<float>::quiet_NaN();assert(!validateUniforms(u,3));
    u=uniforms();u.palette.position[0][3]=0x1p20f;u.viewProjection[0][3]=-0x1p20f;assert(validateUniforms(u,0));
    u.palette.position[0][0]=std::nextafter(32768.f,INFINITY);assert(!validateUniforms(u,0));
    u=uniforms();u.uvTransforms[2].rows[0]={32768,-32768,32768};assert(validateUniforms(u,0));
    u.uvTransforms[2].source=8;assert(!validateUniforms(u,0));
    for(auto bits:{1u,0x7f800001u,0x7fc00000u,0xff800000u}){
        u=uniforms();u.uvTransforms[0].rows[1][2]=std::bit_cast<float>(bits);assert(!validateUniforms(u,0));
        u=uniforms();u.projective.rows[2][0]=std::bit_cast<float>(bits);assert(!validateUniforms(u,0));
    }
    u=uniforms();u.projective.source=PositionSource(255);assert(!validateUniforms(u,0));
    u=uniforms();u.projective.enabled=true;u.projective.rows[2]={};assert(!validateUniforms(u,0));
    u=uniforms();u.lightCount=8;u.channels[3].enabled=true;u.channels[3].lightMask=128;assert(validateUniforms(u,0));
    u.lightCount=7;assert(!validateUniforms(u,0));u.lightCount=9;assert(!validateUniforms(u,0));
    u=uniforms();u.channels[0].diffuse=Diffuse(3);assert(!validateUniforms(u,0));
    u=uniforms();u.channels[2].attenuation=Attenuation(3);assert(!validateUniforms(u,0));
    u=uniforms();u.lightCount=1;u.channels[0].enabled=true;u.channels[0].lightMask=1;u.channels[0].attenuation=Attenuation::spot;
    u.lights[0].distance={1,-2,1};assert(!validateUniforms(u,0));
    u.lights[0].distance={2,-2,1};assert(validateUniforms(u,0));
    u=uniforms();u.fog.enabled=true;assert(!validateUniforms(u,0));u.fog.provided=true;assert(validateUniforms(u,0));
    u.fog.range[9]=-1;assert(!validateUniforms(u,0));u.fog.range[9]=1;u.fog.function=FogFunction(5);assert(!validateUniforms(u,0));
    u=uniforms();u.imageOverrideMask=1;assert(!validateUniforms(u,0));u.imageOverrides[0]=3;assert(validateUniforms(u,0));
    u.imageOverrideMask=0;assert(!validateUniforms(u,0));
    auto m=material();assert(validateMaterial(m));m.textureMask=0;assert(!validateMaterial(m));
    assert(validateMaterial(Material::vertexColor()));
    for(unsigned i=0;i<3;++i)assert(validateMaterial(Material::textured(i)));
    assert(!validateMaterial(Material::textured(3)));
    m=material();m.stages[0].alphaFunction=Combine::dot3;assert(!validateMaterial(m));
    m=material();m.stages[0].rgb[0]=Source(255);assert(!validateMaterial(m));
    m=material();m.stages[0].rgb[0]=Source::previous;assert(!validateMaterial(m));
    m=material();m.bufferRgbMask=16;assert(!validateMaterial(m));
    m=material();m.fog=true;m.fogValues[1]=-1;assert(!validateMaterial(m));
    m=material();m.customBlend.color=BlendEquation(255);assert(!validateMaterial(m));
    m=material();m.customBlend.alphaSource=BlendFactor(255);assert(!validateMaterial(m));
    // Independent sum of authored mip payloads over all accepted dimensions.
    m=Material::textured();m.rasterLookup=true;assert(!validateMaterial(m));
    m.textureMask=3;assert(validateMaterial(m));m.lookupBase=2;assert(!validateMaterial(m));
    m=Material::vertexColor();m.fogCarrier=FogCarrier::secondaryRed;assert(!validateMaterial(m));
    unsigned cases=0;
    for(unsigned fmt:{0u,1u,2u,3u,5u})for(unsigned w=8;w<=1024;w*=2)for(unsigned h=8;h<=1024;h*=2){
        std::size_t sum=0;unsigned a=w,b=h;
        for(unsigned level=1;a>=8&&b>=8;++level,a/=2,b/=2){
            const unsigned bits[]{32,16,4,8,0,16};sum+=std::size_t(a)*b*bits[fmt]/8;
            TextureInfo t{std::uint16_t(w),std::uint16_t(h),TextureFormat(fmt)};t.mipLevels=level;
            assert(textureBytes(t)==sum);++cases;
            t.mipLevels=9;assert(!textureBytes(t));
        }
    }
    assert(!textureBytes({7,8,TextureFormat::rgba8}));
    assert(!textureBytes({8,8,TextureFormat(4)}));
    std::printf("PASS native asset/uniform/material validation and %u independent mip sizes\n",cases);
}
void renderer(){
    using namespace nativeMock;
    auto r=std::make_unique<Renderer>();Limits limits;limits.meshBytes=4096;limits.textureBytes=4096;limits.transientBytes=16384;limits.drawsPerFrame=4;
    auto tooSmall=limits;tooSmall.textureBytes=1;assert(!r->initialize(tooSmall));assert(allocations.empty());
    static_assert(maxModelParts==1024&&maxFrameDraws==4096);
    static_assert(nativeGameLimits.drawsPerFrame==4096&&nativeGameLimits.commandBytes==32768u+(4u<<20));
    // A small reusable arena requires an explicit per-draw completion guard.
    assert(!r->initialize(nativeGameLimits)&&r->error()==Error::budget&&allocations.empty());
    const auto guard=+[]{return true;};
    auto insufficient=nativeGameLimits;insufficient.commandBytes=32768+4096-1;
    assert(!r->initialize(insufficient,guard)&&r->error()==Error::budget&&allocations.empty());
    auto excessive=nativeGameLimits;excessive.drawsPerFrame=maxFrameDraws+1;
    assert(!r->initialize(excessive,guard)&&r->error()==Error::budget&&allocations.empty());
    for(unsigned shader=1;shader<=6;++shader){
        failParseAt=parseCalls+shader;assert(!r->initialize(limits));assert(liveBinaries==0&&liveShaders==0&&allocations.empty());failParseAt=0;
        failSetShaderAt=setShaderCalls+shader;assert(!r->initialize(limits));assert(liveBinaries==0&&liveShaders==0&&allocations.empty());failSetShaderAt=0;
    }
    failAllocationAt=allocationCalls+1;assert(!r->initialize(limits));assert(allocations.empty());failAllocationAt=0;
    failPublication=true;assert(!r->initialize(limits));assert(allocations.empty());failPublication=false;
    assert(r->initialize(limits));assert(allocations.size()==2&&r->statistics().textureBytes==256);
    auto v=vertices();auto m=material();auto u=uniforms();
    failPublication=true;assert(!r->createMesh(v,indices));assert(r->error()==Error::publication);failPublication=false;
    assert(r->statistics().meshBytes==0&&allocations.size()==2);
    auto mesh=r->createMeshBytes(std::as_bytes(std::span(v)),std::as_bytes(std::span(indices)));assert(mesh&&r->statistics().meshBytes==272);
    std::array<std::byte,256> pixels{};
    assert(!r->createTexture({8,8,TextureFormat::rgba8},std::span(pixels).first(255)));
    auto tex=r->createTexture({8,8,TextureFormat::rgba5551},std::span(pixels).first(128));assert(tex);
    auto mat=r->createMaterial(m);assert(mat);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(!r->draw(mesh,mat,{tex,{},{}},u)&&r->error()==Error::protocol);
    assert(r->begin(&target));assert(r->clear(0,0));assert(r->scissor(0,0,240,400));assert(!r->scissor(0,0,241,400));
    int other;tls=&other;assert(!r->release(mesh)&&r->error()==Error::wrongThread);tls=&tlsValue;
    assert(r->draw(mesh,mat,{tex,{},{}},u));assert(draws==1&&buffer.stride==64&&buffer.attributeCount==9&&buffer.order==0x876543210ULL&&attributes.count==9);
    assert(textures[0]&&textures[0]->fmt==GPU_RGBA5551&&textures[0]->size==128);
    assert(selectedShader==1);
    assert(r->statistics().rigidDraws==1&&r->statistics().weightedDraws==0);
    assert(!r->clear(0,0)&&r->error()==Error::protocol);
    assert(env[0].rgb==GPU_MODULATE&&env[0].srcRgb==GPU_TEVSOURCES(GPU_TEXTURE0,GPU_PRIMARY_COLOR,GPU_CONSTANT));
    const auto* resident=static_cast<const Vertex*>(buffer.data);
    assert(resident[0].color==v[0].color&&resident[0].color1==v[0].color1);
    auto admitted=r->createMaterial(m);assert(admitted);
    assert(std::memcmp(resident,v.data(),sizeof(v))==0&&buffer.data==resident);
    assert(r->release(admitted));
    assert(!r->draw(mesh,mat,{},u)&&r->error()==Error::invalidHandle&&draws==1);
    // One transient batch of 32 native quads, one indexed submission, no CPU
    // transform/command decoding. The mock checks actual publication addresses.
    std::array<Vertex,128> batch{};std::array<std::uint16_t,192> batchIx{};
    for(unsigned q=0;q<32;++q){
        for(unsigned i=0;i<4;++i)batch[4*q+i]=v[i];
        for(unsigned i=0;i<6;++i)batchIx[6*q+i]=std::uint16_t(4*q+indices[i]);
    }
    assert(r->drawTransient(batch,batchIx,mat,{tex,{},{}},u));assert(draws==2&&r->statistics().transientBytes>0);
    failBuffer=true;assert(!r->draw(mesh,mat,{tex,{},{}},u)&&r->error()==Error::backend&&draws==2);failBuffer=false;
    assert(r->draw(mesh,mat,{tex,{},{}},u)&&r->draw(mesh,mat,{tex,{},{}},u));
    assert(!r->draw(mesh,mat,{tex,{},{}},u)&&r->error()==Error::budget&&draws==4);
    auto freeBefore=frees;assert(r->release(mesh)&&r->release(tex)&&r->release(mat));
    // Retired metadata slots, like GPU storage, remain unavailable until a
    // successful FrameBegin; admitting new immutable resources cannot recycle them.
    for(unsigned i=0;i<resourceSlots-2;++i){auto h=r->createMaterial(m);assert(h&&r->release(h));}
    assert(!r->createMaterial(m)&&r->error()==Error::budget);
    assert(frees==freeBefore&&r->statistics().meshBytes==272);assert(r->end());
    assert(!r->begin(&target)&&r->error()==Error::busy&&frees==freeBefore);
    assert(!r->createMaterial(m)&&r->error()==Error::budget);
    // Exact retirement barrier: a real backend's NONBLOCK FrameBegin must
    // complete the prior queue. No busy call may recycle retired GPU storage.
    complete();assert(r->begin(&target));assert(frees==freeBefore+2&&r->statistics().meshBytes==0&&r->statistics().textureBytes==256);
    assert(!r->draw(mesh,mat,{tex,{},{}},u)&&r->error()==Error::invalidHandle);
    auto mesh2=r->createMesh(v,indices);auto tex2=r->createTexture({8,8,TextureFormat::rgba8},pixels);
    m.fog=true;for(unsigned i=0;i<256;++i)m.fogValues[i]=float(i)/255;
    auto mat2=r->createMaterial(m);assert(mesh2&&tex2&&mat2&&mesh2.value!=mesh.value&&tex2.value!=tex.value&&mat2.value!=mat.value);
    assert(r->draw(mesh2,mat2,{tex2,{},{}},u));assert(fog);
    auto p=reinterpret_cast<std::uintptr_t>(fog),a=reinterpret_cast<std::uintptr_t>(r.get());assert(p>=a&&p-a<sizeof(Renderer));
    assert(nativeMock::fog->values==m.fogValues&&nativeMock::fogColor==m.fogColor&&nativeMock::fogMode==GPU_FOG);
    QuadBatch ordered(*r);Quad quad;
    for(unsigned i=0;i<4;++i){quad.corners[i]=v[i].position;quad.uv[i]=v[i].uv;quad.colors[i]=v[i].color;}
    assert(ordered.begin(mat2,{tex2,{},{}},u));auto beforeBatch=draws;
    for(unsigned i=0;i<33;++i)assert(ordered.append(quad));
    assert(ordered.pending()==1&&ordered.submissions()==1&&draws==beforeBatch+1);
    assert(ordered.end()&&draws==beforeBatch+2&&ordered.submissions()==2);
    assert(ordered.begin(mat2,{tex2,{},{}},u)&&ordered.append(quad));
    assert(!r->releaseAfterContextDestroyed()&&r->error()==Error::protocol);
    assert(r->end());complete();assert(r->begin(&target));
    assert(!ordered.flush()&&ordered.failed()&&draws==beforeBatch+2); // No packet survives a frame boundary.
    assert(!ordered.end());
    QuadBatch clipped(*r);assert(clipped.begin(mat2,{tex2,{},{}},u)&&clipped.append(quad));
    assert(r->scissor(0,0,100,100));assert(!clipped.end()&&clipped.failed()&&draws==beforeBatch+2);
    QuadBatch interleaved(*r);assert(interleaved.begin(mat2,{tex2,{},{}},u)&&interleaved.append(quad));
    assert(r->draw(mesh2,mat2,{tex2,{},{}},u));
    assert(!interleaved.end()&&interleaved.failed()&&draws==beforeBatch+3); // No transparent-order reordering.
    C3D_RenderTarget depthless{{reinterpret_cast<void*>(3),240,400,nullptr}};
    assert(r->target(&depthless));
    assert(!r->draw(mesh2,mat2,{tex2,{},{}},u)&&r->error()==Error::invalidAsset&&draws==beforeBatch+3);
    const auto queueBefore=queueEvents.size();assert(pendingDraws==1);
    assert(!r->clear(0,0));assert(queueEvents.size()==queueBefore&&pendingDraws==1);
    assert(r->clear(0,0,true,false));
    assert(queueEvents.size()==queueBefore+2&&queueEvents[queueBefore]==1&&queueEvents.back()==2&&pendingDraws==0);
    assert(r->end());complete();contextDestroyed=true; // Test frontend's completed C3D_Fini substitute.
    assert(r->releaseAfterContextDestroyed()&&allocations.empty()&&!liveBinaries&&!liveShaders);assert(!r->begin(&target));
    std::puts("PASS actual native backend against explicit SDK substitutes: publication, ownership, ordered batching, stale handles, retired budgets, NONBLOCK barrier and failure injection");
}
void packedRenderer(const char* name){
    std::ifstream input(name,std::ios::binary);assert(input.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
    MeshPack pack;assert(MeshPack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);
    MeshView view;assert(pack.mesh(3,view)==PackError::none);
    auto renderer=std::make_unique<Renderer>();assert(renderer->initialize({}));
    auto mesh=renderer->createMeshBytes(view.vertices,view.indices);auto material=renderer->createMaterial(Material::vertexColor());assert(mesh&&material);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(renderer->begin(&target));auto u=uniforms();const auto before=nativeMock::draws;
    assert(!renderer->draw(mesh,material,{},u)&&renderer->error()==Error::invalidAsset&&nativeMock::draws==before);
    u.palette.count=3;assert(renderer->draw(mesh,material,{},u)&&nativeMock::draws==before+1);
    assert(nativeMock::selectedShader==0);
    assert(renderer->statistics().weightedDraws==1&&renderer->statistics().rigidDraws==0);
    const auto* uploaded=static_cast<const Vertex*>(nativeMock::buffer.data);
    assert(uploaded[0].weights[0]==128&&uploaded[0].weights[1]==127&&uploaded[0].boneRows[1]==6);
    assert(renderer->end());nativeMock::complete();assert(renderer->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS Python native pack -> C++ bounded view -> checked resident upload -> native submission; palette under-admission refused");
}
void packedTextures(const char* name){
    std::ifstream input(name,std::ios::binary);assert(input.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
    TexturePack pack;assert(TexturePack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));
    auto mesh=r->createMesh(vertices(),indices);auto mat=r->createMaterial(Material::textured());assert(mesh&&mat);
    std::array<TextureHandle,4> handles{};
    for(unsigned id=1;id<=4;++id){
        TextureView v;assert(pack.image(id,v)==PackError::none);
        // Explicit native material sampler. Source sampler conversion is not
        // part of the native image pack and is never inferred by this reader.
        TextureInfo info{v.image.width,v.image.height,v.image.format,Wrap::repeat,Wrap::mirror,false,v.image.mipLevels};
        handles[id-1]=r->createTexture(info,v.payload);assert(handles[id-1]);
    }
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));
    for(unsigned id=1;id<=4;++id){
        TextureView v;assert(pack.image(id,v)==PackError::none);const auto tex=handles[id-1];
        assert(r->draw(mesh,mat,{tex,{},{}},uniforms()));
        const auto* bound=nativeMock::textures[0];assert(bound->maxLevel==1&&bound->size+bound->size/4==v.payload.size());
        assert(std::memcmp(bound->data,v.payload.data(),v.payload.size())==0);
        assert(r->release(tex)); // Completion-dependent retirement still applies.
    }
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native image pack -> checked mip upload -> explicit native sampler -> four native texture format submissions (SDK substitutes)");
}
void uvRenderer(){
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));
    auto v=vertices();v[0].position[2]=.5f;auto mesh=r->createMesh(v,indices);std::array<MaterialHandle,3> materials{};
    for(unsigned n=0;n<3;++n)materials[n]=r->createMaterial(Material::textured(n));
    auto plain=r->createMaterial(Material::vertexColor());std::array<std::byte,256> pixels{};auto tex=r->createTexture({8,8,TextureFormat::rgba8},pixels);
    assert(mesh&&plain&&tex);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));
    auto u=uniforms();assert(r->draw(mesh,materials[0],{tex,tex,tex},u));assert(nativeMock::selectedShader==1&&r->statistics().uvDraws==0);
    for(unsigned unit=0;unit<3;++unit)for(unsigned source=0;source<8;++source)for(bool normalize:{false,true}){
        u=uniforms();auto& t=u.uvTransforms[unit];t.source=source;t.normalize=normalize;t.rows={{{2,.5f,-.25f},{-.75f,3,.125f}}};t.translation={.375f,-.625f};
        assert(r->draw(mesh,materials[unit],{tex,tex,tex},u)&&nativeMock::selectedShader==3);
        const auto base=73+2*unit;
        assert((nativeMock::uniforms[base]==Row{2,.5f,-.25f,.375f}));
        assert((nativeMock::uniforms[base+1]==Row{-.75f,3,.125f,-.625f}));
        for(unsigned n=0;n<4;++n)assert(nativeMock::uniforms[79][n]==(n==unit?float(1+source+8*unsigned(normalize)):0.f));
    }
    assert(r->statistics().uvDraws==48&&r->statistics().rigidDraws==49);
    u=uniforms();u.uvTransforms[0].source=2;
    assert(r->draw(mesh,plain,{},u)&&nativeMock::selectedShader==1&&r->statistics().uvDraws==48); // Unconsumed UV stays on the original shader.
    v[0].weights={128,127,0,0};assert(r->drawTransient(v,indices,materials[0],{tex,tex,tex},u)&&nativeMock::selectedShader==2);
    assert(r->statistics().weightedDraws==1&&r->statistics().uvDraws==49);
    const auto before=nativeMock::draws;u.uvTransforms[0].source=8;
    assert(!r->draw(mesh,materials[0],{tex,tex,tex},u)&&r->error()==Error::invalidAsset&&nativeMock::draws==before);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native GPU UV routing, affine/source uniforms, untouched passthrough variants, weighted/rigid statistics and invalid-input refusal");
}
void projectiveRenderer(){
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto v=vertices();auto mesh=r->createMesh(v,indices);
    auto m=Material::textured();m.textureMask=7;m.stageCount=3;
    for(unsigned n=1;n<3;++n){auto& s=m.stages[n];s.rgb={Source::previous,Source(unsigned(Source::texture0)+n),Source::primary};s.alpha=s.rgb;s.rgbFunction=s.alphaFunction=Combine::modulate;}
    auto mat=r->createMaterial(m),plain=r->createMaterial(Material::vertexColor());std::array<std::byte,256> pixels{};auto tex=r->createTexture({8,8,TextureFormat::rgba8},pixels);assert(mesh&&mat&&plain&&tex);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));auto u=uniforms();
    const auto allocationCount=nativeMock::allocations.size(),textureBytes=r->statistics().textureBytes;
    for(auto source:{PositionSource::object,PositionSource::posed,PositionSource::objectNormal,PositionSource::posedNormal,PositionSource::posedLinearNormal}){
        u=uniforms();u.projective.enabled=true;u.projective.source=source;u.projective.normalize=unsigned(source)>=unsigned(PositionSource::objectNormal);u.projective.rows={{{.5f,0,0,1},{0,-.5f,0,2},{0,0,.25f,3}}};
        u.uvTransforms[1].rows[0][2]=.25f;
        assert(r->draw(mesh,mat,{tex,tex,tex},u)&&nativeMock::selectedShader==5);
        for(unsigned n=0;n<3;++n)assert(nativeMock::uniforms[80+n]==u.projective.rows[n]);
        assert(nativeMock::uniforms[83][0]==float(source));
        assert(nativeMock::uniforms[83][3]==float(u.projective.normalize));
        assert((nativeMock::uniforms[79]==Row{0,2,0,0}));
        for(unsigned n=0;n<3;++n){
            const auto* view=nativeMock::textures[n];assert(((view->param>>28)&7)==(n==0?GPU_TEX_PROJECTION:GPU_TEX_2D));
            const auto a=reinterpret_cast<std::uintptr_t>(r.get()),p=reinterpret_cast<std::uintptr_t>(view);assert(p>=a&&p-a<sizeof(Renderer));
            assert(view->data==nativeMock::textures[0]->data);
        }
        assert(nativeMock::textures[0]!=nativeMock::textures[1]);
        assert(nativeMock::allocations.size()==allocationCount&&r->statistics().textureBytes==textureBytes);
    }
    v[0].weights={128,127,0,0};assert(r->drawTransient(v,indices,mat,{tex,tex,tex},u)&&nativeMock::selectedShader==4);
    assert(r->statistics().projectiveDraws==6&&r->statistics().weightedDraws==1&&r->statistics().rigidDraws==5);
    u=uniforms();assert(r->draw(mesh,mat,{tex,tex,tex},u)&&nativeMock::selectedShader==1);
    for(auto* view:nativeMock::textures)assert(((view->param>>28)&7)==GPU_TEX_2D);
    const auto before=nativeMock::draws;u.projective.enabled=true;u.uvTransforms[0].source=2;
    assert(!r->draw(mesh,mat,{tex,tex,tex},u)&&r->error()==Error::invalidAsset&&nativeMock::draws==before);
    assert(r->draw(mesh,plain,{},u)&&nativeMock::selectedShader==1&&r->statistics().projectiveDraws==6); // Neither coordinate path is consumed.
    assert(!nativeMock::unsupportedUnitBindings);
    assert(r->release(tex));assert(!r->draw(mesh,mat,{tex,tex,tex},uniforms())&&r->error()==Error::invalidHandle);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native projective matrix/source upload, rigid/weighted selection, shared-image sampler views, 2D restoration and explicit conflict/lifetime checks");
}
void packedMaterials(const char* name){
    std::ifstream file(name,std::ios::binary);assert(file.good());std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    MaterialPack pack;assert(MaterialPack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);MaterialView view;assert(pack.material(3,view)==PackError::none&&view.imageIds[0]==17);
    Material state;assert(!instantiateMaterial(view,nullptr,state));FogParameters context;
    assert(instantiateMaterial(view,&context,state)); // Explicit native environment says no fog for this synthetic fixture.
    MaterialView lookupView,fogView;Material decoded;
    assert(pack.material(53,lookupView)==PackError::none&&instantiateMaterial(lookupView,nullptr,decoded));
    assert(decoded.rasterLookup&&decoded.lookupBase==1&&decoded.textureMask==7);
    assert(pack.material(59,fogView)==PackError::none&&instantiateMaterial(fogView,nullptr,decoded));
    assert(decoded.fogCarrier==FogCarrier::primaryAlpha&&!decoded.fog);
    assert((decoded.stages[1].constantBindings==std::array<std::uint8_t,4>{29,30,31,32}));
    assert(!instantiateMaterial(fogView,&context,decoded)); // A depth LUT cannot replace a source fog carrier.
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto mesh=r->createMesh(vertices(),indices);auto mat=r->createMaterial(state);
    std::array<std::byte,256> pixels{};auto tex=r->createTexture({8,8,TextureFormat::rgba8},pixels);assert(mesh&&mat&&tex);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));assert(r->draw(mesh,mat,{tex,{},{}},uniforms()));
    assert((nativeMock::blendState==std::array<u32,6>{GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA}));
    assert(nativeMock::env[0].rgb==GPU_MODULATE&&nativeMock::env[0].alpha==GPU_MODULATE);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native material pack -> required native environment binding -> native combiner/blend preparation -> Citro3D submission (SDK substitutes)");
}
void sourceCompiledMaterials(const char* name){
    std::ifstream file(name,std::ios::binary);assert(file.good());std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    MaterialPack pack;assert(MaterialPack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);MaterialView view;assert(pack.material(1,view)==PackError::none);
    assert((view.imageIds==std::array<std::uint32_t,3>{2,1,0})&&view.state.stageCount==2&&view.state.depth==Compare::greaterEqual);
    Material state;assert(view.initialSnapshotOnly&&!instantiateMaterial(view,nullptr,state));
    assert(instantiateMaterial(view,nullptr,state,true));auto r=std::make_unique<Renderer>();assert(r->initialize({}));
    auto mesh=r->createMesh(vertices(),indices);auto mat=r->createMaterial(state);std::array<std::byte,256> pixels{};auto image=r->createTexture({8,8,TextureFormat::rgba8},pixels);assert(mesh&&mat&&image);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));auto u=uniforms();u.projective.enabled=true;
    assert(r->draw(mesh,mat,{image,image,{}},u)&&nativeMock::selectedShader==5);
    assert(nativeMock::env[0].rgb==GPU_MODULATE&&nativeMock::env[1].rgb==GPU_MODULATE&&nativeMock::env[1].rgbScale==2);
    assert(nativeMock::env[0].srcRgb==GPU_TEVSOURCES(GPU_TEXTURE1,GPU_PRIMARY_COLOR,GPU_PRIMARY_COLOR));
    assert(nativeMock::env[1].srcRgb==GPU_TEVSOURCES(GPU_TEXTURE0,GPU_PREVIOUS,GPU_PRIMARY_COLOR));
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS original-format source fixture -> offline native fragment template -> bounded reader -> native PICA program/state submission (SDK substitutes)");
}
void parameterizedMaterials(const char* name){
    std::ifstream file(name,std::ios::binary);assert(file.good());std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    MaterialPack pack;assert(MaterialPack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);MaterialView view;assert(pack.material(7,view)==PackError::none);
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto mesh=r->createMesh(vertices(),indices);assert(mesh);
    Material state;assert(instantiateMaterial(view,nullptr,state)&&state.stages[0].constant==0xa1b2c3d4);
    // Every selector, destination lane and byte value is independent. Verify
    // no truncation, channel alias, signed shift or last-stage special case.
    unsigned cases=0;
    for(unsigned stage=0;stage<6;++stage)for(unsigned lane=0;lane<4;++lane){
        state.stageCount=stage+1;for(auto& s:state.stages)s.constantBindings={};
        state.stages[stage].constant=0xa1b2c3d4;
        for(unsigned selector=1;selector<=32;++selector){
            state.stages[stage].constantBindings[lane]=std::uint8_t(selector);
            auto mat=r->createMaterial(state);assert(mat);
            C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));
            auto u=uniforms();const unsigned source=selector-1;u.material.suppliedMask=std::uint8_t(1u<<(source/4));
            // Same prepared handle, changing only the native parameter block.
            for(unsigned value=0;value<256;++value){
                if(r->statistics().frameDraws==64){assert(r->end());nativeMock::complete();assert(r->begin(&target));}
                u.material.colors.fill(0x87654321);u.material.colors[source/4]=(u.material.colors[source/4]&~(255u<<(8*(source%4))))|(value<<(8*(source%4)));
                assert(r->draw(mesh,mat,{},u));
                const auto expected=(0xa1b2c3d4&~(255u<<(8*lane)))|(value<<(8*lane));
                assert(nativeMock::env[stage].color==expected);++cases;
            }
            // Missing producer data cannot silently reuse a prior instance.
            assert(r->end());nativeMock::complete();assert(r->begin(&target));
            const auto draws=r->statistics().draws;const auto before=nativeMock::env;u.material.suppliedMask=0;
            assert(!r->draw(mesh,mat,{},u)&&r->error()==Error::invalidAsset&&r->statistics().draws==draws);
            for(unsigned n=0;n<6;++n)assert(nativeMock::env[n].color==before[n].color);
            assert(r->end());nativeMock::complete();assert(r->release(mat));
            // Recycle retired slots before creating the next handle.
            assert(r->begin(&target));assert(r->end());nativeMock::complete();
        }
    }
    state.stageCount=1;state.stages[0].constantBindings={33,0,0,0};assert(!r->createMaterial(state));
    // Mixed RGB and alpha parameter vectors plus literal retention. Alternating
    // instances must never modify the authored template or one another.
    state.stages[0].constantBindings={1,6,0,32};auto mat=r->createMaterial(state);assert(mat);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));auto a=uniforms(),b=uniforms();
    a.material.colors={0x11223344,0x55667788,0,0,0,0,0,0x99aabbcc};a.material.suppliedMask=0x83;
    b.material.colors.fill(0);b.material.suppliedMask=0x83;
    const auto allocated=nativeMock::allocationCalls;
    assert(r->draw(mesh,mat,{},a)&&nativeMock::env[0].color==0x99b27744);
    assert(r->draw(mesh,mat,{},b)&&nativeMock::env[0].color==0x00b20000);
    assert(r->draw(mesh,mat,{},a)&&nativeMock::env[0].color==0x99b27744&&nativeMock::allocationCalls==allocated);
    QuadBatch batch(*r);assert(batch.begin(mat,{},a));Quad quad;assert(batch.append(quad));
    a.material.colors.fill(0); // Batch owns its parameter snapshot.
    assert(batch.end()&&nativeMock::env[0].color==0x99b27744);
    b.material.suppliedMask=0;assert(batch.begin(mat,{},b)&&batch.append(quad));
    const auto beforeFailure=r->statistics().draws;
    assert(!batch.end()&&batch.failed()&&r->statistics().draws==beforeFailure);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::printf("PASS %u independent native parameter byte/lane/stage emissions, required producers, instance isolation and zero per-draw allocation\n",cases);
}
void bufferedCompiledMaterials(const char* name){
    std::ifstream file(name,std::ios::binary);assert(file.good());std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    MaterialPack pack;assert(MaterialPack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);MaterialView view;assert(pack.material(1,view)==PackError::none&&!view.initialSnapshotOnly);
    Material state;assert(instantiateMaterial(view,nullptr,state)&&state.stageCount==4&&state.bufferRgbMask==1&&state.bufferAlphaMask==0);
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto mesh=r->createMesh(vertices(),indices);auto mat=r->createMaterial(state);auto plain=r->createMaterial(Material::vertexColor());
    std::array<std::byte,256> pixels{};auto image=r->createTexture({8,8,TextureFormat::rgba8},pixels);assert(mesh&&mat&&plain&&image);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));auto u=uniforms();u.projective.enabled=true;
    u.material.colors={0x01020304,0,0,0x11223344,0x55667788,0,0x99aabbcc,0};u.material.suppliedMask=0xff;
    assert(r->draw(mesh,mat,{image,image,{}},u));
    assert((nativeMock::bufferMasks==std::array<unsigned,2>{1,0})&&nativeMock::bufferColor==0&&nativeMock::selectedShader==5);
    assert(nativeMock::env[2].srcRgb==GPU_TEVSOURCES(GPU_PREVIOUS_BUFFER,GPU_CONSTANT,GPU_PREVIOUS)&&nativeMock::env[2].rgb==GPU_INTERPOLATE);
    assert(nativeMock::env[0].color==0xff888888&&nativeMock::env[1].color==0x99ffffff&&nativeMock::env[2].color==0xff223344&&nativeMock::env[3].color==0xff020304);
    assert(r->draw(mesh,plain,{},uniforms())&&(nativeMock::bufferMasks==std::array<unsigned,2>{0,0}));
    u.material.colors.fill(0);assert(r->draw(mesh,mat,{image,image,{}},u));
    assert(nativeMock::env[0].color==0xff000000&&nativeMock::env[1].color==0x00ffffff&&nativeMock::env[2].color==0xff000000&&nativeMock::env[3].color==0xff000000&&nativeMock::bufferMasks[0]==1);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS offline branched source -> native four-stage pack -> immutable dynamic constants and delayed-buffer mask submission; literal material restores masks (SDK substitutes)");
}
void samplerRenderer(){
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto mesh=r->createMesh(vertices(),indices);
    auto m=Material::textured();m.textureMask=7;m.stageCount=3;
    for(unsigned n=1;n<3;++n){m.stages[n].rgb={Source::previous,Source(unsigned(Source::texture0)+n),Source::primary};m.stages[n].alpha=m.stages[n].rgb;m.stages[n].rgbFunction=m.stages[n].alphaFunction=Combine::modulate;}
    auto mat=r->createMaterial(m);TextureInfo info{32,32,TextureFormat::rgba8,Wrap::repeat,Wrap::mirror,true,3};std::vector<std::byte> pixels(textureBytes(info));auto image=r->createTexture(info,pixels);assert(mesh&&mat&&image);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));auto u=uniforms();assert(r->draw(mesh,mat,{image,image,image},u));
    auto* owned=nativeMock::textures[0];const auto initial=*owned;const auto allocations=nativeMock::allocationCalls;const auto bytes=r->statistics().textureBytes;
    unsigned cases=0;
    for(unsigned filter=0;filter<8;++filter)for(unsigned s=0;s<3;++s)for(unsigned t=0;t<3;++t)for(unsigned first=0;first<3;++first)for(unsigned last=first;last<3;++last)for(int bias:{-4096,-1,0,1,4095}){
        if(r->statistics().frameDraws==64){assert(r->end());nativeMock::complete();assert(r->begin(&target));}
        u.samplerMask=7;u.projective.enabled=cases&1;
        for(unsigned n=0;n<3;++n)u.samplers[n]={Wrap((s+n)%3),Wrap((t+n)%3),Filter(filter&1),Filter((filter>>1)&1),Filter((filter>>2)&1),std::uint8_t(first),std::uint8_t(last),std::int16_t(bias)};
        assert(r->draw(mesh,mat,{image,image,image},u));
        for(unsigned n=0;n<3;++n){const auto* view=nativeMock::textures[n];const auto& sample=u.samplers[n];assert(view!=owned&&view->data==initial.data);
            const unsigned param=GPU_TEXTURE_MAG_FILTER(unsigned(sample.magnify))|GPU_TEXTURE_MIN_FILTER(unsigned(sample.minify))|GPU_TEXTURE_MIP_FILTER(unsigned(sample.mipmap))|
                GPU_TEXTURE_WRAP_S(sample.wrapS==Wrap::clamp?GPU_CLAMP_TO_EDGE:sample.wrapS==Wrap::repeat?GPU_REPEAT:GPU_MIRRORED_REPEAT)|GPU_TEXTURE_WRAP_T(sample.wrapT==Wrap::clamp?GPU_CLAMP_TO_EDGE:sample.wrapT==Wrap::repeat?GPU_REPEAT:GPU_MIRRORED_REPEAT);
            constexpr unsigned mask=GPU_TEXTURE_MAG_FILTER(1)|GPU_TEXTURE_MIN_FILTER(1)|GPU_TEXTURE_MIP_FILTER(1)|GPU_TEXTURE_WRAP_S(3)|GPU_TEXTURE_WRAP_T(3);
            assert((view->param&mask)==param&&view->minLevel==first&&view->maxLevel==last&&view->lodBias==(unsigned(bias)&0x1fff));
            assert(((view->param>>28)&7)==(n==0&&u.projective.enabled?GPU_TEX_PROJECTION:GPU_TEX_2D));
        }
        assert(nativeMock::textures[0]!=nativeMock::textures[1]&&nativeMock::textures[1]!=nativeMock::textures[2]);
        assert(owned->param==initial.param&&owned->maxLevel==initial.maxLevel&&owned->minLevel==initial.minLevel&&owned->lodBias==initial.lodBias);
        assert(nativeMock::allocationCalls==allocations&&r->statistics().textureBytes==bytes);++cases;
    }
    assert(r->end());nativeMock::complete();assert(r->begin(&target));u=uniforms();assert(r->draw(mesh,mat,{image,image,image},u));for(auto* view:nativeMock::textures)assert(view==owned);
    const auto draws=nativeMock::draws;
    for(unsigned bad=0;bad<5;++bad){u=uniforms();u.samplerMask=1;if(bad==0)u.samplers[0].lastLevel=3;if(bad==1)u.samplers[0].firstLevel=1;if(bad==2)u.samplers[0].lodBias=4096;if(bad==3)u.samplers[0].lodBias=-4097;if(bad==4)u.samplers[0].minify=Filter(2);assert(!r->draw(mesh,mat,{image,image,image},u)&&nativeMock::draws==draws);}
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::printf("PASS %u independent native sampler combinations on three shared-image units, projected/2D views, mip/bias bounds, immutable owned storage and zero per-draw allocation\n",cases);
}
std::vector<char> readFile(const char* path){std::ifstream file(path,std::ios::binary);assert(file.good());return {(std::istreambuf_iterator<char>(file)),{}};}
void nativeModels(const char* meshPath,const char* imagePath,const char* materialPath){
    auto meshBytes=readFile(meshPath),imageBytes=readFile(imagePath),materialBytes=readFile(materialPath);MeshPack geometry;TexturePack pixels;MaterialPack fragments;
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none&&TexturePack::open(std::as_bytes(std::span(imageBytes)),pixels)==PackError::none&&MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    std::array<PartBinding,3> bindings{};bindings[0].mesh=3;bindings[0].material=23;bindings[0].samplerMask=1;bindings[0].samplers[0]={Wrap::repeat,Wrap::mirror,Filter::nearest,Filter::nearest,Filter::nearest,0,1,0};
    bindings[1]=bindings[0];bindings[1].mesh=17;bindings[1].uniformSlot=1;bindings[1].samplers[0]={Wrap::mirror,Wrap::clamp,Filter::linear,Filter::linear,Filter::linear,0,0,-1};
    bindings[2].mesh=3;bindings[2].material=29;
    std::array<DrawUniforms,2> parameters{uniforms(),uniforms()};for(auto& u:parameters){u.palette.count=3;u.material.suppliedMask=1;}parameters[0].material.colors[0]=0x11223344;parameters[1].material.colors[0]=0x55667788;parameters[1].viewProjection[0][0]=2;
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto model=std::make_unique<Model>();
    const auto initialAllocations=nativeMock::allocationCalls;
    assert(!model->load(*r,geometry,fragments,pixels,{})&&nativeMock::allocationCalls==initialAllocations);
    for(unsigned invalid=0;invalid<7;++invalid){auto bad=bindings;
        if(invalid==0)bad[2].mesh=99;if(invalid==1)bad[2].material=37;if(invalid==2)bad[2].material=41;if(invalid==3)bad[2].material=43;if(invalid==4)bad[2].uniformSlot=maxModelParts;if(invalid==5)bad[0].samplers[0].lastLevel=2;if(invalid==6)bad[0].samplerMask=0;
        if(invalid>=1&&invalid<=3)bad[2].samplerMask=1;
        assert(!model->load(*r,geometry,fragments,pixels,bad)&&model->error()==Error::invalidAsset&&model->parts()==0&&nativeMock::allocationCalls==initialAllocations);
    }
    for(unsigned offset=1;offset<=1+pixels.size();++offset){nativeMock::failAllocationAt=nativeMock::allocationCalls+offset;
        assert(!model->load(*r,geometry,fragments,pixels,bindings)&&model->error()==Error::allocation&&model->parts()==0);nativeMock::failAllocationAt=0;
        assert(r->begin(&target));assert(r->statistics().meshBytes==0&&r->statistics().textureBytes==256);assert(r->end());nativeMock::complete();
    }
    for(unsigned offset=1;offset<=1+pixels.size();++offset){nativeMock::failPublicationAt=nativeMock::publicationCalls+offset;
        assert(!model->load(*r,geometry,fragments,pixels,bindings)&&model->error()==Error::publication);nativeMock::failPublicationAt=0;
        assert(r->begin(&target)&&r->statistics().meshBytes==0&&r->statistics().textureBytes==256);assert(r->end());nativeMock::complete();}
    assert(model->load(*r,geometry,fragments,pixels,bindings)&&model->parts()==3);
    // Input binding storage is borrowed only during load; subsequent edits do
    // not mutate the prepared order, pose slots or sampler descriptors.
    auto edited=bindings;edited[0].mesh=999;assert(!model->load(*r,geometry,fragments,pixels,edited)&&model->parts()==3);
    std::vector<char>().swap(meshBytes);std::vector<char>().swap(imageBytes);std::vector<char>().swap(materialBytes); // No file spans may survive load.
    auto saved=bindings;bindings[0].uniformSlot=63;bindings[0].samplers[0].wrapS=Wrap::clamp; // Loaded model owns metadata.
    void* owner=nativeMock::tls;int stranger;nativeMock::tls=&stranger;assert(!model->unload()&&model->error()==Error::wrongThread&&model->parts()==3);nativeMock::tls=owner;
    assert(r->begin(&target));auto badParameters=parameters;badParameters[1].palette.count=1;const auto before=nativeMock::draws;
    assert(!model->draw({})&&nativeMock::draws==before);
    auto result=model->draw(badParameters);assert(!result&&result.submitted==0&&result.error==Error::invalidAsset&&nativeMock::draws==before);
    badParameters=parameters;badParameters[1].material.suppliedMask=0;assert(!model->draw(badParameters)&&nativeMock::draws==before);
    badParameters=parameters;badParameters[1].projective.enabled=true;badParameters[1].uvTransforms[0].source=2;assert(!model->draw(badParameters)&&nativeMock::draws==before);
    nativeMock::traceSize=0;nativeMock::traceDraws=true;const auto allocated=nativeMock::allocationCalls;result=model->draw(parameters);assert(result&&result.submitted==3&&nativeMock::traceSize==3);
    const auto& a=nativeMock::drawTrace[0];const auto& b=nativeMock::drawTrace[1];const auto& c=nativeMock::drawTrace[2];
    assert(a.count==6&&b.count==3&&c.count==6&&a.images[0]==b.images[0]&&a.images[0]!=c.images[0]);
    assert(a.constant==0x11223344&&b.constant==0x55667788&&a.projectionX==1&&b.projectionX==2&&a.sampling[0]!=b.sampling[0]);
    assert(nativeMock::allocationCalls==allocated);
    nativeMock::failBufferAt=nativeMock::bufferCalls+2;result=model->draw(parameters);assert(!result&&result.submitted==1&&result.error==Error::backend);nativeMock::failBufferAt=0;nativeMock::traceDraws=false;
    assert(r->end());const auto retained=r->statistics().textureBytes;assert(model->unload()&&model->parts()==0&&r->statistics().textureBytes==retained);assert(!r->begin(&target)&&r->error()==Error::busy);
    nativeMock::complete();assert(r->begin(&target)&&r->statistics().textureBytes==256&&r->statistics().meshBytes==0);assert(r->end());nativeMock::complete();
    assert(model->unload());assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    meshBytes=readFile(meshPath);imageBytes=readFile(imagePath);materialBytes=readFile(materialPath);
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none&&TexturePack::open(std::as_bytes(std::span(imageBytes)),pixels)==PackError::none&&MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    // Three parts must fit the GPU budget for their two distinct meshes;
    // duplicating the repeated mesh would make this valid model unloadable.
    Limits residency;residency.meshBytes=meshBytes.size()+128; // Bounded room for the owned bank directory, not another geometry copy.
    assert(r->initialize(residency));assert(model->load(*r,geometry,fragments,pixels,saved));
    assert(model->unload()&&r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    Limits cfg;cfg.drawsPerFrame=2;assert(r->initialize(cfg));assert(model->load(*r,geometry,fragments,pixels,saved));assert(r->begin(&target));
    const auto beforeBudget=nativeMock::draws;result=model->draw(parameters);assert(!result&&result.error==Error::budget&&result.submitted==0&&nativeMock::draws==beforeBudget);
    assert(model->unload()&&r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native resident model: shared resources, independent part samplers/poses/colors, complete dependency preflight, allocation/publication rollback, ordered submission, partial backend failure and GPU-dependent retirement");
}
void nativeScenes(const char* meshPath,const char* imagePath,const char* materialPath){
    auto meshBytes=readFile(meshPath),imageBytes=readFile(imagePath),materialBytes=readFile(materialPath);MeshPack geometry;TexturePack pixels;MaterialPack fragments;
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none&&TexturePack::open(std::as_bytes(std::span(imageBytes)),pixels)==PackError::none&&MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    std::array<PartBinding,3> bindings{};bindings[0].mesh=3;bindings[0].material=23;bindings[0].samplerMask=1;bindings[1]=bindings[0];bindings[1].mesh=17;bindings[1].uniformSlot=1;bindings[1].samplers[0].wrapS=Wrap::mirror;bindings[2].mesh=3;bindings[2].material=47;
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    auto r=std::make_unique<Renderer>();Limits limits;limits.drawsPerFrame=Scene::capacity;limits.commandBytes=32768+Scene::capacity*4096;assert(r->initialize(limits));
    auto a=std::make_unique<Model>(),b=std::make_unique<Model>();assert(a->load(*r,geometry,fragments,pixels,bindings));bindings[1].uniformSlot=63;assert(b->load(*r,geometry,fragments,pixels,bindings));
    std::array<DrawUniforms,8> parameters;
    for(unsigned n=0;n<parameters.size();++n){parameters[n]=uniforms();parameters[n].palette.count=3;parameters[n].material.suppliedMask=1;parameters[n].material.colors[0]=0xff000000|n;parameters[n].viewProjection[0][0]=float(n+1);}
    assert(!a->drawPart(0,parameters[0])&&!a->drawParts({},parameters));
    C3D_RenderTarget colorOnly{{reinterpret_cast<void*>(1),240,400,nullptr}};assert(r->begin(&colorOnly));
    std::array<SceneItem,2> noDepth{{{a.get(),&parameters[0],2,0,0},{a.get(),&parameters[1],1,1,0}}};
    const auto beforeDepth=nativeMock::draws;
    auto depthResult=Scene::draw(*r,noDepth);assert(!depthResult&&depthResult.submitted==0&&depthResult.error==Error::invalidAsset&&nativeMock::draws==beforeDepth);
    assert(Scene::draw(*r,std::span(noDepth).first(1))&&a->drawParts(std::array<unsigned,1>{2},parameters));assert(r->end());nativeMock::complete();
    assert(r->begin(&target));const auto before=nativeMock::draws,allocations=nativeMock::allocationCalls;
    auto invalid=parameters[1];invalid.material.suppliedMask=0;
    assert(!a->drawPart(0,invalid)&&nativeMock::draws==before);
    assert(a->drawPart(2,invalid)); // Unselected material's color input is irrelevant.
    assert(b->drawPart(1,parameters[1])); // Direct input does not use pack slot63.
    assert(!a->drawPart(3,parameters[0]));
    std::array<unsigned,3> selected{1,2,1};nativeMock::traceSize=0;nativeMock::traceDraws=true;
    auto result=a->drawParts(selected,parameters);assert(result&&result.submitted==3);
    assert(nativeMock::drawTrace[0].count==3&&nativeMock::drawTrace[1].count==6&&nativeMock::drawTrace[2].count==3);
    assert(nativeMock::drawTrace[0].projectionX==2&&nativeMock::drawTrace[2].projectionX==2);
    selected[2]=64;const auto afterSelected=nativeMock::draws,afterBuffers=nativeMock::bufferCalls;
    assert(!a->drawParts(selected,parameters)&&nativeMock::draws==afterSelected&&nativeMock::bufferCalls==afterBuffers);
    assert(!b->drawParts(std::array<unsigned,1>{1},parameters));
    nativeMock::traceDraws=false;nativeMock::failBufferAt=nativeMock::bufferCalls+2;selected={0,1,2};result=a->drawParts(selected,parameters);assert(!result&&result.submitted==1&&result.error==Error::backend);nativeMock::failBufferAt=0;assert(r->end());nativeMock::complete();
    // Independent minimum-selection oracle, not the implementation's std::sort.
    assert(r->begin(&target));nativeMock::traceSize=0;nativeMock::traceDraws=true;
    assert(a->drawPart(0,parameters[0])&&b->drawPart(1,parameters[1]));
    const auto imageA=nativeMock::drawTrace[0].images[0],imageB=nativeMock::drawTrace[1].images[0];assert(imageA!=imageB);
    nativeMock::traceDraws=false;assert(r->end());nativeMock::complete();
    unsigned seed=0x5363656e;
    for(unsigned fixture=0;fixture<256;++fixture){assert(r->begin(&target));std::array<SceneItem,8> items;
        for(unsigned n=0;n<items.size();++n){seed=1664525u*seed+1013904223u;items[n]={n&1?b.get():a.get(),&parameters[n],n&1,seed%5,std::uint16_t((seed>>16)%3)};}
        std::array<unsigned,8> expected{};std::array<bool,8> used{};
        for(unsigned out=0;out<items.size();++out){unsigned best=unsigned(items.size());
            for(unsigned n=0;n<items.size();++n)if(!used[n]&&(best==items.size()||items[n].pass<items[best].pass||(items[n].pass==items[best].pass&&items[n].order<items[best].order)))best=n;
            used[best]=true;expected[out]=best;
        }
        nativeMock::traceSize=0;nativeMock::traceDraws=true;result=Scene::draw(*r,items);assert(result&&result.submitted==items.size()&&nativeMock::traceSize==items.size());
        for(unsigned n=0;n<items.size();++n){const unsigned entry=expected[n];const auto& trace=nativeMock::drawTrace[n];assert(trace.constant==(0xff000000|entry)&&trace.projectionX==float(entry+1)&&trace.count==(entry&1?3:6)&&trace.images[0]==(entry&1?imageB:imageA));}
        nativeMock::traceDraws=false;assert(r->end());nativeMock::complete();
    }
    assert(r->begin(&target));std::array<SceneItem,2> items{{{a.get(),&parameters[0],0,1,0},{b.get(),&parameters[1],1,0,0}}};
    for(unsigned bad=0;bad<7;++bad){auto rejected=items;auto wrong=parameters[1];
        if(bad==0)rejected[1].model=nullptr;if(bad==1)rejected[1].parameters=nullptr;if(bad==2)rejected[1].part=64;
        if(bad==3){wrong.palette.count=1;rejected[1].parameters=&wrong;}if(bad==4){wrong.material.suppliedMask=0;rejected[1].parameters=&wrong;}
        if(bad==5){wrong.samplerMask=1;wrong.samplers[0].lastLevel=3;rejected[1].parameters=&wrong;}if(bad==6){wrong.projective.enabled=true;wrong.uvTransforms[0].source=2;rejected[1].parameters=&wrong;}
        const auto draws=nativeMock::draws,buffers=nativeMock::bufferCalls;result=Scene::draw(*r,rejected);assert(!result&&result.submitted==0&&nativeMock::draws==draws&&nativeMock::bufferCalls==buffers);
    }
    Model unloaded;items[1].model=&unloaded;assert(!Scene::draw(*r,items));items[1].model=b.get();
    Renderer foreign;result=Scene::draw(foreign,items);assert(!result&&result.error==Error::protocol);
    int stranger;void* owner=nativeMock::tls;nativeMock::tls=&stranger;result=Scene::draw(*r,items);assert(!result&&result.error==Error::wrongThread);assert(!a->drawPart(0,parameters[0])&&!a->drawParts(std::array<unsigned,1>{0},parameters));nativeMock::tls=owner;
    assert(Scene::draw(*r,{}));assert(r->end());nativeMock::complete();
    // Backend failures are real partial submission, never whole-scene success.
    for(unsigned failure=1;failure<=2;++failure){assert(r->begin(&target));nativeMock::failBufferAt=nativeMock::bufferCalls+failure;
        result=Scene::draw(*r,items);assert(!result&&result.error==Error::backend&&result.submitted==failure-1);nativeMock::failBufferAt=0;assert(r->end());nativeMock::complete();}
    assert(r->begin(&target));std::array<SceneItem,Scene::capacity+1> full;
    for(unsigned n=0;n<full.size();++n)full[n]={n&1?a.get():b.get(),&parameters[n%8],n&1,unsigned(full.size()-n),std::uint16_t(n%2)};
    const auto draws=nativeMock::draws;result=Scene::draw(*r,full);assert(!result&&result.error==Error::budget&&nativeMock::draws==draws);
    result=Scene::draw(*r,std::span(full).first(Scene::capacity));assert(result&&result.submitted==Scene::capacity);
    result=Scene::draw(*r,std::span(full).first(1));assert(!result&&result.error==Error::budget&&result.submitted==0);
    assert(nativeMock::allocationCalls==allocations);assert(r->end());nativeMock::complete();
    assert(!Scene::draw(*r,items));assert(a->unload()&&b->unload());assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native part selection and 256 cross-model scene-order oracles, direct slot-independent instances, full preflight, stable ties, exact partial errors, 1024 capacity and zero GPU allocation");
}
void nativePoseShader(const char* path){
    auto bytes=readFile(path);PosePack pack;assert(PosePack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);
    std::vector<Affine> world(pack.nodeCount());std::vector<PoseMatrix> matrices(pack.matrixCount());PoseView pose;
    assert(pack.evaluate(PosePack::identity(),{},world,matrices,pose));
    auto u=uniforms();std::array<std::uint32_t,3> bindings{3,1,2};assert(pose.palette(bindings,u.palette));
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));auto v=vertices();
    v[0].boneRows={6,0,0,0};v[0].weights={128,127,0,0};auto mesh=r->createMesh(v,indices);auto mat=r->createMaterial(Material::vertexColor());assert(mesh&&mat);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));
    const auto allocated=nativeMock::allocationCalls;assert(r->draw(mesh,mat,{},u));
    for(unsigned n=0;n<3;++n)for(unsigned row=0;row<3;++row){assert(nativeMock::uniforms[n*3+row]==matrices[bindings[n]-1].position[row]);assert(nativeMock::uniforms[12+n*3+row]==matrices[bindings[n]-1].normal[row]);}
    assert(std::memcmp(nativeMock::buffer.data,v.data(),sizeof(v))==0&&r->statistics().weightedDraws==1&&nativeMock::allocationCalls==allocated);
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS native pose graph -> native palette IDs -> weighted PICA shader uniform submission; original vertex bytes and zero per-draw GPU allocation (SDK substitutes)");
}

void nativeCameraFrame(C3D_RenderTarget&);
void nativeMirrorFrame(C3D_RenderTarget&,MeshHandle,MaterialHandle);
void nativeGamePackets(const char* meshPath,const char* imagePath,const char* materialPath){
    auto meshBytes=readFile(meshPath),imageBytes=readFile(imagePath),materialBytes=readFile(materialPath);MeshPack geometry;TexturePack pixels;MaterialPack fragments;
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none&&TexturePack::open(std::as_bytes(std::span(imageBytes)),pixels)==PackError::none&&MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    C3D_RenderTarget surface{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(!game::beginFrame()&&!game::initialize(nullptr));assert(game::initialize(&surface));auto& r=game::renderer();auto model=std::make_unique<Model>();
    std::array<PartBinding,3> parts{};parts[0].mesh=3;parts[0].material=23;parts[0].samplerMask=1;parts[1]=parts[0];parts[1].mesh=17;parts[1].uniformSlot=1;parts[2].mesh=3;parts[2].material=29;
    assert(model->load(r,geometry,fragments,pixels,parts));
    auto mirrorMesh=r.createMesh(vertices(),indices);auto mirrorMaterialState=Material::vertexColor();
    mirrorMaterialState.cull=Cull::front;auto mirrorMaterial=r.createMaterial(mirrorMaterialState);
    assert(mirrorMesh&&mirrorMaterial);
    struct Parameters{std::array<DrawUniforms,2> values;unsigned calls{};int failSlot=-1;bool reenter{};};
    Parameters a{{uniforms(),uniforms()}},b=a;for(auto* p:{&a,&b})for(auto& u:p->values){u.palette.count=3;u.material.suppliedMask=1;}
    a.values[0].material.colors[0]=0x11223344;a.values[1].material.colors[0]=0x55667788;b.values[0].material.colors[0]=0xffaabbcc;
    const GameUniformProducer produce=+[](const void* instance,unsigned slot,DrawUniforms& u,void* context)noexcept{
        auto& p=*static_cast<Parameters*>(context);++p.calls;
        if(p.reenter){auto nested=game::bindings().draw(instance,5);assert(!nested&&nested.error==Error::protocol);}
        if(slot>=p.values.size()||int(slot)==p.failSlot)return false;u=p.values[slot];return true;
    };
    int instanceA{},instanceB{},missing{};std::array<unsigned,3> shapeMap{5,5,8};auto& bridge=game::bindings();
    assert(bridge.bind(&instanceA,*model,shapeMap,produce,&a)&&bridge.bind(&instanceB,*model,shapeMap,produce,&b));
    assert(!bridge.bind(&instanceA,*model,shapeMap,produce,&a));auto wrongMap=shapeMap;wrongMap[2]=4096;assert(!bridge.bind(&missing,*model,wrongMap,produce,&a));shapeMap={99,99,99}; // Mapping is owned.
    namespace f=nativePacketFixture;f::surface=&surface;f::J3DShape shape5{5},shape8{8};f::J3DModel modelA{&instanceA},modelB{&instanceB},modelMissing{&missing};
    f::J3DShapePacket hidden{&modelMissing,&shape8,nullptr,true},second{&modelB,&shape8,&hidden,false},first{&modelA,&shape5,&second,false};f::J3DMatPacket packet{&first};
    assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&game::beginFrame());const auto allocations=nativeMock::allocationCalls;
    nativeCameraFrame(surface);
    nativeMirrorFrame(surface,mirrorMesh,mirrorMaterial);
    std::array<std::byte,32> original{};std::memcpy(original.data(),"J3D2bmd3",8);original[11]=std::byte{32};
    SourceIdentity missingContext;assert(identifySource(original,missingContext));missingContext.imageContext.fill(0xa5);
    const GameSourceProducer sourceProducer=+[](const void*,const UniformBinding&,DrawUniforms&,void*)noexcept{return true;};
    const auto beforeContextDraw=nativeMock::draws;nativeReport[0]=0;
    assert(!game::drawSourceShape(&missing,5,missingContext,sourceProducer,nullptr,63));
    assert(game::submittedParts()==0&&nativeMock::draws==beforeContextDraw&&nativeMock::allocationCalls==allocations);
    assert(!std::strcmp(game::failure(),"Native external-image context/variant proof missing"));
    assert(!std::strcmp(nativeReport,"Native draw source_sha256=c7915af33287ca440967b6050224b05018ad2cff170f412037c6b6e983701e6c image_context_sha256=unavailable\n"));
    nativeMock::traceDraws=true;nativeMock::traceSize=0;packet.draw();
    assert(nativeMock::traceSize==3&&nativeMock::drawTrace[0].count==6&&nativeMock::drawTrace[1].count==3&&nativeMock::drawTrace[2].count==6);
    assert(nativeMock::drawTrace[0].constant==0x11223344&&nativeMock::drawTrace[1].constant==0x55667788&&a.calls==2&&b.calls==1&&f::invalidations==2&&f::j3dSys.model==&modelB);
    nativeMock::traceDraws=false;
    const auto rejects=[&](auto fn){const auto before=nativeMock::draws;bool threw=false;try{fn();}catch(const std::runtime_error&){threw=true;}assert(threw&&nativeMock::draws==before);};
    f::J3DShapePacket absent{&modelMissing,&shape5,nullptr,false};rejects([&]{absent.draw();});
    a.failSlot=1;rejects([&]{first.drawFast();});a.failSlot=-1;
    a.reenter=true;rejects([&]{first.drawFast();});a.reenter=false;
    f::J3DDrawPacket rawPacket;f::J3DDisplayListObj rawList;rejects([&]{rawPacket.draw();});rejects([&]{rawList.callDL();});
    f::copySafe=false;rejects([&]{first.draw();});f::copySafe=true;
    hidden.next=&hidden;f::J3DMatPacket cycle{&hidden};rejects([&]{cycle.draw();});hidden.next=nullptr;
    assert(r.release(mirrorMesh)&&r.release(mirrorMaterial));
    assert(nativeMock::allocationCalls==allocations);assert(game::endFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);
    nativeMock::complete();assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&game::beginFrame());
    const auto firstDraw=nativeMock::draws,firstGuard=dusk::native::hostPrepareCalls;
    dusk::native::failHostPrepare=true;const auto buffers=nativeMock::bufferCalls;
    auto guarded=bridge.draw(&instanceA,5);
    assert(!guarded&&guarded.error==Error::backend&&!guarded.submitted&&nativeMock::draws==firstDraw&&nativeMock::bufferCalls==buffers);
    dusk::native::failHostPrepare=false;
    // Repeated instance submissions share the frame budget, not the resident
    // model's part count or one command arena's nominal capacity.
    for(unsigned n=0;n<maxFrameDraws/2-1;++n){auto result=bridge.draw(n&1?&instanceA:&instanceB,5);assert(result&&result.submitted==2);}
    assert(r.statistics().frameDraws==maxFrameDraws-2&&r.statistics().frameDraws>maxModelParts);
    assert(bridge.draw(&instanceA,8).submitted==1&&r.statistics().frameDraws==maxFrameDraws-1);
    const auto producerCalls=a.calls;
    auto over=bridge.draw(&instanceA,5);
    assert(!over&&over.error==Error::budget&&!over.submitted&&a.calls==producerCalls&&r.statistics().frameDraws==maxFrameDraws-1);
    assert(bridge.draw(&instanceB,8).submitted==1&&r.statistics().frameDraws==maxFrameDraws);
    over=bridge.draw(&instanceA,8);
    assert(!over&&over.error==Error::budget&&!over.submitted&&nativeMock::draws-firstDraw==maxFrameDraws);
    assert(dusk::native::hostPrepareCalls-firstGuard==maxFrameDraws+1);
    assert(game::endFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);
    std::puts("PASS game frame budget: 4096 repeated model draws, atomic cap rejection and per-draw command guard (SDK substitutes)");
    const auto resident=r.statistics().meshBytes;assert(resident&&model->unload()&&r.statistics().meshBytes==resident);assert(!C3D_FrameBegin(C3D_FRAME_NONBLOCK));
    nativeMock::complete();assert(C3D_FrameBegin(C3D_FRAME_NONBLOCK)&&game::beginFrame()&&r.statistics().meshBytes==0);
    assert(model->load(r,geometry,fragments,pixels,parts));const auto staleCalls=a.calls;rejects([&]{first.draw();});assert(a.calls==staleCalls); // Same C++ address, newer resident generation.
    assert(bridge.forget(&instanceA));std::array<unsigned,3> restored{5,5,8};assert(bridge.bind(&instanceA,*model,restored,produce,&a));first.next=nullptr;first.drawFast();
    nativeMock::failBufferAt=nativeMock::bufferCalls+2;const auto before=nativeMock::draws;bool threw=false;try{first.draw();}catch(const std::runtime_error&){threw=true;}assert(threw&&nativeMock::draws==before+1&&game::submittedParts()==1);nativeMock::failBufferAt=0;
    game::forget(&instanceA);game::forget(&instanceB);game::forget(&missing);assert(bridge.empty()&&model->unload());assert(game::endFrame());C3D_FrameEnd(GX_CMDLIST_FLUSH);nativeMock::complete();
    assert(game::releaseAfterContextDestroyed()&&nativeMock::allocations.empty());assert(!game::initialize(&surface));
    std::puts("PASS actual game material/shape/fast packet bodies -> native shape ownership and parameter producer -> native model -> Citro3D submission; hidden ownership, strict missing inputs, reentry, expired bindings, partial errors, cache invalidation and shared-frame retirement (SDK substitutes)");
}

}
#include "native_game_assets_cases.hpp"
#include "native_camera_cases.hpp"
#include "native_model_overrides_cases.hpp"
void nativeVramTextures(const char* meshPath,const char* materialPath){
    using namespace nativeMock;
    static bool barrierFails=false;static unsigned barriers=0;
    auto barrier=+[]{
        ++barriers;if(barrierFails)return false;
        C3D_FrameSplit(GX_CMDLIST_FLUSH);completeTransfers();return true;
    };
    auto r=std::make_unique<Renderer>();assert(r->initialize({})&&r->setTextureUploadBarrier(barrier));
    auto mesh=r->createMesh(vertices(),indices);auto material=r->createMaterial(Material::textured());assert(mesh&&material);
    TextureInfo info{16,16,TextureFormat::rgba8};info.mipLevels=2;
    std::array<std::byte,1280> image{};for(unsigned n=0;n<image.size();++n)image[n]=std::byte((n*29+(n>>8))&255);
    failVramAllocation=false;failAllocationAt=allocationCalls+1;
    assert(!r->createTexture(info,image)&&r->error()==Error::allocation&&vramAllocations.empty());
    failAllocationAt=0;failPublication=true;
    assert(!r->createTexture(info,image)&&r->error()==Error::publication&&vramAllocations.empty());
    failPublication=false;
    failVramAllocation=false;const auto transfersBefore=transferCalls,completedBefore=transferCompletions;
    const auto linearBefore=allocations.size();auto synchronous=r->createTexture(info,image);assert(synchronous);
    assert(transferCalls==transfersBefore+1&&transferCompletions==completedBefore+1&&lastTransfer.size==image.size());
    assert(vramAllocations.count(lastTransfer.destination)&&std::memcmp(lastTransfer.destination,image.data(),image.size())==0);
    assert(allocations.size()==linearBefore&&barriers==0); // Outside a frame the SDK waits for PPF.
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};
    assert(r->begin(&target)&&r->draw(mesh,material,{synchronous,{},{}},::uniforms()));
    assert(std::memcmp(textures[0]->data,image.data(),image.size())==0);
    auto first=r->createTexture(info,image);assert(first&&transfers.size()==1);
    const auto firstStage=const_cast<void*>(transfers[0].source);auto firstImage=transfers[0].destination;
    assert(allocations.count(firstStage)&&vramAllocations.count(firstImage));
    barrierFails=true;const auto completed=transferCompletions;
    assert(!r->createTexture(info,image)&&r->error()==Error::backend);
    assert(transferCompletions==completed&&transfers.size()==1&&allocations.count(firstStage));
    barrierFails=false;const auto freed=frees;
    auto second=r->createTexture(info,image);assert(second&&transferCompletions==completed+1&&frees==freed+1);
    assert(std::memcmp(firstImage,image.data(),image.size())==0&&transfers.size()==1);
    const auto pendingStage=const_cast<void*>(transfers[0].source);assert(allocations.count(pendingStage));
    assert(r->draw(mesh,material,{first,{},{}},::uniforms()));
    assert(textures[0]->data==firstImage&&std::memcmp(textures[0]->data,image.data(),image.size())==0);
    failVramAllocation=true;const auto transferCount=transferCalls;
    auto fallback=r->createTexture(info,image);assert(fallback&&transferCalls==transferCount);
    assert(r->draw(mesh,material,{fallback,{},{}},::uniforms()));
    void* linearImage=textures[0]->data;assert(allocations.count(linearImage)&&!vramAllocations.count(linearImage));
    assert(std::memcmp(linearImage,image.data(),image.size())==0);
    const auto vramFreed=vramFrees;
    assert(r->release(synchronous)&&r->release(first)&&r->release(second)&&r->release(fallback));
    assert(r->end()&&!r->begin(&target)&&r->error()==Error::busy);
    assert(allocations.count(pendingStage)&&allocations.count(linearImage)&&vramFrees==vramFreed);
    complete();assert(r->begin(&target));
    assert(vramAllocations.empty()&&vramFrees==vramFreed+3&&!allocations.count(linearImage));
    assert(r->statistics().textureBytes==256&&r->end());complete();
    assert(r->releaseAfterContextDestroyed()&&allocations.empty());

    // Model unload uses the same delayed retirement for both memory pools.
    auto meshBytes=readFile(meshPath),materialBytes=readFile(materialPath);auto bankBytes=modelOverrideCases::bank();
    MeshPack geometry;MaterialPack fragments;TexturePack bank;
    assert(MeshPack::open(std::as_bytes(std::span(meshBytes)),geometry)==PackError::none);
    assert(MaterialPack::open(std::as_bytes(std::span(materialBytes)),fragments)==PackError::none);
    assert(TexturePack::open(bankBytes,bank)==PackError::none);
    std::array<PartBinding,1> parts{};parts[0].mesh=3;parts[0].material=23;parts[0].samplerMask=1;
    auto vramModel=std::make_unique<Model>(),linearModel=std::make_unique<Model>();
    assert(r->initialize({}));failVramAllocation=false;
    assert(vramModel->load(*r,geometry,fragments,bank,parts)&&vramAllocations.size()==bank.size());
    failVramAllocation=true;assert(linearModel->load(*r,geometry,fragments,bank,parts));
    TextureView expected;assert(bank.imageAt(0,expected)==PackError::none);
    auto u=::uniforms();u.palette.count=3;u.material.suppliedMask=1;
    assert(r->begin(&target)&&vramModel->drawPart(0,u));
    assert(vramAllocations.count(textures[0]->data)&&std::memcmp(textures[0]->data,expected.payload.data(),expected.payload.size())==0);
    assert(linearModel->drawPart(0,u));linearImage=textures[0]->data;
    assert(allocations.count(linearImage)&&std::memcmp(linearImage,expected.payload.data(),expected.payload.size())==0);
    const auto modelFreed=vramFrees;assert(vramModel->unload()&&linearModel->unload());
    assert(vramFrees==modelFreed&&allocations.count(linearImage)&&r->end());
    assert(!r->begin(&target)&&r->error()==Error::busy);complete();assert(r->begin(&target));
    assert(vramAllocations.empty()&&vramFrees==modelFreed+bank.size()&&!allocations.count(linearImage));
    assert(r->statistics().textureBytes==256&&r->end());complete();
    assert(r->releaseAfterContextDestroyed()&&allocations.empty());
    std::puts("PASS native VRAM/full-mip transfers, completion-held staging, linear fallback and mixed-pool model retirement (SDK substitutes)");
}
void nativeProducerValues(){
    namespace f=nativeSourceFixture;f::Data data;f::J3DModel model{&data};
    UniformBinding binding;binding.material=23;binding.sourceStageCount=1;binding.sourceTexGenCount=1;binding.sourceAttributes=1u<<f::GX_VA_NRM;auto u=uniforms();
    binding.parameterReadMasks[0]=15;binding.parameterReadMasks[6]=15;
    data.material.color.count=2;
    for(unsigned i=0;i<4;++i){auto& channel=data.material.color.channels[i];channel.enabled=1;channel.mask=1u<<(i+4);}
    for(unsigned i=4;i<8;++i){f::globalLightProvided[i]=true;f::globalLights[i].mColor={0,0,0,255};}
    f::globalLights[4].mColor={17,34,51,0};f::globalLights[6].mColor={68,85,102,0};
    data.material.color.material[1]={17,34,51,68};
    data.material.registers[0]={1,2,3,4};data.material.konst[3]={5,6,7,8};
    assert(model.copyNativeParameters(binding,u)&&u.lightCount==4);
    assert(u.channels[0].lightMask==1&&u.channels[1].lightMask==4&&u.channels[2].lightMask==2&&u.channels[3].lightMask==8);
    assert((u.materialColors[1]==Row{17/255.f,34/255.f,51/255.f,68/255.f}));
    assert(u.material.colors[0]==0x04030201&&u.material.colors[6]==0x08070605&&u.material.suppliedMask==0xc1);
    f::globalLightProvided[7]=false;assert(!model.copyNativeParameters(binding,u));f::globalLightProvided[7]=true;
    binding.sourceAttributes=0;assert(!model.copyNativeParameters(binding,u));binding.sourceAttributes=1u<<f::GX_VA_NRM;
    data.material.color.channels[0].materialVertex=1;assert(!model.copyNativeParameters(binding,u));
    binding.sourceAttributes=(1u<<f::GX_VA_CLR0)|(1u<<f::GX_VA_NRM);
    assert(model.copyNativeParameters(binding,u));data.material.color.channels[0].materialVertex=0;
    data.material.registers[0].r=-1;assert(!model.copyNativeParameters(binding,u));data.material.registers[0].r=1;
    auto& fog=data.material.fog;fog.mType=4;fog.mColor={17,34,51,68};fog.mStartZ=10;fog.mEndZ=110;fog.mNearZ=1;fog.mFarZ=101;
    assert(model.copyNativeParameters(binding,u)&&u.fog.enabled&&u.fog.function==FogFunction::exp);
    assert(std::abs(u.fog.a-.0101f)<1e-6f&&std::abs(u.fog.b-1.01f)<1e-6f&&std::abs(u.fog.c-.1f)<1e-6f);
    assert(u.fog.color==0x44332211&&u.material.colors[7]==u.fog.color);
    fog.mAdjEnable=1;fog.mFogAdjTable.r[9]=4096;assert(!model.copyNativeParameters(binding,u));
    fog.mFogAdjTable.r[9]=384;assert(model.copyNativeParameters(binding,u)&&u.fog.range[9]==1.5f);
    fog.mType=3;assert(!model.copyNativeParameters(binding,u));fog.mType=0;fog.mAdjEnable=0;
    binding.textureSlots={0,255,255};binding.textureGenerators={0,255,255};
    assert(!model.copyNativeParameters(binding,u));binding.sourceAttributes|=1u<<f::GX_VA_TEX0;
    data.material.texture.images[0].wrapS=2;data.material.texture.images[0].LODBias=-125;
    assert(model.copyNativeParameters(binding,u)&&u.samplerMask==1&&u.imageOverrideMask==1&&u.imageOverrides[0]==1);
    assert(u.samplers[0].wrapS==Wrap::mirror&&u.samplers[0].lodBias==-320);
    data.material.coordinates[0].source=255;assert(!model.copyNativeParameters(binding,u));
    data.material.coordinates[0].source=f::GX_TG_TEX0;data.material.textureNumbers[0]=1;
    assert(!model.copyNativeParameters(binding,u));
    f::globalLightProvided.fill(false);
    std::puts("PASS actual typed material/fog/sampler/texgen producers: upper light slots, alpha-only lights, missing source attributes, signed-color and fog-range refusals");
}
void lightingFogRenderer(){
    auto r=std::make_unique<Renderer>();assert(r->initialize({}));
    auto mesh=r->createMesh(vertices(),indices);auto plain=r->createMaterial(Material::vertexColor());
    auto fogMaterial=Material::vertexColor();fogMaterial.stageCount=2;fogMaterial.fogCarrier=FogCarrier::primaryAlpha;
    fogMaterial.stages[0].alpha[0]=Source::constant;
    auto& stage=fogMaterial.stages[1];stage.rgb={Source::constant,Source::previous,Source::primary};
    stage.rgbFunction=Combine::interpolate;stage.rgbOperand={ColorOperand::color,ColorOperand::color,ColorOperand::alpha};
    stage.alpha[0]=Source::previous;stage.constantBindings={29,30,31,32};
    auto fogged=r->createMaterial(fogMaterial);
    auto lookup=Material::textured();lookup.textureMask=3;lookup.rasterLookup=true;
    auto lookupHandle=r->createMaterial(lookup);std::array<std::byte,256> pixels{};
    auto image=r->createTexture({8,8,TextureFormat::rgba8},pixels);assert(mesh&&plain&&fogged&&lookupHandle&&image);
    C3D_RenderTarget target{{reinterpret_cast<void*>(1),240,400,reinterpret_cast<void*>(2)}};assert(r->begin(&target));
    auto u=uniforms();u.lightCount=8;
    for(unsigned i=0;i<8;++i){
        auto& light=u.lights[i];light.position={float(i+1),float(i+2),float(i+3)};light.direction={0,0,1};
        light.color={float(i)/8,.25f,.5f,.75f};light.cosine={1,.5f,.25f};light.distance={2,1,.5f};
        light.kind=i&1?LightKind::directional:LightKind::point;
    }
    u.channels[0]={true,false,true,255,Diffuse::signedDot,Attenuation::spot};
    u.channels[1]={true,true,false,128,Diffuse::none,Attenuation::none};
    u.channels[2]={true,false,false,85,Diffuse::clamped,Attenuation::specular};
    u.channels[3]={true,true,true,170,Diffuse::none,Attenuation::none};
    u.materialColors={Row{.1f,.2f,.3f,.4f},Row{.5f,.6f,.7f,.8f}};
    u.ambientColors={Row{.8f,.7f,.6f,.5f},Row{.4f,.3f,.2f,.1f}};
    assert(r->draw(mesh,plain,{},u));
    for(unsigned i=0;i<8;++i){
        assert((nativeMock::uniforms[28+4*i]==Row{float(i+1),float(i+2),float(i+3),1}));
        assert(nativeMock::uniforms[29+4*i]==u.lights[i].color);
        assert((nativeMock::uniforms[30+4*i]==Row{0,0,1,.5f}));
        assert((nativeMock::uniforms[31+4*i]==Row{2,1,.5f,.25f}));
    }
    assert((nativeMock::integerUniforms[0]==std::array<unsigned,4>{7,0,4,0}));
    assert((nativeMock::uniforms[72]==Row{0,1,0,1})&&(nativeMock::uniforms[89]==Row{0,1,0,1}));
    assert((nativeMock::uniforms[64]==Row{2,1,2,15})&&(nativeMock::uniforms[65]==Row{1,1,1,1}));
    assert((nativeMock::uniforms[66]==Row{1,0,3,8})&&(nativeMock::uniforms[67]==Row{}));
    assert((nativeMock::uniforms[68]==Row{0,2,1,5})&&(nativeMock::uniforms[69]==Row{1,0,1,0}));
    assert((nativeMock::uniforms[70]==Row{3,0,3,10})&&(nativeMock::uniforms[71]==Row{0,1,0,1}));
    for(unsigned i=0;i<2;++i){assert(nativeMock::uniforms[60+i]==u.materialColors[i]);assert(nativeMock::uniforms[62+i]==u.ambientColors[i]);}
    for(auto& channel:u.channels)channel.enabled=false;
    u.channels[3].enabled=true;u.channels[3].lightMask=129;
    assert(r->draw(mesh,plain,{},u));
    assert(nativeMock::uniforms[29]==u.lights[0].color&&nativeMock::uniforms[33]==u.lights[7].color);
    assert((nativeMock::integerUniforms[0]==std::array<unsigned,4>{1,0,4,0}));
    assert((nativeMock::uniforms[71]==Row{1,1,0,0}));
    u=uniforms();u.fog.provided=true;u.fog.enabled=true;u.fog.function=FogFunction::reverseExpSquared;
    u.fog.rangeEnabled=true;u.fog.orthographic=true;u.fog.a=.25f;u.fog.b=2;u.fog.c=.5f;
    u.fog.rangeScale=3;u.fog.rangeBias=4;u.fog.color=0x87654321;
    for(unsigned i=0;i<10;++i)u.fog.range[i]=float(i+1)/4;
    u.material.suppliedMask=128;u.material.colors[7]=u.fog.color;
    assert(r->draw(mesh,fogged,{},u)&&nativeMock::env[1].color==0x87654321);
    assert((nativeMock::uniforms[84]==Row{.25f,2,.5f,3})&&(nativeMock::uniforms[85]==Row{4,5,1,1}));
    assert((nativeMock::uniforms[86]==Row{.25f,.5f,.75f,1})&&(nativeMock::uniforms[87]==Row{1.25f,1.5f,1.75f,2}));
    assert((nativeMock::uniforms[88]==Row{2.25f,2.5f,2.5f,2.5f})&&nativeMock::uniforms[83][1]==float(FogCarrier::primaryAlpha));
    const auto before=nativeMock::draws;
    u.material.colors[7]^=1;assert(!r->draw(mesh,fogged,{},u)&&nativeMock::draws==before);
    u.material.colors[7]=u.fog.color;u.fog.provided=false;assert(!r->draw(mesh,fogged,{},u)&&nativeMock::draws==before);
    u.fog.provided=true;assert(!r->draw(mesh,plain,{},u)&&nativeMock::draws==before);
    u=uniforms();assert(r->draw(mesh,lookupHandle,{image,image,{}},u));
    assert(nativeMock::uniforms[79][3]==1);
    u.samplerMask=1;assert(!r->draw(mesh,lookupHandle,{image,image,{}},u));
    u=uniforms();u.uvTransforms[1].source=0;assert(!r->draw(mesh,lookupHandle,{image,image,{}},u));
    assert(r->end());nativeMock::complete();assert(r->releaseAfterContextDestroyed()&&nativeMock::allocations.empty());
    std::puts("PASS eight-light/four-channel uploads, sparse upper membership, fog values/carrier refusal and lookup sampler ownership (host SDK captures)");
}
#include "native_mesh_bank_cases.hpp"
#include "native_model_large_cases.hpp"
int main(int argc,char** argv){assert(argc==8);nativeCameraContracts();contracts();renderer();nativeVramTextures(argv[1],argv[3]);nativeMeshBanks();lightingFogRenderer();nativeProducerValues();packedRenderer(argv[1]);packedTextures(argv[2]);uvRenderer();projectiveRenderer();packedMaterials(argv[3]);sourceCompiledMaterials(argv[4]);parameterizedMaterials(argv[3]);bufferedCompiledMaterials(argv[5]);samplerRenderer();nativeModels(argv[1],argv[2],argv[3]);nativeModelOverrides(argv[1],argv[3]);nativeFramebufferBorrow(argv[1],argv[3]);nativeModelLargeCases(argv[3],argv[7]);nativeScenes(argv[1],argv[2],argv[3]);nativePoseShader(argv[6]);nativeGameSources(argv[7]);nativeGameAssets(argv[7]);nativeGamePackets(argv[1],argv[2],argv[3]);}
