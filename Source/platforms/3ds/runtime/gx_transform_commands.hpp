#pragma once
#include <cstdint>
namespace dusk::native::gx {
// Return false if no command stream is selected. Otherwise validate and update
// the same register bank used by J3D lists, or fail explicitly through OSPanic.
bool commandProjection(const float* matrix,unsigned type);
bool commandViewport(float x,float y,float width,float height,float nearZ,float farZ);
bool commandPositionMatrix(const float* matrix,std::uint32_t id);
bool commandNormalMatrix(const float* matrix,std::uint32_t id);
bool commandTextureMatrix(const float* matrix,std::uint32_t id,unsigned type);
bool commandCurrentMatrix(std::uint32_t id);
bool commandScissor(std::uint32_t x,std::uint32_t y,std::uint32_t width,std::uint32_t height);
bool commandBlend(unsigned mode,unsigned source,unsigned destination,unsigned logic);
bool commandDepth(unsigned enabled,unsigned compare,unsigned write);
bool commandAlpha(unsigned compare0,std::uint8_t ref0,unsigned op,unsigned compare1,std::uint8_t ref1);
bool commandCull(unsigned mode);
bool commandZLocation(unsigned beforeTexture);
bool commandLineWidth(std::uint8_t width,unsigned offsets);
// Unported typed entry points must not silently mutate the old J2D state while
// a command stream is selected. The existing diagnostic uses no command stream.
void requireImmediateMode(const char* call);
// Typed resource objects are shared by both draw paths. Finish earlier stream
// bytes before changing object/map lifetimes; this does not emulate BP pointers.
void finishCommandBoundary(const char* call);
void commandTextureBinding(unsigned unit);
}
