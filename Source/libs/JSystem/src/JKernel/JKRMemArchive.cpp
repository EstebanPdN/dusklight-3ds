#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JKernel/JKRDecomp.h"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include "JSystem/JUtility/JUTAssert.h"
#include "JSystem/JUtility/JUTException.h"
#include <cstring>
#include "global.h"
#include <stdint.h>
#include "os_report.h"
#if defined(DUSK_TARGET_3DS)
#include "JSystem/JKernel/JKRExpHeap.h"
#include "../../../../platforms/3ds/runtime/texture_resources.hpp"
#endif

JKRMemArchive::JKRMemArchive(s32 entryNum, JKRArchive::EMountDirection mountDirection)
    : JKRArchive(entryNum, MOUNT_MEM) {
    mIsMounted = false;
    mMountDirection = mountDirection;
    if (!open(entryNum, mMountDirection)) {
        return;
    }

    mVolumeType = 'RARC';
    mVolumeName = mStringTable + mNodes->name_offset;

    sVolumeList.prepend(&mFileLoaderLink);
    mIsMounted = true;
}

JKRMemArchive::JKRMemArchive(void* buffer, u32 bufferSize, JKRMemBreakFlag param_3)
    : JKRArchive((intptr_t)buffer, MOUNT_MEM) {
    mIsMounted = false;
    if (!open(buffer, bufferSize, param_3)) {
        return;
    }

    mVolumeType = 'RARC';
    mVolumeName = mStringTable + mNodes->name_offset;

    sVolumeList.prepend(&mFileLoaderLink);
    mIsMounted = true;
}

JKRMemArchive::~JKRMemArchive() {
#if defined(DUSK_TARGET_3DS)
    dusk::native::unregisterTextureArchive(this);
#endif
    if (mIsMounted == true) {
        if (mIsOpen) {
            if (mArcHeader)
                JKRFreeToHeap(mHeap, mArcHeader);
        }

        sVolumeList.remove(&mFileLoaderLink);
        mIsMounted = false;
    }
}

static void dummy() {
    OS_REPORT(__FILE__);
    OS_REPORT("isMounted()");
    OS_REPORT("mMountCount == 1");
}

bool JKRMemArchive::open(s32 entryNum, JKRArchive::EMountDirection mountDirection) {
    mArcHeader = NULL;
    mArcInfoBlock = NULL;
    mArchiveData = NULL;
    mNodes = NULL;
    mFiles = NULL;
    mStringTable = NULL;
    mIsOpen = false;
    mMountDirection = mountDirection;

    u32 loadedSize=0;
    if (mMountDirection == JKRArchive::MOUNT_DIRECTION_HEAD) {
        mArcHeader = (SArcHeader *)JKRDvdToMainRam(
            entryNum, NULL, EXPAND_SWITCH_UNKNOWN1, 0, mHeap, JKRDvdRipper::ALLOC_DIRECTION_FORWARD,
            0, (int *)&mCompression, &loadedSize);
        if (mArcHeader) {
            DCInvalidateRange(mArcHeader, loadedSize);
        }
    }
    else {
        mArcHeader = (SArcHeader *)JKRDvdToMainRam(
            entryNum, NULL, EXPAND_SWITCH_UNKNOWN1, 0, mHeap,
            JKRDvdRipper::ALLOC_DIRECTION_BACKWARD, 0, (int *)&mCompression, &loadedSize);
        if (mArcHeader) {
            DCInvalidateRange(mArcHeader, loadedSize);
        }
    }

    if (!mArcHeader) {
        mMountMode = UNKNOWN_MOUNT_MODE;
    }
    else {
#if defined(DUSK_TARGET_3DS)
        if(!dusk::native::registerTextureArchive(this,{reinterpret_cast<const u8*>(mArcHeader),loadedSize})){
            JKRFreeToHeap(mHeap,mArcHeader);mArcHeader=NULL;
            mMountMode=UNKNOWN_MOUNT_MODE;return false;
        }
#endif
        JUT_ASSERT(438, mArcHeader->signature == 'RARC');
        mArcInfoBlock = (SArcDataInfo *)((u8 *)mArcHeader + mArcHeader->header_length);
        mNodes = (SDIDirEntry *)((u8 *)&mArcInfoBlock->num_nodes + mArcInfoBlock->node_offset);
        mFiles = (SDIFileEntry *)((u8 *)&mArcInfoBlock->num_nodes + mArcInfoBlock->file_entry_offset);
        mStringTable = (char *)((u8 *)&mArcInfoBlock->num_nodes + mArcInfoBlock->string_table_offset);

#if TARGET_PC
        initFileDataPointers();
#if defined(DUSK_TARGET_3DS)
        if(!mFileData){
            dusk::native::unregisterTextureArchive(this);
            JKRFreeToHeap(mHeap,mArcHeader);mArcHeader=NULL;
            mMountMode=UNKNOWN_MOUNT_MODE;return false;
        }
#endif
#endif

        mArchiveData =
            (u8 *)((uintptr_t)mArcHeader + mArcHeader->header_length + mArcHeader->file_data_offset);
        mIsOpen = true;
    }

#if DEBUG
    if (mMountMode == 0) {
        OSReport(":::Cannot alloc memory [%s][%d]\n", __FILE__, 460);
    }
#endif

    return (mMountMode == UNKNOWN_MOUNT_MODE) ? false : true;
}

