#include "scene_bootstrap.hpp"
#include "runtime/archive_bootstrap.hpp"
#include "runtime/disc.hpp"
#include "runtime/dvd_provider.hpp"
#include "dusk/version.hpp"
#include "runtime/bootstrap.hpp"
#include <JSystem/J3DGraphLoader/J3DModelLoader.h>
#include <JSystem/JKernel/JKRMemArchive.h>
#include <JSystem/J3DGraphAnimator/J3DModelData.h>
#include <JSystem/J3DGraphAnimator/J3DModel.h>
#include "runtime/gx_stream.hpp"
#include "runtime/gx_vertex_transform.hpp"
#include "runtime/gx_raster_plan.hpp"
#include "runtime/gx_pica_material.hpp"
#include "gx_geometry_pica.hpp"
#include "gx_texture_pica.hpp"
#include "runtime/gx_texture_commands.hpp"
#include "runtime/gx_raster_color.hpp"
#include <JSystem/J3DGraphBase/J3DMaterial.h>
#include <JSystem/J3DGraphBase/J3DTevs.h>
#include <JSystem/J3DGraphBase/J3DTexture.h>
#include <JSystem/JUtility/JUTNameTab.h>
#include <dolphin/gd.h>
#include "gx_pica.hpp"
#include <JSystem/J3DGraphBase/J3DSys.h>
#include <JSystem/J3DGraphBase/J3DPacket.h>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
#include "d/d_kankyo.h"
#include "d/d_com_inf_game.h"
#include "d/d_resorce.h"
#include "runtime/stage_data.hpp"
#endif
static_assert(sizeof(GXVtxAttrFmtList)==16, "GX resource ABI requires 32-bit enums");
namespace dusk::native {
namespace {
ArchiveBootstrap room,stage;
std::array<gx::TextureHandle,8> stageTextures{};
J3DModelData* data;
J3DModel* model;
gx::TextureBlockResources textureResources;
gx::CommandMemory memory;
gx::CommandProcessor processor(memory);
std::array<u8,1024> scratch;
gx::CommandStream stream(processor,scratch);
gx::MemoryHandle modelMemory{},matrixMemory[4]{};
Vec center{}, eye{}, up{0,1,0};
float radius;
unsigned draws,meshCount;
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
unsigned attemptedMeshes,omittedMaterials;
#endif
struct SceneMesh {unsigned handle{};gx::RasterPlan raster{};gx::PicaMaterialPlan material{};std::array<gx::Texture,3> textures{};};
std::array<SceneMesh,64> meshes;
std::array<float,16> sceneTransform;
bool cached;
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
dKy_tevstr_c roomLighting;
void* stageMetadata;
void* roomMetadata;
#endif
const char* drawFailure="none";
bool geometrySink(const gx::CommandDraw& draw,void*) {
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
    const unsigned attempted=attemptedMeshes++;
#endif
    auto fail=[](const char* why){drawFailure=why;fatal(why);return false;};
    if(meshCount==meshes.size())return fail("Ordon mesh capacity exceeded");
    gx::RasterPlan raster;gx::RasterError rasterError;
    gx::FogPolicy fogPolicy=gx::FogPolicy::requireSupported;
#ifdef DUSK_SCENE_FOG_INSPECTION
    fogPolicy=gx::FogPolicy::diagnosticDisabled;
#endif
    if(!gx::planRaster(draw.registers,raster,&rasterError,fogPolicy)) {
        const char* witnessPath="sdmc:/3ds/dusklight/ordon-raster-failure.txt";
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
        char witness[112];std::snprintf(witness,sizeof(witness),"sdmc:/3ds/dusklight/ordon-raster-omitted-%04u.txt",attempted);
        witnessPath=witness;
#endif
        if(auto* f=std::fopen(witnessPath,"w")) {
            std::fprintf(f,"mesh %u: %s\n",meshCount,gx::rasterErrorText(rasterError));
            for(unsigned i=0xe8;i<=0xf3;++i)std::fprintf(f,"BP %02x valid %u word %08lx\n",i,unsigned(draw.registers.bpValid[i]),static_cast<unsigned long>(draw.registers.bp[i]));
            std::fclose(f);
        }
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
        if(rasterError==gx::RasterError::alpha){++omittedMaterials;return true;}
#endif
        return fail(gx::rasterErrorText(rasterError));
    }
    if(raster.scissor.empty||raster.cull==3)return true;
    gx::PicaMaterialPlan material{};
    std::array<gx::Texture,3> textures{};
    gx::RasterColorPlan colorPlan{};
    gx::VertexTransformPlan transform;gx::VertexTransformError transformError;
    gx::RasterColorError colorError;
    std::array<std::array<float,2>,3> scales{};
#ifdef DUSK_SCENE_TEXTURES
    gx::MaterialState state;gx::MaterialError materialError;
    if(!gx::decodeMaterial(draw.registers,state,&materialError))return fail("Ordon material registers missing");
    gx::MaterialPlanError planError;
    if(!gx::planPicaMaterial(state,material,&planError)){
        const char* witnessPath="sdmc:/3ds/dusklight/ordon-material-failure.txt";
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
        char witness[112];std::snprintf(witness,sizeof(witness),"sdmc:/3ds/dusklight/ordon-material-omitted-%04u.txt",attempted);
        witnessPath=witness;
#endif
        if(auto* f=std::fopen(witnessPath,"w")){
            std::fprintf(f,"mesh %u: %s stages %u\n",meshCount,gx::materialPlanErrorText(planError),state.stageCount);
            for(unsigned i=0;i<state.stageCount;++i){const auto& s=state.stages[i];
                std::fprintf(f,"%u rgb %u %u %u %u alpha %u %u %u %u K %u %u op %u %u %u %u/%u %u %u %u clamp %u %u tex %u coord %u enabled %u raster %u swaps %u %u\n",i,s.color[0],s.color[1],s.color[2],s.color[3],s.alpha[0],s.alpha[1],s.alpha[2],s.alpha[3],s.konstColor,s.konstAlpha,s.colorOp.operation,s.colorOp.bias,s.colorOp.scale,s.colorOp.output,s.alphaOp.operation,s.alphaOp.bias,s.alphaOp.scale,s.alphaOp.output,s.colorOp.clamp,s.alphaOp.clamp,s.texture,s.coordinate,unsigned(s.textureEnabled),s.raster,s.textureSwap,s.rasterSwap);
                for(unsigned r=0xc0+2*i;r<=0xc1+2*i;++r)std::fprintf(f,"BP %02x valid %u word %08lx\n",r,unsigned(draw.registers.bpValid[r]),static_cast<unsigned long>(draw.registers.bp[r]));
                std::fprintf(f,"indirect %08lx\n",static_cast<unsigned long>(s.indirect));
            }
            std::fprintf(f,"indirect stages %u\n",state.indirectCount);
            for(unsigned i=0;i<4;++i)std::fprintf(f,"reg%u %d %d %d %d konst %u %u %u %u\n",i,state.colors[i][0],state.colors[i][1],state.colors[i][2],state.colors[i][3],state.konst[i][0],state.konst[i][1],state.konst[i][2],state.konst[i][3]);
            std::fclose(f);
        }
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
        // This optional inspection submits only fully admitted material draws.
        // It does not substitute a direct material for an unsupported effect.
        // Every omission is recorded and shown in the persistent scene status.
        ++omittedMaterials;return true;
#else
        return fail(gx::materialPlanErrorText(planError));
#endif
    }
    if(!gx::planRasterColor(draw.registers,material,colorPlan,&colorError)){
        if(auto* f=std::fopen("sdmc:/3ds/dusklight/ordon-lighting-failure.txt","w")){
            std::fprintf(f,"mesh %u: %s\n",meshCount,gx::rasterColorErrorText(colorError));
            for(unsigned r=0x1009;r<=0x1011;++r)std::fprintf(f,"XF %04x valid %u word %08lx\n",r,unsigned(draw.registers.xfValid[r]),static_cast<unsigned long>(draw.registers.xf[r]));
            for(unsigned i=0;i<8;++i){std::fprintf(f,"light %u valid:",i);for(unsigned c=3;c<16;++c)std::fprintf(f," %u",unsigned(draw.registers.xfValid[0x600+i*16+c]));std::fprintf(f,"\n");}
            std::fclose(f);
        }
        return fail(gx::rasterColorErrorText(colorError));
    }
    u8 coordinates=0;
    for(unsigned unit=0;unit<material.textureCount;++unit)coordinates|=1u<<material.textures[unit].coordinate;
    if(!gx::planVertexTransform(draw.registers,coordinates,transform,&transformError))return fail(gx::vertexTransformErrorText(transformError));
    if(!gx::routeProjectiveTexture(material,transform.projectiveMask()))return fail("Ordon multiple projected textures pending");
    for(unsigned unit=0;unit<material.textureCount;++unit){
        const char* failure;
        if(!gx::resolveCommandTexture(draw.registers,material.textures[unit].map,textures[unit],failure))return fail(failure);
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&material.projectiveTexture0))return fail(gxTextureFailure());
    }

#else
    material.stageCount=1;
    material.stages[0].rgb.inputs[0]={gx::MaterialSource::primary,gx::MaterialOperand::color,false};
    material.stages[0].alpha.inputs[0]={gx::MaterialSource::primary,gx::MaterialOperand::alpha,false};
#endif
    std::size_t count=draw.indexCount;
    if(!count) switch(draw.primitive){
        case GX_TRIANGLES:count=draw.vertexCount;break;
        case GX_QUADS:count=(draw.vertexCount/4)*6;break;
        case GX_TRIANGLESTRIP:case GX_TRIANGLEFAN:count=(draw.vertexCount-2)*3;break;
        default:return fail("Ordon primitive unsupported");
    }
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(draw.vertexCount,count,batch))return fail(gxGeometryFailure());
    auto cancel=[&](const char* why){cancelGxGeometry(batch);return fail(why);};
    for(unsigned i=0;i<draw.vertexCount;++i){
        gx::DecodedVertex vertex;
        if(!draw.vertex(i,vertex))return cancel("Ordon vertex decode failed");
        if((vertex.present&(1u<<GX_VA_PNMTXIDX))&&vertex.matrix[0]!=0)return cancel("Static Ordon needs one matrix");
        auto& out=batch.vertices[i];out={};out.position={vertex.position[0],vertex.position[1],vertex.position[2],1};
        out.color={.65f,.72f,.58f,1.f};
        if(vertex.present&(1u<<GX_VA_CLR0))std::copy_n(vertex.color[0],4,out.color.begin());
        out.color[3]=1;
#ifdef DUSK_SCENE_TEXTURES
        gx::TransformedVertex transformed;
        if(!gx::transformVertex(transform,vertex,transformed,&transformError,colorPlan.needsNormal))return cancel(gx::vertexTransformErrorText(transformError));
        if(!gx::evaluateRasterColor(colorPlan,vertex,out.color,&colorError,&transformed))return cancel(gx::rasterColorErrorText(colorError));
        for(unsigned unit=0;unit<material.textureCount;++unit){const auto& uv=transformed.texcoord[material.textures[unit].coordinate];
            const float q=unit==0&&material.projectiveTexture0?uv[2]:1;
            if(!std::isfinite(q)||q<=0)return cancel("Ordon projected texture divisor invalid");
            if(unit==0)out.textureQ=q;
            out.texcoord[unit]={uv[0]*scales[unit][0],q-uv[1]*scales[unit][1]};}
#endif
    }
    if(auto* f=std::fopen("sdmc:/3ds/dusklight/ordon-admitted-plans.txt",meshCount?"a":"w")) {
        std::array<float,3> low{1e30f,1e30f,1e30f},high{-1e30f,-1e30f,-1e30f};
        unsigned valid=0;
        for(const auto& v:batch.vertices) {
            std::array<float,4> clip{};
            for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)clip[r]+=sceneTransform[r*4+c]*v.position[c];
            if(clip[3]<=0)continue;
            ++valid;
            for(unsigned r=0;r<3;++r){const float ndc=clip[r]/clip[3];low[r]=std::min(low[r],ndc);high[r]=std::max(high[r],ndc);}
        }
        std::fprintf(f,"mesh %u RGB %u cull %u depth %u/%u/%u alpha %u/%u projective %u vertices %u in front %u NDC min %.6g %.6g %.6g max %.6g %.6g %.6g\n",meshCount,unsigned(raster.writeRgb),raster.cull,unsigned(raster.depth.enabled),raster.depth.compare,unsigned(raster.depth.write),raster.alpha.comparison,raster.alpha.reference,unsigned(material.projectiveTexture0),draw.vertexCount,valid,low[0],low[1],low[2],high[0],high[1],high[2]);
        std::fclose(f);
    }
    if(draw.indexCount){for(unsigned i=0;i<count;++i)if(!draw.triangleIndex(i,batch.indices[i]))return cancel("Ordon triangle index invalid");}
    else{size_t at=0;if(!aurora::gx::dl::expand_triangles(draw.primitive,draw.vertexCount,[&](u16 a,u16 b,u16 c){batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;}))return cancel("Ordon triangulation failed");}
    unsigned handle;
    if(!uploadStaticGxGeometry(batch,handle))return cancel(gxGeometryFailure());
    cancelGxGeometry(batch);
    meshes[meshCount++]={handle,raster,material,textures};
    if(!submitStaticGxMesh(raster,material,handle,sceneTransform))return fail(gxPicaFailure());
    ++draws;return true;
}
char status[96]="Ordon scene not loaded";
}
bool initializeNativeScene(){
    Disc disc;
    if(disc.open(gameImagePath)!=DiscStatus::ready)return false;
    {
        DvdProvider metadata(disc);
        if(!metadata.bound())fatal("Ordon metadata worker unavailable");
        dusk::version::init();
    }
    constexpr ArchiveLimits limits{{8*1024*1024,4096,65536,255},8*1024*1024};
    auto result=room.load(disc,"/res/Stage/F_SP103/R00_00.arc",limits);
    if(result!=ArchiveStatus::ready){std::snprintf(status,sizeof(status),"Ordon archive: %s",archiveStatusText(result));return false;}
    ResourceSpan resource;
    if(room.findBasename("model.bmd",resource)!=ArchiveStatus::ready)return false;
    if(resource.size<32||std::memcmp(resource.data,"J3D2bmd3",8))return false;
#ifdef DUSK_SCENE_TEXTURES
    auto be32=[](const u8* p){return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3];};
    size_t block=32;
    for(unsigned i=0;i<be32(resource.data+12);++i){
        if(block+8>resource.size)return false;
        const auto size=be32(resource.data+block+4);if(size<8||size>resource.size-block)return false;
        if(!std::memcmp(resource.data+block,"TEX1",4)){
            if(!gx::registerTextureBlock({resource.data+block,size},textureResources,true))fatal("Ordon TEX1 registration failed");
        }
        block+=size;
    }
