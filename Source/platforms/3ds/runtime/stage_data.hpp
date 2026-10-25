#pragma once
#include "resource_archive.hpp"
#include <array>

namespace dusk::native {
struct StageChunk {
    std::array<char,4> tag{};
    std::uint32_t count = 0, offset = 0;
    ResourceSpan contents;
};
// Original DZS/DZR big-endian chunk table. Borrowed bytes remain unchanged;
// relocation is performed separately on caller-owned original-layout copies.
class StageData {
    ResourceSpan bytes;
    std::uint32_t chunks = 0;
public:
    ResourceStatus open(ResourceSpan source);
    std::uint32_t chunkCount() const { return chunks; }
    ResourceStatus chunk(std::uint32_t index, StageChunk& output) const;
    ResourceStatus find(const std::array<char,4>& tag, StageChunk& output) const;
    ResourceStatus require(const std::array<char,4>& tag, std::size_t stride,
                           StageChunk& output) const;
};
struct StageEnvironmentLayout {
    unsigned layer = 0;
    StageChunk stag, palette, pselect, environment, vrbox, roomLights;
};
// Canonical original record layouts; absent optional tables retain the original
// engine's fallback policy. Only validates; never relocates or assigns game state.
ResourceStatus inspectStageEnvironment(ResourceSpan stage, ResourceSpan room,
    unsigned roomNo, unsigned layer, StageEnvironmentLayout& output);
}
