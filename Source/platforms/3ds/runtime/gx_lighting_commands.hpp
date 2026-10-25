#pragma once
#include <cstdint>
namespace dusk::native::gx {
bool commandNumTexGens(unsigned count);
bool commandTexCoordGen(unsigned destination,unsigned function,unsigned source,std::uint32_t matrix,
                        unsigned normalize,std::uint32_t postMatrix);
bool commandNumChannels(unsigned count);
bool commandChannelControl(unsigned channel,unsigned enabled,unsigned ambient,unsigned material,
                           std::uint32_t lights,unsigned diffuse,unsigned attenuation);
bool commandChannelColor(unsigned channel,std::uint32_t rgba,bool material);
}
