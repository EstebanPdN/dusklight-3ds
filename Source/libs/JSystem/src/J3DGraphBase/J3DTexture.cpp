#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DAssert.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/JKernel/JKRHeap.h"
#if defined(DUSK_TARGET_3DS)
#include "JSystem/JKernel/JKRExpHeap.h"
#include "../../../../platforms/3ds/runtime/texture_resources.hpp"
#include "../../../../platforms/3ds/runtime/gx_state.hpp"
#endif

J3DTexture::~J3DTexture() {
#if defined(DUSK_TARGET_3DS)
    releaseNativeTextures();
    for(u16 i=0;i<mNum;++i) dusk::native::gx::destroyTlutObject(&mpTlutObj[i]);
    delete[] mpNativeSources;
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
void J3DTexture::retainNativeTexture(u16 index, const ResTIMG& source) {
    using namespace dusk::native;
    gx::Texture metadata;
    if (!collectTextureResources() || !gx::queryTextureObject(&mpTexObj[index], metadata))
        OSPanic(__FILE__,__LINE__,"Native J3D texture object/retirement: %s",textureResourceFailure());
    if (retainArchiveTexture(&mpTexObj[index], &source, metadata.handle)) return;
    // ARAM archives can decompress a complete BMD into an independent EXP
    // allocation. Use its actual live extent, never the heap's capacity.
    JKRHeap* heap=JKRHeap::findFromRoot(const_cast<ResTIMG*>(&source));
    const s32 remaining=heap && heap->getHeapType()=='EXPH' ?
        static_cast<JKRExpHeap*>(heap)->getRemainingAllocationSize(&source) : -1;
    if (remaining>=32 && retainTextureResource(&mpTexObj[index],
        {reinterpret_cast<const u8*>(&source),static_cast<std::size_t>(remaining)},mpImgDataPtr[index])) return;
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