bool JKRMemArchive::open(void* buffer, u32 bufferSize, JKRMemBreakFlag flag) {
#if defined(DUSK_TARGET_3DS)
    mHeap=JKRHeap::findFromRoot(buffer);
    const s32 allocationBytes=mHeap?(mHeap->getHeapType()=='EXPH'?
        static_cast<JKRExpHeap*>(mHeap)->getRemainingAllocationSize(buffer):mHeap->getSize(buffer)):-1;
    // The historical mount(void*) entry point passes 0xffff as a sentinel.
    // It is not evidence that an arbitrary buffer contains that many bytes.
    if(bufferSize==0||bufferSize==0xffff){
        if(allocationBytes<64){mMountMode=UNKNOWN_MOUNT_MODE;return false;}
        bufferSize=allocationBytes;
    }else if(allocationBytes>0&&bufferSize>static_cast<u32>(allocationBytes)){
        mMountMode=UNKNOWN_MOUNT_MODE;return false;
    }
    if(!dusk::native::registerTextureArchive(this,{static_cast<const u8*>(buffer),bufferSize})){
        mMountMode=UNKNOWN_MOUNT_MODE;return false;
    }
#endif
    mArcHeader = (SArcHeader *)buffer;
    JUT_ASSERT(491, mArcHeader->signature == 'RARC');
    mArcInfoBlock = (SArcDataInfo *)((u8 *)mArcHeader + mArcHeader->header_length);
    mNodes = (SDIDirEntry *)((u8 *)&mArcInfoBlock->num_nodes + mArcInfoBlock->node_offset);
    mFiles = (SDIFileEntry *)((u8 *)&mArcInfoBlock->num_nodes + mArcInfoBlock->file_entry_offset);
    mStringTable = (char *)((u8 *)&mArcInfoBlock->num_nodes + mArcInfoBlock->string_table_offset);
    mArchiveData = (u8 *)(((uintptr_t)mArcHeader + mArcHeader->header_length) + mArcHeader->file_data_offset);
    mIsOpen = (flag == JKRMEMBREAK_FLAG_UNKNOWN1) ? true : false; // mIsOpen might be u8
    mHeap = JKRHeap::findFromRoot(buffer);
    mCompression = COMPRESSION_NONE;

#if TARGET_PC
    initFileDataPointers();
#if defined(DUSK_TARGET_3DS)
    if (mFileData == nullptr) {
        dusk::native::unregisterTextureArchive(this);
        mMountMode = UNKNOWN_MOUNT_MODE;
        return false;
    }
#endif
#endif

    return true;
}

