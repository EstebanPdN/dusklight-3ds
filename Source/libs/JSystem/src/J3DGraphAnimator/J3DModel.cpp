#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DShapeTable.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/JKernel/JKRHeap.h"

#if TARGET_PC
#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/material.h"
#include "dusk/interp/vertex.h"
#endif

#define J3D_ASSERTMSG(LINE, COND, MSG) JUT_ASSERT_MSG(LINE, (COND) != 0, MSG)
#define J3D_WARN1(LINE, MSG, ARG1) JUT_WARN(LINE, MSG, ARG1)

#if defined(DUSK_PICA_GAME_DRAW)
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/J3DGraphBase/J3DTransform.h"
#include <cmath>

// This exports declarations, not the GXLightObj/command image produced by load().
extern bool dKy_getNativeGlobalLight(unsigned slot, J3DLightInfo& out);

namespace {
u32 nativeColor(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
dusk::pica::Row nativeColorRow(const J3DGXColor& c) {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}
bool nativeFog(J3DMaterial& material, dusk::pica::DrawUniforms& out) {
    out.fog = {};
    const J3DFog* fog = material.getFog();
    // Fogless PE blocks do not publish a fog declaration (nor disable inherited
    // GX fog). Leave it unprovided: a compiled fog consumer must reject it.
    if (!fog) return true;
    const unsigned function = fog->mType & 7;
    if ((fog->mType & ~15u) || fog->mAdjEnable > 1) return false;
    out.fog.provided = true;
    out.fog.enabled = function != 0;
    out.fog.orthographic = (fog->mType & 8) != 0;
    switch (function) {
    case 0: case 2: out.fog.function = dusk::pica::FogFunction::linear; break;
    case 4: out.fog.function = dusk::pica::FogFunction::exp; break;
    case 5: out.fog.function = dusk::pica::FogFunction::expSquared; break;
    case 6: out.fog.function = dusk::pica::FogFunction::reverseExp; break;
    case 7: out.fog.function = dusk::pica::FogFunction::reverseExpSquared; break;
    default: return false;
    }
    out.fog.color = nativeColor(fog->mColor.r, fog->mColor.g, fog->mColor.b, fog->mColor.a);
    out.material.colors[7] = out.fog.color;
    out.material.suppliedMask |= 0x80;
    out.fog.rangeEnabled = fog->mAdjEnable != 0;
    if (out.fog.rangeEnabled) {
        for (unsigned i = 0; i < 10; ++i) {
            const unsigned coefficient = fog->mFogAdjTable.r[i];
            if (coefficient > 4095) return false;
            out.fog.range[i] = coefficient / 256.0f;
        }
    }
    if (!out.fog.enabled) return true;
    const float start = fog->mStartZ, end = fog->mEndZ;
    const float near = fog->mNearZ, far = fog->mFarZ;
    for (float value : {start, end, near, far}) if (!std::isfinite(value)) return false;
    if (far < 0 || far < near) return false;
    // Original GXSetFog/J3DGDSetFog high-level coefficients, before register
    // quantization. Keep the declared degenerate case, not an invented distance.
    if (far == near || end == start) {
        out.fog.a = out.fog.c = 0;
        out.fog.b = .5f;
    } else if (out.fog.orthographic) {
        out.fog.a = (far - near) / (end - start);
        out.fog.c = (start - near) / (end - start);
    } else {
        out.fog.a = (far * near) / ((far - near) * (end - start));
        out.fog.b = far / (far - near);
        out.fog.c = start / (end - start);
    }
    return std::isfinite(out.fog.a) && std::isfinite(out.fog.b) && std::isfinite(out.fog.c);
}
bool nativeSampler(const ResTIMG& image, dusk::pica::Sampler& out) {
    using namespace dusk::pica;
    if (image.wrapS > 2 || image.wrapT > 2 || image.minFilter > 5 || image.magFilter > 1 ||
        image.mipmapEnabled > 1 || image.doEdgeLOD || image.biasClamp || image.maxAnisotropy ||
        !image.mipmapCount || image.mipmapCount > 8) return false;
    out = {};
    out.wrapS = Wrap(image.wrapS); out.wrapT = Wrap(image.wrapT);
    out.magnify = Filter(image.magFilter); out.minify = Filter(image.minFilter & 1);
    out.mipmap = Filter(image.minFilter >= 4);
    if (image.minFilter >= 2) {
        if (image.minLOD < 0 || image.maxLOD < 0) return false;
        const unsigned first = image.minLOD > 80 ? 80 : image.minLOD;
        const unsigned last = image.maxLOD > 80 ? 80 : image.maxLOD;
        if (first % 8 || last % 8 || first > last || last / 8 >= image.mipmapCount) return false;
        out.firstLevel = first / 8; out.lastLevel = last / 8;
    }
    // Match J3D's hundredths -> float -> SDK signed 3.5 bias, transported
    // exactly as native signed 8.8. This is not clamping a TEV signed input.
    float bias = s16(image.LODBias) * (1.0f / 100.0f);
    if (bias < -4) bias = -4;
    if (bias >= 4) bias = 3.99f;
    out.lodBias = int(bias * 32) * 8;
    return true;
}
bool nativeBillboard(Mtx m, unsigned mode, bool unitScale, dusk::pica::Palette& out, unsigned slot) {
    if ((mode != 1 && mode != 2) || slot >= dusk::pica::paletteSize) return false;
    if (mode == 1) J3DCalcBBoardMtx(m);
    else {
        if (!(m[1][1] * m[1][1] + m[2][1] * m[2][1] > 0)) return false;
        J3DCalcYBBoardMtx(m);
    }
    Mtx inverse;
    if (mode == 2 && !unitScale && !MTXInverse(m, inverse)) return false;
    for (unsigned r = 0; r < 3; ++r) {
        if (mode == 1 && m[r][r] == 0) return false;
        const float reciprocal = mode == 1 ? 1 / m[r][r] : 0;
        for (unsigned c = 0; c < 4; ++c) {
            const float p = m[r][c];
            const float n = c == 3 ? 0 : mode == 1 ? (r == c ? reciprocal : 0) :
                            unitScale ? m[r][c] : inverse[c][r];
            if (!std::isfinite(p) || !std::isfinite(n)) return false;
            out.position[3*slot+r][c] = p;
            out.normal[3*slot+r][c] = n;
        }
    }
    return true;
}
bool nativeTexGen(J3DMaterial& material, J3DTexMtxObj* deferred, bool post,
                  unsigned generator, unsigned unit, const dusk::pica::UniformBinding& binding,
                  dusk::pica::DrawUniforms& out) {
    using namespace dusk::pica;
    if (unit >= 3 || generator >= material.getTexGenNum() || generator >= 8) return false;
    J3DTexCoord* coord = material.getTexCoord(generator);
    if (!coord || coord->getTexGenType() > GX_TG_MTX2x4) return false;
    const unsigned source = coord->getTexGenSrc(), matrix = coord->getTexGenMtx();
    const unsigned reg = coord->getTexMtxReg();
    const bool uv = source >= GX_TG_TEX0 && source <= GX_TG_TEX2;
    const bool normal = source == GX_TG_NRM;
    if (!uv && !normal && source != GX_TG_POS) return false;
    const unsigned attribute = uv ? GX_VA_TEX0 + source - GX_TG_TEX0 : normal ? GX_VA_NRM : GX_VA_POS;
    if (!(binding.sourceAttributes & (1u << attribute))) return false;
    unsigned index = generator;
    const bool identity = matrix == GX_IDENTITY;
    if (!identity) {
        if (matrix < GX_TEXMTX0 || matrix > GX_TEXMTX9 || (matrix - GX_TEXMTX0) % 3) return false;
        index = (matrix - GX_TEXMTX0) / 3;
        if (index >= 8) return false;
    }
    Mtx complete;
    MTXIdentity(complete);
    PositionSource space = normal ? PositionSource::objectNormal : PositionSource::object;
    if (!identity) {
        if (post) index = generator;
        J3DTexMtx* tex = material.getTexMtx(index);
        if (!tex) return false;
        if (deferred) {
            if (index >= deferred->getNumTexMtx() || !deferred->mpTexMtx) return false;
            MTXCopy(deferred->getMtx(index), complete);
        } else MTXCopy(tex->getMtx(), complete);
        const unsigned mode = tex->getTexMtxInfo().mInfo & 0x3f;
        if (mode > 11) return false;
        if (deferred) {
            // Exactly the matrix recipe in J3DDifferedTexMtx::loadExecute.
            // Modes1/6/7 concatenate the POSITION linear matrix, not N^-T.
            if (!post && (mode == 1 || mode == 6 || mode == 7)) {
                if (!normal) return false;
                space = PositionSource::posedLinearNormal;
            } else if (!post && (mode == 3 || mode == 9)) {
                if (source != GX_TG_POS) return false;
                space = PositionSource::posed;
            }
            if (mode == 5 || mode == 10 || mode == 11) {
                if (!normal) return false;
                if (!deferred->mpEffectMtx) return false;
                auto& info = tex->getTexMtxInfo();
                Mtx44 srt;
                const bool maya = (info.mInfo & 0x80) != 0;
                if (mode == 5) {
                    if (maya) J3DGetTextureMtxMayaOld(info.mSRT, srt);
                    else J3DGetTextureMtxOld(info.mSRT, info.mCenter, srt);
                } else {
                    // Deferred mode10 deliberately uses the NEW SRT helper;
                    // calcTexMtx's mode10 uses Old. Preserve that distinction.
                    if (maya) J3DGetTextureMtxMaya(info.mSRT, srt);
                    else J3DGetTextureMtx(info.mSRT, info.mCenter, srt);
                    Mtx bias = {{.5f,0,mode == 11 ? .5f : 0,mode == 10 ? .5f : 0},
                                {0,-.5f,mode == 11 ? .5f : 0,mode == 10 ? .5f : 0},
                                {0,0,1,0}};
                    MTXConcat(srt, bias, srt);
                }
                J3DMtxProjConcat(srt, deferred->getEffectMtx(index), complete);
                Mtx inverseView;
                if (!MTXInverse(j3dSys.getViewMtx(), inverseView)) return false;
                MTXConcat(complete, inverseView, complete);
                // The original zeros translation AFTER concatenating param_0.
                // A W0 linear palette input is equivalent without that joint's
                // translation, followed by this translation-free complete map.
                complete[0][3] = complete[1][3] = complete[2][3] = 0;
                if (!post) space = PositionSource::posedLinearNormal;
            } else if (post && (mode == 2 || mode == 8)) {
                Mtx inverseView;
                if (!MTXInverse(j3dSys.getViewMtx(), inverseView)) return false;
                MTXConcat(complete, inverseView, complete);
            }
        }
    }
    if (post) {
        if (reg == 0) {
            if (source != GX_TG_POS) return false;
            space = PositionSource::posed;
        } else if (reg == GX_TEXMTX0) {
            if (!normal) return false;
            space = PositionSource::posedNormal;
        } else if (reg != GX_IDENTITY) return false;
        if (coord->getTexGenType() == GX_TG_MTX2x4 && !identity) {
            // GX replaces the base texgen's third result with1 BEFORE the
            // dual/post matrix. The post dot4 still has implicit W1.
            for (auto& row : complete) { row[3] += row[2]; row[2] = 0; }
        }
    }
    for (const auto& row : complete) for (float value : row) if (!std::isfinite(value)) return false;
    // J3D's DUALTEX words contain only matrix index, no normalize bit. Lighting
    // normal normalization must not leak into this independent texgen input.
    if (uv) {
        if (coord->getTexGenType() != GX_TG_MTX2x4 || space != PositionSource::object) return false;
        auto& transform = out.uvTransforms[unit];
        transform = UvTransform::identity(source - GX_TG_TEX0);
        // GX UV input is (S,T,1,1); retain existing native caller offset semantics.
        for (unsigned row = 0; row < 2; ++row)
            transform.rows[row] = {complete[row][0], complete[row][1], complete[row][2] + complete[row][3]};
    } else if (coord->getTexGenType() == GX_TG_MTX2x4) {
        auto& transform = out.uvTransforms[unit];
        transform = {};
        transform.source = 3 + unsigned(space);
        for (unsigned row = 0; row < 2; ++row) {
            for (unsigned col = 0; col < 3; ++col) transform.rows[row][col] = complete[row][col];
            transform.translation[row] = complete[row][3];
        }
    } else {
        if (unit != 0 || out.projective.enabled) return false;
        out.projective.enabled = true;
        out.projective.source = space;
        out.projective.normalize = false;
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 4; ++col) out.projective.rows[row][col] = complete[row][col];
    }
    return true;
}
} // namespace

