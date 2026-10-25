#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JUtility/JUTTexture.h"
#include "JSystem/JUtility/JUTPalette.h"
#include <gx.h>
#include "os_report.h"
#if !defined(DUSK_TARGET_3DS)
#include "dusk/logging.h"
#endif

#include "JSystem/JKernel/JKRHeap.h"
#if defined(DUSK_TARGET_3DS)
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "../../../../platforms/3ds/runtime/texture_resources.hpp"
#include "../../../../platforms/3ds/runtime/gx_state.hpp"
#include "../../../../platforms/3ds/runtime/gx_copy_images.hpp"
#endif

JUTTexture::~JUTTexture() {
#if defined(DUSK_TARGET_3DS)
    dusk::native::releaseTextureResources(this);
#endif
    if (getCaptureFlag()) {
        JKR_DELETE_ARRAY(field_0x3c);
    }
    if (getEmbPaletteDelFlag()) {
        JKR_DELETE(mEmbPalette);
    }
}

void JUTTexture::storeTIMG(ResTIMG const* param_0, u8 param_1) {
    if (param_0 && param_1 < 0x10) {
#if defined(DUSK_TARGET_3DS)
        dusk::native::releaseTextureResources(this);
#endif
        mTexInfo = param_0;
        s32 imgOffset = mTexInfo->imageOffset;
        mTexData = (void*)((intptr_t)mTexInfo + imgOffset);

        if (mTexInfo->imageOffset == 0) {
            mTexData = (void*)((intptr_t)mTexInfo + 0x20);
        }

        mPalette = NULL;
        mTlutName = 0;
        mWrapS = mTexInfo->wrapS;
        mWrapT = mTexInfo->wrapT;
        mMinFilter = mTexInfo->minFilter;
        mMagFilter = mTexInfo->magFilter;
        mMinLOD = (s8)mTexInfo->minLOD;
        mMaxLOD = (s8)mTexInfo->maxLOD;
        mLODBias = mTexInfo->LODBias;

        u16 numColors = mTexInfo->numColors;

        if (numColors == 0) {
            initTexObj();
        } else {
            GXTlut tlut;
            if (numColors > 0x100) {
                tlut = (GXTlut)((param_1 % 4) + GX_BIGTLUT0);
            } else {
                tlut = (GXTlut)param_1;
            }

            s32 palOffset = mTexInfo->paletteOffset;

            if (mEmbPalette == NULL || !getEmbPaletteDelFlag()) {
                mEmbPalette = JKR_NEW JUTPalette(tlut, (GXTlutFmt)mTexInfo->colorFormat,
                                             (JUTTransparency)mTexInfo->alphaEnabled,
                                             numColors,
                                             (void*)((intptr_t)mTexInfo + palOffset));
                setEmbPaletteDelFlag(true);
            } else {
                mEmbPalette->storeTLUT(tlut, (GXTlutFmt)mTexInfo->colorFormat,
                                       (JUTTransparency)mTexInfo->alphaEnabled,
                                       numColors,
                                       (void*)((intptr_t)mTexInfo + palOffset));
            }
            attachPalette(mEmbPalette);
        }
    }
}

void JUTTexture::storeTIMG(ResTIMG const* param_0, JUTPalette* param_1) {
    GXTlut type;

    if (param_1 != NULL) {
        type = param_1->getTlutName();
    } else {
        type = GX_TLUT0;
    }
    storeTIMG(param_0, param_1, type);
}

void JUTTexture::storeTIMG(ResTIMG const* param_0, JUTPalette* param_1, GXTlut param_2) {
    GXTlut type;

    if (param_0 == NULL) {
        return;
    }
#if defined(DUSK_TARGET_3DS)
    dusk::native::releaseTextureResources(this);
#endif
    mTexInfo = param_0;
    mTexData = ((u8*)mTexInfo) + mTexInfo->imageOffset;
    if (mTexInfo->imageOffset == 0) {
        mTexData = ((u8*)mTexInfo) + sizeof(ResTIMG);
    }
    if (getEmbPaletteDelFlag()) {
            JKR_DELETE(mEmbPalette);
    }
    mEmbPalette = param_1;
    setEmbPaletteDelFlag(false);
    mPalette = NULL;
    if (param_1 != NULL) {
        mTlutName = param_2;
        if (param_2 != param_1->getTlutName()) {
            GXTlutFmt format = param_1->getFormat();
            JUTTransparency transperancy = param_1->getTransparency();
            u16 numColors = param_1->getNumColors();
            ResTLUT* colorTable = param_1->getColorTable();
            param_1->storeTLUT(param_2, format, transperancy, numColors, colorTable);
        }
    }

    mWrapS = mTexInfo->wrapS;
    mWrapT = mTexInfo->wrapT;
    mMinFilter = mTexInfo->minFilter;
    mMagFilter = mTexInfo->magFilter;
    mMinLOD = mTexInfo->minLOD;
    mMaxLOD = mTexInfo->maxLOD;
    mLODBias = mTexInfo->LODBias;
    init();
}

