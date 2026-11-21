#include "model_lod_store.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <sys/types.h>

namespace dusk::native {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
static_assert(std::endian::native == std::endian::little, "model level records are little endian");
u32 le16(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8); }
u32 le32(const u8* p) { return le16(p) | (le16(p + 2) << 16); }
u64 le64(const u8* p) { return u64(le32(p)) | (u64(le32(p + 4)) << 32); }
float lef(const u8* p) { const u32 v = le32(p); float f; std::memcpy(&f, &v, 4); return f; }
constexpr u32 maxModels = 1u << 20;
constexpr u32 maxRecordBytes = 16u << 20;
bool readAt(std::FILE* f, u64 offset, void* output, std::size_t size) {
    if (offset > u64(std::numeric_limits<off_t>::max())) return false;
    if (fseeko(f, off_t(offset), SEEK_SET)) return false;
    return std::fread(output, 1, size, f) == size;
}
}

std::uint32_t modelLodCrc32(std::uint32_t crc, const void* data, std::size_t size) {
    static u32 table[256];
    static bool made = false;
    if (!made) {
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
            table[n] = c;
        }
        made = true;
    }
    const auto* p = static_cast<const u8*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

bool ModelLodRecord::parse(std::span<const std::uint8_t> bytes) {
    data = {};
    shapeCount = groupCount = batchCount = 0;
    firstIndex.reset();
    if (bytes.size() < modelLodRecordHeader || le32(bytes.data()) != modelLodRecordMagic) return false;
    const u32 shapes = le16(bytes.data() + 4), groups = le16(bytes.data() + 6), batches = le32(bytes.data() + 8),
              words = le32(bytes.data() + 12);
    if (!shapes || !groups || !batches || batches > (1u << 20) || words > (64u << 20)) return false;
    const std::size_t gAt = modelLodRecordHeader + std::size_t(shapes) * modelLodShapeBytes;
    const std::size_t bAt = gAt + std::size_t(groups) * modelLodGroupBytes;
    const std::size_t iAt = bAt + std::size_t(batches) * modelLodBatchBytes;
    if (iAt > bytes.size() || std::size_t(words) * 2 != bytes.size() - iAt || iAt % 2) return false;
    std::unique_ptr<u32[]> first(new (std::nothrow) u32[batches]);
    if (!first) return false;
    // Groups cover the batches in order; shapes cover the groups in order.
    u32 nextGroup = 0, nextBatch = 0, nextWord = 0;
    for (u32 s = 0; s < shapes; ++s) {
        const u8* r = bytes.data() + modelLodRecordHeader + std::size_t(s) * modelLodShapeBytes;
        const u32 firstGroup = le16(r), count = r[2], levels = r[3];
        if (firstGroup != nextGroup || !count || levels > modelLodLevels || count > groups - nextGroup) return false;
        for (u32 l = 0; l < levels; ++l) {
            const float e = lef(r + 4 + 4 * l);
            if (!(e >= 0.0f && e < 1.0e7f)) return false;
        }
        for (u32 g = firstGroup; g < firstGroup + count; ++g) {
            const u8* gr = bytes.data() + gAt + std::size_t(g) * modelLodGroupBytes;
            const u32 firstBatch = le16(gr + 12), n = le16(gr + 14);
            // A shape without levels lists its groups (each list is checked) but no batches.
            if (firstBatch != nextBatch || (levels ? !n : n != 0) || n > batches - nextBatch || !le32(gr + 8)) return false;
            for (u32 b = firstBatch; b < firstBatch + n; ++b) {
                const u8* br = bytes.data() + bAt + std::size_t(b) * modelLodBatchBytes;
                const u32 vertices = le16(br), full = le32(br + 4);
                if (!vertices || !full || full % 3) return false;
                first[b] = nextWord;
                for (u32 l = 0; l < modelLodLevels; ++l) {
                    const u32 c = le32(br + 12 + 4 * l);
                    if (l >= levels ? c != 0 : (c % 3 || c > full || c > words - nextWord)) return false;
                    const u8* at = bytes.data() + iAt + std::size_t(nextWord) * 2;
                    for (u32 k = 0; k < c; ++k)
                        if (le16(at + 2 * k) >= vertices) return false;
                    nextWord += c;
                }
            }
            nextBatch += n;
        }
        nextGroup += count;
    }
    if (nextGroup != groups || nextBatch != batches || nextWord != words) return false;
    data = bytes;
    shapeCount = shapes; groupCount = groups; batchCount = batches;
    groupsAt = gAt; batchesAt = bAt; indicesAt = iAt;
    firstIndex = std::move(first);
    return true;
}
ModelLodShapeView ModelLodRecord::shape(unsigned i) const {
    ModelLodShapeView v;
    const u8* r = data.data() + modelLodRecordHeader + std::size_t(i) * modelLodShapeBytes;
    v.firstGroup = std::uint16_t(le16(r)); v.groups = r[2]; v.levels = r[3];
    for (unsigned l = 0; l < modelLodLevels; ++l) v.error[l] = l < v.levels ? lef(r + 4 + 4 * l) : 0.0f;
    return v;
}
ModelLodGroupView ModelLodRecord::group(unsigned i) const {
    ModelLodGroupView v;
    const u8* r = data.data() + groupsAt + std::size_t(i) * modelLodGroupBytes;
    v.listHash = le64(r); v.listBytes = le32(r + 8); v.firstBatch = std::uint16_t(le16(r + 12)); v.batches = std::uint16_t(le16(r + 14));
    return v;
}
ModelLodBatchView ModelLodRecord::batch(unsigned i) const {
    ModelLodBatchView v;
    const u8* r = data.data() + batchesAt + std::size_t(i) * modelLodBatchBytes;
    v.vertexCount = std::uint16_t(le16(r)); v.fullIndices = le32(r + 4); v.fullHash = le32(r + 8);
    u32 word = firstIndex[i];
    for (unsigned l = 0; l < modelLodLevels; ++l) {
        v.count[l] = le32(r + 12 + 4 * l);
        v.indices[l] = v.count[l] ? reinterpret_cast<const std::uint16_t*>(data.data() + indicesAt + std::size_t(word) * 2) : nullptr;
        word += v.count[l];
    }
    return v;
}

