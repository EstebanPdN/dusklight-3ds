// J2D material memo against the real native typed setters. The setters of
// J2DColorBlock, J2DTexGenBlock, J2DTevBlockN and J2DPEBlock are called in
// the order J2DMaterial::setGX calls them: once through the memo, as
// nativeMaterialSetGX (J2DMaterial.cpp) does, on one stream, and once
// plainly on a second stream. After every pane of a simulated HUD the two
// register banks, epochs, state key hashes and command counts must be
// identical. Texture matrices are named to the recorder as
// J2DTexGenBlock::setGX names its J2DTexMtx objects.
#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
// Defined by the native draw code in the game; the processor only counts batches in it.
thread_local dusk::native::gx::StaticGeometryHint dusk::native::gx::staticGeometryHint{};
namespace {
struct Stage {u8 color[4],alpha[4],cop,cscale,creg,aop,ascale,areg,ras,tex,coord,map,channel,kc,ka;};
// The bytes J2DMaterial::setGX reads, packed like the J2D blocks.
struct Material {
    GXColor matColor[2];u8 chans,matSrc[4],cull;                            // J2DColorBlock
    u8 texGens,genType[8],genSrc[8],genMtx[8],texMtx,sourced;                 // J2DTexGenBlock
    u8 textures,stages;Stage stage[4];GXColorS10 tevColor[4];GXColor kColor[4];u8 swap[4][4]; // J2DTevBlockN
    u8 comp0,ref0,alphaOp,comp1,ref1,blendType,blendSrc,blendDst,logic,dither; // J2DPEBlock
};
CommandStream& current(){auto* s=commandStream();assert(s);return *s;}
template<class Stream> constexpr bool forwardsMaskedWrites(){return requires(Stream& stream){stream.writeBpMasked(0,0,0);};}
// A J2DTexMtx stand-in: its info, then the matrix its load sends.
struct TexObject {u32 info[9];float matrix[3][4];};
TexObject texObject{{0,0,0,0,0,0,0,0,0},{{1,0,0,0.25f},{0,1,0,0.5f},{0,0,1,0}}};
void colorAndTexGen(const Material& m){
    for(int i=0;i<2;++i)GXSetChanMatColor(GXChannelID(GX_COLOR0A0+i),m.matColor[i]);
    GXSetNumChans(m.chans);
    const GXChannelID mapping[4]{GX_COLOR0,GX_ALPHA0,GX_COLOR1,GX_ALPHA1};
    for(int i=0;i<m.chans*2;++i)GXSetChanCtrl(mapping[i],GX_DISABLE,GX_SRC_REG,GXColorSrc(m.matSrc[i]),GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
    GXSetCullMode(GXCullMode(m.cull));
    GXSetNumTexGens(m.texGens);
    for(int i=0;i<m.texGens;++i)GXSetTexCoordGen(GXTexCoordID(i),GXTexGenType(m.genType[i]),GXTexGenSrc(m.genSrc[i]),m.genMtx[i]);
    if(m.texMtx){static const float matrix[3][4]{{1,0,0,0.25f},{0,1,0,0.5f},{0,0,1,0}};GXLoadTexMtxImm(matrix,GX_TEXMTX0,GX_MTX2x4);}
    if(m.sourced){
        if(materialMemoRecording())materialMemoSource(current().registers(),&texObject,sizeof(texObject),texObject.matrix);
        GXLoadTexMtxImm(texObject.matrix,GX_TEXMTX1,GX_MTX2x4);
    }
}
// The texture loads of J2DTevBlockN::setGX, at the register level of
// commandTextureBinding (texture objects need the full texture runtime).
void textures(const Material& m){
    auto& s=current();
    for(unsigned unit=0;unit<m.textures;++unit){
        const unsigned offset=(unit/4)*32+unit%4;
        assert(s.writeBp(0xfe,0xffffff)&&s.writeBp(u8(0x80+offset),0x11+unit)&&s.writeBp(u8(0x84+offset),0)&&
               s.writeBp(u8(0x88+offset),0x3ff|(0xffu<<10)|(unit<<20))&&s.writeBp(u8(0x94+offset),0x1000+unit));
        if(unit==1)assert(s.writeBp(0x64,0x1001)&&s.writeBp(0x65,((0xf0000+(unit<<13)-0x80000)>>9)|(1u<<10))&&
                          s.writeBp(u8(0x98+offset),(0xf0000+(unit<<13)-0x80000)>>9));
    }
}
void tevAndPe(const Material& m){
    for(int i=0;i<m.stages;++i){const auto& t=m.stage[i];GXSetTevOrder(GXTevStageID(i),GXTexCoordID(t.coord),GXTexMapID(t.map),GXChannelID(t.channel));}
    for(int i=0;i<4;++i)GXSetTevColorS10(i!=3?GXTevRegID(i+1):GX_TEVPREV,m.tevColor[i]);
    GXSetNumTevStages(m.stages);
    for(int i=0;i<m.stages;++i){
        const auto& t=m.stage[i];const auto id=GXTevStageID(i);
        GXSetTevColorIn(id,GXTevColorArg(t.color[0]),GXTevColorArg(t.color[1]),GXTevColorArg(t.color[2]),GXTevColorArg(t.color[3]));
        GXSetTevAlphaIn(id,GXTevAlphaArg(t.alpha[0]),GXTevAlphaArg(t.alpha[1]),GXTevAlphaArg(t.alpha[2]),GXTevAlphaArg(t.alpha[3]));
        GXSetTevColorOp(id,GXTevOp(t.cop),GX_TB_ZERO,GXTevScale(t.cscale),GX_TRUE,GXTevRegID(t.creg));
        GXSetTevAlphaOp(id,GXTevOp(t.aop),GX_TB_ZERO,GXTevScale(t.ascale),GX_TRUE,GXTevRegID(t.areg));
        GXSetTevKColorSel(id,GXTevKColorSel(t.kc));GXSetTevKAlphaSel(id,GXTevKAlphaSel(t.ka));
    }
    for(int i=0;i<4;++i)GXSetTevKColor(GXTevKColorID(i),m.kColor[i]);
    for(int i=0;i<m.stages;++i)GXSetTevSwapMode(GXTevStageID(i),GXTevSwapSel(m.stage[i].ras),GXTevSwapSel(m.stage[i].tex));
    for(int i=0;i<4;++i)GXSetTevSwapModeTable(GXTevSwapSel(i),GXTevColorChan(m.swap[i][0]),GXTevColorChan(m.swap[i][1]),
                                              GXTevColorChan(m.swap[i][2]),GXTevColorChan(m.swap[i][3]));
    for(int i=0;i<m.stages;++i)GXSetTevIndirect(GXTevStageID(i),GX_INDTEXSTAGE0,GX_ITF_8,GX_ITB_NONE,GX_ITM_OFF,GX_ITW_OFF,GX_ITW_OFF,GX_FALSE,GX_FALSE,GX_ITBA_OFF);
    GXSetAlphaCompare(GXCompare(m.comp0),m.ref0,GXAlphaOp(m.alphaOp),GXCompare(m.comp1),m.ref1);
    GXSetBlendMode(GXBlendMode(m.blendType),GXBlendFactor(m.blendSrc),GXBlendFactor(m.blendDst),GXLogicOp(m.logic));
    GXSetDither(GXBool(m.dither));
}
void original(const Material& m){colorAndTexGen(m);textures(m);tevAndPe(m);}
MaterialMemoVerdict lastVerdict;
// nativeMaterialSetGX: the key is the material identity and bytes.
MaterialMemoAction memoized(const Material& m,const void* identity){
    auto& s=current();assert(s.finish());
    if(materialMemoGivenUp(identity)){original(m);return MaterialMemoAction::plain;}
    MaterialMemoSum sum;materialMemoChecksum(sum,&identity,sizeof(identity));materialMemoChecksum(sum,&m,sizeof(m));
    const auto ticket=materialMemoLookup(s.registers(),identity,sum,m.stages,m.texGens);
    if(ticket.action==MaterialMemoAction::replay){materialMemoReplay(ticket,0);textures(m);materialMemoReplay(ticket,1);}
    else if(ticket.action==MaterialMemoAction::record){colorAndTexGen(m);materialMemoSplit();textures(m);tevAndPe(m);lastVerdict=materialMemoFinish(ticket);}
    else original(m);
    return ticket.action;
}
// J2DPane::draw and J2DPictureEx::drawSelf around setGX: cull mode before,
// vertex setup, position matrix and pane alpha after (no geometry here).
void before(const Material& m){GXSetCullMode(GXCullMode(m.cull==GX_CULL_NONE?GX_CULL_NONE:GX_CULL_BACK));}
void after(unsigned pane,u8 alpha){
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    const float matrix[3][4]{{1,0,0,float(pane)},{0,1,0,2},{0,0,1,0}};GXLoadPosMtxImm(matrix,GX_PNMTX0);
    if(alpha!=255)GXSetChanMatColor(GX_ALPHA0,GXColor{255,255,255,alpha});
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_S16,8);
}
// What a 3D scene leaves before the HUD: different on every frame.
void scene(unsigned frame){
    GXSetNumTevStages(u8(frame%3+1));GXSetTevColor(GX_TEVREG0,GXColor{u8(frame*10),20,30,40});
    GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_CLEAR);GXSetChanMatColor(GX_COLOR0A0,GXColor{u8(frame),1,2,3});
    GXSetNumTexGens(u8(frame%2+1));GXSetTexCoordGen(GX_TEXCOORD1,GX_TG_MTX2x4,GX_TG_TEX1,GX_TEXMTX1);
    GXSetTevKColorSel(GX_TEVSTAGE0,GX_TEV_KCSEL_K1);GXSetDither(GXBool(frame&1));GXSetCurrentMtx(GX_PNMTX0);
}
void same(const CommandProcessor& a,const CommandProcessor& b,const char* where,unsigned pane){
    const auto& x=a.registers();const auto& y=b.registers();
    bool ok=x.cp==y.cp&&x.bp==y.bp&&x.xf==y.xf&&x.cpValid==y.cpValid&&x.bpValid==y.bpValid&&x.xfValid==y.xfValid&&
        x.tevColor==y.tevColor&&x.tevColorValid==y.tevColorValid&&x.matrixIndex==y.matrixIndex&&x.matrixIndexValid==y.matrixIndexValid&&
        x.tlutOverflow==y.tlutOverflow&&x.bpMask==y.bpMask&&x.stateEpoch==y.stateEpoch&&x.vertexSetupEpoch==y.vertexSetupEpoch&&
        x.vertexCacheEpoch==y.vertexCacheEpoch&&a.statistics().commands==b.statistics().commands&&
        x.stateHash.bp==y.stateHash.bp&&x.stateHash.texture==y.stateHash.texture&&x.stateHash.cp==y.stateHash.cp&&x.stateHash.xf==y.stateHash.xf;
    for(unsigned i=0;i<x.tluts.size();++i)ok=ok&&x.tluts[i].token==y.tluts[i].token&&x.tluts[i].offset==y.tluts[i].offset&&x.tluts[i].entries==y.tluts[i].entries;
    if(!ok){
        for(unsigned i=0;i<256;++i)if(x.bp[i]!=y.bp[i]||x.bpValid[i]!=y.bpValid[i])fprintf(stderr,"BP %02x memo %06x original %06x\n",i,x.bp[i],y.bp[i]);
        for(unsigned i=0;i<x.xf.size();++i)if(x.xf[i]!=y.xf[i]||x.xfValid[i]!=y.xfValid[i])fprintf(stderr,"XF %04x memo %08x original %08x\n",i,x.xf[i],y.xf[i]);
        fprintf(stderr,"%s pane %u: epochs %u/%u commands %llu/%llu\n",where,pane,x.stateEpoch,y.stateEpoch,
                (unsigned long long)a.statistics().commands,(unsigned long long)b.statistics().commands);
    }
    assert(ok);
}
Stage modulate(u8 coord,u8 map){
    return Stage{{GX_CC_ZERO,GX_CC_TEXC,GX_CC_RASC,GX_CC_ZERO},{GX_CA_ZERO,GX_CA_TEXA,GX_CA_RASA,GX_CA_ZERO},
                 GX_TEV_ADD,GX_CS_SCALE_1,GX_TEVPREV,GX_TEV_ADD,GX_CS_SCALE_1,GX_TEVPREV,GX_TEV_SWAP0,GX_TEV_SWAP0,
                 coord,map,GX_COLOR0A0,GX_TEV_KCSEL_1,GX_TEV_KASEL_1};
}
Material base(){
    Material m{};
    m.matColor[0]={255,255,255,255};m.matColor[1]={255,255,255,255};m.chans=1;m.cull=GX_CULL_NONE;
    m.texGens=1;m.genType[0]=GX_TG_MTX2x4;m.genSrc[0]=GX_TG_TEX0;m.genMtx[0]=GX_IDENTITY;
    m.textures=1;m.stages=1;m.stage[0]=modulate(GX_TEXCOORD0,GX_TEXMAP0);
    for(int i=0;i<4;++i){m.tevColor[i]={0,0,0,0};m.kColor[i]={255,255,255,255};}
    for(int i=0;i<4;++i){m.swap[i][0]=0;m.swap[i][1]=1;m.swap[i][2]=2;m.swap[i][3]=3;}
    m.swap[1][0]=m.swap[1][1]=m.swap[1][2]=0;
    m.comp0=GX_ALWAYS;m.alphaOp=GX_AOP_AND;m.comp1=GX_ALWAYS;
    m.blendType=GX_BM_BLEND;m.blendSrc=GX_BL_SRCALPHA;m.blendDst=GX_BL_INVSRCALPHA;m.logic=GX_LO_NOOP;
    return m;
}
}
int main(){
    CommandMemory memory;CommandProcessor memoProcessor(memory),plainProcessor(memory);
    std::array<u8,256> memoScratch{},plainScratch{};
    CommandStream memoStream(memoProcessor,memoScratch),plainStream(plainProcessor,plainScratch);
    using A=MaterialMemoAction;
    // A HUD: plain pictures, a two-stage picture with konst colour, a text
    // box with two texgens and channels, an untextured window, an additive
    // pane, one with a texture matrix object that changes every third frame,
    // one with an animated TEV colour and a plain one after it, one with an
    // animated material colour and one after it loading an undeclared texture
    // matrix (refused).
    std::vector<Material> panes(10,base());
    panes[1].stages=2;panes[1].textures=2;panes[1].texGens=2;panes[1].genSrc[1]=GX_TG_TEX1;panes[1].genMtx[1]=GX_IDENTITY;
    panes[1].stage[1]=modulate(GX_TEXCOORD1,GX_TEXMAP1);panes[1].stage[1].color[0]=GX_CC_CPREV;panes[1].stage[1].kc=GX_TEV_KCSEL_K0;
    panes[1].kColor[0]={200,100,50,255};panes[1].tevColor[1]={-12,300,1023,-1024};
    panes[2].chans=2;panes[2].matSrc[1]=GX_SRC_VTX;panes[2].texGens=2;panes[2].genSrc[1]=GX_TG_TEX0;panes[2].genMtx[1]=GX_TEXMTX2;
    panes[2].matColor[0]={10,20,30,128};panes[2].comp0=GX_GREATER;panes[2].ref0=8;panes[2].dither=1;
    panes[3].textures=0;panes[3].texGens=0;panes[3].stage[0].coord=GX_TEXCOORD_NULL;panes[3].stage[0].map=GX_TEXMAP_NULL;
    panes[3].stage[0].color[1]=GX_CC_ZERO;panes[3].stage[0].alpha[1]=GX_CA_ZERO;panes[3].cull=GX_CULL_BACK;
    panes[4].blendSrc=GX_BL_ONE;panes[4].blendDst=GX_BL_ONE;
    panes[5].sourced=1;panes[5].texGens=2;panes[5].genSrc[1]=GX_TG_TEX0;panes[5].genMtx[1]=GX_TEXMTX1;
    panes[9].texMtx=1;
    const std::array<u8,10> alpha{255,255,128,255,255,255,255,255,255,64};
    std::array<std::array<std::uint64_t,3>,10> perPane{};
    auto run=[&](unsigned frames,std::array<std::uint64_t,3>* counts){
        for(unsigned frame=0;frame<frames;++frame){
            assert(setCommandStream(&memoStream));scene(frame);assert(setCommandStream(&plainStream));scene(frame);
            same(memoProcessor,plainProcessor,"scene",0);
            panes[6].tevColor[1].r=s16(frame*13); // animated TEV colour: a new key every frame
            panes[8].matColor[0].a=u8(frame*37); // animated material colour: a new key every frame
            if(frame%3==0)texObject.matrix[0][3]=0.25f+0.125f*float(frame); // an SRT animation step
            for(unsigned pane=0;pane<panes.size();++pane){
                const auto& m=panes[pane];
                assert(setCommandStream(&memoStream));before(m);const auto action=memoized(m,&panes[pane]);after(pane,alpha[pane]);
                assert(setCommandStream(&plainStream));before(m);original(m);after(pane,alpha[pane]);
                same(memoProcessor,plainProcessor,"pane",pane);
                if(counts&&frame>=2){++(*counts)[unsigned(action)];++perPane[pane][unsigned(action)];}
            }
        }
    };
    MaterialMemoStatistics s;materialMemoStatistics(s,true);
    TypedWriteStatistics typed;typedWriteStatistics(typed,true);
    std::array<std::uint64_t,3> counts{};run(8,&counts);
    materialMemoStatistics(s,true);
    // The TEV setters make their masked writes in one processor call once
    // the stream forwards CommandProcessor::writeBpMasked, two writes before.
    typedWriteStatistics(typed,true);
    constexpr bool forwarded=forwardsMaskedWrites<CommandStream>();
    assert(forwarded?typed.masked>0:typed.masked==0);
    printf("TEV masked writes: %s (one-call writes=%llu unchanged=%llu)\n",forwarded?"one processor call":"two writes",
        (unsigned long long)typed.masked,(unsigned long long)typed.unchanged);
    printf("HUD frames: setGX=%llu hits=%llu recorded=%llu stored=%llu refused=%llu volatile=%llu bypassed=%llu given-up=%llu sources-changed=%llu; frames 3-8 plain=%llu replay=%llu record=%llu\n",
        (unsigned long long)s.calls,(unsigned long long)s.replays,(unsigned long long)s.records,(unsigned long long)s.stores,
        (unsigned long long)s.refused,(unsigned long long)s.volatileSkips,(unsigned long long)s.bypassed,
        (unsigned long long)s.givenUp,(unsigned long long)s.sourceChanges,
        (unsigned long long)counts[0],(unsigned long long)counts[1],(unsigned long long)counts[2]);
    // Pane 0 follows the scene (new state every frame); panes 1-4 replay
    // once settled; pane 5 replays except on the frames its matrix object
    // changed; pane 7 replays although pane 6 leaves new TEV colours every
    // frame; pane 9 is refused (undeclared matrix) and given up.
    for(const unsigned pane:{1u,2u,3u,4u,7u})assert(perPane[pane][unsigned(A::replay)]==6);
    assert(perPane[5][unsigned(A::replay)]>=3&&perPane[9][unsigned(A::replay)]==0);
    assert(s.mismatches==0&&s.refused>0&&s.sourceChanges>=2&&s.refusedBy[unsigned(MaterialMemoRefusal::matrix)]>0&&s.givenUp>0);
    // Verification: every replay and recording checked, none differs.
    setMaterialMemo(true,true);run(6,nullptr);
    materialMemoStatistics(s,true);
    printf("HUD frames verified: checks=%llu hits=%llu mismatches=%llu\n",(unsigned long long)s.verified,
        (unsigned long long)s.replays,(unsigned long long)s.mismatches);
    assert(s.verified>0&&s.replays>0&&s.mismatches==0);
    setMaterialMemo(true,false);
    // A BP mask left pending by other code: the memo stays out of the way.
    assert(setCommandStream(&memoStream));assert(current().writeBp(0xfe,0xff));
    assert(memoized(panes[0],&panes[0])==A::plain);
    assert(setCommandStream(&plainStream));assert(current().writeBp(0xfe,0xff));original(panes[0]);
    same(memoProcessor,plainProcessor,"pending mask",0);
    assert(setCommandStream(nullptr));
    puts("PASS material memo with the native typed setters: memoized J2D materials leave the same registers, epochs and command counts as the setters across a multi-frame HUD, animated and refused panes, verification and a pending BP mask");
}
