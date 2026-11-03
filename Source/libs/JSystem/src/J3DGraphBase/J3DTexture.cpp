#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DAssert.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/JKernel/JKRHeap.h"
#if defined(DUSK_TARGET_3DS)
#include "JSystem/JKernel/JKRExpHeap.h"
#include "m_Do/m_Do_graphic.h"
#include "../../../../platforms/3ds/runtime/texture_resources.hpp"
#include "../../../../platforms/3ds/runtime/gx_copy_images.hpp"
#include "../../../../platforms/3ds/runtime/gx_state.hpp"
#include "../../../../platforms/3ds/native/game_source.hpp"
#include "../../../../platforms/3ds/runtime/resource_archive.hpp"
#endif

J3DTexture::~J3DTexture() {
#if defined(DUSK_TARGET_3DS)
    releaseNativeTextures();
    for(u16 i=0;i<mNum;++i) dusk::native::gx::destroyTlutObject(&mpTlutObj[i]);
    delete[] mpNativeSources;
    delete[] mpNativeImageDigests;
    delete[] mpNativeImageState;
    if (mNativeResOwned) JKR_DELETE_ARRAY(mpRes);
#endif
#if TARGET_PC
    delete[] mpTexObj;
    delete[] mpTlutObj;
    delete[] mpImgDataPtr;
    delete[] mpTlutDataPtr;
#endif
}

#if defined(DUSK_TARGET_3DS)
void J3DTexture::releaseNativeTexture(u16 index) {
    dusk::native::releaseTextureResources(&mpTexObj[index]);
}
void J3DTexture::releaseNativeTextures() {
    for (u16 i=0; i<mNum; ++i) releaseNativeTexture(i);
}
void J3DTexture::captureNativeImage(u16 index, const ResTIMG& source) {
    // These are typed resource-ledger lookups, not command/register decoding.
    // The source reference supplies the ORIGINAL header, never mpRes+offset.
    std::uint32_t token{};
    dusk::native::gx::Texture retained;
    dusk::native::BtiInfo info;
    auto state=NativeImageState::valid;
    if(!dusk::native::gx::textureCommandToken(mpImgDataPtr[index],token))
        state=NativeImageState::missingToken;
    else if(!dusk::native::gx::textureFromCommandToken(token,retained))
        state=NativeImageState::missingRegistration;
    else if(!retained.immutablePixels) {
        dusk::native::gx::Texture object;
        if(&source==mDoGph_gInf_c::getFrameBufferTimg() &&
           mpImgDataPtr[index]==mDoGph_gInf_c::getFrameBufferTex() &&
           dusk::native::gx::textureAliasImage(retained.handle)==mpImgDataPtr[index] &&
           retained.format==GX_TF_RGBA8 && retained.mipCount==1 &&
           dusk::native::registeredFramebufferCapture(mpImgDataPtr[index],retained.handle) &&
           dusk::native::gx::queryTextureObject(&mpTexObj[index],object) &&
           object.handle==retained.handle)
            mpNativeImageDigests[index]=dusk::pica::identifyFramebufferColor();
        else
            state=NativeImageState::mutablePixels;
    }
    else if(dusk::native::inspectBti({retained.bti.data(),retained.bti.size()},1024,info)!=dusk::native::ResourceStatus::ready)
        state=NativeImageState::invalidBti;
    else if(!dusk::pica::identifyImage({reinterpret_cast<const uint8_t*>(&source),sizeof(source)},
            {info.image.data,info.image.size},{info.palette.data,info.palette.size},mpNativeImageDigests[index]))
        state=NativeImageState::invalidIdentity;
    mpNativeImageState[index]=state;
    mNativeImageContextValid=false;
}
bool J3DTexture::getNativeImageContext(std::array<uint8_t,32>& out) {
    out={};
    if(!mNativeImageContextValid) {
        if(mNum>4096) {
            OSReport("Native image context rejected slot=unavailable count=%u reason=table-limit\n",unsigned(mNum));
            return false;
        }
        for(u16 i=0;i<mNum;++i)if(mpNativeImageState[i]!=NativeImageState::valid) {
            const char* reason="unresolved-image";
            switch(mpNativeImageState[i]) {
            case NativeImageState::unresolved: reason=mpNativeSources[i]?"unresolved-image":"unassigned-slot";break;
            case NativeImageState::missingToken: reason="missing-texture-token";break;
            case NativeImageState::missingRegistration: reason="missing-texture-registration";break;
            case NativeImageState::mutablePixels: reason="mutable-pixels";break;
            case NativeImageState::invalidBti: reason="invalid-retained-bti";break;
            case NativeImageState::invalidIdentity: reason="invalid-image-identity";break;
            case NativeImageState::valid: break;
            }
            OSReport("Native image context rejected slot=%u count=%u reason=%s\n",unsigned(i),unsigned(mNum),reason);
            return false;
        }
        if(!dusk::pica::identifyImageContext({mpNativeImageDigests,mNum},mNativeImageContext)) {
            OSReport("Native image context rejected slot=unavailable count=%u reason=context-hash\n",unsigned(mNum));
            return false;
        }
        mNativeImageContextValid=true;
    }
    out=mNativeImageContext;return true;
}
void J3DTexture::retainNativeTexture(u16 index, const ResTIMG& source) {
    using namespace dusk::native;
    gx::Texture metadata;
    if (!collectTextureResources() || !gx::queryTextureObject(&mpTexObj[index], metadata))
        OSPanic(__FILE__,__LINE__,"Native J3D texture object/retirement: %s",textureResourceFailure());
    if (retainArchiveTexture(&mpTexObj[index], &source, metadata.handle)) {
        captureNativeImage(index,source);return;
    }
    // ARAM archives can decompress a complete BMD into an independent EXP
    // allocation. Use its actual live extent, never the heap's capacity.
    JKRHeap* heap=JKRHeap::findFromRoot(const_cast<ResTIMG*>(&source));
    const s32 remaining=heap && heap->getHeapType()=='EXPH' ?
        static_cast<JKRExpHeap*>(heap)->getRemainingAllocationSize(&source) : -1;
    if (remaining>=32 && retainTextureResource(&mpTexObj[index],
        {reinterpret_cast<const u8*>(&source),static_cast<std::size_t>(remaining)},mpImgDataPtr[index])) {
        captureNativeImage(index,source);return;
    }
    OSPanic(__FILE__,__LINE__,"Native J3D texture bounds: %s (index=%u format=%u size=%ux%u allocation=%ld)",
        textureResourceFailure(),unsigned(index),unsigned(source.format),unsigned(source.width),unsigned(source.height),long(remaining));
}
#endif