bool J3DModel::copyNativeMaterial(J3DMaterial& material, const dusk::pica::UniformBinding& binding,
                                  dusk::pica::DrawUniforms& out) {
    using namespace dusk::pica;
    J3DColorBlock* color = material.getColorBlock();
    if ((binding.channelReadMask && !color) || !material.getPEBlock()) return false;
    out.material = {}; out.channels = {}; out.lights = {}; out.lightCount = 0;
    out.materialColors = {}; out.ambientColors = {};
    const unsigned count = binding.channelReadMask ? color->getColorChanNum() : 0;
    if (binding.channelReadMask > 15) return false;
    if (count > 2) return false;
    unsigned used = 0;
    // Source getter order is RGB0,ALPHA0,RGB1,ALPHA1.
    constexpr unsigned sourceChannel[]{0,2,1,3};
    for (unsigned i = 0; i < 4; ++i) {
        auto& channel = out.channels[i];
        channel.materialVertex = false;
        const unsigned bank = i & 1;
        if (!(binding.channelReadMask & (1u << i))) continue;
        const J3DColorChan* declaration = color->getColorChan(sourceChannel[i]);
        if (!declaration || declaration->getEnable() > 1 || declaration->getMatSrc() > 1) return false;
        channel.enabled = declaration->getEnable();
        channel.materialVertex = declaration->getMatSrc() == GX_SRC_VTX;
        if (channel.enabled) {
            if (declaration->getAmbSrc() > 1 || declaration->getDiffuseFn() > 2 ||
                declaration->getAttnFn() > 2) return false;
            channel.ambientVertex = declaration->getAmbSrc() == GX_SRC_VTX;
            channel.diffuse = Diffuse(declaration->getDiffuseFn());
            channel.attenuation = Attenuation(declaration->getAttnFn());
            channel.lightMask = declaration->getLightMask();
        }
        const bool vertexAvailable = (binding.sourceAttributes & (1u << (GX_VA_CLR0 + bank))) != 0;
        if ((channel.materialVertex || (channel.enabled && channel.ambientVertex)) && !vertexAvailable) return false;
        if (!channel.materialVertex) {
            const J3DGXColor* value = color->getMatColor(bank);
            if (!value) return false;
            const auto row = nativeColorRow(*value);
            if (i < 2) for (unsigned c = 0; c < 3; ++c) out.materialColors[bank][c] = row[c];
            else out.materialColors[bank][3] = row[3];
        }
        if (channel.enabled && !channel.ambientVertex) {
            const J3DGXColor* value = color->getAmbColor(bank);
            if (!value) return false;
            const auto row = nativeColorRow(*value);
            if (i < 2) for (unsigned c = 0; c < 3; ++c) out.ambientColors[bank][c] = row[c];
            else out.ambientColors[bank][3] = row[3];
        }
        if (channel.lightMask && (channel.diffuse != Diffuse::none || channel.attenuation != Attenuation::none) &&
            !(binding.sourceAttributes & (1u << GX_VA_NRM))) return false;
        used |= channel.lightMask;
    }
    std::array<unsigned,8> remap{};
    for (unsigned slot = 0; slot < 8; ++slot) if (used & (1u << slot)) {
        J3DLightInfo declaration;
        if (J3DLightObj* override = color->getLight(slot)) declaration = *override->getLightInfo();
        else if (!dKy_getNativeGlobalLight(slot, declaration)) return false;
        // Skip only colors which cannot contribute to any consuming RGB/alpha
        // channel. Black RGB with nonzero alpha still contributes to alpha.
        bool contributes = false;
        for (unsigned i = 0; i < 4; ++i) if (out.channels[i].lightMask & (1u << slot))
            contributes |= i < 2 ? (declaration.mColor.r || declaration.mColor.g || declaration.mColor.b) :
                                      declaration.mColor.a != 0;
        if (!contributes) continue;
        const unsigned index = out.lightCount++;
        remap[slot] = 1u << index;
        auto& light = out.lights[index];
        bool rgbRead = false, alphaRead = false, vectorRead = false, attenuationRead = false;
        for (unsigned i = 0; i < 4; ++i) if (out.channels[i].lightMask & (1u << slot)) {
            const auto& channel = out.channels[i];
            rgbRead |= i < 2; alphaRead |= i >= 2;
            attenuationRead |= channel.attenuation != Attenuation::none;
            vectorRead |= channel.diffuse != Diffuse::none || channel.attenuation != Attenuation::none;
        }
        if (vectorRead) light.position = {declaration.mLightPosition.x, declaration.mLightPosition.y, declaration.mLightPosition.z};
        if (rgbRead) {
            light.color[0] = declaration.mColor.r / 255.0f;
            light.color[1] = declaration.mColor.g / 255.0f;
            light.color[2] = declaration.mColor.b / 255.0f;
        }
        if (alphaRead) light.color[3] = declaration.mColor.a / 255.0f;
        if (attenuationRead) {
            light.direction = {declaration.mLightDirection.x, declaration.mLightDirection.y, declaration.mLightDirection.z};
            light.cosine = {declaration.mCosAtten.x, declaration.mCosAtten.y, declaration.mCosAtten.z};
            light.distance = {declaration.mDistAtten.x, declaration.mDistAtten.y, declaration.mDistAtten.z};
        }
        light.kind = LightKind::point; // Outward direction retained; the shader supplies the minus dot.
        for (const auto* vector : {&light.position, &light.direction, &light.cosine, &light.distance})
            for (float value : *vector) if (!std::isfinite(value)) return false;
    }
    for (auto& channel : out.channels) {
        const unsigned mask = channel.lightMask;
        channel.lightMask = 0;
        for (unsigned slot = 0; slot < 8; ++slot) if (mask & (1u << slot)) channel.lightMask |= remap[slot];
    }
    for (unsigned i = 0; i < 7; ++i) {
        const unsigned read = binding.parameterReadMasks[i], wrap = binding.parameterWrapMasks[i],
                       signedD = binding.parameterSignedMasks[i];
        if (read > 15 || ((wrap | signedD) & ~read) || (i >= 3 && wrap)) return false;
        if (!read) continue;
        if (!material.getTevBlock()) return false;
        std::array<int,4> components{};
        if (i < 3) {
            const J3DGXColorS10* value = material.getTevColor(i);
            if (!value) return false;
            components = {value->r,value->g,value->b,value->a};
        } else {
            const J3DGXColor* value = material.getTevKColor(i - 3);
            if (!value) return false;
            components = {value->r,value->g,value->b,value->a};
        }
        for (unsigned c = 0; c < 4; ++c) if (read & (1u << c)) {
            int component = components[c];
            // Only A/B/C initial-register reads wrap. A lane also read as D
            // must be representable without truncation, even when A/B/C wrap.
            if ((signedD & (1u << c)) || !(wrap & (1u << c))) {
                if (component < 0 || component > 255) return false;
            } else component = unsigned(component) & 255;
            out.material.colors[i] |= u32(component) << (8 * c);
        }
        out.material.suppliedMask |= 1u << i;
    }
    return nativeFog(material, out);
}

