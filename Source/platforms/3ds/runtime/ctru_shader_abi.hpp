#pragma once
// Enum size at the SDK boundary for shader binaries (see ctru_abi.hpp for the
// frame buffer side and the reason). DVLE_s, one program of a parsed shader
// binary, starts with two enum members: in the SDK's short-enum build its
// DVLP pointer is at offset 8 and the struct is 92 bytes, in this tree's int
// enums at 16 and 100 (found in Alpha 21, when a second program of one binary
// was first used). Program n of a binary is found ONLY through dvle() below;
// Tools/3ds/check_ctru_abi.py fails the build if a platform source names a
// DVLE_s member or indexes DVLB_s::DVLE past the first program. A header of
// its own: code that reads only frame buffers needs no shader types (host
// tests build stand-in SDKs without them).
#include <3ds.h>
#include <cstddef>
#include <cstdint>

namespace dusk::native::abi {
#if defined(__3DS__)
namespace layout {
struct Dvle {
    u8 type; // DVLE_type
    bool mergeOutmaps;
    u8 gshMode; // DVLE_geoShaderMode
    u8 gshFixedVtxStart, gshVariableVtxNum, gshFixedVtxNum;
    DVLP_s* dvlp;
    u32 mainOffset, endmainOffset;
    u32 constTableSize;
    DVLE_constEntry_s* constTableData;
    u32 outTableSize;
    DVLE_outEntry_s* outTableData;
    u32 uniformTableSize;
    DVLE_uniformEntry_s* uniformTableData;
    char* symbolTableData;
    u8 outmapMask;
    u32 outmapData[8];
    u32 outmapMode;
    u32 outmapClock;
};
// Offsets of the short-enum SDK build (libctru 2.7).
static_assert(sizeof(Dvle) == 92 && offsetof(Dvle, dvlp) == 8 && offsetof(Dvle, outmapClock) == 88);
}
// Program n of a parsed shader binary.
inline DVLE_s* dvle(DVLB_s* binary, unsigned n) {
    return reinterpret_cast<DVLE_s*>(reinterpret_cast<std::uint8_t*>(binary->DVLE) + n * sizeof(layout::Dvle));
}
#else
// Host tests compile their stand-in SDK with this compiler's enum size.
inline DVLE_s* dvle(DVLB_s* binary, unsigned n) { return &binary->DVLE[n]; }
#endif
}
