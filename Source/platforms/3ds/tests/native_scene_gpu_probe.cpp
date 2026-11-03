// Asset-free ACTUAL Renderer probe. Build: Tools/3ds/build_native_scene_probe.py.
// Lighting/fog use CPU GX evaluators. Cull comparison uses the compatibility
// projection/cull helpers as a proxy, not the full GX command renderer.
#include <3ds.h>
#include <citro3d.h>
#include "../native/renderer.hpp"
#include "../native/pack.hpp"
#include "../native/camera.hpp"
#include "../native/material_pack.hpp"
#include "../native/color_lookup.hpp"
#include "../runtime/gx_raster_color.hpp"
#include "../runtime/gx_vertex_transform.hpp"
#include "../runtime/gx_fog.hpp"
#include "../runtime/gx_pica_math.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern const unsigned char native_scene_fog_texture[8*256*4];
extern const unsigned char native_scene_cull_geometry[];
extern const unsigned int native_scene_cull_geometry_size;
extern const unsigned char native_scene_cull_material_0[],native_scene_cull_material_1[],native_scene_cull_material_2[];
extern const unsigned int native_scene_cull_material_0_size,native_scene_cull_material_1_size,native_scene_cull_material_2_size;

namespace {
namespace p = dusk::pica;
namespace gx = dusk::native::gx;
using Color = std::array<float,4>;
constexpr unsigned W=240,H=400,Bytes=W*H*4;
constexpr u32 Clear=0x17354f6b;
constexpr unsigned ExpectedCases=297,ExpectedCullCases=12;
static_assert(unsigned(p::Cull::none)==GPU_CULL_NONE&&unsigned(p::Cull::front)==GPU_CULL_FRONT_CCW&&unsigned(p::Cull::back)==GPU_CULL_BACK_CCW);
constexpr u8 ClearBytes[]{0x6b,0x4f,0x35,0x17};
constexpr u32 Transfer=GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_OUT_TILED(0)|
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
constexpr u32 LcdTransfer=GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_OUT_TILED(0)|
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
constexpr float XY[3][2]{{12,24},{228,64},{82,376}};
constexpr std::uint16_t Indices[]{0,1,2};
constexpr Color Texture{173/255.f,211/255.f,227/255.f,199/255.f};
constexpr Color Buffer{59/255.f,113/255.f,197/255.f,173/255.f};
constexpr Color Surface{67/255.f,139/255.f,213/255.f,179/255.f};
enum class Output { primary,secondary,product,fog,ordinary,fogProduct };
struct Case {
    char name[80]{};
    p::DrawUniforms u{};
    std::array<p::Vertex,3> vertices{};
    p::Material material{};
    Output output=Output::primary;
    bool textured=false;
    unsigned tolerance=3;
};
float dot(const std::array<float,3>& a,const std::array<float,3>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
bool directory(const char* path){return mkdir(path,0777)==0||errno==EEXIST;}
p::Source tex(unsigned unit){return p::Source(unsigned(p::Source::texture0)+unit);}
void pair(p::Stage& s,p::Source a,p::Source b,p::Combine function){s.rgb[0]=s.alpha[0]=a;s.rgb[1]=s.alpha[1]=b;s.rgbFunction=s.alphaFunction=function;}

Case fixture(const char* name,bool perspective=true){
    Case c;snprintf(c.name,sizeof(c.name),"%s",name);
    c.material=p::Material::vertexColor();c.material.cull=p::Cull::none;c.material.depthTest=c.material.depthWrite=false;
    // Explicit physical viewport. No implicit 3DS camera rotation: NDC maps to XY.
    // Perspective: clip=(x,y,.35*z-.1,-z); orthographic=(x,y,.22*z-.12,1).
    c.u.viewProjection={p::Row{1,0,0,0},p::Row{0,1,0,0},
        perspective?p::Row{0,0,.35f,-.1f}:p::Row{0,0,.22f,-.12f},
        perspective?p::Row{0,0,-1,0}:p::Row{0,0,0,1}};
    constexpr float ws[]{1,3,.65f};
    constexpr std::uint8_t a[3][4]{{240,35,210,61},{31,231,58,239},{167,93,247,113}};
    constexpr std::uint8_t b[3][4]{{21,217,74,226},{231,48,197,39},{107,157,28,151}};
    constexpr float normals[3][3]{{.3f,.1f,.95f},{-.7f,.2f,-.68f},{.1f,-.55f,.83f}};
    for(unsigned i=0;i<3;++i){auto& v=c.vertices[i];const float w=perspective?ws[i]:1;
        v.position={(2*XY[i][0]/W-1)*w,(2*XY[i][1]/H-1)*w,-ws[i]};
        for(unsigned j=0;j<3;++j)v.normal[j]=normals[i][j];
        for(unsigned j=0;j<4;++j){v.color[j]=a[i][j];v.color1[j]=b[i][j];}
        for(auto& uv:v.uv)uv={.3f+.1f*i,.6f-.1f*i};
    }
    c.u.materialColors={p::Row{.71f,.43f,.62f,.56f},p::Row{.37f,.68f,.49f,.73f}};
    c.u.ambientColors={p::Row{.39f,.33f,.29f,.41f},p::Row{.21f,.36f,.44f,.32f}};
    c.u.lightCount=8;
    for(unsigned i=0;i<8;++i){auto& l=c.u.lights[i];
        l.position={-.8f+.23f*i,.6f-.13f*i,1.4f+.17f*i};
        l.direction={.2f,-.1f,-.974679434f};
        l.color={.05f+.007f*i,.11f-.008f*i,.04f+.011f*i,.09f-.005f*i};
        l.cosine={.23f,.37f,.19f};l.distance={1.2f,.17f,.035f};
    }
    return c;
}
void lookup(Case& c,unsigned base,Output output,bool textured=false){
    c.output=output;c.textured=textured;auto& m=c.material;
    m.rasterLookup=true;m.lookupBase=base;m.textureMask=(1u<<(base+2))-1;m.stageCount=2;
    // Reconstruct B.rgb and B.a from separate RG/BA mathematical images.
    pair(m.stages[0],tex(base),tex(base+1),p::Combine::add);
    if(output==Output::secondary){pair(m.stages[1],p::Source::previous,p::Source::primary,p::Combine::replace);}
    else {
        m.bufferRgbMask=m.bufferAlphaMask=1;
        pair(m.stages[1],p::Source::primary,p::Source::primary,p::Combine::replace);
        pair(m.stages[2],p::Source::previous,p::Source::buffer,p::Combine::modulate);m.stageCount=3;
    }
    if(textured){pair(m.stages[m.stageCount++],p::Source::previous,p::Source::texture0,p::Combine::modulate);}
    c.tolerance=output==Output::secondary?3:4; // Bytes, including LUT/TEV UNORM8 rounding.
}
void fogMaterial(Case& c,p::FogCarrier carrier,unsigned base,bool product=false){
    if(product){lookup(c,0,Output::product);c.output=Output::fogProduct;}
    else {
        c.output=Output::fog;c.tolerance=3;c.material.stageCount=1;
        c.material.stages[0].rgb[0]=c.material.stages[0].alpha[0]=p::Source::constant;c.material.stages[0].constant=0xb3d58b43;
    }
    auto& m=c.material;m.fogCarrier=carrier;
    const bool secondary=carrier==p::FogCarrier::secondaryAlpha||carrier==p::FogCarrier::secondaryRed;
    const bool texture=carrier>=p::FogCarrier::texture0;
    const unsigned unit=texture?unsigned(carrier)-unsigned(p::FogCarrier::texture0):0;
    if(secondary){m.rasterLookup=true;m.lookupBase=base;m.textureMask=(1u<<(base+2))-1;}
    if(texture)m.textureMask|=1u<<unit;
    auto& s=m.stages[m.stageCount++];const bool red=carrier==p::FogCarrier::primaryRed||carrier==p::FogCarrier::secondaryRed;
    const auto source=texture?tex(unit):secondary?tex(base+(red?0:1)):p::Source::primary;
    s.rgb={p::Source::constant,p::Source::previous,source};s.rgbFunction=p::Combine::interpolate;
    s.rgbOperand[2]=red?p::ColorOperand::red:p::ColorOperand::alpha;
    s.alpha[0]=p::Source::previous;s.constantBindings={29,30,31,32};
    c.u.material.suppliedMask=128;c.u.material.colors[7]=c.u.fog.color;
}
Case ordinary(){
    auto c=fixture("ordinary-baseline",false);c.output=Output::ordinary;c.textured=true;
    auto& m=c.material;m.textureMask=1;m.stageCount=4;m.bufferColor=0xadc5713b;
    pair(m.stages[1],p::Source::previous,p::Source::primary,p::Combine::replace);
    pair(m.stages[2],p::Source::previous,p::Source::buffer,p::Combine::modulate);
    pair(m.stages[3],p::Source::previous,p::Source::texture0,p::Combine::modulate);
    return c;
}

// Direct construction of the CPU evaluator plan: no XF/BP registers or decoder.
// Native outward spot directions are negated for GX's surface-to-light convention.
// A native directional light has no GX position equivalent: use analytic N.L;
// these directional fixtures deliberately use attenuation=none.
bool vertexColors(const Case& c,unsigned index,Color (&out)[2],std::array<float,4>& clip){
    const auto& v=c.vertices[index];gx::TransformedVertex transformed{};
    // Both palette entries in weighted fixtures are identity. Apply the actual
    // palette anyway, so the reference is explicit about camera/view coordinates.
    for(unsigned b=0;b<4;++b)if(v.weights[b])for(unsigned r=0;r<3;++r){const auto row=v.boneRows[b]+r;
        const auto& m=c.u.palette.position[row];const auto& n=c.u.palette.normal[row];const float w=v.weights[b]/255.f;
        transformed.viewPosition[r]+=w*(m[0]*v.position[0]+m[1]*v.position[1]+m[2]*v.position[2]+m[3]);
        transformed.viewNormal[r]+=w*(n[0]*v.normal[0]+n[1]*v.normal[1]+n[2]*v.normal[2]);
    }
    const float length=std::sqrt(dot(transformed.viewNormal,transformed.viewNormal));if(!(length>0))return false;
    for(auto& x:transformed.viewNormal)x/=length;transformed.normalValid=true;
    for(unsigned r=0;r<4;++r){const auto& m=c.u.viewProjection[r];clip[r]=m[0]*transformed.viewPosition[0]+m[1]*transformed.viewPosition[1]+m[2]*transformed.viewPosition[2]+m[3];}
    gx::DecodedVertex decoded{};decoded.present=(1u<<GX_VA_CLR0)|(1u<<GX_VA_CLR1);
    for(unsigned j=0;j<4;++j){decoded.color[0][j]=v.color[j]/255.f;decoded.color[1][j]=v.color1[j]/255.f;}
    for(unsigned raster=0;raster<2;++raster){
        gx::RasterColorPlan plan{};plan.required=3;plan.rasterChannel=raster;plan.strictFinite=true;
        plan.material=c.u.materialColors[raster];plan.ambient=c.u.ambientColors[raster];plan.needsNormal=plan.needsPosition=true;
        for(unsigned a=0;a<2;++a){const auto& control=c.u.channels[raster+2*a];
            if(control.enabled)plan.lit|=1u<<a;if(control.materialVertex)plan.vertex|=1u<<a;if(control.ambientVertex)plan.ambientVertex|=1u<<a;
            auto& channel=plan.channels[a];channel.diffuse=unsigned(control.diffuse);channel.attenuation=unsigned(control.attenuation);
            for(unsigned l=0;l<c.u.lightCount;++l)if(control.lightMask&(1u<<l)){
                const auto& light=c.u.lights[l];
                if(light.kind==p::LightKind::directional){
                    float diffuse=control.diffuse==p::Diffuse::none?1:dot(transformed.viewNormal,light.direction);
                    if(control.diffuse==p::Diffuse::clamped)diffuse=std::max(0.f,diffuse);
                    // Fold directional contribution into ambient before GX's final clamp.
                    for(unsigned j=a?3:0;j<(a?4:3);++j)plan.ambient[j]+=diffuse*light.color[j];
                }else{channel.mask|=1u<<l;auto& dest=plan.lights[l];dest.position=light.position;dest.color=light.color;dest.cosine=light.cosine;dest.distance=light.distance;
                    for(unsigned j=0;j<3;++j)dest.direction[j]=(control.attenuation==p::Attenuation::spot?-1:1)*light.direction[j];}
            }
        }
        if(!gx::evaluateRasterColor(plan,decoded,out[raster],nullptr,&transformed))return false;
    }
    return true;
}
float vertexFog(const Case& c,const std::array<float,4>& clip){
    const auto& f=c.u.fog;gx::VertexFogPlan plan{};
    plan.a=f.a;plan.b=f.b;plan.c=f.c;plan.enabled=f.enabled;plan.orthographic=f.orthographic;plan.rangeEnabled=f.rangeEnabled;
    constexpr unsigned functions[]{2,4,5,6,7};plan.function=functions[unsigned(f.function)];
    // Re-express the native affine range coordinate as the GX helper's
    // logical-screen coordinate, retaining the original depth and W.
    // With viewportWidth=H and center=H/2, helper offset=-NDCy.
    auto rangeClip=clip;rangeClip[1]=-(f.rangeScale*clip[1]+f.rangeBias*clip[3]);
    plan.viewportWidth=H;plan.rangeCenter=H*.5;for(unsigned i=0;i<10;++i)plan.rangeK[i]=f.range[i];
    return gx::evaluateVertexFog(plan,rangeClip,H);
}

struct Probe {
    p::Renderer renderer;C3D_RenderTarget* targets[2]{};u8* pixels=nullptr;
    u8 snapshots[3][Bytes]{},lcdPixels[W*H*3]{};
    std::array<p::TextureHandle,3> textures{};p::TextureHandle fogTexture{};p::MeshBankHandle ordinaryMesh{};p::MaterialHandle ordinaryMaterial{};
    Case normal=ordinary();char folder[192]{};const char* error="initialization incomplete";
    bool initialized=false,rendererReady=false;FILE* metrics=nullptr;FILE* cullMetrics=nullptr;
    unsigned completed=0,required=0,cullCompleted=0,cullRequired=0,failed=0;char detail[192]{};
    ~Probe(){shutdown();}
    void shutdown(){
        if(metrics){fclose(metrics);metrics=nullptr;}
        if(cullMetrics){fclose(cullMetrics);cullMetrics=nullptr;}
        if(initialized){if(renderer.frameActive())renderer.end();for(auto*& t:targets)if(t){C3D_RenderTargetDelete(t);t=nullptr;}
            C3D_Fini();initialized=false;if(rendererReady){renderer.releaseAfterContextDestroyed();rendererReady=false;}
            if(pixels){linearFree(pixels);pixels=nullptr;}}
    }
    bool fail(const char* text){error=text;return false;}
    bool native(bool okay){if(okay)return true;snprintf(detail,sizeof(detail),"Renderer error %u: %s",unsigned(renderer.error()),renderer.errorDetail()?renderer.errorDetail():"no detail");return fail(detail);}
    p::MeshBankHandle createBank(const Case& c){
        // The unused first range has a different vertex/index base and needs
        // four palette entries. Only range1 belongs to the CPU reference.
        std::array<p::Vertex,4> decoy{c.vertices[0],c.vertices[1],c.vertices[2],c.vertices[0]};
        for(auto& v:decoy){v.position[0]*=.5f;v.position[1]*=.5f;v.color={255,0,255,255};v.boneRows={9,0,0,0};v.weights={255,0,0,0};}
        constexpr std::uint16_t decoyIndices[]{0,1,3,1,2,3};
        const std::array<p::MeshView,2> ranges{{
            {1,std::as_bytes(std::span(decoy)),std::as_bytes(std::span(decoyIndices))},
            {2,std::as_bytes(std::span(c.vertices)),std::as_bytes(std::span(Indices))}
        }};
        return renderer.createMeshBank(ranges);
    }
    bool save(const char* name,const void* data,size_t size){char path[320];snprintf(path,sizeof(path),"%s/%s",folder,name);FILE* f=fopen(path,"wb");if(!f)return fail("capture open failed");
        const bool written=fwrite(data,1,size,f)==size;const bool closed=fclose(f)==0;return written&&closed?true:fail("capture write/close failed");}
    bool init(){
        if(!directory("sdmc:/3ds")||!directory("sdmc:/3ds/dusklight-native-scene"))return fail("SD root creation failed");
        snprintf(folder,sizeof(folder),"sdmc:/3ds/dusklight-native-scene/run-%016llx",static_cast<unsigned long long>(svcGetSystemTick()));
        if(mkdir(folder,0777)){folder[0]=0;return fail("isolated run directory creation failed");}
        const char pending[]="status=running\nrunner=unlabelled; emulated-or-physical unknown\n";if(!save("receipt.txt",pending,sizeof(pending)-1))return false;
        p::Limits limits;if(!C3D_Init(limits.commandBytes))return fail("C3D_Init failed");initialized=true;
        if(!native(renderer.initialize(limits)))return false;rendererReady=true;
        for(auto& t:targets){t=C3D_RenderTargetCreate(W,H,GPU_RB_RGBA8,-1);if(!t)return fail("render target allocation failed");}
        pixels=static_cast<u8*>(linearAlloc(3*Bytes));if(!pixels)return fail("linear readback allocation failed");memset(pixels,0,3*Bytes);
        if(R_FAILED(GSPGPU_FlushDataCache(pixels,3*Bytes)))return fail("readback cache flush failed");
        p::TextureInfo info{8,8,p::TextureFormat::rgba8};
        for(unsigned i=0;i<3;++i){auto bytes=p::colorLookupImage(i?i-1:0);
            if(!i)for(unsigned at=0;at<bytes.size();at+=4)for(unsigned j=0;j<4;++j)bytes[at+3-j]=std::uint8_t(std::lround(Texture[j]*255));
            textures[i]=renderer.createTexture(info,std::as_bytes(std::span(bytes)));if(!native(bool(textures[i])))return false;
            char name[32];snprintf(name,sizeof(name),"texture%u-tiled.abgr8",i);if(!save(name,bytes.data(),bytes.size()))return false;
        }
        const auto fogBytes=std::as_bytes(std::span(native_scene_fog_texture));
        fogTexture=renderer.createTexture({8,256,p::TextureFormat::rgba8},fogBytes);
        if(!native(bool(fogTexture))||!save("fog-texture-tiled.abgr8",fogBytes.data(),fogBytes.size()))return false;
        ordinaryMesh=createBank(normal);ordinaryMaterial=renderer.createMaterial(normal.material);
        if(!native(bool(ordinaryMesh)&&bool(ordinaryMaterial)))return false;
        char path[320];snprintf(path,sizeof(path),"%s/metrics.csv",folder);metrics=fopen(path,"w");if(!metrics)return fail("metrics open failed");
        fputs("case,tolerance,covered,interior,missing,exterior_changed,over,max_r,max_g,max_b,max_a,restore_pixels,restore_different,negative_over8,negative_max,cpu_ms,gpu_wait_ms,compare_ms,previous_gpu_ms,pass\n",metrics);
        return true;
    }
    std::array<p::TextureHandle,3> bindings(const Case& c){
        std::array<p::TextureHandle,3> tx{};if(c.material.textureMask&1)tx[0]=textures[0];
        if(c.material.rasterLookup){tx[c.material.lookupBase]=textures[1];tx[c.material.lookupBase+1]=textures[2];}
        if(c.material.fogCarrier>=p::FogCarrier::texture0)tx[unsigned(c.material.fogCarrier)-unsigned(p::FogCarrier::texture0)]=fogTexture;
        return tx;
    }
    bool begin(){
        const auto deadline=svcGetSystemTick()+SYSCLOCK_ARM11*5ULL;
        while(!renderer.begin(targets[0])){
            if(renderer.error()!=p::Error::busy)return native(false);
            if(!aptMainLoop()||svcGetSystemTick()>deadline)return fail("GPU busy beyond 5s begin deadline");svcSleepThread(1000000);
        }return true;
    }
    static void acquire(u8* destination,const volatile u8* source,unsigned size){
        // Scalar CPU reads acquire the COMPLETE surface before any FS IPC.
        // A memcpy/IPC read of Azahar's linear transfer memory can download partial rows.
        for(unsigned at=0;at<size;++at)destination[at]=source[at];
    }
    bool readback(unsigned target,unsigned slot,const char* name,const char* lcdName){
        const auto& fb=targets[target]->frameBuf;
        if(fb.width!=W||fb.height!=H||fb.colorFmt!=GPU_RB_RGBA8||fb.block32)return fail("unexpected framebuffer layout");
        // This drains the draw queue and waits PPF OUTSIDE the frame. VBlank is not a fence.
        C3D_SyncDisplayTransfer(static_cast<u32*>(fb.colorBuf),GX_BUFFER_DIM(W,H),reinterpret_cast<u32*>(pixels+slot*Bytes),GX_BUFFER_DIM(W,H),Transfer);
        if(R_FAILED(GSPGPU_InvalidateDataCache(pixels+slot*Bytes,Bytes)))return fail("readback invalidate failed");
        acquire(snapshots[slot],pixels+slot*Bytes,Bytes);
        u16 w=0,h=0;u8* lcd=gfxGetFramebuffer(GFX_TOP,GFX_LEFT,&w,&h);
        if(!lcd||w!=W||h!=H||gfxGetScreenFormat(GFX_TOP)!=GSP_BGR8_OES)return fail("unexpected LCD format");
        if(R_FAILED(GSPGPU_FlushDataCache(lcd,W*H*3)))return fail("LCD flush failed");
        C3D_SyncDisplayTransfer(static_cast<u32*>(fb.colorBuf),GX_BUFFER_DIM(W,H),reinterpret_cast<u32*>(lcd),GX_BUFFER_DIM(W,H),LcdTransfer);
        if(R_FAILED(GSPGPU_InvalidateDataCache(lcd,W*H*3)))return fail("LCD invalidate failed");
        acquire(lcdPixels,lcd,W*H*3);
        for(unsigned at=0;at<W*H;++at)
            if(memcmp(lcdPixels+3*at,snapshots[slot]+4*at+1,3))return fail("LCD RGB transfer disagrees with CPU-acquired RGBA readback");
        gfxScreenSwapBuffers(GFX_TOP,false);gspWaitForVBlank();
        // Both saves and later comparisons use CPU-owned snapshots, never transfer memory.
        return save(name,snapshots[slot],Bytes)&&save(lcdName,lcdPixels,W*H*3);
    }
    bool inputs(const Case& c){
        char name[112];snprintf(name,sizeof(name),"%s-inputs.txt",c.name);char path[320];snprintf(path,sizeof(path),"%s/%s",folder,name);FILE* f=fopen(path,"w");if(!f)return fail("source inputs open failed");
        fprintf(f,"case=%s\noutput=%u textured=%u tolerance=%u lookup=%u base=%u carrier=%u cull=%u\n",c.name,unsigned(c.output),c.textured,c.tolerance,c.material.rasterLookup,c.material.lookupBase,unsigned(c.material.fogCarrier),unsigned(c.material.cull));
        for(unsigned r=0;r<4;++r){const auto& m=c.u.viewProjection[r];fprintf(f,"projection[%u]=%.9g %.9g %.9g %.9g\n",r,m[0],m[1],m[2],m[3]);}
        for(unsigned i=0;i<3;++i){const auto& v=c.vertices[i];fprintf(f,"vertex[%u] position=%.9g %.9g %.9g normal=%.9g %.9g %.9g color=%u %u %u %u color1=%u %u %u %u weights=%u %u %u %u rows=%u %u %u %u\n",i,v.position[0],v.position[1],v.position[2],v.normal[0],v.normal[1],v.normal[2],v.color[0],v.color[1],v.color[2],v.color[3],v.color1[0],v.color1[1],v.color1[2],v.color1[3],v.weights[0],v.weights[1],v.weights[2],v.weights[3],v.boneRows[0],v.boneRows[1],v.boneRows[2],v.boneRows[3]);
            for(unsigned j=0;j<3;++j)fprintf(f,"uv[%u,%u]=%.9g %.9g\n",i,j,v.uv[j][0],v.uv[j][1]);}
        for(unsigned r=0;r<c.u.palette.count*3;++r){const auto& m=c.u.palette.position[r];const auto& n=c.u.palette.normal[r];fprintf(f,"palette[%u]=%.9g %.9g %.9g %.9g normal=%.9g %.9g %.9g\n",r,m[0],m[1],m[2],m[3],n[0],n[1],n[2]);}
        for(unsigned i=0;i<4;++i){const auto& ch=c.u.channels[i];fprintf(f,"channel[%u] enabled=%u materialVertex=%u ambientVertex=%u mask=%02x diffuse=%u attenuation=%u\n",i,ch.enabled,ch.materialVertex,ch.ambientVertex,ch.lightMask,unsigned(ch.diffuse),unsigned(ch.attenuation));}
        for(unsigned i=0;i<2;++i){const auto& m=c.u.materialColors[i];const auto& a=c.u.ambientColors[i];fprintf(f,"material[%u]=%.9g %.9g %.9g %.9g ambient=%.9g %.9g %.9g %.9g\n",i,m[0],m[1],m[2],m[3],a[0],a[1],a[2],a[3]);}
        for(unsigned i=0;i<c.u.lightCount;++i){const auto& l=c.u.lights[i];fprintf(f,"light[%u] kind=%u position=%.9g %.9g %.9g direction=%.9g %.9g %.9g color=%.9g %.9g %.9g %.9g cosine=%.9g %.9g %.9g distance=%.9g %.9g %.9g\n",i,unsigned(l.kind),l.position[0],l.position[1],l.position[2],l.direction[0],l.direction[1],l.direction[2],l.color[0],l.color[1],l.color[2],l.color[3],l.cosine[0],l.cosine[1],l.cosine[2],l.distance[0],l.distance[1],l.distance[2]);}
        const auto& fog=c.u.fog;fprintf(f,"fog provided=%u enabled=%u ortho=%u range=%u function=%u a=%.9g b=%.9g c=%.9g scale=%.9g bias=%.9g color=%08lx\n",fog.provided,fog.enabled,fog.orthographic,fog.rangeEnabled,unsigned(fog.function),fog.a,fog.b,fog.c,fog.rangeScale,fog.rangeBias,static_cast<unsigned long>(fog.color));
        for(unsigned i=0;i<10;++i)fprintf(f,"range[%u]=%.9g\n",i,fog.range[i]);
        for(unsigned i=0;i<3;++i){const auto& t=c.u.uvTransforms[i];fprintf(f,"uvTransform[%u] source=%u rows=%.9g %.9g %.9g / %.9g %.9g %.9g\n",i,t.source,t.rows[0][0],t.rows[0][1],t.rows[0][2],t.rows[1][0],t.rows[1][1],t.rows[1][2]);}
        for(unsigned i=0;i<3;++i){const auto& r=c.u.projective.rows[i];fprintf(f,"projective[%u] enabled=%u source=%u row=%.9g %.9g %.9g %.9g\n",i,c.u.projective.enabled,unsigned(c.u.projective.source),r[0],r[1],r[2],r[3]);}
        const bool okay=!ferror(f);const bool closed=fclose(f)==0;return okay&&closed?true:fail("source inputs write failed");
    }
    bool compare(const Case& c,bool restore,double cpuMs,double waitMs){
        const u64 started=svcGetSystemTick();Color colors[3][2]{};std::array<float,4> clips[3]{};float fog[3]{};
        for(unsigned i=0;i<3;++i){if(!vertexColors(c,i,colors[i],clips[i]))return fail("CPU lighting reference rejected inputs");fog[i]=vertexFog(c,clips[i]);}
        unsigned covered=0,interior=0,missing=0,exterior=0,over=0,maxima[4]{},restorePixels=0,restoreDifferent=0,negative=0,negativeMax=0;
        const float determinant=(XY[1][1]-XY[2][1])*(XY[0][0]-XY[2][0])+(XY[2][0]-XY[1][0])*(XY[0][1]-XY[2][1]);
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const unsigned at=4*(y*W+x);const u8* actual=snapshots[0]+at;
            const bool drawn=memcmp(actual,ClearBytes,4)!=0;covered+=drawn;
            if(restore){const auto* after=snapshots[1]+at;const auto* before=snapshots[2]+at;restorePixels+=memcmp(before,ClearBytes,4)!=0;restoreDifferent+=memcmp(after,before,4)!=0;}
            // Raw rows start at the top; fixture XY uses the bottom-origin viewport.
            const float px=x+.5f,py=H-y-.5f;
            float b[3];b[0]=((XY[1][1]-XY[2][1])*(px-XY[2][0])+(XY[2][0]-XY[1][0])*(py-XY[2][1]))/determinant;
            b[1]=((XY[2][1]-XY[0][1])*(px-XY[2][0])+(XY[0][0]-XY[2][0])*(py-XY[2][1]))/determinant;b[2]=1-b[0]-b[1];
            const float minimum=std::min({b[0],b[1],b[2]});if(minimum<-.025f){exterior+=drawn;continue;}if(minimum<.025f)continue;
            ++interior;missing+=!drawn;float denominator=0;for(unsigned i=0;i<3;++i){b[i]/=clips[i][3];denominator+=b[i];}for(float& value:b)value/=denominator;
            Color a{},s{},wrong{};float amount=0;for(unsigned i=0;i<3;++i){amount+=b[i]*fog[i];for(unsigned j=0;j<4;++j){a[j]+=b[i]*colors[i][0][j];s[j]+=b[i]*colors[i][1][j];wrong[j]+=b[i]*colors[i][0][j]*colors[i][1][j];}}
            Color expected=a;if(c.output==Output::secondary)expected=s;
            if(c.output==Output::product||c.output==Output::fogProduct)for(unsigned j=0;j<4;++j)expected[j]=a[j]*s[j]; // Interpolate separately, THEN multiply.
            if(c.output==Output::fog)expected=Surface;
            if(c.output==Output::fog||c.output==Output::fogProduct)for(unsigned j=0;j<3;++j){const float f=((c.u.fog.color>>(8*j))&255)/255.f;expected[j]=expected[j]*(1-amount)+f*amount;}
            if(c.output==Output::ordinary)for(unsigned j=0;j<4;++j)expected[j]*=Buffer[j];
            if(c.textured)for(unsigned j=0;j<4;++j){expected[j]*=Texture[j];wrong[j]*=Texture[j];}
            unsigned delta=0,negativeDelta=0;for(unsigned j=0;j<4;++j){const int reference=std::lround(std::clamp(expected[j],0.f,1.f)*255);const unsigned d=std::abs(int(actual[3-j])-reference);maxima[j]=std::max(maxima[j],d);delta=std::max(delta,d);
                negativeDelta=std::max(negativeDelta,unsigned(std::abs(reference-int(std::lround(wrong[j]*255)))));}
            over+=delta>c.tolerance;if(c.output==Output::product){negative+=negativeDelta>8;negativeMax=std::max(negativeMax,negativeDelta);}
        }
        const bool negativeRequired=c.output==Output::product&&strncmp(c.name,"lookup-",7)==0;
        const bool okay=interior>20000&&covered>25000&&!missing&&!exterior&&!over&&(!restore||(restorePixels>25000&&!restoreDifferent))&&(!negativeRequired||negative>1000);
        const double compareMs=(svcGetSystemTick()-started)*1000.0/SYSCLOCK_ARM11;
        if(fprintf(metrics,"%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.5f,%.5f,%.5f,%.5f,%u\n",c.name,c.tolerance,covered,interior,missing,exterior,over,maxima[0],maxima[1],maxima[2],maxima[3],restorePixels,restoreDifferent,negative,negativeMax,cpuMs,waitMs,compareMs,renderer.statistics().previousGpuMilliseconds,okay)<0||fflush(metrics))return fail("metrics write failed");
        if(!okay)++failed;printf("%s: %s max=%u/%u/%u/%u coverage=%u\n",c.name,okay?"pass":"FAIL",maxima[0],maxima[1],maxima[2],maxima[3],interior);
        return true; // A comparison failure is recorded, never hides subsequent cases.
    }
    bool one(Case& c){
        ++required;if(!aptMainLoop())return fail("interrupted before matrix completion");if(!inputs(c))return false;
        // All resources resident BEFORE any draw. Never upload after emitted=true.
        const auto mesh=createBank(c);const auto material=renderer.createMaterial(c.material);
        if(!native(bool(mesh)&&bool(material)))return false;
        const u64 started=svcGetSystemTick();if(!begin())return false;
        if(!native(renderer.clear(Clear,0,true,false))||!native(renderer.draw(mesh,1,material,bindings(c),c.u)))return false;
        // Immediate ordinary draw, same frame, without a reset or foreign C3D state mutation.
        if(!native(renderer.target(targets[1]))||!native(renderer.clear(Clear,0,true,false))||!native(renderer.draw(ordinaryMesh,1,ordinaryMaterial,bindings(normal),normal.u))||!native(renderer.end()))return false;
        const u64 submitted=svcGetSystemTick();char name[112],lcdName[112];
        snprintf(name,sizeof(name),"%s-ordinary-after.abgr8",c.name);snprintf(lcdName,sizeof(lcdName),"%s-ordinary-after-lcd.bgr8",c.name);if(!readback(1,1,name,lcdName))return false;
        snprintf(name,sizeof(name),"%s.abgr8",c.name);snprintf(lcdName,sizeof(lcdName),"%s-lcd.bgr8",c.name);if(!readback(0,0,name,lcdName))return false;
        const u64 completedTick=svcGetSystemTick();
        if(!compare(c,true,(submitted-started)*1000.0/SYSCLOCK_ARM11,(completedTick-submitted)*1000.0/SYSCLOCK_ARM11))return false;
        ++completed;return native(renderer.release(mesh))&&native(renderer.release(material));
    }
    bool cullCases(){
        char path[320];snprintf(path,sizeof(path),"%s/cull_metrics.csv",folder);
        cullMetrics=fopen(path,"w");if(!cullMetrics)return fail("cull metrics open failed");
        if(fputs("projection,clockwise,gx_cull,compiled_native_cull,compat_pica_mode,expected_visible,native_covered,compat_covered,interior,missing,exterior,different_pixels,pass\n",cullMetrics)<0)return fail("cull metrics header failed");
        p::MeshPack geometry;
        if(p::MeshPack::open(std::as_bytes(std::span(native_scene_cull_geometry,native_scene_cull_geometry_size)),geometry)!=p::PackError::none)return fail("compiled cull geometry bank rejected");
        p::MeshView sourceMesh;
        if(geometry.mesh(1,sourceMesh)!=p::PackError::none||sourceMesh.vertices.size()!=3*sizeof(p::Vertex)||sourceMesh.indices.size()!=3*sizeof(std::uint16_t))return fail("compiled cull triangle layout changed");
        const unsigned char* materialData[]{native_scene_cull_material_0,native_scene_cull_material_1,native_scene_cull_material_2};
        const unsigned materialSizes[]{native_scene_cull_material_0_size,native_scene_cull_material_1_size,native_scene_cull_material_2_size};
        for(unsigned perspective=0;perspective<2;++perspective)for(unsigned reverse=0;reverse<2;++reverse)for(unsigned mode=0;mode<3;++mode){
            ++cullRequired;if(!aptMainLoop())return fail("interrupted before cull matrix completion");
            char name[80];snprintf(name,sizeof(name),"cull-proj-%u-order-%u-gx-%u",perspective,reverse,mode);
            auto c=fixture(name,perspective);c.u.lightCount=0;c.u.palette.count=2;
            for(unsigned bone=0;bone<2;++bone)c.u.palette.position[bone*3+2][3]=-1;
            for(unsigned i=0;i<3;++i){
                std::uint16_t index;memcpy(&index,sourceMesh.indices.data()+i*sizeof(index),sizeof(index));
                if(index>=3)return fail("compiled cull triangle index invalid");
                memcpy(&c.vertices[i],sourceMesh.vertices.data()+index*sizeof(p::Vertex),sizeof(p::Vertex));
                c.vertices[i].color={239,63,17,255};
            }
            if(reverse)std::swap(c.vertices[1],c.vertices[2]);
            // Canonical GX orthographic/perspective matrices; all W are positive
            // and this triangle is inside the clip volume after the palette.
            const p::Matrix source{{p::Row{.4f,0,0,0},perspective?p::Row{0,.8f,.8f,0}:p::Row{0,.8f,0,-.8f},
                perspective?p::Row{0,0,.5f,0}:p::Row{0,0,.25f,-.25f},perspective?p::Row{0,0,-1,0}:p::Row{0,0,0,1}}};
            p::CameraProjection camera;
            if(!p::prepareCamera(source,{400,240,0,0,400,240,0,1},W,H,camera))return fail("native cull camera rejected");
            c.u.viewProjection=camera.rows;
            std::array<float,16> logical{},compat{};
            for(unsigned r=0;r<4;++r)for(unsigned column=0;column<4;++column)logical[r*4+column]=source[r][column];
            if(!gx::picaProjection(logical,{0,0,400,240,0,1},compat,{400,240,400,240}))return fail("compatibility cull camera rejected");
            p::DrawUniforms compatibility=c.u;
            for(unsigned r=0;r<4;++r)for(unsigned column=0;column<4;++column)compatibility.viewProjection[r][column]=compat[r*4+column];
            p::MaterialPack bank;p::MaterialView view;
            if(p::MaterialPack::open(std::as_bytes(std::span(materialData[mode],materialSizes[mode])),bank)!=p::PackError::none||
               bank.material(1,view)!=p::PackError::none)return fail("compiled cull material bank rejected");
            c.material.cull=view.state.cull;
            const auto nativeCull=unsigned(c.material.cull),compatCull=gx::picaCull(mode);
            if(compatCull>2)return fail("invalid compatibility cull mode");
            auto compatMaterial=c.material;compatMaterial.cull=p::Cull(compatCull);
            if(!inputs(c))return false;
            float xy[3][2]{};
            for(unsigned i=0;i<3;++i){
                const auto& v=c.vertices[i];const float position[]{v.position[0],v.position[1],v.position[2]-1,1};float clip[4]{};
                for(unsigned r=0;r<4;++r)for(unsigned j=0;j<4;++j)clip[r]+=camera.rows[r][j]*position[j];
                if(!(clip[3]>0&&std::abs(clip[0])<clip[3]&&std::abs(clip[1])<clip[3]&&clip[2]>=-clip[3]&&clip[2]<=0))return fail("cull triangle unexpectedly clipped");
                xy[i][0]=(clip[0]/clip[3]+1)*W*.5f;xy[i][1]=(clip[1]/clip[3]+1)*H*.5f;
            }
            const float determinant=(xy[1][1]-xy[2][1])*(xy[0][0]-xy[2][0])+(xy[2][0]-xy[1][0])*(xy[0][1]-xy[2][1]);
            if(std::abs(determinant)<1||((determinant<0)!=(reverse!=0)))return fail("cull fixture winding changed");
            const bool clockwise=determinant<0,expected=mode==0||(mode==1?!clockwise:clockwise);
            const auto mesh=createBank(c);
            const auto candidate=renderer.createMaterial(c.material),reference=renderer.createMaterial(compatMaterial);
            if(!native(bool(mesh)&&bool(candidate)&&bool(reference))||!begin())return false;
            if(!native(renderer.clear(Clear,0,true,false))||!native(renderer.draw(mesh,1,candidate,{},c.u))||
               !native(renderer.target(targets[1]))||!native(renderer.clear(Clear,0,true,false))||
               !native(renderer.draw(mesh,1,reference,{},compatibility))||!native(renderer.end()))return false;
            char raw[112],lcd[112];snprintf(raw,sizeof(raw),"%s-native.abgr8",name);snprintf(lcd,sizeof(lcd),"%s-native-lcd.bgr8",name);
            if(!readback(0,0,raw,lcd))return false;
            snprintf(raw,sizeof(raw),"%s-compat-proxy.abgr8",name);snprintf(lcd,sizeof(lcd),"%s-compat-proxy-lcd.bgr8",name);
            if(!readback(1,1,raw,lcd))return false;
            unsigned covered=0,compatCovered=0,interior=0,missing=0,exterior=0,different=0;
            for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
                const unsigned at=4*(y*W+x);const auto* actual=snapshots[0]+at;const auto* referencePixel=snapshots[1]+at;
                const bool drawn=memcmp(actual,ClearBytes,4)!=0;
                covered+=drawn;compatCovered+=memcmp(referencePixel,ClearBytes,4)!=0;different+=memcmp(actual,referencePixel,4)!=0;
                const float px=x+.5f,py=H-y-.5f;
                const float b0=((xy[1][1]-xy[2][1])*(px-xy[2][0])+(xy[2][0]-xy[1][0])*(py-xy[2][1]))/determinant;
                const float b1=((xy[2][1]-xy[0][1])*(px-xy[2][0])+(xy[0][0]-xy[2][0])*(py-xy[2][1]))/determinant;
                const float minimum=std::min({b0,b1,1-b0-b1});
                if(minimum<-.025f)exterior+=drawn;
                else if(minimum>=.025f){++interior;missing+=expected&&!drawn;}
            }
            const bool okay=interior>20000&&!different&&!exterior&&!missing&&(expected?covered>25000:covered==0);
            if(fprintf(cullMetrics,"%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",perspective,clockwise,mode,nativeCull,compatCull,expected,covered,compatCovered,interior,missing,exterior,different,okay)<0||fflush(cullMetrics))return fail("cull metrics write failed");
            if(!okay)++failed;printf("%s: %s native=%u compat=%u expected=%u\n",name,okay?"pass":"FAIL",covered,compatCovered,expected);
            ++cullCompleted;if(!native(renderer.release(mesh))||!native(renderer.release(candidate))||!native(renderer.release(reference)))return false;
        }
        if(cullCompleted!=ExpectedCullCases||cullRequired!=ExpectedCullCases)return fail("cull matrix did not complete");
        const bool okay=!ferror(cullMetrics);const bool closed=fclose(cullMetrics)==0;cullMetrics=nullptr;
        return okay&&closed?true:fail("cull metrics close failed");
    }
    bool run(){
        if(!inputs(normal)||!begin()||!native(renderer.clear(Clear,0,true,false))||!native(renderer.draw(ordinaryMesh,1,ordinaryMaterial,bindings(normal),normal.u))||!native(renderer.end())||!readback(0,0,"ordinary-baseline.abgr8","ordinary-baseline-lcd.bgr8"))return false;
        memcpy(snapshots[2],snapshots[0],Bytes);if(!compare(normal,false,0,0))return false;
        for(unsigned bit=0;bit<9;++bit){char name[80];snprintf(name,sizeof(name),"light-mask-%02x",bit==8?255:1u<<bit);auto c=fixture(name);lookup(c,0,Output::product);
            // Strong, distinct single-light contributions make upper-bit loss
            // visible well above the bounded quantization tolerance.
            c.u.ambientColors={p::Row{.08f,.11f,.09f,.12f},p::Row{.13f,.07f,.1f,.08f}};
            for(unsigned i=0;i<8;++i)c.u.lights[i].color={.2f+.07f*i,.75f-.07f*i,.3f+.04f*i,.25f+.06f*i};
            for(unsigned ch=0;ch<4;++ch){auto& control=c.u.channels[ch];control.enabled=true;control.materialVertex=true;control.lightMask=bit==8?255:1u<<bit;control.diffuse=p::Diffuse::none;}
            if(!one(c))return false;}
        for(unsigned mixed=0;mixed<2;++mixed){auto c=fixture(mixed?"light-independent-masks-black-member":"light-zero-mask");lookup(c,0,Output::product);
            constexpr unsigned masks[]{0x81,0x42,0x24,0x18};
            c.u.lights[3].color={}; // Declared black member must not shift another channel's mask.
            for(unsigned ch=0;ch<4;++ch){auto& control=c.u.channels[ch];control.enabled=true;control.lightMask=mixed?masks[ch]:0;control.diffuse=p::Diffuse::none;}
            if(!one(c))return false;}
        for(unsigned ch=0;ch<4;++ch)for(unsigned mode=0;mode<6;++mode){char name[80];snprintf(name,sizeof(name),"channel-%u-source-%u",ch,mode);auto c=fixture(name);lookup(c,0,Output::secondary);
            // Primary output observes RGB0/ALPHA0; lookup observes RGB1/ALPHA1.
            if(ch%2==0){c.output=Output::primary;c.material.rasterLookup=false;c.material.lookupBase=0;c.material.textureMask=0;c.material.stageCount=1;pair(c.material.stages[0],p::Source::primary,p::Source::primary,p::Combine::replace);}
            auto& control=c.u.channels[ch];control.enabled=mode<4;control.materialVertex=mode<4?(mode&1):mode==5;control.ambientVertex=mode<4&&(mode&2);control.lightMask=0x92;control.diffuse=p::Diffuse::none;
            if(!one(c))return false;}
        for(unsigned diffuse=0;diffuse<3;++diffuse)for(unsigned attenuation=0;attenuation<3;++attenuation){char name[80];snprintf(name,sizeof(name),"point-diffuse-%u-attenuation-%u",diffuse,attenuation);auto c=fixture(name);
            for(auto& control:c.u.channels){control.enabled=true;control.materialVertex=false;control.lightMask=0xa5;control.diffuse=p::Diffuse(diffuse);control.attenuation=p::Attenuation(attenuation);}if(!one(c))return false;}
        for(unsigned mode=0;mode<3;++mode){char name[80];snprintf(name,sizeof(name),"directional-%u",mode);auto c=fixture(name);
            for(auto& l:c.u.lights){l.kind=p::LightKind::directional;l.direction={.6f,0,.8f};}
            for(auto& control:c.u.channels){control.enabled=true;control.materialVertex=false;control.lightMask=0xf0;control.diffuse=p::Diffuse(mode);}if(!one(c))return false;}
        for(unsigned base=0;base<2;++base)for(unsigned product=0;product<2;++product){char name[80];snprintf(name,sizeof(name),"lookup-base-%u-product-%u",base,product);auto c=fixture(name);lookup(c,base,product?Output::product:Output::secondary,base!=0);if(!one(c))return false;}
        for(unsigned variant=0;variant<6;++variant){char name[80];snprintf(name,sizeof(name),"program-variant-%u",variant);auto c=fixture(name);c.material.textureMask=1;c.textured=true;pair(c.material.stages[0],p::Source::primary,p::Source::texture0,p::Combine::modulate);
            if(!(variant&1)){c.u.palette.count=2;for(auto& v:c.vertices){v.weights={128,127,0,0};v.boneRows={0,3,0,0};}}
            if(variant/2==1)c.u.uvTransforms[0].rows={std::array<float,3>{.5f,0,.1f},std::array<float,3>{0,.5f,.2f}};
            if(variant/2==2){c.u.projective.enabled=true;c.u.projective.rows={p::Row{.1f,0,0,.4f},p::Row{0,.1f,0,.5f},p::Row{0,0,0,1}};}
            if(!one(c))return false;}
        // range mode0: no range correction; mode1: enabled; mode2: fog disabled.
        for(unsigned function=0;function<5;++function)for(unsigned ortho=0;ortho<2;++ortho)for(unsigned range=0;range<3;++range)for(unsigned carrier=1;carrier<=7;++carrier){char name[80];snprintf(name,sizeof(name),"fog-%u-ortho-%u-range-%u-carrier-%u",function,ortho,range,carrier);auto c=fixture(name,!ortho);
            auto& f=c.u.fog;f.provided=true;f.enabled=range!=2;f.orthographic=ortho;f.rangeEnabled=range!=0;f.function=p::FogFunction(function);f.a=ortho?.9f:.28f;f.b=1.25f;f.c=.12f;f.rangeScale=-1;f.rangeBias=-.6f;f.color=0xffd14f27;
            for(unsigned i=0;i<10;++i)f.range[i]=.55f+.07f*i;
            // Vertex1 offset=.08 samples K8/K9 with observable range gain;
            // vertex2 reaches K0. Identical data with range=false must be ignored.
            f.range[8]=.04f;f.range[9]=.12f;
            fogMaterial(c,p::FogCarrier(carrier),(carrier+range)%2);if(!one(c))return false;}
        // Exact original-vertex range-table endpoints, using distinct K entries
        // and an unsaturated base. offset=0 selects K9; offset=2 selects K0.
        // At zero offset K cancels mathematically; near-K9 sensitivity is above.
        for(unsigned function=0;function<5;++function)for(unsigned ortho=0;ortho<2;++ortho)for(unsigned endpoint=0;endpoint<2;++endpoint){char name[80];snprintf(name,sizeof(name),"fog-range-endpoint-%u-function-%u-ortho-%u",endpoint,function,ortho);auto c=fixture(name,!ortho);auto& f=c.u.fog;f.provided=f.enabled=f.rangeEnabled=true;f.orthographic=ortho;f.function=p::FogFunction(function);f.a=ortho?.5f:.24f;f.b=1.25f;f.c=.1f;f.rangeScale=0;f.rangeBias=endpoint?2:0;f.color=0xffd14f27;
            for(unsigned i=0;i<10;++i)f.range[i]=1.1f+.13f*i;
            fogMaterial(c,p::FogCarrier::primaryAlpha,0);if(!one(c))return false;}
        // Full RGBA A and B survive independently; only texture2 carries fog.
        for(unsigned function=0;function<5;++function)for(unsigned ortho=0;ortho<2;++ortho){char name[80];snprintf(name,sizeof(name),"fog-product-%u-ortho-%u",function,ortho);auto c=fixture(name,!ortho);
            auto& f=c.u.fog;f.provided=f.enabled=f.rangeEnabled=true;f.orthographic=ortho;f.function=p::FogFunction(function);f.a=ortho?.9f:.28f;f.b=1.25f;f.c=.12f;f.rangeScale=-1;f.rangeBias=-.6f;f.color=0xffd14f27;
            for(unsigned i=0;i<10;++i)f.range[i]=.55f+.07f*i;
            f.range[8]=.04f;f.range[9]=.12f;
            fogMaterial(c,p::FogCarrier::texture2,0,true);if(!one(c))return false;}
        if(completed!=ExpectedCases||required!=ExpectedCases)return fail("case matrix did not complete");
        if(!cullCases())return false;
        const bool metricsOkay=!ferror(metrics);const bool metricsClosed=fclose(metrics)==0;metrics=nullptr;
        if(!metricsOkay||!metricsClosed)return fail("metrics close failed");
        error=failed?"one or more GPU comparisons failed":"all bounded GPU comparisons passed";return failed==0;
    }
    bool receipt(bool pass){
        if(!folder[0])return false;char text[4096];const int n=snprintf(text,sizeof(text),
            "status=%s\nreason=%s\ncases_completed=%u\ncases_started=%u\ncases_expected=%u\ncomparisons_failed=%u\n"
            "cull_cases_completed=%u\ncull_cases_started=%u\ncull_cases_expected=%u\n"
            "runner=unlabelled; emulated-or-physical unknown; parent must attach runner identity\n"
            "physical_hardware=unknown; an Azahar pass is emulated evidence only\n"
            "raw=240x400 linear ABGR8, flip_vert=0, 384000 bytes\nlcd=actual top framebuffer BGR8, 288000 bytes, double buffering off\n"
            "synchronization=SyncDisplayTransfer outside frame drains queue and waits PPF; cache invalidation before reads; no FrameSync-only proof\n"
            "acquisition=complete volatile scalar ABGR8 and LCD BGR8 copies into CPU-owned snapshots before either save; exact RGB equality at every pixel required for each target\n"
            "candidate=actual dusk::pica::Renderer; all six real scene variants assembled with scene_shared.pica\n"
            "reference=direct RasterColorPlan + evaluateRasterColor, direct VertexFogPlan + evaluateVertexFog; no command decoder; directional N.L analytic\n"
            "reference_transform=CPU affine palette and matrix multiply, unit view normal; perspective interpolation barycentric/W renormalized\n"
            "fog_scope=original-vertex GX equations then perspective interpolation; texture carriers use production offline 8x256 helper; NOT per-fragment depth-fog equivalence\n"
            "product_scope=interpolate A and B independently then multiply, then RGB fog for combined cases with product alpha unchanged; dedicated lookup product negative control must differ >8 bytes in >1000 interior pixels from analytic wrong vertex-products\n"
            "criteria=>20000 interior pixels; >25000 drawn; no missing interior; no changed far exterior; max channel error <=3 bytes (<=4 for LUT products); no percentage outlier allowance\n"
            "coverage=raw top-origin row y maps to bottom-origin viewport sample (x+.5,400-y-.5); no image flip; barycentric margin .025 excludes ambiguous edges; outside margin -.025 must retain exact clear ABGR(107,79,53,23)\n"
            "restoration=following ordinary draw in SAME frame on target1; delayed TEV buffer consumer and original texture0, exact full-image baseline equality; baseline independently CPU-checked\n"
            "cull_scope=12 source-compiled geometry/material cases: both windings, GX none/front/back, orthographic/perspective; source cull-all is rejected at build, not lowered\n"
            "cull_reference=actual native Renderer using production compatibility projection and picaCull as proxy; NOT the full compatibility renderer or GX decoder\n"
            "cull_criteria=independent GX CW-front visibility, >20000 interior and >25000 covered when visible, exact clear when culled, no far exterior changes, exact native/proxy image equality\n"
            "timings=cpu_ms covers begin/submit; gpu_wait_ms includes synchronized transfers and raw SD writes; compare_ms CPU reference; previous_gpu_ms SDK previous frame, not current-case timing\n"
            "resource_lifetime=uploads before draws; retirement only after sync readback; targets deleted before C3D_Fini, renderer released after context destruction\n"
            "shader_constraints=build receipt records actual binary instruction/uniform counts; native ABI c0..c89 plus constants c90,c91; temporaries r0..r15; main->colors->channel call depth2; physical stack/call/instruction execution limits NOT proven by emulator\n"
            "limits=bounded sampled scenes, no assets/game integration, no physical PICA proof, no bit-exact guarantee, no screenshot-OCR or automatic emulator detection\n",
            pass?"pass":"fail",error,completed,required,ExpectedCases,failed,cullCompleted,cullRequired,ExpectedCullCases);
        return n>0&&size_t(n)<sizeof(text)&&save("receipt.txt",text,size_t(n));
    }
};
// Renderer owns stable SDK pointers and is large: never put it on the ARM stack.
Probe probe;
}
int main(){
    gfxInitDefault();gfxSetDoubleBuffering(GFX_TOP,false);gfxSet3D(false);consoleInit(GFX_BOTTOM,nullptr);
    printf("Native scene GPU probe\nAsset-free actual Renderer; SD evidence\n");
    const bool passed=probe.init()&&probe.run();const bool saved=probe.receipt(passed);
    printf("\n%s\n%s\n%s\n",passed&&saved?"PASS (bounded GPU comparisons)":"FAIL",probe.error,probe.folder);
    for(unsigned frame=0;frame<600&&aptMainLoop();++frame){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
    // Destroy the exclusive owner BEFORE gfxExit (global probe destructor is late).
    probe.shutdown();
    gfxExit();return passed&&saved?0:1;
}