bool J3DModel::copyNativeParameters(const dusk::pica::UniformBinding& binding, dusk::pica::DrawUniforms& out) {
    if (!mModelData || !binding.material || binding.material > mModelData->getMaterialNum()) return false;
    // Logical material index, never the remapped/shared display-list ID.
    J3DMaterial* material = mModelData->getMaterialNodePointer(binding.material - 1);
    if (!material || !material->getTevBlock() || !material->getTexGenBlock() || !binding.sourceStageCount) return false;
    const unsigned stages = material->getTevStageNum(), texgens = material->getTexGenNum();
    if (stages != binding.sourceStageCount || texgens != binding.sourceTexGenCount) {
#if defined(__3DS__)
        OSReport("Native program mismatch shape=%u material=%u stages=%u/%u texgens=%u/%u\n",
                 unsigned(binding.sourceShape), unsigned(binding.material), stages, unsigned(binding.sourceStageCount),
                 texgens, unsigned(binding.sourceTexGenCount));
#endif
        return false;
    }
    // The prepared program is immutable, but pose, channels, colors, texture
    // selections, samplers and matrices remain live inputs. Warp changes both
    // counts without changing the source or image-table asset key.
    if (!copyNativeMaterial(*material, binding, out)) return false;
    out.samplerMask = out.imageOverrideMask = 0;
    out.imageOverrides = {}; out.projective = {};
    for (unsigned unit = 0; unit < 3; ++unit) out.uvTransforms[unit] = dusk::pica::UvTransform::identity(unit);
    J3DTexture* texture = mModelData->getTexture();
    J3DTexMtxObj* deferred = nullptr;
    if (mShapePacket) {
        if (binding.sourceShape >= mModelData->getShapeNum()) return false;
        deferred = getShapePacket(binding.sourceShape)->getTexMtxObj();
    }
    for (unsigned unit = 0; unit < 3; ++unit) {
        const unsigned slot = binding.textureSlots[unit], generator = binding.textureGenerators[unit];
        if (slot == 255 && generator == 255) continue; // Synthetic lookup or genuinely unused unit.
        if (slot >= 8 || generator >= 8 || !texture || !material->getTexGenBlock() || !material->getTevBlock()) return false;
        const unsigned image = material->getTexNo(slot);
        if (image >= texture->getNum() || !nativeSampler(*texture->getResTIMG(image), out.samplers[unit]) ||
            !nativeTexGen(*material, deferred, mModelData->checkFlag(0x20), generator, unit, binding, out)) return false;
        out.samplerMask |= 1u << unit;
        out.imageOverrideMask |= 1u << unit;
        out.imageOverrides[unit] = image + 1;
    }
    return true;
}
bool J3DModel::copyNativePalette(const dusk::pica::UniformBinding& binding, dusk::pica::Palette& out) {
    out = {}; out.count = 0;
#if defined(__3DS__)
    // Diagnostic only: report the guard inputs when this producer refuses.
    struct Report { J3DModel& m; const dusk::pica::UniformBinding& b; const dusk::pica::Palette& p; ~Report() {
        if (p.count) return;
        J3DShape* s = m.mModelData && b.sourceShape < m.mModelData->getShapeNum() ? m.mModelData->getShapeNodePointer(b.sourceShape) : nullptr;
        const u32 t = s && s->getMtxGroupNum() && s->getShapeMtx(0) ? s->getShapeMtx(0)->getType() : 0;
        OSReport("Native palette refused data=%08x model=%08x calc=%u view=%u/%u bb=%u count=%u ids=%u,%u deform=%u%u%u%u%u nbt=%u groups=%u type=%.4s draw=%u nrm=%u user=%u evlp=%u\n",
            m.mModelData ? unsigned(m.mModelData->getFlag()) : 0u, unsigned(m.mFlags), unsigned(m.getMtxCalcMode()),
            m.mMtxBuffer ? unsigned(m.mMtxBuffer->mCurrentViewNo) : 0u, m.mMtxBuffer ? unsigned(m.mMtxBuffer->mMtxNum) : 0u,
            unsigned(b.billboardMode), unsigned(b.paletteCount), unsigned(b.palette[0]), unsigned(b.palette[1]),
            m.mDeformData != nullptr, m.mSkinDeform != nullptr, m.mVtxColorCalc != nullptr, m.mUnkCalc1 != nullptr, m.mUnkCalc2 != nullptr,
            s ? unsigned(s->getNBTFlag()) : 9u, s ? unsigned(s->getMtxGroupNum()) : 0u, reinterpret_cast<const char*>(&t),
            m.mMtxBuffer && m.mMtxBuffer->mpDrawMtxArr[1], m.mMtxBuffer && m.mMtxBuffer->mpNrmMtxArr[1],
            m.mMtxBuffer && m.mMtxBuffer->mpUserAnmMtx, m.mMtxBuffer && m.mMtxBuffer->mpWeightEvlpMtx);
    } } report{*this, binding, out};
#endif
    if (!mModelData || !mMtxBuffer || mMtxBuffer->mCurrentViewNo >= mMtxBuffer->mMtxNum ||
        mModelData->checkFlag(0x100) || binding.billboardMode > 3 ||
        (getMtxCalcMode() != 0 && getMtxCalcMode() != J3DMdlFlag_UseDefaultJ3D) ||
        mDeformData || mSkinDeform || mVtxColorCalc || mUnkCalc1 || mUnkCalc2 ||
        checkFlag(J3DMdlFlag_SkinPosCpu | J3DMdlFlag_SkinNrmCpu)) return false;
    if (binding.sourceShape >= mModelData->getShapeNum()) return false;
    J3DShape* shape = mModelData->getShapeNodePointer(binding.sourceShape);
    if (!shape || shape->getNBTFlag()) return false;
    if (mModelData->checkFlag(0x10)) {
        // Concat-view shapes have no per-view draw bank: the original loader
        // combines the shape packet's base with the remapped animation matrix.
        if (getMtxCalcMode() != 0 || checkFlag(J3DMdlFlag_EnableLOD) || !binding.paletteCount ||
            binding.paletteCount > dusk::pica::paletteSize || !shape->getMtxGroupNum()) return false;
        if (!binding.billboardMode &&
            (!mShapePacket || !getShapePacket(binding.sourceShape)->getBaseMtxPtr())) return false;
        for (unsigned group = 0; group < shape->getMtxGroupNum(); ++group) {
            const auto* mtx = shape->getShapeMtx(group);
            if (!mtx) return false;
            const u32 type = mtx->getType();
            if (binding.billboardMode == 0 ? (type != 'SMCV' && type != 'SMMC') :
                binding.billboardMode == 1 ? type != 'SMBB' :
                binding.billboardMode == 2 ? type != 'SMYB' : true) return false;
        }
        dusk::pica::DrawUniforms check;
        check.palette = {}; check.palette.count = binding.paletteCount;
        for (unsigned i = 0; i < binding.paletteCount; ++i) {
            const unsigned id = binding.palette[i];
            if (!id || id > mModelData->getDrawMtxNum()) return false;
            const unsigned draw = id - 1;
            bool found = false;
            for (unsigned group = 0; group < shape->getMtxGroupNum(); ++group) {
                const auto* mtx = shape->getShapeMtx(group);
                for (unsigned n = 0; n < mtx->getUseMtxNum(); ++n)
                    found |= mtx->getUseMtxIndex(n) == draw;
            }
            if (!found) return false;
            const unsigned flag = mModelData->getDrawMtxFlag(draw);
            const unsigned index = mModelData->getDrawMtxIndex(draw);
            if (flag > 1 || index >= (flag ? mModelData->getWEvlpMtxNum() : mModelData->getJointNum()) ||
                (flag ? !mMtxBuffer->mpWeightEvlpMtx : !mMtxBuffer->mpUserAnmMtx)) return false;
            MtxP animation = flag ? getWeightAnmMtx(index) : mMtxBuffer->getUserAnmMtx(index);
            Mtx m;
            MTXConcat(binding.billboardMode ? j3dSys.getViewMtx() :
                      *getShapePacket(binding.sourceShape)->getBaseMtxPtr(), animation, m);
            if (binding.billboardMode) {
                if (binding.billboardMode == 2 && (draw >= mModelData->getJointNum() ||
                    !mMtxBuffer->getScaleFlagArray())) return false;
                const bool unitScale = binding.billboardMode == 2 && mMtxBuffer->getScaleFlag(draw) == 1;
                if (!nativeBillboard(m, binding.billboardMode, unitScale, check.palette, i)) return false;
            } else {
                // Both concat loaders index the joint scale flags by the
                // remapped animation index, including for envelope matrices.
                // An envelope index past the joint table reads beyond that
                // array in the original (e.g. Link al_face: 5 joints, 35
                // envelopes); use the always-correct inverse transpose there.
                if (!mMtxBuffer->getScaleFlagArray()) return false; // Joint indices are range-checked above.
                const bool unitScale = index < mModelData->getJointNum() && mMtxBuffer->getScaleFlag(index) == 1;
                Mtx inverse;
                if (!unitScale && !MTXInverse(m, inverse)) return false;
                for (unsigned r = 0; r < 3; ++r) {
                    for (unsigned c = 0; c < 4; ++c) {
                        check.palette.position[3*i+r][c] = m[r][c];
                        check.palette.normal[3*i+r][c] = c == 3 ? 0 :
                            unitScale ? m[r][c] : inverse[c][r];
                    }
                }
            }
        }
        for (unsigned n = 0; n < 4; ++n) check.viewProjection[n][n] = 1;
        if (!dusk::pica::validateUniforms(check, binding.paletteCount - 1)) return false;
        out = check.palette;
        return true;
    }
    if (!mMtxBuffer->mpDrawMtxArr[1] || !mMtxBuffer->mpNrmMtxArr[1]) return false;
    const auto* positions = getDrawMtxPtr();
    const auto* normals = getNrmMtxPtr();
    if (!positions || !normals ||
        !dusk::pica::copyGamePalette(binding, mModelData->getDrawMtxNum(), positions, normals, out)) return false;
    // Ordinary shape loaders consume the calculated banks, including any
    // joint billboards already applied by calcBBoardMtx.
    for (unsigned group = 0; group < shape->getMtxGroupNum(); ++group) {
        const auto* mtx = shape->getShapeMtx(group);
        if (!mtx || (mtx->getType() != 'SMTX' && mtx->getType() != 'SMML')) return false;
    }
    return true;
}
namespace {
bool nativeGameParameters(const void* instance, const dusk::pica::UniformBinding& binding,
                          dusk::pica::DrawUniforms& out, void*) noexcept {
    auto* model = const_cast<J3DModel*>(static_cast<const J3DModel*>(instance));
    const auto reject = [&](const char* stage) {
#if defined(__3DS__)
        OSReport("Native producer rejected stage=%s shape=%u material=%u\n",
                 stage, binding.sourceShape, binding.material);
#else
        (void)stage;
#endif
        return false;
    };
    if (!model || !model->copyNativePalette(binding, out.palette)) return reject("palette");
    if (!model->copyNativeParameters(binding, out)) return reject("parameters");
    if (!dusk::pica::game::applyMirrorDraw(instance, binding, out)) return reject("mirror-override");
    const J3DFog* fog = model->getModelData()->getMaterialNodePointer(binding.material - 1)->getFog();
    if (!dusk::pica::game::applyCamera(out, fog ? fog->mCenter : 0)) return reject("camera");
    return true;
}
}
bool J3DModel::drawNativeShape(unsigned shape) {
    dusk::pica::SourceIdentity source = mModelData ? mModelData->getNativeSource() : dusk::pica::SourceIdentity{};
    if (mModelData && mModelData->getTexture())
        source.hasImageContext = mModelData->getTexture()->getNativeImageContext(source.imageContext);
    return dusk::pica::game::drawSourceShape(this, shape, source, nativeGameParameters, nullptr,
        dusk::pica::poseInput | dusk::pica::orderingInput | dusk::pica::colorInput |
        dusk::pica::lightingInput | dusk::pica::texgenInput | dusk::pica::fogInput);
}
#endif

