#pragma once
#include <cstddef>
#include <cstdint>
using u8=std::uint8_t;
using u16=std::uint16_t;
using u32=std::uint32_t;
#define R_FAILED(x) ((x)<0)
enum {GFX_TOP,GFX_LEFT};
void* linearAlloc(std::size_t);
void linearFree(void*);
int GSPGPU_InvalidateDataCache(void*,std::size_t);
int GSPGPU_FlushDataCache(void*,std::size_t);
u8* gfxGetFramebuffer(int,int,u16*,u16*);
void gfxScreenSwapBuffers(int,bool);
