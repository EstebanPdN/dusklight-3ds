#ifndef _DOLPHIN_GD_BASE_H
#define _DOLPHIN_GD_BASE_H

#include <dolphin/types.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXCommandList.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GDOverflowCb)(void);

typedef struct _GDLObj {
	u8* start;
	u32 length;
	u8* ptr;
	u8* top;
} GDLObj;

#if defined(DUSK_TARGET_3DS)
#ifdef __cplusplus
extern thread_local GDLObj* __GDCurrentDL;
#else
extern _Thread_local GDLObj* __GDCurrentDL;
#endif
void GDNativeCheck(u32 size);
// logic.txt gdwrite=0: every byte through __GDWrite, as before Alpha 21 (A/B).
extern unsigned char dusk_native_gd_bytewise;
u32 GDNativeGetOffset(const GDLObj* dl);
void GDNativeSetOffset(u32 offset);
void GDSetCurrent(GDLObj* dl);
#else
extern GDLObj* __GDCurrentDL;
#endif

void GDInitGDLObj(GDLObj* dl, void* start, u32 length);
void GDFlushCurrToMem(void);
void GDPadCurr32(void);
void GDOverflowed(void);
void GDSetOverflowCallback(GDOverflowCb callback);
GDOverflowCb GDGetOverflowCallback(void);

inline static void GDOverflowCheck(u32 size) {
#if defined(DUSK_TARGET_3DS)
    // Inline bound test; the out-of-line check (full validation, overflow
    // callback) only runs when the list is missing or the bytes do not fit.
    GDLObj* const dl = __GDCurrentDL;
    if (!dl || size > (u32)(dl->top - dl->ptr)) GDNativeCheck(size);
#else
    if (__GDCurrentDL->ptr + size > __GDCurrentDL->top) {
        GDOverflowed();
    }
#endif
}

inline static void __GDWrite(u8 data) {
#if defined(DUSK_TARGET_3DS)
    // J3D also writes directly, bypassing the ordinary GDWrite_* wrappers.
    // One thread-local read and one compare per byte (was a call with the
    // complete bounds validation per byte).
    GDLObj* dl = __GDCurrentDL;
    if (!dl || dl->ptr >= dl->top) { GDNativeCheck(1); dl = __GDCurrentDL; }
    *dl->ptr++ = data;
#else
    *__GDCurrentDL->ptr++ = data;
#endif
}

#if defined(DUSK_TARGET_3DS)
// The bytes of one command with one read of the thread's current list and one
// bound test. The list is thread-local: on the ARM11 every read of it is a
// call to __aeabi_read_tp and three loads, and __GDWrite pays that per byte
// (17 instructions a byte; a material diff writes hundreds of bytes). When the
// bytes do not all fit, each one takes __GDWrite's checked path as before, so
// the overflow callback and its failures see the same calls.
inline static void __GDWriteBytes(const u8* data, u32 count) {
    GDLObj* const dl = __GDCurrentDL;
    if (!dusk_native_gd_bytewise && dl && dl->ptr < dl->top && (u32)(dl->top - dl->ptr) >= count) {
        u8* const at = dl->ptr;
        for (u32 i = 0; i < count; i++) at[i] = data[i];
        dl->ptr = at + count;
        return;
    }
    for (u32 i = 0; i < count; i++) __GDWrite(data[i]);
}
#else
inline static void __GDWriteBytes(const u8* data, u32 count) {
    for (u32 i = 0; i < count; i++) __GDWrite(data[i]);
}
#endif

inline static void GDWrite_data(const void* data, u32 length) {
    GDOverflowCheck(length);
    __GDWriteBytes(static_cast<const u8*>(data), length);
}

inline static void GDWrite_u8(u8 data) {
    GDOverflowCheck(sizeof(u8));
    __GDWrite(data);
}

inline static void GDWrite_u16(u16 data) {
    GDOverflowCheck(sizeof(u16));
    const u8 bytes[2] = {(u8)(data >> 8), (u8)data};
    __GDWriteBytes(bytes, 2);
}

inline static void GDWrite_u24(u32 data) {
    GDOverflowCheck(3);
    const u8 bytes[3] = {(u8)(data >> 16), (u8)(data >> 8), (u8)data};
    __GDWriteBytes(bytes, 3);
}