void J3DModel::initialize() {
    mModelData = NULL;
    mFlags = 0;
    mDiffFlag = 0;
    mCalcCallBack = NULL;
    mUserArea = 0;

    mBaseScale.x = 1.0f;
    mBaseScale.y = 1.0f;
    mBaseScale.z = 1.0f;

    MTXIdentity(mBaseTransformMtx);
    MTXIdentity(mInternalView);
    IF_DUSK(MTXIdentity(mPresentationBase));

    mMtxBuffer = NULL;
    mMatPacket = NULL;
    mShapePacket = NULL;
    mDeformData = NULL;
    mSkinDeform = NULL;
    mVtxColorCalc = NULL;
    mUnkCalc1 = NULL;
    mUnkCalc2 = NULL;
}

s32 J3DModel::entryModelData(J3DModelData* pModelData, u32 mdlFlags, u32 mtxNum) {
    J3D_ASSERTMSG(83, pModelData != NULL, "Error : null pointer.");
    J3D_ASSERTMSG(84, mtxNum != 0, "Error : non-zero argument is specified 0.");
    int ret = kJ3DError_Success;

    mModelData = pModelData;
    mMtxBuffer = JKR_NEW J3DMtxBuffer();

    if (mMtxBuffer == NULL) {
        return kJ3DError_Alloc;
    }

    ret = mMtxBuffer->create(pModelData, mtxNum);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    ret = createShapePacket(pModelData);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    ret = createMatPacket(pModelData, mdlFlags);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    mVertexBuffer.setVertexData(&pModelData->getVertexData());
    prepareShapePackets();

    if (mdlFlags & J3DMdlFlag_Unk1) {
        onFlag(J3DMdlFlag_Unk1);
    }
    if (mdlFlags & J3DMdlFlag_UseDefaultJ3D) {
        onFlag(J3DMdlFlag_UseDefaultJ3D);
    }
    if (mdlFlags & J3DMdlFlag_EnableLOD) {
        onFlag(J3DMdlFlag_EnableLOD);
    }
    if (mdlFlags & J3DMdlFlag_UseSharedDL) {
        onFlag(J3DMdlFlag_UseSharedDL);
    }
    if (mdlFlags & J3DMdlFlag_UseSingleDL) {
        onFlag(J3DMdlFlag_UseSingleDL);
    }
    if (mdlFlags & J3DMdlFlag_DifferedDLBuffer) {
        onFlag(J3DMdlFlag_DifferedDLBuffer);
    }
    if (mdlFlags & J3DMdlFlag_SkinPosCpu) {
        J3D_WARN1(149, "%s", "Don't forget to call J3DModel::setSkinDeform\n if you set J3DMdlFlag_SkinPosCpu.");
        onFlag(J3DMdlFlag_SkinPosCpu);
    }
    if (mdlFlags & J3DMdlFlag_SkinNrmCpu) {
        J3D_WARN1(154, "%s", "Don't forget to call J3DModel::setSkinDeform\n if you set J3DMdlFlag_SkinNrmCpu.");
        onFlag(J3DMdlFlag_SkinNrmCpu);
    }

    return kJ3DError_Success;
}