#endif
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
    // The private Ordon RARC identifies this model as BMDE. Preserve the
    // original resource owner's light blocks, texture-matrix policy and setup.
    if(!room.archiveForLoading()->findTypeResource('BMDE',"model.bmd"))
        fatal("Ordon model resource type differs from BMDE");
    data=dRes_info_c::loaderBasicBmd('BMDE',const_cast<u8*>(resource.data));
#else
    data=J3DModelLoaderDataBase::load(resource.data,0);
#endif
    if(data&&(data->getJointNum()!=1||data->getDrawMtxNum()!=1||data->getDrawMtxFlag(0)!=0||data->getDrawMtxIndex(0)!=0))fatal("Ordon static path needs one rigid matrix");
    if(!data){std::snprintf(status,sizeof(status),"Ordon original model loader failed");return false;}
#ifdef DUSK_SCENE_TEXTURES
    if(stage.load(disc,"/res/Stage/F_SP103/STG_00.arc",limits)!=ArchiveStatus::ready)fatal("Ordon stage archive failed");
    auto* texture=data->getTexture();unsigned external=0;
    for(unsigned i=0;i<texture->getNum();++i)if(texture->getResTIMG(i)->imageOffset==0){
        if(external==stageTextures.size())fatal("Ordon external texture capacity");
        char name[96];std::snprintf(name,sizeof(name),"%s.bti",data->getTextureName()->getName(i));
        ResourceSpan bti;
        if(stage.findBasename(name,bti)!=ArchiveStatus::ready)fatal("Ordon shared texture absent");
        if(!gx::registerTexture({bti.data,bti.size},stageTextures[external++]))fatal("Ordon shared texture registration failed");
        texture->setResTIMG(i,*reinterpret_cast<const ResTIMG*>(bti.data));
    }
