#pragma once
#include "pack.hpp"
namespace dusk::pica {
// Native image storage only. Sampler and material semantics are deliberately
// separate and must be supplied by a native material, never inferred at draw.
struct TextureImage {
    std::uint16_t width{},height{};
    TextureFormat format{};
    unsigned mipLevels{};
};
struct TextureView {
    std::uint32_t id{};
    TextureImage image{};
    std::span<const std::byte> payload{};
};
class TexturePack {
public:
    static PackError open(std::span<const std::byte>,TexturePack&);
    PackError image(std::uint32_t id,TextureView&)const;
    unsigned size()const{return count;}
private:
    struct Entry {
        std::uint32_t id{},offset{},size{},checksum{};
        TextureImage image{};
    };
    std::array<Entry,64> entries{};
    std::span<const std::byte> bytes{};
    unsigned count{};
};
}
