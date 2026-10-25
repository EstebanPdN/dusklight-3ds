#include "../runtime/gx_state.hpp"
#include <JSystem/J2DGraph/J2DOrthoGraph.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXExtra.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Host diagnostics for the original J2D/math units; failures remain fatal.
extern "C" void OSReport(const char* message, ...) {
    va_list args; va_start(args, message); std::vfprintf(stderr, message, args); va_end(args);
}
extern "C" void OSPanic(const char* file, int line, const char* message, ...) {
    std::fprintf(stderr, "%s:%d: ", file, line);
    va_list args; va_start(args, message); std::vfprintf(stderr, message, args); va_end(args);
    std::abort();
}
namespace gx = dusk::native::gx;
namespace {
struct Capture {
    gx::DrawPacket packet{};
    std::array<gx::Vertex, gx::maxVertices> vertices{};
    unsigned calls = 0;
    bool accept = true;
} capture;
bool sink(const gx::DrawPacket& packet, void* user) {
    auto& out = *static_cast<Capture*>(user);
    ++out.calls; out.packet = packet;
    assert(packet.vertices.size() <= out.vertices.size());
    for (std::size_t i=0; i<packet.vertices.size(); ++i) out.vertices[i]=packet.vertices[i];
    out.packet.vertices={out.vertices.data(),packet.vertices.size()};
    return out.accept;
}
void reset() {
    gx::resetState(); capture.calls=0; capture.accept=true;
    gx::setDrawSink(sink,&capture);
}
void close(float actual, float expected) { assert(std::fabs(actual-expected)<0.00001f); }
std::uint32_t rgba(const gx::Vertex& v) {
    return (std::uint32_t(v.r)<<24)|(std::uint32_t(v.g)<<16)|(std::uint32_t(v.b)<<8)|v.a;
}
void good(unsigned draws, unsigned vertices) {
    const auto stats=gx::statistics();
    if(stats.errors) std::fprintf(stderr,"unexpected GX error: %s at %s\n",gx::errorText(stats.firstError),stats.firstErrorCall);
    assert(stats.errors==0 && stats.drawsDropped==0 && stats.drawsSubmitted==draws && stats.verticesSubmitted==vertices);
    assert(capture.calls==draws);
}
void bad(gx::Error expected, unsigned drops=1) {
    const auto stats=gx::statistics();
    if(stats.firstError!=expected) std::fprintf(stderr,"expected %s, got %s at %s\n",gx::errorText(expected),gx::errorText(stats.firstError),stats.firstErrorCall);
    assert(stats.firstError==expected && stats.drawsDropped==drops && stats.drawsSubmitted==0);
    assert(capture.calls==0);
}
void prepare(J2DOrthoGraph& graph) {
    reset(); graph.setPort();
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
}
void vertices(unsigned count) {
    for(unsigned i=0;i<count;++i) { GXPosition3f32(float(i%400),float(i/400),0); GXColor1u32(0x12345678); }
}
void quad() { GXBegin(GX_QUADS,GX_VTXFMT0,4); vertices(4); GXEnd(); }
void originalJ2D() {
    reset(); J2DOrthoGraph graph(0,0,400,240,-1,1); graph.setPort();
    graph.setColor(0x112233ff,0x445566ff,0x778899ff,0xaabbccff);
    graph.fillBox({0,0,400,240}); good(1,4);
    const auto& p=capture.packet;
    assert(p.primitive==gx::Primitive::quads && p.vertices.size()==4);
    assert(rgba(p.vertices[0])==0x112233ff && rgba(p.vertices[1])==0x445566ff);
    // Preserve the original J2D call order, including its BL/BR field ordering.
    assert(rgba(p.vertices[2])==0xaabbccff && rgba(p.vertices[3])==0x778899ff);
    close(p.vertices[2].x,400); close(p.vertices[2].y,240);
    close(p.projection[0]*0+p.projection[3],-1);
    close(p.projection[5]*0+p.projection[7],1);
    close(p.projection[0]*400+p.projection[3],1);
    close(p.projection[5]*240+p.projection[7],-1);
    close(p.modelView[0],1); close(p.modelView[5],1); close(p.modelView[10],1);
    assert(p.scissor.x==0 && p.scissor.y==0 && p.scissor.width==400 && p.scissor.height==240);
    close(p.viewport.width,400); close(p.viewport.height,240);
    assert(p.blend.mode==GX_BM_NONE && p.blend.src==GX_BL_ONE && p.blend.dst==GX_BL_ZERO);
    assert(!p.depth.enabled && !p.depth.write && p.depth.compare==GX_LEQUAL);
    assert(p.alpha.compare0==GX_GREATER && p.alpha.reference0==0 && p.alpha.op==GX_AOP_OR);
    assert(p.alpha.compare1==GX_GREATER && p.alpha.reference1==0 && p.cull==GX_CULL_NONE);
    // fillBox leaves an inactive integer position format; the next call fixes it.
    graph.fillBox({20,30,40,50}); good(2,8);
    graph.setColor(0x10203080); graph.fillBox({0,0,10,10}); good(3,12);
    assert(p.blend.mode==GX_BM_BLEND && p.blend.src==GX_BL_SRCALPHA && p.blend.dst==GX_BL_INVSRCALPHA);
    assert(rgba(p.vertices[3])==0x10203080);
    graph.setLineWidth(12); graph.drawFrame({5,6,7,8}); good(4,17);
    assert(p.primitive==gx::Primitive::lineStrip && p.vertices.size()==5);
    close(p.lineWidthPixels,2); close(p.vertices.front().x,p.vertices.back().x);
    close(p.vertices.front().y,p.vertices.back().y);
    graph.line({2,3},{4,5}); good(5,19);
    assert(p.primitive==gx::Primitive::lines && p.vertices.size()==2);
    graph.scissor(11.25f,12.75f,18.0f,19.0f); graph.setScissor(); graph.fillBox({0,0,1,1}); good(6,23);
    assert(p.scissor.x==11 && p.scissor.y==12 && p.scissor.width==19 && p.scissor.height==20);
    std::puts("PASS original J2D fillBox/drawFrame/line, color endian, projection, alpha blend and scissor");
}
void primitiveCapture() {
    J2DOrthoGraph graph(0,0,400,240,-1,1);
    struct Case { GXPrimitive input; gx::Primitive output; unsigned count; };
    const Case cases[]={{GX_QUADS,gx::Primitive::quads,8},{GX_TRIANGLES,gx::Primitive::triangles,6},
        {GX_TRIANGLESTRIP,gx::Primitive::triangleStrip,5},{GX_TRIANGLEFAN,gx::Primitive::triangleFan,5},
        {GX_LINES,gx::Primitive::lines,4},{GX_LINESTRIP,gx::Primitive::lineStrip,4}};
    for(const auto& c:cases) {
        prepare(graph); GXBegin(c.input,GX_VTXFMT0,c.count); vertices(c.count); GXEnd(); good(1,c.count);
        assert(capture.packet.primitive==c.output && rgba(capture.packet.vertices[0])==0x12345678);
    }
    prepare(graph); GXBegin(GX_QUADS,GX_VTXFMT0,gx::maxVertices); vertices(gx::maxVertices); GXEnd(); good(1,gx::maxVertices);
    prepare(graph); GXBegin(GX_TRIANGLESTRIP,GX_VTXFMT0,gx::maxVertices+1); vertices(gx::maxVertices+1); GXEnd(); bad(gx::Error::vertexOverflow);
    prepare(graph); GXBegin(GX_QUADS,GX_VTXFMT0,4); vertices(5); GXEnd(); bad(gx::Error::vertexOverflow);
    prepare(graph); GXBegin(GX_TRIANGLES,GX_VTXFMT0,4); GXEnd(); bad(gx::Error::invalidParameter);
    prepare(graph); GXBegin(GX_POINTS,GX_VTXFMT0,1); GXEnd(); bad(gx::Error::unsupportedState);
    std::puts("PASS six primitive captures, exact 4096 vertex bound, excess and invalid counts");
}
void activeStateAndMatrix() {
    J2DOrthoGraph graph(0,0,400,240,-1,1); prepare(graph);
    gx::setVertexFormat(7,13,900,900,255); gx::setTevOperation(7,900);
    gx::setTexCoordGen(7,900,900,900,900,900); quad(); good(1,4);
    prepare(graph); GXSetNumTevStages(2); quad(); bad(gx::Error::unsupportedState);
    prepare(graph); GXSetNumTexGens(1); quad(); bad(gx::Error::unsupportedState);
    prepare(graph); GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE); quad(); bad(gx::Error::unsupportedState);
    prepare(graph); GXSetVtxDesc(GX_VA_POS,GX_INDEX8); quad(); bad(gx::Error::unsupportedState);
    prepare(graph); GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_S16,0); quad(); bad(gx::Error::unsupportedState);
    prepare(graph); gx::setVertexFormat(8,9,1,4,0); quad(); bad(gx::Error::invalidParameter);
    prepare(graph); gx::setTevDirect(16); quad(); bad(gx::Error::invalidParameter);
    prepare(graph); gx::setChannelControl(6,0,0,1,0,0,0); quad(); bad(gx::Error::invalidParameter);
    prepare(graph); gx::setScissor(2040,0,20,10); quad(); bad(gx::Error::invalidParameter);
    prepare(graph); gx::setViewport(0,0,0,240,0,1); quad(); bad(gx::Error::invalidParameter);
    prepare(graph); float affine[12]={1,0,0,17,0,1,0,23,0,0,1,0};
    gx::loadPositionMatrix(affine,3); gx::setCurrentMatrix(3); affine[3]=900;
    quad(); good(1,4); close(capture.packet.modelView[3],17); close(capture.packet.modelView[7],23);
    prepare(graph); GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for(unsigned i=0;i<4;++i) { GXPosition3f32(0,0,0); GXColor1u32(0xffffffff); GXTexCoord2f32(i*0.25f,i*0.5f); }
    GXEnd(); good(1,4); close(capture.packet.vertices[3].u,0.75f); close(capture.packet.vertices[3].v,1.5f);
    std::puts("PASS active-state rejection, inactive configuration, matrix copy and ordered UV capture");
}
void protocol() {
    J2DOrthoGraph graph(0,0,400,240,-1,1); prepare(graph);
    GXBegin(GX_QUADS,GX_VTXFMT0,4); vertices(3); GXPosition3f32(1,2,3); GXEnd(); bad(gx::Error::incompleteDraw);
    prepare(graph); GXBegin(GX_QUADS,GX_VTXFMT0,4); GXColor1u32(0); GXEnd(); bad(gx::Error::vertexProtocol);
    prepare(graph); GXBegin(GX_QUADS,GX_VTXFMT0,4); GXPosition3f32(std::numeric_limits<float>::infinity(),0,0); GXEnd(); bad(gx::Error::invalidParameter);
    prepare(graph); GXBegin(GX_QUADS,GX_VTXFMT0,4); GXSetNumChans(1); vertices(4); GXEnd(); bad(gx::Error::vertexProtocol);
    const auto first=gx::statistics().firstErrorCall; GXEnd(); assert(gx::statistics().firstErrorCall==first);
    prepare(graph); GXBegin(GX_QUADS,GX_VTXFMT0,4); GXBegin(GX_QUADS,GX_VTXFMT0,4); vertices(4); GXEnd();
    assert(gx::statistics().drawsSubmitted==1 && gx::statistics().drawsDropped==1 && capture.calls==1);
    prepare(graph); capture.accept=false; quad();
    assert(gx::statistics().firstError==gx::Error::backendRejected && gx::statistics().drawsDropped==1 && capture.calls==1);
    prepare(graph); gx::setDrawSink(nullptr); quad(); bad(gx::Error::missingSink);
    reset(); gx::resetState(); good(0,0); graph.setPort(); graph.fillBox({0,0,1,1}); good(1,4);
    std::puts("PASS protocol ordering, incomplete draws, nonfinite input, backend errors and reset sink preservation");
}
std::array<std::uint8_t,64> smallBti() {
    std::array<std::uint8_t,64> bytes{};
    bytes[3]=8; bytes[5]=8; bytes[20]=1; bytes[21]=1; bytes[24]=1; bytes[31]=32;
    return bytes; // I4 8x8, one mip, no palette, clamp, linear.
}
void texturedState(J2DOrthoGraph& graph, GXTexObj* object) {
    prepare(graph); GXLoadTexObj(object,GX_TEXMAP0);
    GXSetNumTexGens(1); GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_S16,8);
    GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR_NULL);
    GXSetTevColor(GX_TEVREG2,{255,255,255,255});
    GXSetTevColorIn(GX_TEVSTAGE0,GX_CC_TEXC,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO);
    GXSetTevAlphaIn(GX_TEVSTAGE0,GX_CA_A2,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
}
void texturedQuad(bool floats=false) {
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for(int i=0;i<4;++i) {
        GXPosition3f32(i,0,0); GXColor1u32(0xffffffff);
        if(floats) GXTexCoord2f32(0,0); else GXTexCoord2s16(i*256,-128);
    }
    GXEnd();
}
void textureLifetimesAndLimits() {
    J2DOrthoGraph graph(0,0,400,240,-1,1);
    auto bytes=smallBti(); gx::TextureHandle handle=0;
    for(std::size_t size=0;size<bytes.size();++size) {
        reset(); assert(!gx::registerTexture({bytes.data(),size},handle) && handle==0);
    }
    reset(); assert(gx::registerTexture(bytes,handle));
    gx::TextureHandle duplicate=0; assert(!gx::registerTexture(bytes,duplicate) && duplicate==0);
    GXTexObj object{};
    GXInitTexObj(&object,bytes.data()+32,8,8,GX_TF_I4,GX_CLAMP,GX_CLAMP,GX_FALSE);
    texturedState(graph,&object); texturedQuad(); good(1,4);
    assert(capture.packet.textured && capture.packet.texture.handle==handle && capture.packet.texture.image.size()==32);
    close(capture.packet.vertices[3].u,3); close(capture.packet.vertices[0].v,-0.5f);
    // Changing the object after load does not alter already loaded GX state.
    texturedState(graph,&object);
    GXInitTexObjLOD(&object,GX_LIN_MIP_LIN,GX_LINEAR,0,1,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    texturedQuad(); good(1,4);
    texturedState(graph,&object); texturedQuad(); bad(gx::Error::unsupportedState);
    GXInitTexObjLOD(&object,GX_LINEAR,GX_LINEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    texturedState(graph,&object); texturedQuad(); good(1,4);
    texturedState(graph,&object); texturedQuad(true); bad(gx::Error::vertexProtocol);
    texturedState(graph,&object); GXSetNumTevStages(3); texturedQuad(); bad(gx::Error::unsupportedState);
    texturedState(graph,&object); GXSetNumTexGens(2); texturedQuad(); bad(gx::Error::unsupportedState);
    texturedState(graph,&object); GXSetTevColorOp(GX_TEVSTAGE0,GX_TEV_SUB,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
    texturedQuad(); bad(gx::Error::unsupportedState);
    texturedState(graph,&object); GXDestroyTexObj(&object); texturedQuad(); bad(gx::Error::unsupportedState);
    GXInitTexObj(&object,bytes.data()+32,8,8,GX_TF_I4,GX_CLAMP,GX_CLAMP,GX_FALSE);
    texturedState(graph,&object);
    GXInitTexObj(&object,bytes.data()+32,8,8,GX_TF_I4,GX_CLAMP,GX_CLAMP,GX_FALSE);
    texturedQuad(); bad(gx::Error::unsupportedState); // old load's generation cannot alias reinitialization
    texturedState(graph,&object); texturedQuad(); good(1,4);
    texturedState(graph,&object); assert(gx::unregisterTexture(handle)); texturedQuad(); bad(gx::Error::unsupportedState);
    assert(gx::registerTexture(bytes,duplicate) && duplicate!=handle);
    texturedState(graph,&object); texturedQuad(); bad(gx::Error::unsupportedState); // same image address does not revive a stale object
    GXDestroyTexObj(&object); assert(gx::unregisterTexture(duplicate));
    GXInitTexObj(&object,reinterpret_cast<const void*>(std::uintptr_t(1)),8,8,GX_TF_I4,GX_CLAMP,GX_CLAMP,GX_FALSE);
    texturedState(graph,&object); texturedQuad(); bad(gx::Error::unsupportedState); // untrusted raw pointer never read
    GXDestroyTexObj(&object);
    std::array<std::array<std::uint8_t,64>,65> assets{};
    std::array<gx::TextureHandle,65> handles{};
    reset();
    for(unsigned i=0;i<assets.size();++i) {
        assets[i]=smallBti(); const bool registered=gx::registerTexture(assets[i],handles[i]);
        assert(registered==(i<64));
    }
    assert(gx::statistics().errors==1 && handles[64]==0);
    std::array<GXTexObj,129> objects{};
    reset();
    for(auto& value:objects) GXInitTexObj(&value,assets[0].data()+32,8,8,GX_TF_I4,GX_CLAMP,GX_CLAMP,GX_FALSE);
    assert(gx::statistics().errors==1);
    for(auto& value:objects) GXDestroyTexObj(&value);
    for(unsigned i=0;i<64;++i) assert(gx::unregisterTexture(handles[i]));
    reset(); GXInitTexObjCI(&object,bytes.data()+32,8,8,GX_TF_C4,GX_CLAMP,GX_CLAMP,GX_FALSE,0);
    // Indexed descriptors are now supported, but this raw image is not
    // registered and must never become a usable binding.
    gx::Texture metadata;
    assert(gx::statistics().errors==0&&gx::queryTextureObject(&object,metadata));
    assert(metadata.width==8&&metadata.height==8&&metadata.format==GX_TF_C4&&!metadata.handle);
    GXLoadTexObj(&object,GX_TEXMAP0);assert(!gx::textureBinding(0,metadata));
    GXDestroyTexObj(&object);
    reset(); GXTlutObj palette{}; GXInitTlutObj(&palette,bytes.data()+32,GX_TL_IA8,16);
    GXLoadTlut(&palette,0); GXDestroyTlutObj(&palette);
    assert(gx::statistics().firstError==gx::Error::invalidParameter && gx::statistics().errors==1);
    std::puts("PASS validated texture registration, truncation, bounded pools, loaded sampler snapshots, stale generations and unregistered palette rejection");
}
}
int main() { originalJ2D(); primitiveCapture(); activeStateAndMatrix(); protocol(); textureLifetimesAndLimits(); std::puts("GX/J2D state integration: PASS"); }