#if TARGET_PC
void J3DModel::calc_presentation_base_mtx() {
    Mtx identity;
    MTXIdentity(identity);
    J3DCalcViewBaseMtx(identity, mBaseScale, mBaseTransformMtx, mPresentationBase);
    dusk::interp::record_final_mtx(mPresentationBase);
    prepare_presentation_view();
}

void J3DModel::prepare_presentation_view() {
    Mtx replacement;
    MtxP presentationBase = mPresentationBase;
    if (dusk::interp::lookup_replacement(mPresentationBase, replacement)) {
        presentationBase = replacement;
    }
    MTXConcat(j3dSys.getViewMtx(), presentationBase, mInternalView);
}

void J3DModel::setAnmMtx(int jointNo, Mtx m) {
    mMtxBuffer->setAnmMtx(jointNo, m);
    dusk::interp::record_final_mtx(mMtxBuffer->getAnmMtx(jointNo));
}

void J3DModel::forgetMtx() {
    dusk::interp::vertex::reset(&mVertexBuffer);
    dusk::interp::forget_mtx(mPresentationBase);
    for (u16 i = 0; i < mModelData->getJointNum(); ++i) {
        dusk::interp::forget_mtx(getAnmMtx(i));
    }
    for (u16 i = 0; i < mModelData->getWEvlpMtxNum(); ++i) {
        dusk::interp::forget_mtx(getWeightAnmMtx(i));
    }
}
#endif

