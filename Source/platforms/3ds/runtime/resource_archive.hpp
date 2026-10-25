#pragma once
#include <cstddef>
#include <cstdint>

namespace dusk::native {
enum class ResourceStatus { ready, notReady, notFound, ambiguous, unsupported, malformed,
                            truncated, limitExceeded, outputTooSmall, overlappingBuffers };
const char* resourceStatusText(ResourceStatus status);
// No allocation. Both size discovery and decoding enforce the caller's budget.
ResourceStatus yaz0ExpandedSize(const void* input, std::size_t inputBytes,
                                std::size_t limit, std::uint32_t& expandedBytes);
// Input/output must not overlap. On failure output may be partial and written=0.
ResourceStatus decodeYaz0(const void* input, std::size_t inputBytes, void* output,
                          std::size_t capacity, std::size_t limit, std::size_t& written);

struct ResourceSpan { const unsigned char* data = nullptr; std::size_t size = 0; };
struct RarcLimits {
    std::size_t maxArchiveBytes;
    std::uint32_t maxNodes, maxEntries, maxNameBytes;
};
struct RarcEntry {
    const char* name = nullptr;
    ResourceSpan contents;
    std::uint16_t id = 0;
    std::uint8_t flags = 0;
    bool directory = false;
};
// Borrowed immutable data; caller keeps the source alive and unchanged. No
// relocation, extraction, heap allocation or implicit inner-file decompression.
// Open/find are loading-worker work; retain validated spans for subsequent use.
class RarcArchive {
    const unsigned char* memory = nullptr;
    std::size_t bytes = 0, nodeOffset = 0, fileOffset = 0, stringOffset = 0, dataOffset = 0;
    std::uint32_t nodes = 0, files = 0, stringBytes = 0, dataBytes = 0;
    RarcLimits limits{};
    ResourceStatus validate();
    const char* name(std::uint32_t offset) const;
public:
    ResourceStatus open(const void* input, std::size_t inputBytes, const RarcLimits& budget);
    std::uint32_t nodeCount() const { return nodes; }
    std::uint32_t entryCount() const { return files; }
    ResourceStatus entry(std::uint32_t index, RarcEntry& output) const;
    // Basename lookup; duplicate matches are reported rather than guessed.
    ResourceStatus findFile(const char* basename, RarcEntry& output) const;
};

struct BtiInfo {
    std::uint16_t width = 0, height = 0, paletteEntries = 0;
    std::uint8_t format = 0, mipCount = 0, paletteFormat = 0;
    ResourceSpan image, palette;
};
// Metadata and padded GX mip extents only; does not decode or upload pixels.
ResourceStatus inspectBti(ResourceSpan file, std::uint32_t maxDimension, BtiInfo& output);
}