void J3DTexture::loadGX(u16 idx, GXTexMapID texMapID) const {
    J3D_ASSERT_RANGE(29, idx < mNum);

    ResTIMG* timg = getResTIMG(idx);

#if defined(DUSK_TARGET_3DS)
    // Named external TEX1 entries are resolved by the original resource loader
    // through setResTIMG. They have no image until that assignment occurs.
    if (timg->imageOffset == 0)
        OSPanic(__FILE__,__LINE__,"Native J3D named external texture was not resolved");
#endif

#if TARGET_PC
    if (timg->width == 0 || timg->height == 0)
        return;
    if (idx >= mNum) {
        OSReport("J3DTexture::loadGX: idx %d out of bounds (mNum=%d)!\n", idx, mNum);
        return;
    }
    if (timg->indexTexture) {
        GXLoadTlut(&mpTlutObj[idx], (GXTlut)texMapID);
        GXInitTexObjTlut(&mpTexObj[idx], (GXTlut)texMapID);
    }
    GXLoadTexObj(&mpTexObj[idx], texMapID);
#else
    TGXTexObj texObj;
    GXTlutObj tlutObj;

    if (!timg->indexTexture) {
        GXInitTexObj(&texObj, ((u8*)timg) + timg->imageOffset, timg->width, timg->height,
                     (GXTexFmt)timg->format, (GXTexWrapMode)timg->wrapS, (GXTexWrapMode)timg->wrapT,
                     timg->mipmapEnabled);
    } else {
        GXInitTexObjCI(&texObj, ((u8*)timg) + timg->imageOffset, timg->width, timg->height,
                       (GXCITexFmt)timg->format, (GXTexWrapMode)timg->wrapS,
                       (GXTexWrapMode)timg->wrapT, timg->mipmapEnabled, (u32)texMapID);
        GXInitTlutObj(&tlutObj, ((u8*)timg) + timg->paletteOffset, (GXTlutFmt)timg->colorFormat,
                      timg->numColors);
        GXLoadTlut(&tlutObj, texMapID);
    }

    const f32 kLODClampScale = 1.0f / 8.0f;
    const f32 kLODBiasScale = 1.0f / 100.0f;
    GXInitTexObjLOD(&texObj, (GXTexFilter)timg->minFilter, (GXTexFilter)timg->magFilter,
                    timg->minLOD * kLODClampScale, timg->maxLOD * kLODClampScale,
                    timg->LODBias * kLODBiasScale, timg->biasClamp, timg->doEdgeLOD,
                    (GXAnisotropy)timg->maxAnisotropy);
    GXLoadTexObj(&texObj, texMapID);
#endif
}

