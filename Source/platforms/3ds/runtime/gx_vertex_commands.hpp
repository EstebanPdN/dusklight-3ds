#pragma once
#include <cstdint>
namespace dusk::native::gx {
bool commandClearDescriptors();
bool commandDescriptor(unsigned attribute,unsigned type);
bool commandVertexFormat(unsigned format,unsigned attribute,unsigned count,unsigned type,std::uint8_t fraction);
bool commandArray(unsigned attribute,const void* data,std::uint32_t size,std::uint8_t stride,bool little);
}
