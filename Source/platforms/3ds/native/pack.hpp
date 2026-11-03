#pragma once
#include "contracts.hpp"
#include "storage.hpp"
#include <utility>
namespace dusk::pica {
enum class PackError {none,header,bounds,layout,checksum,missing,allocation};
struct MeshView {
    std::uint32_t id{};
    std::span<const std::byte> vertices{},indices{};
};
std::uint32_t crc32(std::span<const std::byte>,std::span<const std::byte> second={});
// Borrowed native mesh payloads with an owned, count-sized directory. No vertex
// casts, decompression or command translation. Retain file bytes until upload.
class MeshPack {
public:
    MeshPack()=default;
    MeshPack(const MeshPack&)=delete;
    MeshPack& operator=(const MeshPack&)=delete;
    MeshPack(MeshPack&& other) noexcept {*this=std::move(other);}
    MeshPack& operator=(MeshPack&& other) noexcept {
        if(this!=&other){entries=std::move(other.entries);bytes=std::exchange(other.bytes,{});count=std::exchange(other.count,0);}
        return *this;
    }
    static PackError open(std::span<const std::byte>,MeshPack&);
    PackError mesh(std::uint32_t id,MeshView&)const;
    unsigned size()const{return count;}
private:
    struct Entry{std::uint32_t id{},vertexOffset{},indexOffset{},vertexBytes{},indexBytes{},checksum{};};
    Storage<Entry> entries{}; // Copied metadata; never trust later file-header edits.
    std::span<const std::byte> bytes{};
    unsigned count{};
};
}
