// Standalone, asset-free GPU probe. Build with Tools/3ds/build_color_lookup_probe.py.
// No host simulation: all compared pixels come from Citro3D render targets.
#include <3ds.h>
#include <citro3d.h>
#include "../native/color_lookup.hpp"
#include "../gx_material_pica.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern "C" {
extern const unsigned char color_lookup_shader[];
extern const unsigned int color_lookup_shader_size;
}

namespace {
namespace gx = dusk::native::gx;
constexpr unsigned Width = 240, Height = 400, Bytes = Width * Height * 4;
constexpr unsigned MaxVertices = 16 * 16 * 6;
constexpr u32 Clear = 0x17354f6b; // In linear RGBA8 memory: A B G R.
constexpr u8 ClearBytes[]{0x6b, 0x4f, 0x35, 0x17};
constexpr u32 Transfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
constexpr u32 LcdTransfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
using Color = std::array<float, 4>;
struct Vertex {
    float position[4], color[4], uv[3][2], q;
};
static_assert(sizeof(Vertex) == 15 * sizeof(float));

bool directory(const char* path) { return mkdir(path, 0777) == 0 || errno == EEXIST; }

struct Probe {
    char folder[192]{};
    const char* error = "initialization not completed";
    bool initialized = false, programInitialized = false;
    unsigned textureCount = 0, completed = 0;
    DVLB_s* shader = nullptr;
    shaderProgram_s program{};
    C3D_Tex textures[3]{}; // Original texture, RG lookup, BA lookup.
    C3D_RenderTarget* targets[4]{}; // Reference, lookup, ordinary before, ordinary after.
    Vertex* vertices = nullptr; // Immutable A, B, and deliberately wrong vertex-product banks.
    u8* pixels = nullptr; // Four synchronized, linear RGBA8 readbacks.
    FILE* metrics = nullptr;
    unsigned count = 0;
    gx::PicaMaterialPlan primary{}, textureOnly{}, ordinary{};

    ~Probe() {
        if (metrics) fclose(metrics);
        if (initialized) {
            // Unlinked RenderTargetDelete waits/clears the GX queue. Delete targets
            // BEFORE their textures/VBOs; FrameSync alone is only a VBlank wait.
            for (auto* target : targets) if (target) C3D_RenderTargetDelete(target);
            for (unsigned i = 0; i < textureCount; ++i) C3D_TexDelete(&textures[i]);
            if (vertices) linearFree(vertices);
            if (pixels) linearFree(pixels);
            if (programInitialized) shaderProgramFree(&program);
            if (shader) DVLB_Free(shader);
            C3D_Fini();
        }
    }

    bool save(const char* name, const void* data, size_t size) {
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", folder, name);
        FILE* file = fopen(path, "wb");
        if (!file) { error = "opening capture file failed"; return false; }
        const bool written = fwrite(data, 1, size, file) == size;
        const bool closed = fclose(file) == 0;
        if (!written || !closed) error = "writing/closing capture file failed";
        return written && closed;
    }

