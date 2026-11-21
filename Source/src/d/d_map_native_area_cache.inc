#include "../../platforms/3ds/runtime/gx_state.hpp"
#include "../../platforms/3ds/runtime/gx_stream.hpp"
#include "../../platforms/3ds/runtime/gx_worker_sync.hpp"
#include "../../platforms/3ds/runtime/gx_framebuffer.hpp"
#include "../../platforms/3ds/runtime/gx_copy_images.hpp"
#include "../../platforms/3ds/gx_command_draw.hpp"
#include "JSystem/JUtility/JUTVideo.h"
#include <algorithm>
#include <array>
#include <cstring>

// One native area map is painted at a time, on the game thread. Its retained
// bytes contain only the red index plane before doors, treasure and cursors.
// Included only by the original area map translation unit on the native target.
u32 dMapRoomPositionBytes(const dDrawPath_c::room_class* room);
namespace {
namespace native = dusk::native;
namespace mapgx = dusk::native::gx;
constexpr unsigned side = 216;
constexpr unsigned planeBytes = side * side;
struct RoomStyle {
    const dDrawPath_c::room_class* geometry{};
    u8 red{}, width0{}, width1{}, width2{};
    bool operator==(const RoomStyle&) const = default;
};
struct BaseInputs {
    RoomStyle room[2][64]{};
    std::array<u32, 32> switches{};  // exact pass/fail for at most 1024 conditional groups
    std::array<u8, 128> caps{};
    std::array<u32, 13> viewportProjection{};
    std::array<u32, 11> raster{};
    std::array<u8, 8> swapTables{};
    u8 texOffsets{}, swapSelectors{};
    bool bodyPasses{}, boxOffsetKnown{}, texOffsetsKnown{};
    std::array<u32, 12> viewMatrix{};
    u8 capWrapS{}, capWrapT{};
    u8 innerWidth{}, outerWidth{};
    s16 trimCount{};
    std::array<s16, 4> trimWidths{};
    int floor{}, display{};
    bool tall{};
    const dDrawPath_c::room_class* lastRoom{};
    const mapgx::CommandStream* stream{};
    native::FramebufferHandle target{};
    bool operator==(const BaseInputs&) const = default;
};
struct CachedPlane {
    BaseInputs saved;
    mapgx::TextureHandle handle{};
    GXTexObj sampler{};
    // GX texture registration borrows these bytes until the owner's teardown.
    alignas(32) u8 bti[32 + planeBytes]{};
    bool seen{}, valid{};
    u32 linePoint{};
    GXColor color0{}, color1{};
};
struct MapBase {
    const renderingAmap_c* owner{};
    CachedPlane variants[2]{};
    CachedPlane* capture{};
    const native::CommandDrawContext* context{};
    u64 rejected{}, simplified{}, startTrim{};
    bool restored{};
};
MapBase mapBase;
static_assert(sizeof(MapBase) < 100 * 1024);

void beginAreaMapDraw(renderingAmap_c& map) {
    mapBase.context = nullptr;
    if (map.mTexWidth != side || map.mTexHeight != side || !map.isDrawOutSideTrim())
        return;
    auto* stream = mapgx::commandStream();
    mapBase.context = stream ?
                          static_cast<const native::CommandDrawContext*>(
                              stream->nativeProcessor().contextForSink(native::drawGxCommand)) :
                          nullptr;
    if (mapBase.context) {
        native::syncGxWorker();
        mapBase.rejected = mapBase.context->rejected;
        mapBase.simplified = mapBase.context->simplifiedDraws;
    }
}

bool readBaseInputs(renderingAmap_c& map, BaseInputs& inputs) {
    const auto& extent = mapgx::currentTargetExtent;
    if (map.mTexWidth != side || map.mTexHeight != side || extent.pixelWidth != side ||
        extent.pixelHeight != side || extent.logicalWidth != side || extent.logicalHeight != side ||
        !map.isDrawOutSideTrim())
        return false;
    inputs.display = map.getDispType();
    if (inputs.display != 0 && inputs.display != 4)
        return false;
    auto* stream = mapgx::commandStream();
    if (!stream || stream->automaticSuEnabled() || !mapBase.context)
        return false;
    inputs.stream = stream;
    inputs.target = native::activeFramebuffer();
    if (!inputs.target || inputs.target == native::mainFramebuffer())
        return false;
    const auto& r = stream->registers();
    // preRenderingMap and preDrawPath set the viewport, projection, matrix 0, Z,
    // blend, fog, clip, vertex descriptor and the untextured TEV. Capture the
    // effective matrix and projection bit for bit, including view transitions.
    std::copy_n(r.xf.begin(), 12, inputs.viewMatrix.begin());
    // The projection has seven words and the viewport six. Neither uses another
    // position matrix. The cap selects identity texgen/postmatrix explicitly.
    std::copy_n(
        r.xf.begin() + 0x101a, inputs.viewportProjection.size(), inputs.viewportProjection.begin());
    // Words consulted by planRaster, including pixel storage, write masks,
    // compare location, scissor offset and Z textures. Refuse alternate
    // write/blend modes: decals later paint opaque indices over this plane.
    unsigned rasterIndex = 0;
    for (unsigned reg : {0u, 0x20u, 0x21u, 0x40u, 0x41u, 0x42u, 0x43u, 0x59u, 0xf1u, 0xf3u, 0xf5u})
    {
        if (!r.bpValid[reg] && reg != 0x59)
            return false;
        inputs.raster[rasterIndex++] = r.bp[reg];
    }
    if ((r.bp[0x41] & 0xb) != 8 || (r.bp[0x40] & 17) || (r.bp[0xf1] >> 20) || r.bp[0x10] ||
        !r.bpValid[0x10] || !r.xfValid[0x1005] || r.xf[0x1005] || (r.matrixIndex[0] & 63) ||
        !r.matrixIndexValid[0])
        return false;
    // The ordinary (nonindirect) PICA line/point expander reads bits 18/19
    // only. Its sampling scale comes from the bound texture's dimensions.
    // Automatic SU can stage additional writes, so that mode draws normally.
    inputs.texOffsets = (r.bp[0x30] >> 18) & 3;
    // Stage 0 sets its operands for each material and inherits its swap tables
    // and konst alpha selector.
    inputs.swapSelectors = r.bp[0xc1] & 15;
    for (unsigned i = 0xf6; i <= 0xfd; ++i) {
        if (!r.bpValid[i])
            return false;
        inputs.swapTables[i - 0xf6] = r.bp[i] & 15;
    }
    const unsigned sw = 0xf6 + 2 * ((r.bp[0xc1] >> 2) & 3);
    if (!r.bpValid[sw] || !r.bpValid[sw + 1] || (r.bp[sw] & 15) != 4 || (r.bp[sw + 1] & 15) != 14)
        return false;
    // Only decoration bodies read konst alpha. Their sole use of alpha is
    // GREATER 0; the copy stores red and blending/depth writes are disabled.
    const unsigned selector = (r.bp[0xf6] >> 9) & 31;
    if (selector == 0)
        inputs.bodyPasses = true;
    else if (selector >= 16) {
        const unsigned slot = 2 * (selector & 3), component = (selector - 16) / 4;
        if (!r.tevColorValid[8 + slot] || !r.tevColorValid[9 + slot])
            return false;
        const u32 ra = r.tevColor[1][slot], bg = r.tevColor[1][slot + 1];
        const u8 rgba[] = {u8(ra), u8(bg >> 12), u8(bg), u8(ra >> 12)};
        inputs.bodyPasses = rgba[component] != 0;
    } else
        return false;
    inputs.boxOffsetKnown = r.bpValid[0x59];
    inputs.texOffsetsKnown = r.bpValid[0x30];
    mapgx::Texture tex{};
    if (!mapgx::queryTextureObject(dMpath_n::m_texObjAgg.getTexObjPointer(6), tex) || !tex.handle ||
        tex.format != GX_TF_I4 || tex.width != 16 || tex.height != 16 || tex.minFilter != GX_NEAR ||
        tex.magFilter != GX_NEAR || tex.mipCount != 1 || tex.image.size() != inputs.caps.size())
        return false;
    std::copy(tex.image.begin(), tex.image.end(), inputs.caps.begin());
    inputs.capWrapS = tex.wrapS;
    inputs.capWrapT = tex.wrapT;
    inputs.floor = map.mRenderedFloor;
    inputs.tall = JUTVideo::getManager()->getRenderHeight() / 448.0f > 1.0f;
    const auto& style = *dMap_HIO_prm_res_dst_s::m_res;
    // Two widths for type 4, number of outer passes and the four pass widths.
    inputs.innerWidth = style.field_0x1a8;
    inputs.outerWidth = style.field_0x1a9;
    inputs.trimCount = style.field_0x1ae;
    if (inputs.trimCount < 1 || inputs.trimCount > 4)
        return false;
    inputs.trimWidths = {
        style.field_0x1b0, style.field_0x1b2, style.field_0x1b4, style.field_0x1b6};
    unsigned bits = 0;
    for (auto* room = map.getFirstRoomPointer(); room; room = map.getNextRoomPointer()) {
        auto& key = inputs.room[map.field_0x28][map.mRoomNo];
        inputs.lastRoom = room;
        key.geometry = room;
        // getColor(0) uniquely identifies all three immutable color tables.
        key.red = map.getColor(0)->r;
        key.width0 = map.getLineWidthSub(0);
        key.width1 = map.getLineWidthSub(1);
        key.width2 = map.getLineWidthSub(2);

        const dDrawPath_c::floor_class* floors = room->mpFloor;
        for (unsigned f = 0; floors && f < room->mFloorNum; ++f) {
            if (!map.isRenderingFloor(floors[f].mFloorNo))
                continue;
            const dDrawPath_c::group_class* groups = floors[f].mpGroup;
            for (unsigned g = 0; groups && g < floors[f].mGroupNum; ++g) {
                if (groups[g].mSwbit == 0xff)
                    continue;
                if (bits == inputs.switches.size() * 32)
                    return false;
                if (map.isSwitch(groups + g))
                    inputs.switches[bits / 32] |= 1u << (bits % 32);
                ++bits;
            }
        }
    }
    return inputs.lastRoom != nullptr;
}

GXColor savedColor(const mapgx::CommandRegisters& regs, unsigned gxRegister) {
    const u32 ra = regs.tevColor[0][2 * gxRegister], bg = regs.tevColor[0][2 * gxRegister + 1];
    return {u8(ra), u8(bg >> 12), u8(bg), u8(ra >> 12)};
}

void checkTrimWasDrawn() {
    if (mapBase.capture) {
        native::syncGxWorker();
        if (mapBase.context->submitted <= mapBase.startTrim)
            mapBase.capture = nullptr;
    }
}

bool captureAreaBase(CachedPlane& plane) {
    native::syncGxWorker();
    if (mapBase.context->rejected != mapBase.rejected ||
        mapBase.context->simplifiedDraws != mapBase.simplified)
        return false;
    const auto& regs = mapgx::commandStream()->registers();
    plane.linePoint = regs.bp[0x22];
    plane.color0 = savedColor(regs, GX_TEVREG0);
    plane.color1 = savedColor(regs, GX_TEVREG1);
    if (!native::copyAreaMapBase(side, plane.handle, {plane.bti + 32, planeBytes}))
        return false;
    if (!plane.handle) {
        plane.bti[0] = GX_TF_I8;
        plane.bti[3] = side;
        plane.bti[5] = side;
        plane.bti[24] = 1;
        plane.bti[31] = 32;
        if (!mapgx::registerTexture(plane.bti, plane.handle))
            return false;
    }
    GXInitTexObj(
        &plane.sampler, plane.bti + 32, side, side, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&plane.sampler, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    mapgx::Texture source;
    return mapgx::queryTextureObject(&plane.sampler, source) && source.handle == plane.handle;
}

bool tryAreaBase(renderingAmap_c& map) {
    mapBase.capture = nullptr;
    mapBase.restored = false;
    if (mapBase.owner && mapBase.owner != &map)
        return false;
    BaseInputs now{};
    if (!readBaseInputs(map, now))
        return false;
    mapBase.owner = &map;
    // The particle renderer toggles point texture offsets. Both masks occur
    // during normal play and paint different caps; retain them separately.
    auto& plane = mapBase.variants[now.texOffsets >> 1];
    if (!plane.seen || !(plane.saved == now)) {
        plane.saved = now;
        plane.seen = true;
        plane.valid = false;
        return false;
    }
    if (!plane.valid) {
        native::syncGxWorker();
        mapBase.startTrim = mapBase.context->submitted;
        mapBase.capture = &plane;
        return false;
    }
    // A retired texture may have left the bounded PICA cache. Bind before
    // skipping geometry so memory pressure can use the ordinary map path.
    mapgx::Texture source;
    native::syncGxWorker();
    if (!mapgx::queryTextureObject(&plane.sampler, source) || source.handle != plane.handle ||
        source.image.data() != plane.bti + 32)
    {
        plane.valid = false;
        return false;
    }
    if (!native::bindAreaMapBase(source))
        return false;
    const auto submitted = mapBase.context->submitted;
    const auto rejected = mapBase.context->rejected;
    const auto simplified = mapBase.context->simplifiedDraws;
    // A one-to-one nearest sample of R8. Nothing is remapped through the live
    // area-map palette until the HUD samples the completed indexed image.
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXLoadTexObj(&plane.sampler, GX_TEXMAP0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    const float x = map.field_0x8 * .5f, y = map.field_0xc * .5f;
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition2f32(-x, y);
    GXTexCoord2f32(0, 0);
    GXPosition2f32(x, y);
    GXTexCoord2f32(1, 0);
    GXPosition2f32(x, -y);
    GXTexCoord2f32(1, 1);
    GXPosition2f32(-x, -y);
    GXTexCoord2f32(0, 1);
    GXEnd();
    native::syncGxWorker();
    if (mapBase.context->submitted != submitted + 1 || mapBase.context->rejected != rejected ||
        mapBase.context->simplifiedDraws != simplified)
    {
        plane.valid = false;
        map.setTevSettingNonTextureDirectColor();
        map.drawBack();
        map.preDrawPath();
        return false;
    }
    GXLoadTexObj(dMpath_n::m_texObjAgg.getTexObjPointer(6), GX_TEXMAP0);
    map.setTevSettingIntensityTextureToCI();
    map.setTevSettingNonTextureDirectColor();
    map.preDrawPath();
    GXSetLineWidth(plane.linePoint & 255, GXTexOffset((plane.linePoint >> 16) & 7));
    GXSetPointSize((plane.linePoint >> 8) & 255, GXTexOffset((plane.linePoint >> 19) & 7));
    GXSetTevColor(GX_TEVREG0, plane.color0);
    GXSetTevColor(GX_TEVREG1, plane.color1);
    GXSetArray(GX_VA_POS, now.lastRoom->mpFloatData, dMapRoomPositionBytes(now.lastRoom), 8, false);
    map.m_outSideBlackLineCnt = now.trimCount;
    map.field_0x38 = 2;
    mapBase.restored = true;
    return true;
}
// beforeDrawPath calls drawPath once per trim width. The caller then issues
// one normal path pass (phase 2). Only that last pass can complete a plane.
void drawNativeAreaPaths(renderingAmap_c& map) {
    if (mapBase.restored) {
        mapBase.restored = false;
        return;
    }
    const bool finalPass = mapBase.capture && map.field_0x38 == 2;
    if (finalPass)
        checkTrimWasDrawn();
    map.dDrawPath_c::drawPath();
    if (finalPass && mapBase.capture) {
        mapBase.capture->valid = captureAreaBase(*mapBase.capture);
        mapBase.capture = nullptr;
    }
}
}  // namespace

void invalidateNativeAreaBase() {
    for (auto& plane : mapBase.variants)
        plane.seen = plane.valid = false;
    mapBase.capture = nullptr;
    mapBase.restored = false;
}
static void releaseAreaBase(renderingAmap_c& map) {
    if (mapBase.owner != &map)
        return;
    for (auto& plane : mapBase.variants) {
        if (!plane.handle)
            continue;
        GXDestroyTexObj(&plane.sampler);
        if (!native::releaseAreaMapBase(plane.handle))
            OSPanic(__FILE__, __LINE__, "Area map texture release");
        plane.handle = 0;
    }
    invalidateNativeAreaBase();
    mapBase.owner = nullptr;
}
