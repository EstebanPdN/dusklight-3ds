#include "dusk/dvd_asset.hpp"

#if !defined(DUSK_TARGET_3DS)
#include "dusk/logging.h"
#else
#include <dolphin/os.h>
#include "JSystem/JKernel/JKRExpHeap.h"
#endif
#include "helpers/endian.h"
#include <cstring>

#include "DynamicLink.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRDvdRipper.h"

#include <dolphin/dvd.h>

namespace dusk {

static const u8* s_dolData = nullptr; // pointer to dol data
static size_t    s_dolSize = 0;

struct DolHeader {
    BE(u32) textOffset[7];
    BE(u32) dataOffset[11];
    BE(u32) textAddr[7];
    BE(u32) dataAddr[11];
    BE(u32) textSize[7];
    BE(u32) dataSize[11];
};

struct DolSection {
    u32 fileOffset;
    u32 vaddr;
    u32 size;
};

static DolSection s_dolSections[18]; // 7 text + 11 data
static int        s_dolSectionCount = 0;

static bool EnsureDolParsed() {
    if (s_dolData) return true;

    s32 sz = 0;
    const u8* p = DVDGetDOLLocation(&sz);
    if (!p || sz < 256) {

#if defined(DUSK_TARGET_3DS)
    OSPanic(__FILE__, __LINE__, "dvd_asset: DVDGetDOLLocation failed (size=%d)", sz);
#else
    DuskLog.fatal("dvd_asset: DVDGetDOLLocation failed (size={})", sz);
#endif
        return false;
    }

    s_dolData = p;
    s_dolSize = sz;

    const auto* hdr = (const DolHeader*)s_dolData;
    s_dolSectionCount = 0;

    for (int i = 0; i < 7;  i++) {
        u32 off = hdr->textOffset[i];
        u32 addr = hdr->textAddr[i];
        u32 sz = hdr->textSize[i];
        if (sz > 0 && off > 0) {
            s_dolSections[s_dolSectionCount++] = {off, addr, sz};
        }
    }

    for (int i = 0; i < 11; i++) {
        u32 off = hdr->dataOffset[i];
        u32 addr = hdr->dataAddr[i];
        u32 sz = hdr->dataSize[i];
        if (sz > 0 && off > 0) {
            s_dolSections[s_dolSectionCount++] = {off, addr, sz};
        }
    }

    return true;
}

static s32 DolVaToFileOffset(u32 va) {
    if (!EnsureDolParsed()) return -1;
    for (int i = 0; i < s_dolSectionCount; i++) {
        const auto& sec = s_dolSections[i];
        if (va >= sec.vaddr && va < sec.vaddr + sec.size) {
            return static_cast<s32>(sec.fileOffset + (va - sec.vaddr));
        }
    }

#if defined(DUSK_TARGET_3DS)
    OSPanic(__FILE__, __LINE__, "dvd_asset: VA 0x%08x not found in DOL", va);
#else
    DuskLog.fatal("dvd_asset: VA 0x{:08X} not found in any DOL section", va);
#endif
    return -1;
}

static u32 GetOffsetForVersion(std::initializer_list<OffsetVersion> version) {
    const auto gameVersion = dusk::version::getGameVersion();
    for (auto elem : version) {
        if (elem.mGameVersion == gameVersion) {
            return elem.mOffset;
        }
    }

#if defined(DUSK_TARGET_3DS)
    OSPanic(__FILE__, __LINE__, "dvd_asset: no offset for this game version");
#else
    DuskLog.fatal("Unable to find offset for this game version!");
#endif
}

bool LoadDolAsset(void* dst, std::initializer_list<OffsetVersion> virtualAddress, s32 size) {
    s32 fileOffset = DolVaToFileOffset(GetOffsetForVersion(virtualAddress));
    if (fileOffset < 0) {
        return false;
    }

    if (size <= 0 || (size_t)(fileOffset + size) > s_dolSize) {

#if defined(DUSK_TARGET_3DS)
    OSPanic(__FILE__, __LINE__, "dvd_asset: DOL read out of range (offset=0x%x size=0x%x DOL size=%zu)", fileOffset, size, s_dolSize);
#else
    DuskLog.fatal("dvd_asset: DOL read out of range (offset={:#x} size={:#x} dolSize={})", fileOffset, size, s_dolSize);
#endif
        return false;
    }

    std::memcpy(dst, s_dolData + fileOffset, size);
    return true;
}

bool LoadRelAsset(void* dst, const char* dvdPath, std::initializer_list<OffsetVersion> offset, s32 size) {
    auto resOffset = GetOffsetForVersion(offset);
    void* p = JKRDvdRipper::loadToMainRAM(dvdPath, (u8*)dst, EXPAND_SWITCH_UNKNOWN1, (u32)size, nullptr, JKRDvdRipper::ALLOC_DIRECTION_FORWARD, resOffset, nullptr, nullptr);

#if defined(DUSK_TARGET_3DS)
    if (!p) OSPanic(__FILE__, __LINE__, "dvd_asset: failed to load %s (offset=0x%x size=0x%x)", dvdPath, resOffset, size);
#else
    if (!p) DuskLog.fatal("dvd_asset: failed to load {} (offset={:#x} size={:#x})", dvdPath, resOffset, size);
#endif
    return p != nullptr;
}

bool LoadArchivedRelAsset(void* dst, u32 memType, const char* relFileName, std::initializer_list<OffsetVersion> offset, s32 size) {
    auto resOffset = GetOffsetForVersion(offset);

    // On TARGET_PC, cDyl_InitCallback skips DynamicModuleControl::initialize() due to static linking
    // Mount RELS.arc on first use so sArchive is available
    static bool s_mountAttempted = false;
    if (!DynamicModuleControl::sArchive && !s_mountAttempted) {
        s_mountAttempted = true; DynamicModuleControl::initialize();
    }

    if (!DynamicModuleControl::sArchive) {

#if defined(DUSK_TARGET_3DS)
    OSPanic(__FILE__, __LINE__, "dvd_asset: RELS archive not mounted"); return false;
#else
    DuskLog.fatal("dvd_asset: RELS archive not mounted"); return false;
#endif
    }

    const u8* rel = static_cast<const u8*>(DynamicModuleControl::sArchive->getResource(memType, relFileName));
    if (!rel) {

#if defined(DUSK_TARGET_3DS)
    OSPanic(__FILE__, __LINE__, "dvd_asset: %s not found in RELS archive", relFileName); return false;
#else
    DuskLog.fatal("dvd_asset: {} not found in RELS archive", relFileName); return false;
#endif
    }

    // getResource can return a decompressed ARAM/DVD member. getResSize is
    // the stored compressed length; offsets refer to the expanded REL image.
    const s32 resourceSize = DynamicModuleControl::sArchive->getExpandedResSize(rel);
    if (!dst || size <= 0 || resourceSize < 0 || resOffset > static_cast<u32>(resourceSize) ||
        static_cast<u32>(size) > static_cast<u32>(resourceSize) - resOffset) {
#if defined(DUSK_TARGET_3DS)
        OSPanic(__FILE__, __LINE__, "dvd_asset: archived REL read out of range (offset=0x%x size=%d resource=%d)", resOffset, size, resourceSize);
#else
        DuskLog.fatal("dvd_asset: archived REL read out of range (offset={:#x} size={} resource={})", resOffset, size, resourceSize);
#endif
        return false;
    }
#if defined(DUSK_TARGET_3DS)
    JKRHeap* heap=JKRHeap::findFromRoot(const_cast<u8*>(rel));
    const s32 remaining=heap&&heap->getHeapType()=='EXPH'?
        static_cast<JKRExpHeap*>(heap)->getRemainingAllocationSize(rel):-1;
    if(remaining<resourceSize)
        OSPanic(__FILE__,__LINE__,"dvd_asset: expanded REL lacks a live bounded allocation (expanded=%ld remaining=%ld)",
            long(resourceSize),long(remaining));
#endif
    std::memcpy(dst, rel + resOffset, size);
    return true;
}

}  // namespace dusk
