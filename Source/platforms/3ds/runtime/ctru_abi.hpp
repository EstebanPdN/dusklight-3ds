#pragma once
// Enum size at the SDK boundary.
//
// The game, like its GameCube original and the PC port, assumes int-sized
// enums: it reads them out of file data (`*(TEInterpolate*)p`, structs laid
// over resources) and stores out-of-range values in them. devkitARM defaults to
// -fshort-enums (an enum with values 0..3 is one byte), and until Alpha 2 the
// whole tree was built that way: every such read took the first byte of a
// big-endian word, i.e. zero. The cutscene camera, for one, lost its
// interpolation mode and stepped from key to key. The tree is now built with
// -fno-short-enums.
//
// libctru and Citro3D stay prebuilt with short enums. Three of their structs
// have enum members whose offsets differ between the two settings
// (C3D_FrameBuf, C3D_RenderTarget, ERRF_ExceptionInfo; found by compiling
// every SDK struct both ways). Their fields from the first enum member on are
// read ONLY through the accessors below; Tools/3ds/check_ctru_abi.py fails the
// build if a platform source names one of those members directly. Everything
// else, and everything passed by value, is byte-identical in both settings.
#include <3ds.h>
#include <citro3d.h>
#include <cstddef>
#include <cstdint>

namespace dusk::native::abi {
#if defined(__3DS__)
namespace layout {
struct FrameBuf {
    void* colorBuf;
    void* depthBuf;
    u16 width;
    u16 height;
    u8 colorFormat; // GPU_COLORBUF
    u8 depthFormat; // GPU_DEPTHBUF
    bool block32Mode;
    u8 colorWriteMask : 4;
    u8 depthWriteMask : 4;
};
struct RenderTarget {
    C3D_RenderTarget* next;
    C3D_RenderTarget* prev;
    FrameBuf frameBuf;
    bool inUse;
    bool ownsColorBuffer, ownsDepthBuffer;
    bool isLinked;
    u8 screenId; // gfxScreen_t
    u8 sideId;   // gfx3dSide_t
    u32 transferBits;
};
struct ExceptionInfo {
    u8 kind; // ERRF_ExceptionType
    u8 reserved[3];
    u32 fsr;
    u32 far;
    u32 fpexc;
    u32 fpinst;
    u32 fpinst2;
};
// Offsets of the short-enum SDK build (libctru 2.7, Citro3D 1.7).
static_assert(sizeof(FrameBuf) == 16 && offsetof(FrameBuf, colorFormat) == 12 && offsetof(FrameBuf, depthFormat) == 13 &&
              offsetof(FrameBuf, block32Mode) == 14);
static_assert(sizeof(RenderTarget) == 36 && offsetof(RenderTarget, frameBuf) == 8 && offsetof(RenderTarget, inUse) == 24 &&
              offsetof(RenderTarget, screenId) == 28 && offsetof(RenderTarget, transferBits) == 32);
static_assert(sizeof(ExceptionInfo) == 24 && offsetof(ExceptionInfo, fsr) == 4);
inline const FrameBuf& of(const C3D_FrameBuf* fb) { return *reinterpret_cast<const FrameBuf*>(fb); }
inline const RenderTarget& of(const C3D_RenderTarget* target) { return *reinterpret_cast<const RenderTarget*>(target); }
inline const ExceptionInfo& of(const ERRF_ExceptionInfo* info) { return *reinterpret_cast<const ExceptionInfo*>(info); }
}
inline unsigned colorFormat(const C3D_FrameBuf* fb) { return layout::of(fb).colorFormat; }
inline unsigned depthFormat(const C3D_FrameBuf* fb) { return layout::of(fb).depthFormat; }
inline bool isBlock32(const C3D_FrameBuf* fb) { return layout::of(fb).block32Mode; }
inline unsigned colorWriteMask(const C3D_FrameBuf* fb) { return layout::of(fb).colorWriteMask; }
inline unsigned depthWriteMask(const C3D_FrameBuf* fb) { return layout::of(fb).depthWriteMask; }
inline bool ownsColorBuffer(const C3D_RenderTarget* target) { return layout::of(target).ownsColorBuffer; }
inline bool ownsDepthBuffer(const C3D_RenderTarget* target) { return layout::of(target).ownsDepthBuffer; }
inline unsigned exceptionKind(const ERRF_ExceptionInfo* info) { return layout::of(info).kind; }
inline u32 exceptionFsr(const ERRF_ExceptionInfo* info) { return layout::of(info).fsr; }
inline u32 exceptionFar(const ERRF_ExceptionInfo* info) { return layout::of(info).far; }
#else
// Host tests compile their stand-in SDK with this compiler's enum size.
inline unsigned colorFormat(const C3D_FrameBuf* fb) { return unsigned(fb->colorFmt); }
inline unsigned depthFormat(const C3D_FrameBuf* fb) { return unsigned(fb->depthFmt); }
inline bool isBlock32(const C3D_FrameBuf* fb) { return fb->block32; }
inline unsigned colorWriteMask(const C3D_FrameBuf* fb) { return fb->colorMask; }
inline unsigned depthWriteMask(const C3D_FrameBuf* fb) { return fb->depthMask; }
inline bool ownsColorBuffer(const C3D_RenderTarget* target) { return target->ownsColor; }
inline bool ownsDepthBuffer(const C3D_RenderTarget* target) { return target->ownsDepth; }
#endif
// The fields before the first enum member (buffers, width, height) keep their
// offsets and may be read directly; this is the frame buffer of a target.
inline const C3D_FrameBuf* frameBuf(const void* renderTarget) { return &static_cast<const C3D_RenderTarget*>(renderTarget)->frameBuf; }
}