inline static void GDWrite_u32(u32 data) {
    GDOverflowCheck(sizeof(u32));
    const u8 bytes[4] = {(u8)(data >> 24), (u8)(data >> 16), (u8)(data >> 8), (u8)data};
    __GDWriteBytes(bytes, 4);
}

inline static void GDWrite_u64(u64 data) {
  GDOverflowCheck(sizeof(u64));
  __GDWrite((u8)((data >> 56) & 0xFF));
  __GDWrite((u8)((data >> 48) & 0xFF));
  __GDWrite((u8)((data >> 40) & 0xFF));
  __GDWrite((u8)((data >> 32) & 0xFF));
  __GDWrite((u8)((data >> 24) & 0xFF));
  __GDWrite((u8)((data >> 16) & 0xFF));
  __GDWrite((u8)((data >> 8) & 0xFF));
  __GDWrite((u8)((data >> 0) & 0xFF));
}

inline static void GDWrite_f32(f32 data) {
    union {
        f32 f;
        u32 u;
    } fid;
    fid.f = data;
    GDWrite_u32(fid.u);
}

inline static void GDWriteXFCmdHdr(u16 addr, u8 len) {
    GDWrite_u8(GX_LOAD_XF_REG);
    GDWrite_u16(len - 1);
    GDWrite_u16(addr);
}

inline static void GDWriteXFCmd(u16 addr, u32 val) {
    GDWrite_u8(GX_LOAD_XF_REG);
    GDWrite_u16(0);
    GDWrite_u16(addr);
    GDWrite_u32(val);
}

inline static void GDWriteXFIndxDCmd(u16 addr, u8 len, u16 index) {
    GDWrite_u8(GX_LOAD_INDX_D);
    GDWrite_u16(index);
    GDWrite_u16((len - 1) << 12 | addr);
}

inline static void GDWriteXFIndxACmd(u16 addr, u8 len, u16 index) {
    GDWrite_u8(GX_LOAD_INDX_A);
    GDWrite_u16(index);
    GDWrite_u16(((len - 1) << 12) | addr);
}

inline static void GDWriteXFIndxBCmd(u16 addr, u8 len, u16 index) {
    GDWrite_u8(GX_LOAD_INDX_B);
    GDWrite_u16(index);
    GDWrite_u16(((len - 1) << 12) | addr);
}

inline static void GDWriteXFIndxCCmd(u16 addr, u8 len, u16 index) {
    GDWrite_u8(GX_LOAD_INDX_C);
    GDWrite_u16(index);
    GDWrite_u16(((len - 1) << 12) | addr);
}

inline static void GDWriteCPCmd(u8 addr, u32 val) {
    GDWrite_u8(GX_LOAD_CP_REG);
    GDWrite_u8(addr);
    GDWrite_u32(val);
}

inline static void GDWriteBPCmd(u32 regval) {
    GDWrite_u8(GX_LOAD_BP_REG);
    GDWrite_u32(regval);
}

inline static void GDWriteAuroraCmd(u16 subCommand) {
  GDWrite_u8(GX_AURORA);
  GDWrite_u16(subCommand);
}

#if !defined(DUSK_TARGET_3DS)
inline static void GDSetCurrent(GDLObj* dl) {
    __GDCurrentDL = dl;
}
#endif

static inline u32 GDGetCurrOffset(void) {
#if defined(DUSK_TARGET_3DS)
    return GDNativeGetOffset(__GDCurrentDL);
#else
    return (u32)(__GDCurrentDL->ptr - __GDCurrentDL->start);
#endif
}

static inline void GDSetCurrOffset(u32 offset) {
#if defined(DUSK_TARGET_3DS)
    GDNativeSetOffset(offset);
#else
    __GDCurrentDL->ptr = __GDCurrentDL->start + offset;
#endif
}

static inline void* GDGetCurrPointer(void) {
#if defined(DUSK_TARGET_3DS)
    if (!__GDCurrentDL) GDNativeCheck(0);
#endif
    return (void*)__GDCurrentDL->ptr;
}

static inline u8* GDGetCurrPointer2(void) {
#if defined(DUSK_TARGET_3DS)
    if (!__GDCurrentDL) GDNativeCheck(0);
#endif
    return __GDCurrentDL->ptr;
}

static inline u32 GDGetGDLObjOffset(const GDLObj* dl) {
#if defined(DUSK_TARGET_3DS)
    return GDNativeGetOffset(dl);
#else
    return (u32)(dl->ptr - dl->start);
#endif
}

#ifdef __cplusplus
}
#endif

#endif
