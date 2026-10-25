#include "../runtime/bootstrap.hpp"
#include "../runtime/clock.hpp"
#include "../runtime/gx_state.hpp"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JKernel/JKRHeap.h"
#if defined(DUSK_TEST_PRIVATE_DISC)
#include "../runtime/archive_bootstrap.hpp"
#include "../runtime/disc.hpp"
#endif
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace dusk::native {
void logText(const char* text) { std::fputs(text, stderr); }
[[noreturn]] void fatal(const char* text) { logText(text); std::abort(); }
void waitForRetrace() { std::atomic_thread_fence(std::memory_order_seq_cst); }
void invalidateCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
void flushCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
std::uint64_t counterTicks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() * (nativeTickFrequency / 1000);
}
std::uint64_t wallMilliseconds() { return epoch1900To2000Ms; }
}
namespace gx = dusk::native::gx;
namespace {
struct Capture {
    gx::DrawPacket packet{};
    std::array<gx::Vertex, 4> vertices{};
    unsigned draws = 0;
} capture;
bool sink(const gx::DrawPacket& packet, void*) {
    assert(packet.vertices.size() == 4);
    capture.packet = packet;
    std::copy(packet.vertices.begin(), packet.vertices.end(), capture.vertices.begin());
    capture.packet.vertices = capture.vertices;
    ++capture.draws;
    return true;
}
void near(float actual, float expected) { assert(std::fabs(actual - expected) < 0.00001f); }
std::uint32_t rgba(const gx::Vertex& v) { return std::uint32_t(v.r) << 24 | std::uint32_t(v.g) << 16 | std::uint32_t(v.b) << 8 | v.a; }
std::uint32_t rgba(const gx::Rgba& v) { return std::uint32_t(v.r) << 24 | std::uint32_t(v.g) << 16 | std::uint32_t(v.b) << 8 | v.a; }
void prepare(J2DOrthoGraph& graph) {
    gx::resetState(); gx::setDrawSink(sink); capture.draws = 0;
    graph.setPort();
}
void good(unsigned count = 1) {
    const auto stats = gx::statistics();
    if (stats.errors) std::fprintf(stderr, "GX failure: %s at %s\n", gx::errorText(stats.firstError), stats.firstErrorCall);
    assert(stats.errors == 0 && stats.drawsDropped == 0 && stats.drawsSubmitted == count && stats.verticesSubmitted == count * 4);
    assert(capture.draws == count);
}
std::vector<unsigned char> bti(unsigned format, bool alpha) {
    const std::size_t bytes = format == GX_TF_I4 ? 32 : 128;
    std::vector<unsigned char> image(32 + bytes);
    image[0] = format; image[1] = alpha;
    image[3] = 8; image[5] = 8; image[6] = GX_CLAMP; image[7] = GX_CLAMP;
    image[20] = GX_LINEAR; image[21] = GX_LINEAR; image[24] = 1; image[31] = 32;
    for (std::size_t i = 32; i < image.size(); ++i) image[i] = static_cast<unsigned char>(i * 7);
    return image;
}
void flagAndConstructorTests(const ResTIMG* image) {
    alignas(JUTTexture) std::array<unsigned char, sizeof(JUTTexture)> storage;
    for (const auto poison : {0x00, 0xa5, 0xff}) {
        storage.fill(poison);
        auto* texture = ::new (storage.data()) JUTTexture;
        assert(!texture->getCaptureFlag() && !texture->getEmbPaletteDelFlag());
        texture->setCaptureFlag(true); assert(texture->getCaptureFlag());
        texture->setEmbPaletteDelFlag(true); assert(texture->getCaptureFlag() && texture->getEmbPaletteDelFlag());
        texture->setCaptureFlag(false); assert(!texture->getCaptureFlag() && texture->getEmbPaletteDelFlag());
        texture->setEmbPaletteDelFlag(false); assert(!texture->getCaptureFlag() && !texture->getEmbPaletteDelFlag());
        texture->~JUTTexture();
        storage.fill(poison);
        texture = ::new (storage.data()) JUTTexture(image, 0);
        assert(!texture->getCaptureFlag() && !texture->getEmbPaletteDelFlag());
        assert(texture->getTexInfo() == image && texture->getWidth() == 8 && texture->getHeight() == 8);
        texture->~JUTTexture();
    }
    std::puts("PASS JUTTexture: poisoned default/image construction and independent capture/palette bits");
}
void inspectBase(gx::TextureHandle handle, const std::vector<unsigned char>& bytes, unsigned format, bool alpha) {
    const auto& packet = capture.packet;
    assert(packet.textured && packet.texture.handle == handle && packet.primitive == gx::Primitive::quads);
    assert(packet.texture.bti.data() == bytes.data() && packet.texture.bti.size() == bytes.size());
    assert(packet.texture.image.data() == bytes.data() + 32 && packet.texture.image.size() == bytes.size() - 32);
    assert(packet.texture.width == 8 && packet.texture.height == 8 && packet.texture.format == format);
    assert(packet.texture.wrapS == GX_CLAMP && packet.texture.wrapT == GX_CLAMP);
    assert(packet.texture.minFilter == GX_LINEAR && packet.texture.magFilter == GX_LINEAR);
    const auto& stage = packet.tev[0];
    assert(stage.coordinate == GX_TEXCOORD0 && stage.texture == GX_TEXMAP0 && stage.channel == GX_COLOR_NULL);
    assert(stage.color[0] == GX_CC_TEXC && stage.color[1] == GX_CC_ZERO && stage.color[2] == GX_CC_ZERO && stage.color[3] == GX_CC_ZERO);
    assert(stage.alpha[0] == (alpha ? GX_CA_TEXA : GX_CA_A2));
    assert(stage.alpha[1] == GX_CA_ZERO && stage.alpha[2] == GX_CA_ZERO && stage.alpha[3] == GX_CA_ZERO);
    assert(stage.colorOp.operation == GX_TEV_ADD && stage.colorOp.bias == GX_TB_ZERO && stage.colorOp.scale == GX_CS_SCALE_1 && stage.colorOp.clamp == GX_TRUE && stage.colorOp.output == GX_TEVPREV);
    assert(stage.alphaOp.operation == GX_TEV_ADD && stage.alphaOp.output == GX_TEVPREV);
    assert(rgba(packet.registers[GX_TEVREG2]) == 0xffffffff);
    assert(packet.blend.mode == GX_BM_BLEND && packet.blend.src == GX_BL_SRCALPHA && packet.blend.dst == GX_BL_INVSRCALPHA);
}
void pictureTests(unsigned format, bool alpha) {
    const auto before = dusk::native::kernelStats();
    auto bytes = bti(format, alpha);
    const auto original = bytes;
    gx::TextureHandle handle = 0;
    gx::resetState();
    assert(gx::registerTexture(bytes, handle) && handle);
    const auto* image = reinterpret_cast<const ResTIMG*>(bytes.data());
    flagAndConstructorTests(image);
    {
        J2DPicture picture(image);
        assert(picture.getTextureCount() == 1 && picture.getTexture(0)->getTexInfo() == image && picture.isUsed(image));
        J2DOrthoGraph graph(0, 0, 400, 240, -1, 1);
        prepare(graph); picture.draw(10, 20, 80, 40, false, false, false); good();
        inspectBase(handle, bytes, format, alpha);
        const auto& p = capture.packet;
        assert(p.tevCount == 1);
        near(p.modelView[3], 10); near(p.modelView[7], 20);
        const float normalUv[4][2]{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (unsigned i = 0; i < 4; ++i) {
            near(p.vertices[i].u, normalUv[i][0]); near(p.vertices[i].v, normalUv[i][1]);
            assert(rgba(p.vertices[i]) == 0xffffffff);
        }
        near(p.vertices[0].x, 0); near(p.vertices[0].y, 0);
        near(p.vertices[2].x, 80); near(p.vertices[2].y, 40);
        prepare(graph); picture.draw(10, 20, 80, 40, true, false, false); good();
        for (unsigned i = 0; i < 4; ++i) { near(p.vertices[i].u, 1 - normalUv[i][0]); near(p.vertices[i].v, normalUv[i][1]); }
        prepare(graph); picture.draw(10, 20, 80, 40, false, false, true); good();
        const float rotatedUv[4][2]{{0, 1}, {0, 0}, {1, 0}, {1, 1}};
        for (unsigned i = 0; i < 4; ++i) { near(p.vertices[i].u, rotatedUv[i][0]); near(p.vertices[i].v, rotatedUv[i][1]); }
        // Original float-UV path retains the caller's valid resource and uses
        // clipping-derived UV fractions instead of the signed fixed-point path.
        prepare(graph); picture.drawOut({2, 2, 6, 6}, {0, 0, 8, 8}); good();
        near(p.vertices[0].u, .25f); near(p.vertices[0].v, .25f);
        near(p.vertices[2].u, .75f); near(p.vertices[2].v, .75f);
        picture.setCornerColor(0x11223380, 0x445566ff, 0x77889940, 0xaabbcc20); picture.setAlpha(128);
        prepare(graph); picture.draw(0, 0, 8, 8, false, false, false); good();
        inspectBase(handle, bytes, format, alpha);
        assert(p.tevCount == 2 && p.tev[1].channel == GX_COLOR0A0);
        assert(p.tev[1].color[0] == GX_CC_ZERO && p.tev[1].color[1] == GX_CC_CPREV && p.tev[1].color[2] == GX_CC_RASC && p.tev[1].color[3] == GX_CC_ZERO);
        assert(p.tev[1].alpha[0] == GX_CA_ZERO && p.tev[1].alpha[1] == GX_CA_APREV && p.tev[1].alpha[2] == GX_CA_RASA && p.tev[1].alpha[3] == GX_CA_ZERO);
        assert(rgba(p.vertices[0]) == 0x11223340 && rgba(p.vertices[1]) == 0x44556680);
        assert(rgba(p.vertices[2]) == 0xaabbcc10 && rgba(p.vertices[3]) == 0x77889920);
        picture.setCornerColor(0xffffffff); picture.setAlpha(255);
        assert(picture.setBlackWhite(0x10203040, 0xa0b0c0d0));
        prepare(graph); picture.draw(0, 0, 8, 8, false, false, false); good();
        assert(p.tevCount == 2 && p.tev[1].channel == GX_COLOR_NULL);
        assert(p.tev[1].color[0] == GX_CC_C0 && p.tev[1].color[1] == GX_CC_C1 && p.tev[1].color[2] == GX_CC_CPREV && p.tev[1].color[3] == GX_CC_ZERO);
        assert(rgba(p.registers[GX_TEVREG0]) == 0x10203040 && rgba(p.registers[GX_TEVREG1]) == 0xa0b0c0d0);
        picture.hide(); prepare(graph); picture.draw(0, 0, 8, 8, false, false, false); good(0);
        assert(bytes == original); // original JUTTexture borrows, never frees or rewrites the image
    }
    assert(bytes == original);
    assert(gx::unregisterTexture(handle));
    assert(dusk::native::kernelStats().free == before.free && dusk::native::kernelStats().healthy);
    std::printf("PASS original J2DPicture: format=%u alpha=%d; dimensions, S16/F32 UV, mirror/rotation, corner alpha, TEV tint, visibility, lifetime\n", format, alpha);
}
#if defined(DUSK_TEST_PRIVATE_DISC)
void privatePicture(const char* path) {
    using namespace dusk::native;
    const auto before = kernelStats();
    {
        Disc disc; assert(disc.open(path) == DiscStatus::ready);
        ArchiveBootstrap archive;
        constexpr ArchiveLimits limits{{1024 * 1024, 4096, 8192, 255}, 1024 * 1024};
        assert(archive.load(disc, "/res/Object/LogoUs.arc", limits) == ArchiveStatus::ready);
        ResourceSpan image;
        assert(archive.findBasename("nintendo_376x104.bti", image) == ArchiveStatus::ready);
        gx::TextureHandle handle = 0;
        gx::resetState(); assert(gx::registerTexture({image.data, image.size}, handle));
        {
            J2DPicture picture(reinterpret_cast<const ResTIMG*>(image.data));
            J2DOrthoGraph graph(0, 0, 400, 240, -1, 1);
            prepare(graph); picture.draw(12, 68, 376, 104, false, false, false); good();
            const auto& packet = capture.packet;
            assert(packet.textured && packet.tevCount == 1 && packet.texture.handle == handle);
            assert(packet.texture.width == 376 && packet.texture.height == 104 && packet.texture.format == GX_TF_IA8);
            assert(packet.texture.bti.data() == image.data && packet.texture.bti.size() == image.size);
            near(packet.vertices[2].x, 376); near(packet.vertices[2].y, 104);
            near(packet.vertices[0].u, 0); near(packet.vertices[0].v, 0);
            near(packet.vertices[2].u, 1); near(packet.vertices[2].v, 1);
            near(packet.modelView[3], 12); near(packet.modelView[7], 68);
            std::printf("PASS private picture: original JKR archive -> original JUTTexture/J2DPicture -> real GX capture; resource_bytes=%zu dimensions=%ux%u format=%u tev_stages=%u; no payload output\n",
                        image.size, packet.texture.width, packet.texture.height, packet.texture.format, packet.tevCount);
        }
        assert(gx::unregisterTexture(handle));
    }
    assert(kernelStats().free == before.free && kernelStats().healthy);
}
#endif
}
int main(int argc, char** argv) {
    static_assert(sizeof(ResTIMG) == 32);
    alignas(32) static std::array<unsigned char, 2 * 1024 * 1024> arena;
    assert(dusk::native::initializeKernel(arena.data(), arena.size()));
    pictureTests(GX_TF_IA8, true);
    pictureTests(GX_TF_I4, false);
#if defined(DUSK_TEST_PRIVATE_DISC)
    if (argc == 3 && !std::strcmp(argv[1], "--private-disc")) privatePicture(argv[2]);
    else assert(argc == 1);
#else
    (void)argv; assert(argc == 1);
#endif
}