#endif
    model=new J3DModel();
    if(!model||model->entryModelData(data,0,1)){std::snprintf(status,sizeof(status),"Ordon model instance failed");return false;}
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
    ResourceSpan stageData,roomData;
    if(stage.findBasename("stage.dzs",stageData)!=ArchiveStatus::ready ||
       room.findBasename("room.dzr",roomData)!=ArchiveStatus::ready)
        fatal("Original Ordon stage/room metadata missing");
    StageEnvironmentLayout environmentLayout;
    if(inspectStageEnvironment(stageData,roomData,0,0,environmentLayout)!=ResourceStatus::ready)
        fatal("Original Ordon environment metadata invalid");
    stageMetadata=JKRHeap::getCurrentHeap()->alloc(stageData.size,32);
    roomMetadata=JKRHeap::getCurrentHeap()->alloc(roomData.size,32);
    if(!stageMetadata||!roomMetadata)fatal("Original Ordon metadata allocation failed");
    std::memcpy(stageMetadata,stageData.data,stageData.size);
    std::memcpy(roomMetadata,roomData.data,roomData.size);
    dComIfGp_getStartStage()->set("F_SP103",0,0,0);
    if(!dStage_nativeLoadSceneEnvironment(stageMetadata,stageData.size,roomMetadata,roomData.size,0,0))
        fatal("Original Ordon environment startup failed");
    dKy_nativeInitializeSceneColor();
    dKy_tevstr_init(&roomLighting,0,0xff);
    if(auto* evidence=std::fopen("sdmc:/3ds/dusklight/ordon-environment-startup.txt","w")) {
        std::fprintf(evidence,"Original stage/room environment initialized\nroom %d layer %u\nstage bytes %zu room bytes %zu\npalettes %u selectors %u environments %u sky colors %u room lights %u\n",
            int(roomLighting.room_no),environmentLayout.layer,stageData.size,roomData.size,
            environmentLayout.palette.count,environmentLayout.pselect.count,environmentLayout.environment.count,
            environmentLayout.vrbox.count,environmentLayout.roomLights.count);
        std::fclose(evidence);
    }
    OSReport("Original Ordon room-light state initialized (room %d)\n",int(roomLighting.room_no));