s32 J3DModel::createShapePacket(J3DModelData* pModelData) {
    J3D_ASSERTMSG(173, pModelData != NULL, "Error : null pointer.");

    if (pModelData->getShapeNum() != 0) {
        mShapePacket = JKR_NEW_ARRAY(J3DShapePacket, pModelData->getShapeNum());

        if (mShapePacket == NULL) {
            return kJ3DError_Alloc;
        }

        for (int i = 0; i < pModelData->getShapeNum(); i++) {
            mShapePacket[i].setShape(pModelData->getShapeNodePointer(i));
            mShapePacket[i].setModel(this);
        }
    }

    return kJ3DError_Success;
}

s32 J3DModel::createMatPacket(J3DModelData* pModelData, u32 mdlFlags) {
    J3D_ASSERTMSG(207, pModelData != NULL, "Error : null pointer.");
    s32 ret = 0;

    if (pModelData->getMaterialNum() != 0) {
        mMatPacket = JKR_NEW_ARRAY(J3DMatPacket, pModelData->getMaterialNum());

        if (mMatPacket == NULL) {
            return kJ3DError_Alloc;
        }
    }
    
    u16 matNum = pModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* materialNode = pModelData->getMaterialNodePointer(i);
        J3DMatPacket* matPacket = mMatPacket + i;
        J3DShapePacket* shapePacket = mShapePacket + materialNode->getShape()->getIndex();

        matPacket->setMaterial(materialNode);
        matPacket->setInitShapePacket(shapePacket);
        matPacket->addShapePacket(shapePacket);
        matPacket->setTexture(pModelData->getTexture());
        matPacket->setMaterialID(materialNode->mMaterialID);

        if (pModelData->getModelDataType() == 1) {
            matPacket->lock();
        }

        if (mdlFlags & J3DMdlFlag_DifferedDLBuffer) {
            J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
            J3D_ASSERTMSG(253, dlobj != NULL, "Error : User need to call J3DModelData::newSharedDisplayList()  before using DifferedDLBuffer.");
            matPacket->setDisplayListObj(dlobj);
        } else {
            if (pModelData->getModelDataType() == 1) {
                if (mdlFlags & J3DMdlFlag_UseSingleDL) {
                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    matPacket->setDisplayListObj(dlobj);
                } else {
                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    ret = dlobj->single_To_Double();
                    if (ret != kJ3DError_Success)
                        return ret;

                    matPacket->setDisplayListObj(dlobj);
                }
            } else if (mdlFlags & J3DMdlFlag_UseSharedDL) {
                if (mdlFlags & J3DMdlFlag_UseSingleDL) {
                    ret = materialNode->newSingleSharedDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;

                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    matPacket->setDisplayListObj(dlobj);
                } else {
                    ret = materialNode->newSharedDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;

                    J3DDisplayListObj* dlobj = materialNode->getSharedDisplayListObj();
                    ret = dlobj->single_To_Double();
                    if (ret != kJ3DError_Success)
                        return ret;

                    matPacket->setDisplayListObj(dlobj);
                }
            } else {
                if (mdlFlags & J3DMdlFlag_UseSingleDL) {
                    ret = matPacket->newSingleDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;
                } else {
                    ret = matPacket->newDisplayList(materialNode->countDLSize());
                    if (ret != kJ3DError_Success)
                        return ret;
                }
            }
        }
    }

    return kJ3DError_Success;
}