void JUTTexture::attachPalette(JUTPalette* param_0) {
    if (mTexInfo->indexTexture) {
        if (param_0 == NULL && mEmbPalette != NULL) {
            mPalette = mEmbPalette;
        } else {
            mPalette = param_0;
        }
        initTexObj(mPalette->getTlutName());
    }
}

void JUTTexture::init() {
    if (mTexInfo->numColors == 0) {
        initTexObj();
    } else {
        if (mEmbPalette != NULL) {
            mPalette = mEmbPalette;

            initTexObj(mPalette->getTlutName());
        } else {
            OS_REPORT("This texture is CI-Format, but EmbPalette is NULL.\n");
        }
    }
}

void JUTTexture::initTexObj() {
    GXBool mipmapEnabled = mTexInfo->mipmapEnabled != 0 ? GX_TRUE : GX_FALSE;
    u8* image = ((u8*)mTexInfo);
    s32 imgOffset = mTexInfo->imageOffset;
    image += (imgOffset ? imgOffset : 0x20);
#ifdef TARGET_PC
    mTexObj.reset();
#endif
    GXInitTexObj(&mTexObj, image, mTexInfo->width, mTexInfo->height,
                 (GXTexFmt)mTexInfo->format, (GXTexWrapMode)mWrapS, (GXTexWrapMode)mWrapT,
                 mipmapEnabled);
    GXInitTexObjLOD(&mTexObj, (GXTexFilter)mMinFilter, (GXTexFilter)mMagFilter, mMinLOD / 8.0f,
                    mMaxLOD / 8.0f, mLODBias / 100.0f, mTexInfo->biasClamp,
                    mTexInfo->doEdgeLOD, (GXAnisotropy)mTexInfo->maxAnisotropy);
}

void JUTTexture::initTexObj(GXTlut param_0) {
    GXBool mipmapEnabled = mTexInfo->mipmapEnabled != 0 ? GX_TRUE : GX_FALSE;
    mTlutName = param_0;
    u8* image = ((u8*)mTexInfo);
    s32 imgOffset = mTexInfo->imageOffset;
    /*
    DuskLog.debug("initTexObj: Offset={}, W={}, H={}, Ptr={}", imgOffset, (u16)mTexInfo->width,
           (u16)mTexInfo->height,
           (void*)mTexInfo);
    */
    image += (imgOffset ? imgOffset : 0x20);
#ifdef TARGET_PC
    mTexObj.reset();
#endif
    GXInitTexObjCI(&mTexObj, image, mTexInfo->width, mTexInfo->height,
                 (GXCITexFmt)mTexInfo->format, (GXTexWrapMode)mWrapS,
                 (GXTexWrapMode)mWrapT, mipmapEnabled, param_0);
    GXInitTexObjLOD(&mTexObj, (GXTexFilter)mMinFilter, (GXTexFilter)mMagFilter, mMinLOD / 8.0f,
                    mMaxLOD / 8.0f, mLODBias / 100.0f, mTexInfo->biasClamp,
                    mTexInfo->doEdgeLOD, (GXAnisotropy)mTexInfo->maxAnisotropy);
}

void JUTTexture::load(GXTexMapID param_0) {
#if defined(DUSK_TARGET_3DS)
    if(!dusk::native::refreshCopyImagePalette(mTexData))
        OSPanic(__FILE__,__LINE__,"Native indexed texture palette refresh");
    dusk::native::gx::Texture metadata;
    if(!dusk::native::collectTextureResources())
        OSPanic(__FILE__,__LINE__,"Native texture retirement: %s",dusk::native::textureResourceFailure());
    if(!dusk::native::gx::queryTextureObject(&mTexObj,metadata))
        OSPanic(__FILE__,__LINE__,"Native JUT texture object is missing");
    if(!dusk::native::retainArchiveTexture(this,mTexInfo,metadata.handle)) {
        // ARAM/JPA resources may be independently expanded into a real game
        // allocation rather than retained in a mounted memory archive.
        auto* heap=JKRHeap::findFromRoot(const_cast<ResTIMG*>(mTexInfo));
        s32 remaining=-1;
        if(heap&&heap->getHeapType()=='EXPH')
            remaining=static_cast<JKRExpHeap*>(heap)->getRemainingAllocationSize(mTexInfo);
        else if(heap&&heap->getHeapType()=='SLID')
            remaining=static_cast<JKRSolidHeap*>(heap)->getRemainingUsedSize(mTexInfo);
        if(remaining<32||!dusk::native::retainTextureResource(this,
            {reinterpret_cast<const u8*>(mTexInfo),static_cast<std::size_t>(remaining)},mTexData))
            OSPanic(__FILE__,__LINE__,"Native JUT texture: %s info=%p format=%u size=%ux%u allocation=%ld caller=%p",
                dusk::native::textureResourceFailure(),mTexInfo,unsigned(mTexInfo->format),unsigned(mTexInfo->width),
                unsigned(mTexInfo->height),long(remaining),__builtin_return_address(0));
    }
#endif
    if (mPalette) {
        mPalette->load();
    }
    GXLoadTexObj(&mTexObj, param_0);
}