    bool init() {
        if (!directory("sdmc:/3ds") || !directory("sdmc:/3ds/dusklight-color-lookup")) {
            error = "creating SD capture root failed"; return false;
        }
        snprintf(folder, sizeof(folder), "sdmc:/3ds/dusklight-color-lookup/run-%016llx",
                 static_cast<unsigned long long>(svcGetSystemTick()));
        // Refuse collisions instead of overwriting evidence from another run.
        if (mkdir(folder, 0777) != 0) { folder[0] = 0; error = "creating isolated run directory failed"; return false; }
        const char pending[] = "status=running\nNo correctness conclusion until status=pass or status=fail.\n";
        if (!save("receipt.txt", pending, sizeof(pending) - 1)) return false;
        if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) { error = "C3D_Init failed"; return false; }
        initialized = true;
        shader = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(color_lookup_shader)), color_lookup_shader_size);
        if (!shader || !shader->numDVLE) { error = "shader parse failed"; return false; }
        if (R_FAILED(shaderProgramInit(&program))) { error = "shader program init failed"; return false; }
        programInitialized = true;
        if (R_FAILED(shaderProgramSetVsh(&program, &shader->DVLE[0]))) { error = "vertex shader setup failed"; return false; }
        C3D_BindProgram(&program);
        const int transform = shaderInstanceGetUniformLocation(program.vertexShader, "transform");
        if (transform < 0) { error = "production shader transform uniform missing"; return false; }
        for (int row = 0; row < 4; ++row)
            C3D_FVUnifSet(GPU_VERTEX_SHADER, transform + row, row == 0, row == 1, row == 2, row == 3);
        for (auto& target : targets) {
            target = C3D_RenderTargetCreate(Width, Height, GPU_RB_RGBA8, -1);
            if (!target) { error = "render target allocation failed"; return false; }
        }
        vertices = static_cast<Vertex*>(linearAlloc(3 * MaxVertices * sizeof(Vertex)));
        pixels = static_cast<u8*>(linearAlloc(4 * Bytes));
        if (!vertices || !pixels) { error = "linear allocation failed"; return false; }
        // Defined cache contents before handing readback storage to GX; invalidate after each transfer.
        memset(pixels, 0, 4 * Bytes);
        if (R_FAILED(GSPGPU_FlushDataCache(pixels, 4 * Bytes))) { error = "readback cache flush failed"; return false; }
        auto* attr = C3D_GetAttrInfo();
        AttrInfo_Init(attr);
        for (unsigned i = 0; i < 6; ++i) AttrInfo_AddLoader(attr, i, GPU_FLOAT, i < 2 ? 4 : i == 5 ? 1 : 2);
        auto* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        if (BufInfo_Add(buf, vertices, sizeof(Vertex), 6, 0x543210) < 0) { error = "VBO setup failed"; return false; }
        C3D_CullFace(GPU_CULL_NONE);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 255, 255);
        C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, Width, Height);
        C3D_FragOpMode(GPU_FRAGOPMODE_GL);
        replaceBlend();
        for (unsigned i = 0; i < 3; ++i) {
            if (!C3D_TexInit(&textures[i], 8, 8, GPU_RGBA8)) { error = "texture allocation failed"; return false; }
            ++textureCount;
            C3D_TexSetFilter(&textures[i], GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&textures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            if (i) {
                const auto image = dusk::pica::colorLookupImage(i - 1);
                memcpy(textures[i].data, image.data(), image.size());
                char name[40]; snprintf(name, sizeof(name), "lookup%u-native.abgr8", i - 1);
                if (!save(name, image.data(), image.size())) return false;
                const auto bti = dusk::pica::colorLookupBti(i - 1);
                snprintf(name, sizeof(name), "lookup%u.bti", i - 1);
                if (!save(name, bti.data(), bti.size())) return false;
            } else {
                auto* data = static_cast<u8*>(textures[i].data);
                for (unsigned at = 0; at < 256; at += 4) {
                    data[at] = 199; data[at + 1] = 227; data[at + 2] = 211; data[at + 3] = 173;
                }
            }
            C3D_TexFlush(&textures[i]);
        }
        primary.stageCount = 1;
        primary.stages[0].rgb.inputs[0] = {gx::MaterialSource::primary, gx::MaterialOperand::color, false};
        primary.stages[0].alpha.inputs[0] = {gx::MaterialSource::primary, gx::MaterialOperand::alpha, false};
        textureOnly = primary;
        textureOnly.stages[0].rgb.inputs[0].source = textureOnly.stages[0].alpha.inputs[0].source = gx::MaterialSource::texture0;
        // Ordinary draw deliberately reads the initial buffer at stage2: leaked stage0
        // update bits from the carrier would replace this constant with primary RGBA.
        ordinary = primary; ordinary.stageCount = 4; ordinary.bufferConstant = {59, 113, 197, 173};
        ordinary.stages[1].rgb.inputs[0] = {gx::MaterialSource::previous, gx::MaterialOperand::color, false};
        ordinary.stages[1].alpha.inputs[0] = {gx::MaterialSource::previous, gx::MaterialOperand::alpha, false};
        auto& last = ordinary.stages[2];
        last.rgb.function = last.alpha.function = gx::MaterialFunction::modulate;
        last.rgb.inputs[0] = {gx::MaterialSource::primary, gx::MaterialOperand::color, false};
        last.rgb.inputs[1] = {gx::MaterialSource::buffer, gx::MaterialOperand::color, false};
        last.alpha.inputs[0] = {gx::MaterialSource::primary, gx::MaterialOperand::alpha, false};
        last.alpha.inputs[1] = {gx::MaterialSource::buffer, gx::MaterialOperand::alpha, false};
        // Also consume the original texture so a stale lookup binding is observable.
        auto& textured = ordinary.stages[3];
        textured.rgb.function = textured.alpha.function = gx::MaterialFunction::modulate;
        textured.rgb.inputs[0] = {gx::MaterialSource::previous, gx::MaterialOperand::color, false};
        textured.rgb.inputs[1] = {gx::MaterialSource::texture0, gx::MaterialOperand::color, false};
        textured.alpha.inputs[0] = {gx::MaterialSource::previous, gx::MaterialOperand::alpha, false};
        textured.alpha.inputs[1] = {gx::MaterialSource::texture0, gx::MaterialOperand::alpha, false};
        char path[320]; snprintf(path, sizeof(path), "%s/metrics.csv", folder);
        metrics = fopen(path, "w");
        if (!metrics) { error = "opening metrics failed"; return false; }
        fputs("case,comparison,tolerance,drawn_pixels,different_pixels,over_tolerance,max_r,max_g,max_b,max_a,span_r,span_g,span_b,span_a,pass\n", metrics);
        return true;
    }

    static void replaceBlend() { C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); }
    static void productBlend() { C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_DST_ALPHA, GPU_ZERO); }

    void emit(float x, float y, float w, const Color& a, const Color& b, unsigned base) {
        for (unsigned bank = 0; bank < 3; ++bank) {
            auto& v = vertices[bank * MaxVertices + count];
            v = {};
            v.position[0] = (2 * x / Width - 1) * w;
            v.position[1] = (2 * y / Height - 1) * w;
            v.position[2] = -.5f * w; v.position[3] = w;
            for (unsigned c = 0; c < 4; ++c) v.color[c] = bank == 0 ? a[c] : bank == 1 ? b[c] : a[c] * b[c];
            for (auto& uv : v.uv) uv[0] = uv[1] = .5f;
            const auto rg = dusk::pica::colorLookupUv(b[0], b[1]);
            const auto ba = dusk::pica::colorLookupUv(b[2], b[3]);
            for (unsigned c = 0; c < 2; ++c) { v.uv[base][c] = rg[c]; v.uv[base + 1][c] = ba[c]; }
            v.q = 1;
        }
        ++count;
    }

    bool geometry(unsigned shape, unsigned base) {
        count = 0;
        if (shape < 2) {
            constexpr Color a[]{{1, .04f, .85f, .2f}, {.1f, .95f, .15f, 1}, {.7f, .25f, 1, .05f}};
            constexpr Color b[]{{0, 1, .25f, .75f}, {1, .125f, .875f, 0}, {.375f, .625f, 0, 1}};
            constexpr float xy[][2]{{12, 24}, {228, 64}, {82, 376}}, w[]{1, 4, .5f};
            for (unsigned i = 0; i < 3; ++i) emit(xy[i][0], xy[i][1], shape ? w[i] : 1, a[i], b[i], base);
        } else {
            // Each channel visits all 256 bytes, including 0,1,254,255, in a
            // different order; flat cells test endpoints without edge interpolation.
            constexpr unsigned corners[][2]{{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
            for (unsigned value = 0; value < 256; ++value) {
                const Color b{value / 255.f, (255 - value) / 255.f, ((73 * value + 19) & 255) / 255.f, ((151 * value + 7) & 255) / 255.f};
                const Color a{((97 * value + 11) & 255) / 255.f, ((31 * value + 67) & 255) / 255.f, (255 - value) / 255.f, value / 255.f};
                for (const auto& corner : corners)
                    emit(8 + 14 * (value % 16 + corner[0]), 16 + 23 * (value / 16 + corner[1]), 1, a, b, base);
            }
        }
        if (R_FAILED(GSPGPU_FlushDataCache(vertices, 3 * MaxVertices * sizeof(Vertex)))) {
            error = "vertex cache flush failed"; return false;
        }
        return true;
    }

    bool plan(unsigned operation, gx::PicaMaterialPlan& result) {
        gx::MaterialState state{};
        state.channelCount = 2; state.stageCount = operation ? operation + 1 : 1;
        state.texgenCount = operation == 2 ? 1 : 0;
        state.swapValid = 1; state.swaps[0] = {0, 1, 2, 3};
        for (auto& s : state.stages) s.colorOp.clamp = s.alphaOp.clamp = 1;
        auto& first = state.stages[0];
        first.raster = operation ? 0 : 1;
        first.color = {15, 15, 15, 10}; first.alpha = {7, 7, 7, 5};
        if (operation) {
            auto& second = state.stages[1]; second.raster = 1;
            second.color = {15, 0, 10, 15}; second.alpha = {7, 0, 5, 7};
        }
        if (operation == 2) {
            auto& third = state.stages[2]; third.textureEnabled = true;
            third.color = {15, 0, 8, 15}; third.alpha = {7, 0, 4, 7};
        }
        gx::MaterialPlanError why{};
        if (!gx::planRasterLookupMaterial(state, result, &why)) {
            error = gx::materialPlanErrorText(why); return false;
        }
        if (result.textureCount != (operation == 2 ? 3 : 2) || result.bufferUpdateRgb != 1 || result.bufferUpdateAlpha != 1) {
            error = "lookup plan did not provide expected RGBA buffer/texture contract"; return false;
        }
        return true;
    }

    bool begin(unsigned target) {
        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) { error = "frame begin failed"; return false; }
        if (!on(target)) { C3D_FrameEnd(0); return false; }
        return true;
    }
    bool on(unsigned target) {
        C3D_RenderTargetClear(targets[target], C3D_CLEAR_COLOR, Clear, 0);
        if (!C3D_FrameDrawOn(targets[target])) { error = "frame target selection failed"; return false; }
        C3D_SetViewport(0, 0, Width, Height);
        return true;
    }
    void draw(unsigned bank, const gx::PicaMaterialPlan& material) {
        dusk::native::applyPicaMaterial(material);
        C3D_DrawArrays(GPU_TRIANGLES, bank * MaxVertices, count);
    }
    void ordinaryDraw() {
        replaceBlend();
        C3D_TexBind(0, &textures[0]); C3D_TexBind(1, &textures[0]); C3D_TexBind(2, &textures[0]);
        // Exercise the cached production applier on the following ordinary draw.
        dusk::native::applyPreparedPicaMaterial(dusk::native::preparePicaMaterial(ordinary));
        C3D_DrawArrays(GPU_TRIANGLES, 0, count);
    }
    void ordinaryBaselineDraw() {
        // Independent SDK baseline: no production applier or previous-buffer reads.
        replaceBlend();
        for (unsigned unit = 0; unit < 3; ++unit) C3D_TexBind(unit, &textures[0]);
        for (unsigned stage = 0; stage < 6; ++stage) C3D_TexEnvInit(C3D_GetTexEnv(stage));
        C3D_TexEnvBufUpdate(C3D_Both, 0);
        auto* first = C3D_GetTexEnv(0);
        C3D_TexEnvSrc(first, C3D_Both, GPU_PRIMARY_COLOR, GPU_CONSTANT);
        C3D_TexEnvFunc(first, C3D_Both, GPU_MODULATE);
        C3D_TexEnvColor(first, 59u | (113u << 8) | (197u << 16) | (173u << 24));
        auto* last = C3D_GetTexEnv(1);
        C3D_TexEnvSrc(last, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE0);
        C3D_TexEnvFunc(last, C3D_Both, GPU_MODULATE);
        C3D_DrawArrays(GPU_TRIANGLES, 0, count);
    }
    void end() { C3D_FrameEnd(0); }

    bool readback(unsigned target, unsigned slot, const u8* corner = ClearBytes) {
        const auto& fb = targets[target]->frameBuf;
        if (fb.width != Width || fb.height != Height || fb.colorFmt != GPU_RB_RGBA8 || fb.block32) {
            error = "unsupported SDK framebuffer layout (expected tiled RGBA8, block32=false)"; return false;
        }
        // OUTSIDE a frame, SyncDisplayTransfer waits/clears the render queue,
        // submits the transfer and waits for PPF. FrameSync is NOT a GPU fence.
        // Transfer untile/converts; never CPU-index the render target's tiled VRAM.
        C3D_SyncDisplayTransfer(static_cast<u32*>(fb.colorBuf), GX_BUFFER_DIM(fb.width, fb.height),
                                reinterpret_cast<u32*>(pixels + slot * Bytes), GX_BUFFER_DIM(Width, Height), Transfer);
        if (R_FAILED(GSPGPU_InvalidateDataCache(pixels + slot * Bytes, Bytes))) { error = "readback cache invalidate failed"; return false; }
        // Acquire/validate on the CPU BEFORE a filesystem IPC can read this buffer.
        // Azahar's page-sized linear downloads round the source down to a row but
        // omit the leading-byte offset when encoding it. A CPU scalar read flushes
        // the complete dirty surface instead; a flat calibration cannot detect this.
        for (unsigned c = 0; c < 4; ++c)
            if (std::abs(int(pixels[slot * Bytes + c]) - int(corner[c])) > 1) {
                error = "readback corner differs from known geometry/color"; return false;
            }
        return true;
    }

    bool capture(unsigned target, unsigned slot, const char* prefix, const u8* corner = ClearBytes) {
        if (!readback(target, slot, corner)) return false;
        char name[96]; snprintf(name, sizeof(name), "%s.abgr8", prefix);
        if (!save(name, pixels + slot * Bytes, Bytes)) return false;
        u16 width = 0, height = 0;
        u8* lcd = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
        if (!lcd || width != Width || height != Height || gfxGetScreenFormat(GFX_TOP) != GSP_BGR8_OES) {
            error = "unexpected top LCD format/dimensions"; return false;
        }
        if (R_FAILED(GSPGPU_FlushDataCache(lcd, width * height * 3))) { error = "LCD cache flush failed"; return false; }
        C3D_SyncDisplayTransfer(static_cast<u32*>(targets[target]->frameBuf.colorBuf), GX_BUFFER_DIM(Width, Height),
                                reinterpret_cast<u32*>(lcd), GX_BUFFER_DIM(width, height), LcdTransfer);
        if (R_FAILED(GSPGPU_InvalidateDataCache(lcd, width * height * 3))) { error = "LCD cache invalidate failed"; return false; }
        // Likewise acquire the LCD surface before its page-wise SD write, and check
        // the independent RGB8 transfer against the already acquired RGBA8 corner.
        for (unsigned c = 0; c < 3; ++c)
            if (lcd[c] != pixels[slot * Bytes + 1 + c]) {
                error = "LCD RGB transfer disagrees with RGBA readback corner"; return false;
            }
        gfxScreenSwapBuffers(GFX_TOP, false); gspWaitForVBlank();
        snprintf(name, sizeof(name), "%s-lcd.bgr8", prefix);
        return save(name, lcd, width * height * 3);
    }

    bool compare(const char* name, const char* kind, unsigned left, unsigned right, unsigned tolerance, bool negative = false) {
        const u8* a = pixels + left * Bytes; const u8* b = pixels + right * Bytes;
        unsigned drawn = 0, different = 0, over = 0;
        unsigned maximum[4]{}, low[4]{255, 255, 255, 255}, high[4]{};
        for (unsigned at = 0; at < Bytes; at += 4) {
            const bool covered = memcmp(a + at, ClearBytes, 4) != 0;
            drawn += covered;
            unsigned delta = 0;
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned byte = 3 - c;
                const unsigned d = std::abs(int(a[at + byte]) - int(b[at + byte]));
                maximum[c] = std::max(maximum[c], d); delta = std::max(delta, d);
                if (covered) { low[c] = std::min(low[c], unsigned(a[at + byte])); high[c] = std::max(high[c], unsigned(a[at + byte])); }
            }
            different += delta != 0; over += delta > tolerance;
        }
        bool dynamic = drawn > 10000;
        for (unsigned c = 0; c < 4; ++c) dynamic &= high[c] >= low[c] + (strncmp(kind, "restore", 7) == 0 ? 20 : 32);
        const bool okay = dynamic && (negative ? over > 1000 : over == 0);
        if (fprintf(metrics, "%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", name, kind, tolerance, drawn, different, over,
                    maximum[0], maximum[1], maximum[2], maximum[3], high[0] - low[0], high[1] - low[1], high[2] - low[2], high[3] - low[3], okay) < 0 || fflush(metrics) != 0) {
            error = "writing metrics failed"; return false;
        }
        printf("%s %s: max RGBA %u %u %u %u; >%u: %u; %s\n", name, kind,
               maximum[0], maximum[1], maximum[2], maximum[3], tolerance, over, okay ? "pass" : "FAIL");
        return okay;
    }

    bool calibration() {
        count = 0;
        constexpr Color color{17 / 255.f, 83 / 255.f, 149 / 255.f, 227 / 255.f};
        constexpr unsigned corners[][2]{{0, 0}, {Width, 0}, {Width, Height}, {0, 0}, {Width, Height}, {0, Height}};
        for (const auto& xy : corners) emit(xy[0], xy[1], 1, color, color, 0);
        if (R_FAILED(GSPGPU_FlushDataCache(vertices, 3 * MaxVertices * sizeof(Vertex)))) { error = "calibration VBO flush failed"; return false; }
        if (!begin(0)) return false;
        replaceBlend(); draw(0, primary); end();
        constexpr u8 expected[]{227, 149, 83, 17};
        if (!capture(0, 0, "calibration", expected)) return false;
        for (unsigned at = 0; at < Bytes; at += 4) for (unsigned c = 0; c < 4; ++c)
            if (std::abs(int(pixels[at + c]) - int(expected[c])) > 1) {
                error = "RGBA calibration failed: draw, channel order or readback invalid"; return false;
            }
        // A second, nonuniform calibration independently checks vertex positions,
        // interpolation, framebuffer orientation and row/tile layout. Raw row zero
        // is the top of this physical target, whereas emit() uses bottom-origin Y.
        if (!geometry(0, 0) || !begin(0)) return false;
        replaceBlend(); draw(1, primary); end();
        if (!capture(0, 0, "calibration-gradient")) return false;
        constexpr float area = 216.f * 352.f - 40.f * 70.f;
        const float margin[]{std::hypot(146.f, 312.f) / area,
                             std::hypot(70.f, 352.f) / area,
                             std::hypot(216.f, 40.f) / area};
        constexpr Color colors[]{{0, 1, .25f, .75f}, {1, .125f, .875f, 0}, {.375f, .625f, 0, 1}};
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const float dx = x + .5f - 12, dy = Height - y - .5f - 24;
            const float l1 = (dx * 352 - dy * 70) / area;
            const float l2 = (216 * dy - 40 * dx) / area;
            const float weights[]{1 - l1 - l2, l1, l2};
            bool inside = true, outside = false;
            for (unsigned i = 0; i < 3; ++i) {
                inside &= weights[i] > margin[i];
                outside |= weights[i] < -margin[i];
            }
            // Leave only a one-pixel geometric boundary to rasterization rules.
            if (!inside && !outside) continue;
            const unsigned at = 4 * (y * Width + x);
            for (unsigned c = 0; c < 4; ++c) {
                const float wanted = inside ? 255 * (weights[0] * colors[0][c] +
                    weights[1] * colors[1][c] + weights[2] * colors[2][c]) : ClearBytes[3 - c];
                if (std::abs(pixels[at + 3 - c] - wanted) > 1) {
                    error = "gradient calibration failed: geometry/interpolation/orientation/readback invalid"; return false;
                }
            }
        }
        return true;
    }

    bool run() {
        if (!calibration()) return false;
        bool accepted = true;
        constexpr const char* shapes[]{"affine", "perspective", "bytes"};
        constexpr const char* operations[]{"identity", "product", "textured-product"};
        for (unsigned shape = 0; shape < 3; ++shape) for (unsigned operation = 0; operation < 3; ++operation) {
            if (!aptMainLoop()) { error = "interrupted before all cases completed"; return false; }
            gx::PicaMaterialPlan lookup;
            if (!plan(operation, lookup) || !geometry(shape, operation == 2 ? 1 : 0)) return false;
            char name[72], prefix[128]; snprintf(name, sizeof(name), "%s-%s", shapes[shape], operations[operation]);
            if (!begin(0)) return false;
            C3D_TexBind(0, &textures[0]); C3D_TexBind(1, &textures[0]); C3D_TexBind(2, &textures[0]);
            replaceBlend(); draw(1, primary); // B independently interpolated through ordinary primary RGBA.
            if (operation) { productBlend(); draw(0, primary); } // Interpolate A, THEN multiply in framebuffer.
            if (operation == 2) draw(0, textureOnly); // Same final texture multiplication/order as candidate.
            if (!on(2)) { end(); return false; }
            ordinaryDraw(); end();
            snprintf(prefix, sizeof(prefix), "%s-reference", name);
            if (!capture(0, 0, prefix) || !readback(2, 2)) return false;
            snprintf(prefix, sizeof(prefix), "%s-ordinary-before.abgr8", name);
            if (!save(prefix, pixels + 2 * Bytes, Bytes)) return false;
            if (!begin(1)) return false;
            replaceBlend();
            const unsigned base = operation == 2 ? 1 : 0;
            if (base) C3D_TexBind(0, &textures[0]);
            C3D_TexBind(base, &textures[1]); C3D_TexBind(base + 1, &textures[2]);
            if (!base) C3D_TexBind(2, &textures[0]);
            draw(0, lookup);
            // No intervening reset/frame: the next draw applies ONLY normal production state.
            if (!on(3)) { end(); return false; }
            ordinaryDraw(); end();
            snprintf(prefix, sizeof(prefix), "%s-lookup", name);
            if (!capture(1, 1, prefix) || !readback(3, 3)) return false;
            snprintf(prefix, sizeof(prefix), "%s-ordinary-after.abgr8", name);
            if (!save(prefix, pixels + 3 * Bytes, Bytes)) return false;
            accepted &= compare(name, "lookup", 0, 1, operation ? 3 : 2);
            accepted &= compare(name, "restore", 2, 3, 0);
            if (shape < 2 && operation) {
                if (!begin(2)) return false;
                replaceBlend(); C3D_TexBind(0, &textures[0]); draw(2, primary);
                if (operation == 2) { productBlend(); draw(0, textureOnly); }
                end();
                snprintf(prefix, sizeof(prefix), "%s-wrong-vertex-product.abgr8", name);
                if (!readback(2, 2) || !save(prefix, pixels + 2 * Bytes, Bytes)) return false;
                // Must visibly reject interpolating a product formed at the vertices.
                accepted &= compare(name, "negative-vertex-product", 0, 2, 8, true);
            }
            // The production-before/after equality above is necessary but not
            // sufficient: both could share a broken ordinary-material application.
            if (!begin(2)) return false;
            ordinaryBaselineDraw(); end();
            if (!readback(2, 2)) return false;
            snprintf(prefix, sizeof(prefix), "%s-ordinary-baseline.abgr8", name);
            if (!save(prefix, pixels + 2 * Bytes, Bytes)) return false;
            accepted &= compare(name, "restore-baseline", 2, 3, 0);
            ++completed;
            if (ferror(metrics)) { error = "metrics I/O failed"; return false; }
        }
        if (fclose(metrics) != 0) { metrics = nullptr; error = "closing metrics failed"; return false; }
        metrics = nullptr;
        error = accepted ? "bounded sampled GPU comparisons passed" : "one or more GPU comparison criteria failed; inspect metrics.csv";
        return accepted;
    }

    bool receipt(bool passed) {
        if (!folder[0]) return false;
        char text[4096];
        const int length = snprintf(text, sizeof(text),
            "status=%s\nreason=%s\ncases_completed=%u\ncases_required=9\n"
            "physical_width=240\nphysical_height=400\nraw=linear rows; A,B,G,R bytes; GX flip_vert=0; 384000 bytes per .abgr8\n"
            "lcd=actual top framebuffer; B,G,R bytes; physical 240x400; 288000 bytes per -lcd.bgr8\n"
            "display=top double buffering disabled; direct GX transfer; swap then VBlank\n"
            "synchronization=outside-frame SyncDisplayTransfer drains GX queue and waits PPF; invalidate then CPU-validate known corner before filesystem IPC; unlinked RenderTargetDelete waits queue before freeing resources; FrameSync is only VBlank pacing\n"
            "reference=ordinary primary B; product adds separately rasterized A with DST_COLOR/DST_ALPHA blending; textured-product then multiplies RGBA(173,211,227,199)\n"
            "candidate=production gx_color.v.pica, planRasterLookupMaterial, applyPicaMaterial, colorLookupImage/Uv; linear/clamped 8x8 RG and BA textures; stage0 delayed RGBA buffer\n"
            "geometry=affine triangle W(1,1,1); perspective triangle W(1,4,0.5); 16x16 constant byte atlas\n"
            "triangle_pixel_xy=(12,24),(228,64),(82,376); clip_z=-0.5*W; clip_xy=(2*pixel/dimension-1)*W\n"
            "triangle_A=(1,.04,.85,.2),(.1,.95,.15,1),(.7,.25,1,.05)\n"
            "triangle_B=(0,1,.25,.75),(1,.125,.875,0),(.375,.625,0,1)\n"
            "byte_atlas=cell origin(8+14*(byte%%16),16+23*(byte/16)); B=(byte,255-byte,(73*byte+19)%%256,(151*byte+7)%%256)/255\n"
            "criteria=full-image max channel error <=2 identity, <=3 products (separate blend/TEV rounding); >10000 drawn pixels and >=32 range in every reference RGBA channel\n"
            "restore=ordinary initial RGBA buffer consumer at stage2 then original texture0 at stage3, applied via prepare/applyPreparedPicaMaterial immediately after lookup in same frame; exact before/after equality and independent direct-SDK primary*constant*texture baseline, >=20 channel range\n"
            "negative_control=affine/perspective per-vertex products must differ from fragment products by >8 in >1000 pixels\n"
            "calibration=full-screen primary RGBA(17,83,149,227), every channel within 1; affine B gradient independently checked against pixel-center barycentrics and exact clear outside a one-pixel geometric boundary; LCD corner equals RGBA RGB bytes\n"
            "limits=sampled bounded-error equivalence, not bit-exact equivalence or game integration; framebuffer-blend reference has an extra UNORM8 rounding; no automatic hardware/emulator identification; BTI bytes saved but native GPU upload is colorLookupImage, not the game BTI decoder\n",
            passed ? "pass" : "fail", error, completed);
        return length > 0 && size_t(length) < sizeof(text) && save("receipt.txt", text, size_t(length));
    }
};
} // namespace

int main() {
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSet3D(false);
    consoleInit(GFX_BOTTOM, nullptr);
    printf("Dusklight RGBA lookup GPU probe\nAsset-free; writing isolated SD evidence.\n");
    int result = 1;
    {
        Probe probe;
        const bool passed = probe.init() && probe.run();
        const bool saved = probe.receipt(passed);
        printf("\n%s\n%s\n%s\n", passed && saved ? "PASS (bounded sampled comparisons)" : "FAIL", probe.error, probe.folder);
        if (!saved) printf("ERROR: correctness receipt was not saved.\n");
        result = passed && saved ? 0 : 1;
    }
    printf("START to exit; automatic exit after 600 VBlanks.\n");
    for (unsigned frame = 0; frame < 600 && aptMainLoop(); ++frame) {
        hidScanInput(); if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    return result;
}