s32 J3DModel::newDifferedDisplayList(u32 diffFlags) {
    mDiffFlag = diffFlags;

    u16 shapeNum = getModelData()->getShapeNum();
    for (u16 i = 0; i < shapeNum; i++) {
        s32 ret = getShapePacket(i)->newDifferedDisplayList(diffFlags);
        if (ret != kJ3DError_Success)
            return ret;
    }

    return kJ3DError_Success;
}

void J3DModel::ptrToIndex() {
    j3dSys.setModel(this);
    j3dSys.setTexture(mModelData->getTexture());

    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMatPacket* matPacket = getMatPacket(i);

        GDLObj dlobj;
        GDInitGDLObj(&dlobj, matPacket->getDisplayList(0), matPacket->getDisplayListSize());
        GDSetCurrent(&dlobj);

        mModelData->getMaterialNodePointer(i)->getTevBlock()->ptrToIndex();

        GDSetCurrent(NULL);
    }
}

void J3DModel::lock() {
    int matNum = mModelData->getMaterialNum();
    for (int i = 0; i < matNum; i++) {
        mMatPacket[i].lock();
    }
}

void J3DModel::unlock() {
    int matNum = mModelData->getMaterialNum();
    for (int i = 0; i < matNum; i++) {
        mMatPacket[i].unlock();
    }
}

void J3DModel::makeDL() {
    j3dSys.setModel(this);
    j3dSys.setTexture(mModelData->getTexture());

    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);
        mModelData->getMaterialNodePointer(i)->makeDisplayList();
    }
}

void J3DModel::calcMaterial() {
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        j3dSys.onFlag(4);
    } else {
        j3dSys.offFlag(4);
    }

    if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        j3dSys.onFlag(8);
    } else {
        j3dSys.offFlag(8);
    }

    mModelData->syncJ3DSysFlags();
    j3dSys.setTexture(mModelData->getTexture());

    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);

        J3DMaterial* material = mModelData->getMaterialNodePointer(i);
        if (material->getMaterialAnm() != NULL) {
            material->getMaterialAnm()->calc(material);
        }

        MtxP jointMtx = getAnmMtx(material->getJoint()->getJntNo());
#if TARGET_PC
        Mtx presentedJoint;
        if (dusk::interp::is_presentation_active() &&
            dusk::interp::lookup_replacement(jointMtx, presentedJoint))
        {
            jointMtx = presentedJoint;
        }
#endif
        material->calc(jointMtx);
    }
}

void J3DModel::calcDiffTexMtx() {
    j3dSys.setModel(this);

    u16 matNum =  mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);
        J3DMaterial* materialNode = mModelData->getMaterialNodePointer(i);
        materialNode->calcDiffTexMtx(getAnmMtx(materialNode->getJoint()->getJntNo()));
    }

    u16 shapeNum = getModelData()->getShapeNum();
    for (u16 i = 0; i < shapeNum; i++) {
        J3DShapePacket* shapePacket = getShapePacket(i);
        J3DTexGenBlock* texGenBlock = mModelData->getShapeNodePointer(i)->getMaterial()->getTexGenBlock();

        for (u16 j = 0; (int)j < 8; j++) {
            J3DTexMtx* texMtxNode = texGenBlock->getTexMtx(j);
            J3DTexMtxObj* texMtxObj = shapePacket->getTexMtxObj();
            if (texMtxNode != NULL && texMtxObj != NULL) {
                texMtxObj->setMtx(j, texMtxNode->getMtx());
            }
        }
    }
}

void J3DModel::diff() {
    u16 matNum = mModelData->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        j3dSys.setMatPacket(&mMatPacket[i]);
        mModelData->getMaterialNodePointer(i)->diff(mDiffFlag);
    }
}

s32 J3DModel::setDeformData(J3DDeformData* p_deformData, u32 param_1) {
    mDeformData = p_deformData;

    if (p_deformData == NULL) {
        return kJ3DError_Success;
    }

    s32 ret = mVertexBuffer.copyLocalVtxArray(param_1);
    if (ret) {
        return ret;
    }

    if (param_1 & 4) {
        p_deformData->offAllFlag(1);
    }

    return kJ3DError_Success;
}

s32 J3DModel::setSkinDeform(J3DSkinDeform* p_skinDeform, u32 flags) {
    mSkinDeform = p_skinDeform;

    if (p_skinDeform == NULL) {
        offFlag(J3DMdlFlag_SkinPosCpu);
        offFlag(J3DMdlFlag_SkinNrmCpu);
        return 5;
    }

    mSkinDeform->initMtxIndexArray(mModelData);

    if (mModelData->checkFlag(0x100)) {
        mSkinDeform->changeFastSkinDL(mModelData);
        flags &= ~2;
        flags &= ~4;
        mSkinDeform->transformVtxPosNrm(mModelData);
        mSkinDeform->initSkinInfo(mModelData);
    }

    s32 ret = kJ3DError_Success;
    if ((~flags & 2)) {
        ret = mVertexBuffer.allocTransformedVtxPosArray();
        if (ret != kJ3DError_Success) {
            offFlag(J3DMdlFlag_SkinPosCpu);
            return ret;
        }
        onFlag(J3DMdlFlag_SkinPosCpu);
    } else {
        offFlag(J3DMdlFlag_SkinPosCpu);
    }

    if ((~flags & 4)) {
        ret = mVertexBuffer.allocTransformedVtxNrmArray();
        if (ret != kJ3DError_Success) {
            offFlag(J3DMdlFlag_SkinNrmCpu);
            return ret;
        }
        onFlag(J3DMdlFlag_SkinNrmCpu);
    } else {
        offFlag(J3DMdlFlag_SkinNrmCpu);
    }

    return ret;
}