#if TARGET_PC
void J3DTexture::loadGXTexObj(u16 idx) {
    J3D_ASSERT_RANGE(29, idx < mNum);
    ResTIMG* timg = getResTIMG(idx);
#if defined(DUSK_TARGET_3DS)
    dusk::native::gx::destroyTlutObject(&mpTlutObj[idx]);
#endif

#if defined(DUSK_TARGET_3DS)
    if (timg->imageOffset == 0) {
        mpTexObj[idx].reset();
        return;
    }
#endif

    GXTlutObj& tlutObj = mpTlutObj[idx];
    TGXTexObj& texObj = mpTexObj[idx];

    if (!timg->indexTexture) {
        GXInitTexObj(&texObj, mpImgDataPtr[idx], timg->width, timg->height,
                     (GXTexFmt)timg->format, (GXTexWrapMode)timg->wrapS, (GXTexWrapMode)timg->wrapT,
                     timg->mipmapEnabled);
    } else {
        GXInitTexObjCI(&texObj, mpImgDataPtr[idx], timg->width, timg->height,
                       (GXCITexFmt)timg->format, (GXTexWrapMode)timg->wrapS,
                       (GXTexWrapMode)timg->wrapT, timg->mipmapEnabled, GX_TLUT0);
        GXInitTlutObj(&tlutObj, mpTlutDataPtr[idx], (GXTlutFmt)timg->colorFormat,
                      timg->numColors);
    }

    const f32 kLODClampScale = 1.0f / 8.0f;
    const f32 kLODBiasScale = 1.0f / 100.0f;
    GXInitTexObjLOD(&texObj, (GXTexFilter)timg->minFilter, (GXTexFilter)timg->magFilter,
                    timg->minLOD * kLODClampScale, timg->maxLOD * kLODClampScale,
                    timg->LODBias * kLODBiasScale, timg->biasClamp, timg->doEdgeLOD,
                    (GXAnisotropy)timg->maxAnisotropy);
}
#endif

void J3DTexture::entryNum(u16 num) {
    J3D_ASSERT_NONZEROARG(79, num != 0);

#if defined(DUSK_TARGET_3DS)
    releaseNativeTextures();
    for(u16 i=0;i<mNum;++i) dusk::native::gx::destroyTlutObject(&mpTlutObj[i]);
    delete[] mpNativeSources;
    delete[] mpNativeImageDigests;
    delete[] mpNativeImageState;
    mNativeImageContextValid=false;
    if (mNativeResOwned) JKR_DELETE_ARRAY(mpRes);
    mNativeResOwned=true;
#endif

    mNum = num;
    mpRes = JKR_NEW_ARRAY(ResTIMG, num);
    J3D_ASSERT_ALLOCMEM(83, mpRes != NULL);
    
    delete[] mpTexObj;
    delete[] mpTlutObj;
    delete[] mpImgDataPtr;
    delete[] mpTlutDataPtr;
    mpTexObj = new TGXTexObj[num]();
    mpTlutObj = new GXTlutObj[num]();
    mpImgDataPtr = new u8*[num]();
    mpTlutDataPtr = new u8*[num]();
#if defined(DUSK_TARGET_3DS)
    mpNativeSources = new const ResTIMG*[num]();
    mpNativeImageDigests = new std::array<uint8_t,32>[num]();
    mpNativeImageState = new NativeImageState[num]();
#endif

    for (int i = 0; i < mNum; i++) {
        mpRes[i].paletteOffset = 0;
        mpRes[i].imageOffset = 0;
    }
}

void J3DTexture::addResTIMG(u16 newNum, const ResTIMG* newRes) {
    if (newNum == 0)
        return;

    J3D_ASSERT_NULLPTR(105, newRes != NULL);

    u16 oldNum = mNum;
#if defined(DUSK_TARGET_3DS)
    if (unsigned(mNum)+newNum>65535)
        OSPanic(__FILE__,__LINE__,"Native J3D texture count overflow");
    // Preserve the actual original source headers across repeated growth.
    // Copied ResTIMG offsets are relative to their source, not the new table.
    auto** oldSources=new const ResTIMG*[oldNum];
    for (u16 i=0;i<oldNum;++i) oldSources[i]=mpNativeSources[i];
    entryNum(mNum+newNum);
    for (u16 i=0;i<oldNum;++i) {
        if (!oldSources[i]) OSPanic(__FILE__,__LINE__,"Native J3D texture source absent");
        setResTIMG(i,*oldSources[i]);
    }
    delete[] oldSources;
    for (u16 i=oldNum;i<mNum;++i) setResTIMG(i,newRes[i]);
#else
    ResTIMG* oldRes = mpRes;

    entryNum(mNum + newNum);

    for (u16 i = 0; i < oldNum; i++) {
        setResTIMG(i, oldRes[i]);
    }

    for (u16 i = oldNum; i < mNum; i++) {
        setResTIMG(i, newRes[i]);
    }
#endif
}