#endif
    if(!memory.add({resource.data,resource.size},modelMemory))return false;
    auto& v=data->getVertexData();
    std::array<float,3> low{1e30f,1e30f,1e30f},high{-1e30f,-1e30f,-1e30f};
    const unsigned type=v.getVtxPosType(),stride=v.getVtxArrStride(GX_VA_POS);
    const unsigned scalarBytes=type==GX_F32?4:(type==GX_S16||type==GX_U16)?2:1;
    if(type>GX_F32||stride<scalarBytes*3||v.getVtxNum()>v.getVtxArrNum(GX_VA_POS)){std::snprintf(status,sizeof(status),"Ordon camera vertex extent invalid");return false;}
    const auto* positions=static_cast<const u8*>(v.getVtxPosArray());
    for(unsigned i=0;i<v.getVtxNum();++i)for(unsigned c=0;c<3;++c){
        const auto* p=positions+i*stride+c*scalarBytes;float value;
        if(type==GX_F32)std::memcpy(&value,p,4);
        else {if(type==GX_S16){s16 n;std::memcpy(&n,p,2);value=n;}
              else if(type==GX_U16){u16 n;std::memcpy(&n,p,2);value=n;}
              else value=type==GX_S8?float(static_cast<s8>(*p)):float(*p);
              value=std::ldexp(value,-v.getVtxPosFrac());}
        low[c]=std::min(low[c],value);high[c]=std::max(high[c],value);
    }
    center={(low[0]+high[0])/2,(low[1]+high[1])/2,(low[2]+high[2])/2};
    radius=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2]});
    eye={center.x,center.y+radius*.45f,center.z+radius*.9f};
    processor.setSink(geometrySink);
    std::snprintf(status,sizeof(status),"Ordon: %u shapes, %u materials",data->getShapeNum(),data->getMaterialNum());
    return true;
}
bool drawNativeScene(float horizontal,float vertical) {
    if(!model)return false;
    eye.x+=horizontal*radius*.003f;eye.z+=vertical*radius*.003f;
    Mtx view;C_MTXLookAt(view,&eye,&up,&center);j3dSys.setViewMtx(view);
    model->calc();model->viewCalc();
    Mtx44 projection;C_MTXPerspective(projection,55,400.f/240,10,200000);
    gx::DrawPacket transformPacket{};std::memcpy(transformPacket.projection,projection,sizeof(projection));
    if(data->checkFlag(0x10)) {
        // ConcatView models intentionally leave the draw-matrix buffer unused.
        // The original shape path concatenates view with the joint matrix on
        // load. The static rigid GPU cache must use that same matrix, not the
        // zero-initialized unused buffer. One joint/draw matrix was checked at
        // startup; weighted or multi-joint models require the full draw path.
        Mtx viewJoint;
        MTXConcat(*model->getShapePacket(0)->getBaseMtxPtr(),model->getMtxBuffer()->getUserAnmMtx(0),viewJoint);
        std::memcpy(transformPacket.modelView,viewJoint,sizeof(viewJoint));
    }else std::memcpy(transformPacket.modelView,model->getMtxBuffer()->getDrawMtxPtr()[0],sizeof(Mtx));
    transformPacket.viewport={0,0,400,240,0,1};
    if(!gx::picaTransform(transformPacket,sceneTransform))return false;
    if(cached){
        for(unsigned i=0;i<meshCount;++i){
            for(unsigned unit=0;unit<meshes[i].material.textureCount;++unit){float u,v;if(!bindGxTexture(meshes[i].textures[unit],u,v,unit,unit==0&&meshes[i].material.projectiveTexture0))return false;}
            if(!submitStaticGxMesh(meshes[i].raster,meshes[i].material,meshes[i].handle,sceneTransform))return false;
        }
        std::snprintf(status,sizeof(status),
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
            "PARTIAL, NO FOG: %u draws, %u omitted",meshCount,omittedMaterials
#elif defined(DUSK_SCENE_FOG_INSPECTION)
            "Ordon inspection (NO FOG): %u draws, %u KiB",meshCount
#else
            "Ordon GPU: %u draws, %u KiB",meshCount
#endif
#ifndef DUSK_SCENE_MATERIAL_INSPECTION
            ,unsigned(staticGxGeometryBytes()/1024)
#endif
            );return true;
    }
    // Only the current matrices are borrowed; the prior frame has drained.
    for(auto& h:matrixMemory)if(h){memory.remove(h);h=0;}
    auto* matrices=model->getMtxBuffer();
    if(!memory.add({reinterpret_cast<const u8*>(matrices->getDrawMtxPtr()),data->getDrawMtxNum()*sizeof(Mtx)},matrixMemory[0])||
       !memory.add({reinterpret_cast<const u8*>(matrices->getNrmMtxPtr()),data->getDrawMtxNum()*sizeof(Mtx33)},matrixMemory[1]))return false;
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
    if(!memory.add({reinterpret_cast<const u8*>(matrices->getUserAnmMtx(0)),data->getJointNum()*sizeof(Mtx)},matrixMemory[2]))return false;
    if(data->getWEvlpMtxNum() && !memory.add({reinterpret_cast<const u8*>(matrices->getWeightAnmMtx(0)),data->getWEvlpMtxNum()*sizeof(Mtx)},matrixMemory[3]))return false;
#endif
#ifdef DUSK_SCENE_TEXTURES
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
    // Match daBg_c's first background part: calculate from the original stage
    // palette, then attach the original J3D ambient/light/fog owners.
    g_env_light.settingTevStruct(0x20,nullptr,&roomLighting);
    g_env_light.setLightTevColorType_MAJI(model->getModelData(),&roomLighting);
#endif
    model->calcMaterial();model->makeDL();
#endif
    if(!gx::setCommandStream(&stream))return false;
#ifdef DUSK_SCENE_ENVIRONMENT_STARTUP
    dKy_GlobalLight_set();
#endif
    GXSetProjection(projection,GX_PERSPECTIVE);GXSetViewport(0,0,400,240,0,1);
    GXSetScissor(0,0,400,240);GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_COPY);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);GXSetZCompLoc(GX_FALSE);
    GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    stream.finish();stream.writeBp(0x41,processor.registers().bp[0x41]|24);
    for(unsigned reg:{0x42u,0xf1u,0xf5u})stream.writeBp(reg,0);
    alignas(32) u8 defaults[512];GDLObj gd;
    GDInitGDLObj(&gd,defaults,sizeof(defaults));GDSetCurrent(&gd);
    GDSetTevSwapModeTable(GX_TEV_SWAP0,GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    GDSetTevSwapModeTable(GX_TEV_SWAP1,GX_CH_RED,GX_CH_RED,GX_CH_RED,GX_CH_ALPHA);
    GDSetTevSwapModeTable(GX_TEV_SWAP2,GX_CH_GREEN,GX_CH_GREEN,GX_CH_GREEN,GX_CH_ALPHA);
    GDSetTevSwapModeTable(GX_TEV_SWAP3,GX_CH_BLUE,GX_CH_BLUE,GX_CH_BLUE,GX_CH_ALPHA);
    for(unsigned i=0;i<4;++i){
        GDSetTevKColor(static_cast<GXTevKColorID>(i),j3dDefaultTevKColor);
        GDSetTevColorS10(static_cast<GXTevRegID>(i),j3dDefaultTevColor);
    }
    const auto defaultBytes=GDGetCurrOffset();GDSetCurrent(nullptr);
    if(!stream.call({defaults,defaultBytes}))fatal("Ordon default texture swaps failed");
    draws=0;
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
    attemptedMeshes=omittedMaterials=0;
#endif
#ifdef DUSK_SCENE_TEXTURES
    for(unsigned i=0;i<data->getMaterialNum();++i)model->getMatPacket(i)->draw();
#else
    for(unsigned i=0;i<data->getShapeNum();++i)model->getShapePacket(i)->draw();
#endif
    const bool okay=stream.finish();gx::setCommandStream(nullptr);cached=okay;
    if(!okay)std::snprintf(status,sizeof(status),"Ordon: %.55s (cmd %u)",drawFailure,unsigned(processor.error()));
    else std::snprintf(status,sizeof(status),
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
        "PARTIAL, NO FOG: %u draws, %u omitted",draws,omittedMaterials
#elif defined(DUSK_SCENE_FOG_INSPECTION)
        "Ordon inspection (NO FOG): %u draws",draws
#else
        "Ordon original geometry: %u draws",draws
#endif
    );
    if(okay)if(auto* f=std::fopen("sdmc:/3ds/dusklight/ordon-render-inspection.txt","w")) {
        std::fprintf(f,"Original room static mesh draw\nmeshes %u geometry bytes %zu texture bytes %zu\n",meshCount,staticGxGeometryBytes(),gxTextureBytes());
#ifdef DUSK_SCENE_MATERIAL_INSPECTION
        std::fprintf(f,"PARTIAL material inspection: attempted %u omitted %u\n",attemptedMeshes,omittedMaterials);
#endif
#ifdef DUSK_SCENE_FOG_INSPECTION
        std::fprintf(f,"Fog explicitly disabled for inspection\n");
#endif
        std::fprintf(f,"No player/actor simulation or physical New3DS acceptance\n");std::fclose(f);
    }
    return okay;
}
const char* nativeSceneStatus(){return status;}
}