const char* ModelLodStore::statusText(Status status) {
    switch (status) {
    case Status::ready: return "ready";
    case Status::missing: return "no level file";
    case Status::malformed: return "malformed level file";
    case Status::stale: return "level file made for another runtime";
    case Status::ioError: return "level file read failed";
    case Status::noMemory: return "no memory for the level index";
    }
    return "unknown";
}
void ModelLodStore::close() {
    if (file) std::fclose(file);
    file = nullptr; index.reset(); count = largest = 0; dataOffset = fileBytes = 0;
}
ModelLodStore::Status ModelLodStore::open(const char* path) {
    close();
    if (!path) return Status::missing;
    file = std::fopen(path, "rb");
    if (!file) return Status::missing;
    const auto status = load();
    if (status != Status::ready) close();
    return status;
}
ModelLodStore::Status ModelLodStore::load() {
    if (fseeko(file, 0, SEEK_END)) return Status::ioError;
    const auto end = ftello(file);
    if (end < off_t(modelLodHeaderBytes + modelLodTrailerBytes)) return Status::malformed;
    fileBytes = u64(end);
    u8 trailer[modelLodTrailerBytes];
    if (!readAt(file, fileBytes - sizeof(trailer), trailer, sizeof(trailer))) return Status::ioError;
    const u32 trailerCrc = le32(trailer + 60);
    std::memset(trailer + 60, 0, 4);
    if (std::memcmp(trailer, modelLodTrailerMagic, 8) || modelLodCrc32(0, trailer, sizeof(trailer)) != trailerCrc ||
        le64(trailer + 16) != fileBytes) return Status::malformed;
    u8 header[modelLodHeaderBytes];
    if (!readAt(file, 0, header, sizeof(header))) return Status::ioError;
    const u32 headerCrc = le32(header + 60);
    std::memset(header + 60, 0, 4);
    if (std::memcmp(header, modelLodMagic, 8) || modelLodCrc32(0, header, sizeof(header)) != headerCrc ||
        le64(header + 24) != modelLodHeaderBytes || le64(header + 40) != fileBytes) return Status::malformed;
    if (le32(header + 8) != modelLodFormatVersion || le32(header + 12) != modelLodBatchRule) return Status::stale;
    count = le32(header + 16);
    largest = le32(header + 20);
    dataOffset = le64(header + 32);
    const u64 indexEnd = modelLodHeaderBytes + u64(count) * modelLodIndexBytes;
    if (!count || count > maxModels || largest > maxRecordBytes || dataOffset < indexEnd ||
        dataOffset > fileBytes - modelLodTrailerBytes) return Status::malformed;
    const std::size_t size = std::size_t(count) * modelLodIndexBytes;
    index.reset(new (std::nothrow) u8[size]);
    if (!index) return Status::noMemory;
    if (!readAt(file, modelLodHeaderBytes, index.get(), size)) return Status::ioError;
    if (modelLodCrc32(0, index.get(), size) != le32(header + 56)) return Status::malformed;
    const u64 dataBytes = fileBytes - modelLodTrailerBytes - dataOffset;
    for (u32 n = 0; n < count; ++n) {
        const u8* r = index.get() + std::size_t(n) * modelLodIndexBytes;
        const u64 offset = le32(r + 16), bytes = le32(r + 20);
        if (!bytes || bytes > largest || offset > dataBytes || bytes > dataBytes - offset) return Status::malformed;
        if (n && !(le64(r - modelLodIndexBytes) < le64(r))) return Status::malformed;
    }
    return Status::ready;
}
bool ModelLodStore::find(std::uint64_t key, ModelLodEntry& entry) const {
    entry = {};
    if (!file) return false;
    u32 first = 0, last = count;
    while (first < last) {
        const u32 mid = first + (last - first) / 2;
        const u8* r = index.get() + std::size_t(mid) * modelLodIndexBytes;
        const u64 probe = le64(r);
        if (probe < key) first = mid + 1;
        else if (key < probe) last = mid;
        else {
            entry.key = probe; entry.lists = le32(r + 8); entry.listBytes = le32(r + 12);
            entry.offset = dataOffset + le32(r + 16); entry.bytes = le32(r + 20);
            return true;
        }
    }
    return false;
}
bool ModelLodStore::read(const ModelLodEntry& entry, void* output, std::size_t size) {
    if (!file || !output || size != entry.bytes || entry.offset + size > fileBytes) return false;
    if (!readAt(file, entry.offset, output, size)) return false;
    ++readCount; readBytes += size;
    return true;
}
}