void J3DModel::calcAnmMtx() {
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_UseDefaultJ3D)) {
        getModelData()->getJointTree().calc(mMtxBuffer, j3dDefaultScale, j3dDefaultMtx);
    } else {
        getModelData()->getJointTree().calc(mMtxBuffer, mBaseScale, mBaseTransformMtx);
    }
}

void J3DModel::calcWeightEnvelopeMtx() {
    if (getModelData()->getWEvlpMtxNum() != 0 && !checkFlag(J3DMdlFlag_EnableLOD)) {
        if (!getModelData()->checkFlag(0x100)) {
            mMtxBuffer->calcWeightEnvelopeMtx();
        }
    }
}

void J3DModel::update() {
    calc();
    entry();
}

void J3DModel::calc() {
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinPosCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
    }

    if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinNrmCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
    }

    mModelData->syncJ3DSysFlags();
    mVertexBuffer.frameInit();

    if (mUnkCalc2 != NULL) {
        mUnkCalc2->calc(mModelData);
    }

    if (mDeformData != NULL) {
        mDeformData->deform(this);
    }

    if (mVtxColorCalc != NULL) {
        mVtxColorCalc->calc(this);
    }

    if (mUnkCalc1 != NULL) {
        mUnkCalc1->calc(this);
    }

    calcAnmMtx();
    calcWeightEnvelopeMtx();

    if (mSkinDeform != NULL) {
        mSkinDeform->deform(this);
    }

    if (mCalcCallBack != NULL) {
        mCalcCallBack(this, 0);
    }

#if TARGET_PC
    dusk::interp::vertex::capture(&mVertexBuffer, mDeformData);

    for (u16 i = 0; i < mModelData->getJointNum(); ++i) {
        dusk::interp::record_final_mtx(getAnmMtx(i));
    }

    for (u16 i = 0; i < mModelData->getWEvlpMtxNum(); ++i) {
        dusk::interp::record_final_mtx(getWeightAnmMtx(i));
    }
#endif
}

void J3DModel::entry() {
    j3dSys.setModel(this);

    if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinPosCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
    }

    if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        j3dSys.onFlag(J3DSysFlag_SkinNrmCpu);
    } else {
        j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
    }

    mModelData->syncJ3DSysFlags();
    j3dSys.setTexture(mModelData->getTexture());

    for (u16 i = 0; i < mModelData->getJointNum(); i++) {
        J3DJoint* joint = mModelData->getJointNodePointer(i);
        if (joint->getMesh() != NULL) {
            joint->entryIn();
        }
    }

#if TARGET_PC
    if (mModelData->needsInterpCallBack()) {
        dusk::interp::material::record_model(this);
    }
#endif
}

void J3DModel::viewCalc() {
    mMtxBuffer->swapDrawMtx();
    mMtxBuffer->swapNrmMtx();

    if (getModelData()->checkFlag(0x10)) {
        if (getMtxCalcMode() == 2) {
#if TARGET_PC
            calc_presentation_base_mtx();
#else
            J3DCalcViewBaseMtx(j3dSys.getViewMtx(), mBaseScale, mBaseTransformMtx,
                               (MtxP)&mInternalView);
#endif
        }
    } else if (isCpuSkinningOn()) {
        if (getMtxCalcMode() == 2) {
#if TARGET_PC
            calc_presentation_base_mtx();
#else
            J3DCalcViewBaseMtx(j3dSys.getViewMtx(), mBaseScale, mBaseTransformMtx,
                               (MtxP)&mInternalView);
#endif
        }
    } else if (checkFlag(J3DMdlFlag_SkinPosCpu)) {
        mMtxBuffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
        calcNrmMtx();
        calcBumpMtx();
        DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
        DCStoreRange(getNrmMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx33));
    } else if (checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        mMtxBuffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
        calcBBoardMtx();
        DCStoreRange(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
    } else {
        mMtxBuffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
        calcNrmMtx();
        calcBBoardMtx();
        calcBumpMtx();
        DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
        DCStoreRange(getNrmMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx33));
    }

    prepareShapePackets();
}

void J3DModel::calcNrmMtx() {
    mMtxBuffer->calcNrmMtx();
}

void J3DModel::calcBumpMtx() {
    if (getModelData()->checkBumpFlag()) {
        u32 bumpMtxIdx = 0;
        u16 materialNum = getModelData()->getMaterialNum();

        for (u16 i = 0; i < materialNum; i++) {
            J3DMaterial* material = getModelData()->getMaterialNodePointer(i);
            if (material->getNBTScale()->mbHasScale == TRUE) {
                material->getShape()->calcNBTScale(*material->getNBTScale()->getScale(),
                                                   getNrmMtxPtr(), getBumpMtxPtr(bumpMtxIdx));
                DCStoreRange(getBumpMtxPtr(bumpMtxIdx), mModelData->getDrawMtxNum() * sizeof(Mtx33));
                bumpMtxIdx++;
            }
        }
    }
}

void J3DModel::calcBBoardMtx() {
    if (getModelData()->checkBBoardFlag()) {
        mMtxBuffer->calcBBoardMtx();
    }
}

void J3DModel::prepareShapePackets() {
    u16 shapeNum = mModelData->getShapeNum();

    for (u16 i = 0; i < shapeNum; i++) {
        J3DShape* shapeNode = mModelData->getShapeNodePointer(i);
        J3DShapePacket* shapePacket = &mShapePacket[i];
        shapePacket->setMtxBuffer(mMtxBuffer);

        if (getMtxCalcMode() == 2) {
            shapePacket->setBaseMtxPtr(&mInternalView);
        } else {
            shapePacket->setBaseMtxPtr((Mtx*)j3dSys.getViewMtx());
        }
    }
}
