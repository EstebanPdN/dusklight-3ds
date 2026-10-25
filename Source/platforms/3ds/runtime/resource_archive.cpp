#include "resource_archive.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace dusk::native {
namespace {
std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint16_t be16(const unsigned char* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
bool contains(std::uint64_t limit, std::uint64_t offset, std::uint64_t size) {
    return offset <= limit && size <= limit - offset;
}
bool overlap(std::uintptr_t a, std::size_t aBytes, std::uintptr_t b, std::size_t bBytes) {
    return aBytes && bBytes && (a <= b ? b - a < aBytes : a - b < bBytes);
}
unsigned char fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool equalName(const char* a, const char* b) {
    while (*a && *b && fold(*a) == fold(*b)) { ++a; ++b; }
    return !*a && !*b;
}
}
ResourceStatus yaz0ExpandedSize(const void* input, std::size_t inputBytes,
                                std::size_t limit, std::uint32_t& expandedBytes) {
    expandedBytes = 0;
    if (!input || inputBytes < 16) return ResourceStatus::truncated;
    const auto* source = static_cast<const unsigned char*>(input);
    if (std::memcmp(source, "Yaz0", 4)) return ResourceStatus::unsupported;
    const auto size = be32(source + 4);
    if (size > limit) return ResourceStatus::limitExceeded;
    expandedBytes = size;
    return ResourceStatus::ready;
}
ResourceStatus decodeYaz0(const void* input, std::size_t inputBytes, void* output,
                          std::size_t capacity, std::size_t limit, std::size_t& written) {
    written = 0;
    std::uint32_t expanded = 0;
    const auto status = yaz0ExpandedSize(input, inputBytes, limit, expanded);
    if (status != ResourceStatus::ready) return status;
    if (expanded > capacity || (!output && expanded)) return ResourceStatus::outputTooSmall;
    if (overlap(reinterpret_cast<std::uintptr_t>(input), inputBytes,
                reinterpret_cast<std::uintptr_t>(output), expanded)) return ResourceStatus::overlappingBuffers;
    const auto* source = static_cast<const unsigned char*>(input);
    auto* target = static_cast<unsigned char*>(output);
    std::size_t cursor = 16, produced = 0;
    while (produced < expanded) {
        if (cursor == inputBytes) return ResourceStatus::truncated;
        const auto controls = source[cursor++];
        for (unsigned mask = 0x80; mask && produced < expanded; mask >>= 1) {
            if (controls & mask) {
                if (cursor == inputBytes) return ResourceStatus::truncated;
                target[produced++] = source[cursor++];
            } else {
                if (!contains(inputBytes, cursor, 2)) return ResourceStatus::truncated;
                const auto a = source[cursor++], b = source[cursor++];
                const std::size_t distance = ((a & 15) << 8 | b) + 1;
                std::size_t count = a >> 4;
                if (!count) {
                    if (cursor == inputBytes) return ResourceStatus::truncated;
                    count = source[cursor++] + 18;
                } else count += 2;
                if (distance > produced || count > expanded - produced) return ResourceStatus::malformed;
                // Forward byte copies deliberately support overlapping backreferences.
                for (std::size_t i = 0; i < count; ++i, ++produced)
                    target[produced] = target[produced - distance];
            }
        }
    }
    written = produced;
    return ResourceStatus::ready;
}

const char* RarcArchive::name(std::uint32_t offset) const {
    if (offset >= stringBytes) return nullptr;
    const auto available = std::min<std::uint64_t>(std::uint64_t(stringBytes) - offset,
                                                  std::uint64_t(limits.maxNameBytes) + 1);
    const auto* start = memory + stringOffset + offset;
    const auto* end = static_cast<const unsigned char*>(std::memchr(start, 0, available));
    if (!end || start == end) return nullptr;
    for (auto* p = start; p != end; ++p) if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return nullptr;
    return reinterpret_cast<const char*>(start);
}
ResourceStatus RarcArchive::open(const void* input, std::size_t inputBytes, const RarcLimits& budget) {
    *this = {};
    if (!input || inputBytes < 64) return ResourceStatus::truncated;
    if (inputBytes > budget.maxArchiveBytes || !budget.maxNodes || !budget.maxEntries || !budget.maxNameBytes)
        return ResourceStatus::limitExceeded;
    memory = static_cast<const unsigned char*>(input); bytes = inputBytes; limits = budget;
    const auto status = validate();
    if (status != ResourceStatus::ready) *this = {};
    return status;
}
ResourceStatus RarcArchive::validate() {
    if (std::memcmp(memory, "RARC", 4)) return ResourceStatus::unsupported;
    const auto declared = be32(memory + 4), header = be32(memory + 8);
    if (declared > bytes) return ResourceStatus::truncated;
    if (declared < 64 || header < 32 || !contains(declared, header, 32)) return ResourceStatus::malformed;
    bytes = declared;
    const auto* info = memory + header;
    nodes = be32(info); files = be32(info + 8); stringBytes = be32(info + 16);
    if (!nodes || nodes > limits.maxNodes || files > limits.maxEntries) return ResourceStatus::limitExceeded;
    const std::uint64_t data = std::uint64_t(header) + be32(memory + 12);
    dataBytes = be32(memory + 16);
    if (!contains(bytes, data, dataBytes) || data < std::uint64_t(header) + 32) return ResourceStatus::malformed;
    dataOffset = data;
    const std::uint64_t tableStarts[] = {std::uint64_t(header) + be32(info + 4),
                                       std::uint64_t(header) + be32(info + 12),
                                       std::uint64_t(header) + be32(info + 20)};
    const std::uint64_t tableBytes[] = {std::uint64_t(nodes) * 16, std::uint64_t(files) * 20, stringBytes};
    if (!stringBytes) return ResourceStatus::malformed;
    for (unsigned i = 0; i < 3; ++i) {
        if (tableStarts[i] < std::uint64_t(header) + 32 || !contains(data, tableStarts[i], tableBytes[i]))
            return ResourceStatus::malformed;
        for (unsigned j = 0; j < i; ++j)
            if (tableBytes[i] && tableBytes[j] && tableStarts[i] < tableStarts[j] + tableBytes[j] &&
                tableStarts[j] < tableStarts[i] + tableBytes[i]) return ResourceStatus::malformed;
    }
    nodeOffset = tableStarts[0]; fileOffset = tableStarts[1]; stringOffset = tableStarts[2];
    for (std::uint32_t i = 0; i < nodes; ++i) {
        const auto* node = memory + nodeOffset + std::size_t(i) * 16;
        if (!name(be32(node + 4)) || !contains(files, be32(node + 12), be16(node + 10)))
            return ResourceStatus::malformed;
    }
    for (std::uint32_t i = 0; i < files; ++i) {
        const auto* entry = memory + fileOffset + std::size_t(i) * 20;
        const auto flagsName = be32(entry + 4), flags = flagsName >> 24;
        const auto* basename = name(flagsName & 0xffffff);
        if (!basename || ((flags & 3) != 1 && (flags & 3) != 2)) return ResourceStatus::malformed;
        const auto offset = be32(entry + 8), size = be32(entry + 12);
        if (flags & 2) {
            if (offset >= nodes && !(offset == UINT32_MAX && !std::strcmp(basename, "..")))
                return ResourceStatus::malformed;
        } else if (!std::strcmp(basename, ".") || !std::strcmp(basename, "..") || !contains(dataBytes, offset, size))
            return ResourceStatus::malformed;
    }
    return ResourceStatus::ready;
}
ResourceStatus RarcArchive::entry(std::uint32_t index, RarcEntry& output) const {
    output = {};
    if (!memory) return ResourceStatus::notReady;
    if (index >= files) return ResourceStatus::notFound;
    const auto* source = memory + fileOffset + std::size_t(index) * 20;
    const auto flagsName = be32(source + 4);
    output.name = name(flagsName & 0xffffff); output.flags = flagsName >> 24;
    output.id = be16(source); output.directory = (output.flags & 2) != 0;
    if (!output.directory) output.contents = {memory + dataOffset + be32(source + 8), be32(source + 12)};
    return ResourceStatus::ready;
}
ResourceStatus RarcArchive::findFile(const char* basename, RarcEntry& output) const {
    output = {};
    if (!memory) return ResourceStatus::notReady;
    if (!basename || !*basename) return ResourceStatus::notFound;
    bool found = false;
    for (std::uint32_t i = 0; i < files; ++i) {
        RarcEntry candidate;
        if (entry(i, candidate) != ResourceStatus::ready) return ResourceStatus::malformed;
        if (!candidate.directory && equalName(candidate.name, basename)) {
            if (found) { output = {}; return ResourceStatus::ambiguous; }
            output = candidate; found = true;
        }
    }
    return found ? ResourceStatus::ready : ResourceStatus::notFound;
}

ResourceStatus inspectBti(ResourceSpan file, std::uint32_t maxDimension, BtiInfo& output) {
    output = {};
    if (!file.data || file.size < 32) return ResourceStatus::truncated;
    const auto* p = file.data;
    const auto width = be16(p + 2), height = be16(p + 4);
    if (!width || !height) return ResourceStatus::malformed;
    if (width > maxDimension || height > maxDimension) return ResourceStatus::limitExceeded;
    unsigned blockWidth, blockHeight, blockBytes = 32;
    switch (p[0]) {
    case 0: case 8: case 14: blockWidth = blockHeight = 8; break;
    case 1: case 2: case 9: blockWidth = 8; blockHeight = 4; break;
    case 3: case 4: case 5: case 10: blockWidth = blockHeight = 4; break;
    case 6: blockWidth = blockHeight = 4; blockBytes = 64; break;
    default: return ResourceStatus::unsupported;
    }
    unsigned maxMips = 1;
    for (auto dimension = std::max(width, height); dimension > 1; dimension >>= 1) ++maxMips;
    if (!p[24] || p[24] > maxMips) return ResourceStatus::malformed;
    std::uint64_t imageBytes = 0;
    unsigned w = width, h = height;
    for (unsigned i = 0; i < p[24]; ++i) {
        imageBytes += std::uint64_t((w + blockWidth - 1) / blockWidth) * ((h + blockHeight - 1) / blockHeight) * blockBytes;
        w = std::max(1u, w >> 1); h = std::max(1u, h >> 1);
    }
    const auto imageOffset = be32(p + 28), paletteOffset = be32(p + 12);
    if (imageOffset < 32 || !contains(file.size, imageOffset, imageBytes)) return ResourceStatus::truncated;
    const auto colors = be16(p + 10);
    const bool indexed = p[0] == 8 || p[0] == 9 || p[0] == 10;
    if (indexed) {
        const unsigned maxColors = p[0] == 8 ? 16 : p[0] == 9 ? 256 : 16384;
        if (!p[8] || p[9] > 2 || !colors || colors > maxColors || paletteOffset < 32 ||
            !contains(file.size, paletteOffset, std::uint64_t(colors) * 2)) return ResourceStatus::malformed;
        if (overlap(imageOffset, imageBytes, paletteOffset, colors * 2)) return ResourceStatus::malformed;
    }
    output.width = width; output.height = height; output.format = p[0]; output.mipCount = p[24];
    output.image = {p + imageOffset, static_cast<std::size_t>(imageBytes)};
    if (indexed) {
        output.paletteFormat = p[9]; output.paletteEntries = colors;
        output.palette = {p + paletteOffset, std::size_t(colors) * 2};
    }
    return ResourceStatus::ready;
}
const char* resourceStatusText(ResourceStatus status) {
    switch (status) {
    case ResourceStatus::ready: return "Validated";
    case ResourceStatus::notReady: return "Archive is not open";
    case ResourceStatus::notFound: return "Entry not found";
    case ResourceStatus::ambiguous: return "Ambiguous entry name";
    case ResourceStatus::unsupported: return "Unsupported resource format";
    case ResourceStatus::malformed: return "Malformed resource";
    case ResourceStatus::truncated: return "Truncated resource";
    case ResourceStatus::limitExceeded: return "Resource exceeds caller budget";
    case ResourceStatus::outputTooSmall: return "Output buffer is too small";
    case ResourceStatus::overlappingBuffers: return "Input/output buffers overlap";
    }
    return "Unknown resource status";
}
}
