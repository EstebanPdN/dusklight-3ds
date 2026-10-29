#pragma once
// Host test substitute only. ARM compilation uses the installed Citro3D.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
using u8=std::uint8_t;using u16=std::uint16_t;using u32=std::uint32_t;using Result=int;
#define BIT(n) (1u<<(n))
#define R_FAILED(r) ((r)<0)
#include <3ds/gpu/enums.h>
struct DVLE_s{unsigned type{};};struct DVLB_s{DVLE_s DVLE[1];};
struct shaderInstance_s{unsigned type{};};struct shaderProgram_s{shaderInstance_s* vertexShader{};};
struct C3D_Tex{void* data{};std::size_t size{};unsigned maxLevel{},width{},height{},param{},minLevel{},lodBias{};};
struct C3D_TexInitParams{unsigned width,height,maxLevel;GPU_TEXCOLOR format;GPU_TEXTURE_MODE_PARAM type;bool onVram;};
struct C3D_FrameBuf{void* colorBuf{};unsigned width{},height{};void* depthBuf{};};
struct C3D_RenderTarget{C3D_FrameBuf frameBuf;};
struct C3D_FogLut{u32 data[128];};
struct C3D_TexEnv{u16 srcRgb{},srcAlpha{};u32 rgbOp{},alphaOp{},color{};GPU_COMBINEFUNC rgb=GPU_REPLACE,alpha=GPU_REPLACE;unsigned rgbScale{},alphaScale{};};
enum {C3D_RGB=1,C3D_Alpha=2,C3D_FRAME_NONBLOCK=2,C3D_UNSIGNED_SHORT=1,GX_CMDLIST_FLUSH=1};
enum C3D_ClearBits {C3D_CLEAR_COLOR=1,C3D_CLEAR_DEPTH=2};
struct C3D_AttrInfo{unsigned count{};std::array<unsigned,8> reg{},type{},size{};};
struct C3D_BufInfo{const void* data{};unsigned stride{};};
namespace nativeMock {
inline std::map<void*,std::size_t> allocations;
inline std::map<void*,std::size_t> published;
inline std::vector<const void*> held;
inline std::array<C3D_Tex*,3> textures{};
inline std::array<C3D_TexEnv,6> env{};
inline std::array<unsigned,2> bufferMasks{};
inline u32 bufferColor{};
inline std::array<std::array<float,4>,96> uniforms{};
inline std::array<u32,6> blendState{};
inline C3D_AttrInfo attributes;inline C3D_BufInfo buffer;
inline C3D_FogLut* fog{};
inline unsigned draws{},begins{},frees{},allocationCalls{},failAllocationAt{};
inline unsigned bufferCalls{},failBufferAt{},publicationCalls{},failPublicationAt{};
struct DrawRecord {unsigned count{};std::array<const void*,3> images{};std::array<u32,3> sampling{};u32 constant{};float projectionX{};};
inline std::array<DrawRecord,16> drawTrace{};inline unsigned traceSize{};inline bool traceDraws{};
inline unsigned pendingDraws{};
inline std::vector<unsigned> queueEvents; // 1=submitted draw list, 2=GSP fill.
inline unsigned selectedShader{};
inline std::array<unsigned,5> scissor{};
inline unsigned unsupportedUnitBindings{};
inline unsigned parseCalls{},failParseAt{},setShaderCalls{},failSetShaderAt{},liveBinaries{},liveShaders{};
inline bool inFrame{},inFlight{},failPublication{},failBuffer{},failTarget{},contextDestroyed{};
inline int tlsValue;inline void* tls=&tlsValue;
inline bool contains(const void* p){
    const auto x=reinterpret_cast<std::uintptr_t>(p);
    for(auto [a,n]:allocations){auto y=reinterpret_cast<std::uintptr_t>(a);if(x>=y&&x-y<n)return true;}return false;
}
inline bool visible(const void* p){
    const auto x=reinterpret_cast<std::uintptr_t>(p);
    for(auto [a,n]:published){auto y=reinterpret_cast<std::uintptr_t>(a);if(x>=y&&x-y<n)return true;}return false;
}
inline void complete(){assert(!inFrame);inFlight=false;held.clear();}
}
inline void* getThreadLocalStorage(){return nativeMock::tls;}
inline void* linearMemAlign(std::size_t n,std::size_t align){
    using namespace nativeMock;++allocationCalls;if(allocationCalls==failAllocationAt)return nullptr;
    void* p=nullptr;if(posix_memalign(&p,align,n))return nullptr;std::memset(p,0xcd,n);allocations[p]=n;return p;
}
inline void linearFree(void* p){
    using namespace nativeMock;if(!p)return;assert(allocations.count(p));
    for(auto heldPointer:held){const auto a=reinterpret_cast<std::uintptr_t>(p),h=reinterpret_cast<std::uintptr_t>(heldPointer);assert(!(h>=a&&h-a<allocations[p]));}
    ++frees;published.erase(p);allocations.erase(p);std::free(p);
}
inline Result GSPGPU_FlushDataCache(const void* p,std::size_t n){
    ++nativeMock::publicationCalls;if(nativeMock::failPublication||nativeMock::publicationCalls==nativeMock::failPublicationAt)return -1;assert(nativeMock::contains(p));nativeMock::published[const_cast<void*>(p)]=n;return 0;
}
inline DVLB_s* DVLB_ParseFile(u32* bytes,unsigned){++nativeMock::parseCalls;if(nativeMock::parseCalls==nativeMock::failParseAt)return nullptr;auto* b=new DVLB_s;b->DVLE[0].type=reinterpret_cast<u8*>(bytes)[0];++nativeMock::liveBinaries;return b;}
inline void DVLB_Free(DVLB_s* p){if(p)--nativeMock::liveBinaries;delete p;}
inline Result shaderProgramInit(shaderProgram_s* p){p->vertexShader=nullptr;return 0;}
inline Result shaderProgramSetVsh(shaderProgram_s* p,DVLE_s* v){++nativeMock::setShaderCalls;if(nativeMock::setShaderCalls==nativeMock::failSetShaderAt)return -1;p->vertexShader=new shaderInstance_s{v->type};++nativeMock::liveShaders;return 0;}
inline Result shaderProgramFree(shaderProgram_s* p){if(p->vertexShader)--nativeMock::liveShaders;delete p->vertexShader;p->vertexShader=nullptr;return 0;}
inline int shaderInstanceGetUniformLocation(shaderInstance_s*,const char*){return 0;}
inline unsigned C3D_TexCalcTotalSize(unsigned n,unsigned level){unsigned total=0;for(unsigned i=0;i<=level;++i){total+=n;n>>=2;}return total;}
inline bool C3D_TexInitWithParams(C3D_Tex* t,void*,C3D_TexInitParams p){
    unsigned pixels=p.width*p.height;t->size=p.format==GPU_RGBA8?pixels*4:p.format==GPU_RGB565?pixels*2:p.format==GPU_ETC1?pixels/2:pixels;
    t->maxLevel=p.maxLevel;t->width=p.width;t->height=p.height;t->data=linearMemAlign(C3D_TexCalcTotalSize(t->size,t->maxLevel),16);return t->data;
}
inline bool C3D_TexInit(C3D_Tex* t,unsigned w,unsigned h,GPU_TEXCOLOR f){return C3D_TexInitWithParams(t,nullptr,{w,h,0,f,GPU_TEX_2D,false});}
inline void C3D_TexDelete(C3D_Tex* t){linearFree(t->data);t->data=nullptr;}
inline void C3D_TexSetFilter(C3D_Tex* t,GPU_TEXTURE_FILTER_PARAM a,GPU_TEXTURE_FILTER_PARAM b){t->param&=~(GPU_TEXTURE_MAG_FILTER(GPU_LINEAR)|GPU_TEXTURE_MIN_FILTER(GPU_LINEAR));t->param|=GPU_TEXTURE_MAG_FILTER(a)|GPU_TEXTURE_MIN_FILTER(b);}
inline void C3D_TexSetFilterMipmap(C3D_Tex* t,GPU_TEXTURE_FILTER_PARAM a){t->param&=~GPU_TEXTURE_MIP_FILTER(GPU_LINEAR);t->param|=GPU_TEXTURE_MIP_FILTER(a);}
inline void C3D_TexSetWrap(C3D_Tex* t,GPU_TEXTURE_WRAP_PARAM a,GPU_TEXTURE_WRAP_PARAM b){t->param&=~(GPU_TEXTURE_WRAP_S(3)|GPU_TEXTURE_WRAP_T(3));t->param|=GPU_TEXTURE_WRAP_S(a)|GPU_TEXTURE_WRAP_T(b);}
inline bool C3D_FrameBegin(unsigned f){assert(f==C3D_FRAME_NONBLOCK);++nativeMock::begins;if(nativeMock::inFlight||nativeMock::inFrame)return false;nativeMock::inFrame=true;return true;}
inline bool C3D_FrameDrawOn(C3D_RenderTarget*){return !nativeMock::failTarget&&nativeMock::inFrame;}
inline void C3D_FrameSplit(unsigned flags){assert(nativeMock::inFrame&&flags==GX_CMDLIST_FLUSH);if(nativeMock::pendingDraws){nativeMock::queueEvents.push_back(1);nativeMock::pendingDraws=0;}}
inline void C3D_RenderTargetClear(C3D_RenderTarget*,C3D_ClearBits,u32,u32){assert(nativeMock::inFrame);nativeMock::queueEvents.push_back(2);}
inline void C3D_FrameEnd(unsigned){assert(nativeMock::inFrame);C3D_FrameSplit(GX_CMDLIST_FLUSH);nativeMock::inFrame=false;nativeMock::inFlight=true;}
inline float C3D_GetDrawingTime(){return 1;}
inline float C3D_GetProcessingTime(){return 2;}
inline void C3D_SetScissor(GPU_SCISSORMODE mode,unsigned left,unsigned top,unsigned right,unsigned bottom){nativeMock::scissor={unsigned(mode),left,top,right,bottom};}
inline C3D_AttrInfo* C3D_GetAttrInfo(){return &nativeMock::attributes;}
inline void AttrInfo_Init(C3D_AttrInfo* a){*a={};}
inline void AttrInfo_AddLoader(C3D_AttrInfo* a,unsigned r,unsigned t,unsigned n){assert(a->count<8);auto i=a->count++;a->reg[i]=r;a->type[i]=t;a->size[i]=n;}
inline C3D_BufInfo* C3D_GetBufInfo(){return &nativeMock::buffer;}
inline void BufInfo_Init(C3D_BufInfo* b){*b={};}
inline int BufInfo_Add(C3D_BufInfo* b,const void* p,unsigned stride,unsigned n,std::uint64_t order){assert(n==8&&order==0x76543210);b->data=p;b->stride=stride;++nativeMock::bufferCalls;return nativeMock::failBuffer||nativeMock::bufferCalls==nativeMock::failBufferAt?-1:0;}
inline void C3D_BindProgram(shaderProgram_s* p){assert(p&&p->vertexShader);nativeMock::selectedShader=p->vertexShader->type;}
inline void C3D_FVUnifSet(GPU_SHADER_TYPE,unsigned r,float x,float y,float z,float w){assert(r<96);nativeMock::uniforms[r]={x,y,z,w};}
inline void C3D_TexBind(unsigned n,C3D_Tex* p){assert(n<3&&p&&p->data);if(n>0&&((p->param>>28)&7)!=GPU_TEX_2D){++nativeMock::unsupportedUnitBindings;return;}nativeMock::textures[n]=p;}
inline void C3D_TexEnvInit(C3D_TexEnv* e){*e={};e->srcRgb=e->srcAlpha=GPU_PREVIOUS;e->color=0xffffffff;}
inline void C3D_TexEnvSrc(C3D_TexEnv* e,unsigned mode,GPU_TEVSRC a,GPU_TEVSRC b,GPU_TEVSRC c){(mode==C3D_RGB?e->srcRgb:e->srcAlpha)=u16(GPU_TEVSOURCES(a,b,c));}
inline void C3D_TexEnvOpRgb(C3D_TexEnv* e,GPU_TEVOP_RGB a,GPU_TEVOP_RGB b,GPU_TEVOP_RGB c){e->rgbOp=GPU_TEVOPERANDS(a,b,c);}
inline void C3D_TexEnvOpAlpha(C3D_TexEnv* e,GPU_TEVOP_A a,GPU_TEVOP_A b,GPU_TEVOP_A c){e->alphaOp=GPU_TEVOPERANDS(a,b,c);}
inline void C3D_TexEnvFunc(C3D_TexEnv* e,unsigned mode,GPU_COMBINEFUNC f){(mode==C3D_RGB?e->rgb:e->alpha)=f;}
inline void C3D_TexEnvScale(C3D_TexEnv* e,unsigned mode,GPU_TEVSCALE f){(mode==C3D_RGB?e->rgbScale:e->alphaScale)=f;}
inline void C3D_TexEnvColor(C3D_TexEnv* e,u32 c){e->color=c;}
inline void C3D_SetTexEnv(unsigned n,C3D_TexEnv* e){assert(n<6);nativeMock::env[n]=*e;}
inline void C3D_TexEnvBufUpdate(unsigned mode,unsigned mask){if(mode&C3D_RGB)nativeMock::bufferMasks[0]=mask;if(mode&C3D_Alpha)nativeMock::bufferMasks[1]=mask;}
inline void C3D_TexEnvBufColor(u32 color){nativeMock::bufferColor=color;}
inline void C3D_LightEnvBind(void*){}
inline void C3D_FragOpMode(GPU_FRAGOPMODE){}
inline void C3D_EarlyDepthTest(bool,GPU_EARLYDEPTHFUNC,u32){}
inline void C3D_DepthMap(bool,float,float){}
inline void C3D_CullFace(GPU_CULLMODE){}
inline void C3D_StencilTest(bool,GPU_TESTFUNC,int,int,int){}
inline void C3D_StencilOp(GPU_STENCILOP,GPU_STENCILOP,GPU_STENCILOP){}
inline void C3D_DepthTest(bool,GPU_TESTFUNC,GPU_WRITEMASK){}
inline void C3D_AlphaTest(bool,GPU_TESTFUNC,int){}
inline void C3D_AlphaBlend(GPU_BLENDEQUATION c,GPU_BLENDEQUATION a,GPU_BLENDFACTOR cs,GPU_BLENDFACTOR cd,GPU_BLENDFACTOR as,GPU_BLENDFACTOR ad){nativeMock::blendState={u32(c),u32(a),u32(cs),u32(cd),u32(as),u32(ad)};}
inline void C3D_FogGasMode(GPU_FOGMODE,GPU_GASMODE,bool){}
inline void C3D_FogColor(u32){}
inline void C3D_FogLutBind(C3D_FogLut* p){nativeMock::fog=p;}
inline void FogLut_FromArray(C3D_FogLut* p,const float*){std::memset(p,0xa5,sizeof(*p));}
inline void C3D_DrawElements(GPU_Primitive_t mode,int count,int type,const void* ix){
    using namespace nativeMock;assert(inFrame&&mode==GPU_TRIANGLES&&count>0&&type==C3D_UNSIGNED_SHORT);
    assert(unsupportedUnitBindings==0&&"Citro3D rejected a non2D sampler outside unit0");
    assert(contains(buffer.data)&&visible(buffer.data)&&contains(ix)&&visible(ix));
    held.push_back(buffer.data);held.push_back(ix);
    for(auto* t:textures){assert(t&&contains(t->data)&&visible(t->data));held.push_back(t->data);}
    if(traceDraws){assert(traceSize<drawTrace.size());auto& row=drawTrace[traceSize++];row.count=unsigned(count);row.constant=env[0].color;row.projectionX=uniforms[48][0];for(unsigned n=0;n<3;++n){row.images[n]=textures[n]->data;row.sampling[n]=textures[n]->param;}}
    ++draws;++pendingDraws;
}
