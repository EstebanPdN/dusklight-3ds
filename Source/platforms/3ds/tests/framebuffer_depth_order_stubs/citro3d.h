#pragma once
#include <3ds.h>
enum {GPU_RB_RGBA8=1,GPU_RB_DEPTH24_STENCIL8=2,C3D_CLEAR_ALL=3,GX_CMDLIST_FLUSH=4,GX_TRANSFER_FMT_RGBA8=5,GX_TRANSFER_FMT_RGB8=6,GX_TRANSFER_SCALE_NO=0};
#define GX_BUFFER_DIM(w,h) ((u32(w)<<16)|u32(h))
#define GX_TRANSFER_FLIP_VERT(x) (0u)
#define GX_TRANSFER_OUT_TILED(x) (0u)
#define GX_TRANSFER_RAW_COPY(x) (0u)
#define GX_TRANSFER_IN_FORMAT(x) (0u)
#define GX_TRANSFER_OUT_FORMAT(x) (0u)
#define GX_TRANSFER_SCALING(x) (0u)
struct C3D_FrameBuf {
    unsigned width{},height{};
    int colorFmt=GPU_RB_RGBA8,depthFmt=GPU_RB_DEPTH24_STENCIL8;
    bool block32{};
    void* colorBuf{};
    void* depthBuf{};
};
struct C3D_RenderTarget {C3D_FrameBuf frameBuf;};
C3D_RenderTarget* C3D_RenderTargetCreate(unsigned,unsigned,int,int);
bool C3D_FrameDrawOn(C3D_RenderTarget*);
void C3D_FrameSplit(unsigned);
void C3D_RenderTargetClear(C3D_RenderTarget*,unsigned,u32,u32);
void C3D_RenderTargetDetachOutput(C3D_RenderTarget*);
void C3D_RenderTargetSetOutput(C3D_RenderTarget*,int,int,u32);
void C3D_SyncDisplayTransfer(u32*,u32,u32*,u32,u32);
void C3D_SyncTextureCopy(u32*,u32,u32*,u32,std::size_t,u32);