void* JKRMemArchive::fetchResource(SDIFileEntry* fileEntry, u32* resourceSize) {
    JUT_ASSERT(555, isMounted());

#if TARGET_PC
    if (void* data = getOverlayData(fileEntry, resourceSize); data != nullptr) {
        return data;
    }
#endif

    if (!JKAR_DATA(fileEntry)) {
        JKAR_DATA(fileEntry) = mArchiveData + fileEntry->data_offset;
    }

    if (resourceSize) {
        *resourceSize = fileEntry->data_size;
    }

    return JKAR_DATA(fileEntry);
}

void* JKRMemArchive::fetchResource(void* buffer, u32 bufferSize, SDIFileEntry* fileEntry,
                                   u32* resourceSize) {
    JUT_ASSERT(595, isMounted());

#if TARGET_PC
    if (copyOverlayData(buffer, bufferSize, fileEntry, resourceSize)) {
        return buffer;
    }
#endif

    u32 srcLength = fileEntry->data_size;
    if (srcLength > bufferSize) {
        srcLength = bufferSize;
    }

    if (JKAR_DATA(fileEntry) != NULL) {
        memcpy(buffer, JKAR_DATA(fileEntry), srcLength);
    } else {
        u8 flags = fileEntry->type_flags_and_name_offset >> 24;
        JKRCompression compression = JKRConvertAttrToCompressionType(flags);
        srcLength =
            fetchResource_subroutine(mArchiveData + fileEntry->data_offset, srcLength, (u8*)buffer, bufferSize, compression);
    }

    if (resourceSize) {
        *resourceSize = srcLength;
    }

    return buffer;
}

void JKRMemArchive::removeResourceAll(void) {
    JUT_ASSERT(642, isMounted());

    IF_DUSK(removeAllOverlayResources();)

    if (mArcInfoBlock == NULL)
        return;
    if (mMountMode == MOUNT_MEM)
        return;

    // !@bug: looping over file entries without incrementing the fileEntry pointer. Thus, only the
    // first fileEntry will clear/remove the resource data.
    SDIFileEntry* fileEntry = mFiles;
    for (int i = 0; i < mArcInfoBlock->num_file_entries; i++) {
        if (JKAR_DATA(fileEntry)) {
            JKAR_DATA(fileEntry) = NULL;
        }
    }
    fileEntry++;
}

bool JKRMemArchive::removeResource(void* resource) {
    JUT_ASSERT(673, isMounted());

#if TARGET_PC
    if (removeOverlayResource(resource, true)) {
        return true;
    }
#endif

    SDIFileEntry* fileEntry = findPtrResource(resource);
    if (!fileEntry)
        return false;

    JKAR_DATA(fileEntry) = NULL;
    return true;
}

u32 JKRMemArchive::fetchResource_subroutine(u8* src, u32 srcLength, u8* dst, u32 dstLength,
                                            JKRCompression compression) {
    switch (compression) {
    case COMPRESSION_NONE:
        if (srcLength > dstLength) {
            srcLength = dstLength;
        }

        memcpy(dst, src, srcLength);
        return srcLength;

    case COMPRESSION_YAY0:
    case COMPRESSION_YAZ0: {
        u32 expendedSize = JKRDecompExpandSize(src);
        if (expendedSize > dstLength) {
            expendedSize = dstLength;
        }

        JKRDecompress(src, dst, expendedSize, 0);
        return expendedSize;
    }

    default: {
        JUTException::panic(__FILE__, 723, "??? bad sequence\n");
    } break;
    }

    return 0;
}

u32 JKRMemArchive::getExpandedResSize(const void* resource) const {
#if TARGET_PC
    if (u32 overlaySize; getOverlayResourceSize(resource, &overlaySize)) {
        return overlaySize;
    }
#endif

    SDIFileEntry* fileEntry = findPtrResource(resource);
    if (fileEntry == NULL)
        return -1;

    u8 flags = fileEntry->type_flags_and_name_offset >> 24;
    if ((flags & 4) == false) {
        return getResSize(resource);
    }
    u32 expandSize = JKRDecompExpandSize((u8*)resource);
    return expandSize;
}
