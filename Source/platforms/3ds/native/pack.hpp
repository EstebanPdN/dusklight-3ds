#pragma once
#include "contracts.hpp"
namespace dusk::pica {
enum class PackError {none,header,bounds,layout,checksum,missing};
struct MeshView {
    std::uint32_t id{};
    std::span<const std::byte> vertices{},indices{};
};
std::uint32_t crc32(std::span<const std::byte>,std::span<const std::byte> second={});
// Borrowed native mesh pack. No allocation, vertex casts, decompression or
// command translation. Caller retains file bytes until the chosen mesh uploads.
class MeshPack {
public:
    static PackError open(std::span<const std::byte>,MeshPack&);
    PackError mesh(std::uint32_t id,MeshView&)const;
    unsigned size()const{return count;}
private:
    struct Entry{std::uint32_t id{},vertexOffset{},indexOffset{},vertexBytes{},indexBytes{},checksum{};};
    std::array<Entry,64> entries{}; // Own metadata; never trust later file-header edits.
    std::span<const std::byte> bytes{};
    unsigned count{};
};
}
