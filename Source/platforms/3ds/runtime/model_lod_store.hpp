#pragma once
// Levels of detail of the disc's models, made on the Mac by
// Tools/3ds/model_lod.py from the owner's own zelda.twilight and copied by
// him next to it (sdmc:/3ds/DuskLight/zelda.lod). Without the file, or with
// one made for another runtime, the game draws every model as before.
//
// A level is an index list over the vertex records the runtime already makes
// for a batch of a display list (gx_commands.cpp drawBatch: unique packed
// tuples in first-use order, or the raw vertices of a lone primitive), so
// every level and the shadow caster share one decoded vertex buffer and no
// vertex is invented. The file never carries vertex data.
//
// Key: one record per J3D model file, found by a fingerprint of its display
// lists as the loaded model holds them (modelLodListHash of each list, in
// shape and matrix-group order, folded by modelLodCombine). Each list is
// checked again by its own hash and size, and each batch by the vertex count
// and the hash of the full index list the runtime made for it: a record that
// does not describe what the runtime draws is never used.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>

namespace dusk::native {
inline constexpr std::uint32_t modelLodFormatVersion = 1;
// The batching the levels were cut for: CommandProcessor::drawBatch with the
// scratch game_startup.cpp gives it (4096 vertices, 64 KiB packed vertices,
// 16 Ki indices, deduplication), lone primitives expanded by
// aurora::gx::dl::expand_triangles. Bump when either changes: a store made
// for another rule is refused as stale.
inline constexpr std::uint32_t modelLodBatchRule = 1;
inline constexpr unsigned modelLodLevels = 3;

namespace model_lod_hash {
inline constexpr std::uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
inline std::uint32_t rotl(std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline std::uint32_t load32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
inline std::uint32_t avalanche(std::uint32_t h) { h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16; return h; }
}
// The texture store's hash design (four xxHash32 lanes over 16-byte stripes,
// two finalizations for 64 bits), its own copy so that neither file depends
// on the other. Inline: the draw adapter checks batches with it.
inline std::uint64_t modelLodHash(const void* data, std::size_t n) {
    using namespace model_lod_hash;
    using u32 = std::uint32_t;
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    u32 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0u - P1;
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        v1 = rotl(v1 + load32(p + i) * P2, 13) * P1;
        v2 = rotl(v2 + load32(p + i + 4) * P2, 13) * P1;
        v3 = rotl(v3 + load32(p + i + 8) * P2, 13) * P1;
        v4 = rotl(v4 + load32(p + i + 12) * P2, 13) * P1;
    }
    u32 h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18) + u32(n);
    u32 g = rotl(v1 ^ v3, 5) * P3 + rotl(v2 ^ v4, 11) * P5 + (v1 + v2 + v3 + v4) + (u32(n) * P4);
    for (; i + 4 <= n; i += 4) {
        h = rotl(h + load32(p + i) * P3, 17) * P4;
        g = rotl(g ^ (load32(p + i) * P5), 13) * P1;
    }
    for (; i < n; ++i) {
        h = rotl(h + p[i] * P5, 11) * P1;
        g = rotl(g + p[i] * P2, 7) * P3;
    }
    return (std::uint64_t(avalanche(h)) << 32) | avalanche(g ^ (h >> 7));
}
// The hash of a display list as the key and the record use it.
inline std::uint64_t modelLodListHash(const void* list, std::uint32_t bytes) { return modelLodHash(list, bytes); }
// Folds one list into a model key (start from modelLodKeySeed).
inline constexpr std::uint64_t modelLodKeySeed = 0x4d4f444c4c4f4431ull;
inline std::uint64_t modelLodCombine(std::uint64_t key, std::uint64_t listHash, std::uint32_t listBytes) {
    key ^= listHash + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
    key = (key ^ (key >> 31)) * 0xbf58476d1ce4e5b9ull;
    key ^= std::uint64_t(listBytes) * 0x94d049bb133111ebull;
    return key ^ (key >> 29);
}
// Hash of a full index list (the record's check against the runtime's batch).
inline std::uint32_t modelLodIndexHash(const std::uint16_t* indices, std::uint32_t count) {
    return std::uint32_t(modelLodHash(indices, std::size_t(count) * sizeof(std::uint16_t))) ^ count;
}

// ---- Record of one model (little endian) ----
// header 16:  u32 magic 'DLLR', u16 shapes, u16 groups, u32 batches, u32 index words
// shapes:     16 bytes each: u16 first group, u8 groups, u8 levels, f32 error[3]
//             (model units: the largest distance of a point of the shape to
//             its reduced surface at that level)
// groups:     16 bytes each: u64 list hash, u32 list bytes, u16 first batch, u16 batches
//             (0 for the groups of a shape without levels)
// batches:    24 bytes each: u16 vertex records, u16 0, u32 full indices,
//             u32 full index hash, u32 indices of level 1..3 (0: the batch
//             keeps its full list at that level)
// indices:    u16, batch after batch, level after level (levels of its shape)
inline constexpr std::uint32_t modelLodRecordMagic = 0x524c4c44; // "DLLR"
inline constexpr std::size_t modelLodRecordHeader = 16, modelLodShapeBytes = 16, modelLodGroupBytes = 16,
                             modelLodBatchBytes = 24;
struct ModelLodShapeView { std::uint16_t firstGroup{}; std::uint8_t groups{}, levels{}; std::array<float, modelLodLevels> error{}; };
struct ModelLodGroupView { std::uint64_t listHash{}; std::uint32_t listBytes{}; std::uint16_t firstBatch{}, batches{}; };
struct ModelLodBatchView {
    std::uint16_t vertexCount{};
    std::uint32_t fullIndices{}, fullHash{};
    std::array<std::uint32_t, modelLodLevels> count{};
    std::array<const std::uint16_t*, modelLodLevels> indices{}; // into the record bytes
};
// Checks every bound of a record (counts, offsets, every index below its
// batch's vertex count, whole triangles) and gives typed views into it.
class ModelLodRecord {
public:
    bool parse(std::span<const std::uint8_t> bytes);
    unsigned shapes() const { return shapeCount; }
    unsigned groups() const { return groupCount; }
    unsigned batches() const { return batchCount; }
    ModelLodShapeView shape(unsigned i) const;
    ModelLodGroupView group(unsigned i) const;
    ModelLodBatchView batch(unsigned i) const;
private:
    std::span<const std::uint8_t> data;
    unsigned shapeCount = 0, groupCount = 0, batchCount = 0;
    std::size_t groupsAt = 0, batchesAt = 0, indicesAt = 0;
    std::unique_ptr<std::uint32_t[]> firstIndex; // per batch: first u16 of its levels
};

// ---- Store file ----
// [header 64][index 24 * models][records, each 4-aligned][trailer 64]
// header: magic "DLMLOD01", u32 format version, u32 batch rule, u32 models,
//         u32 largest record, u64 index offset (64), u64 data offset,
//         u64 file bytes, u64 0, u32 index CRC-32, u32 header CRC-32 (this
//         field 0)
// index:  u64 model key, u32 lists, u32 list bytes, u32 record offset (from
//         the data offset), u32 record bytes; sorted by key, keys unique
// trailer: magic "DLMLODTR", u32 format version, u32 0, u64 file bytes,
//         zeros, u32 CRC-32 of the trailer at byte 60 (this field 0)
inline constexpr char modelLodMagic[8] = {'D', 'L', 'M', 'L', 'O', 'D', '0', '1'};
inline constexpr char modelLodTrailerMagic[8] = {'D', 'L', 'M', 'L', 'O', 'D', 'T', 'R'};
inline constexpr std::size_t modelLodHeaderBytes = 64, modelLodIndexBytes = 24, modelLodTrailerBytes = 64;
std::uint32_t modelLodCrc32(std::uint32_t crc, const void* data, std::size_t size);

struct ModelLodEntry {
    std::uint64_t key{};
    std::uint32_t lists{}, listBytes{};
    std::uint64_t offset{}; // absolute, in the file
    std::uint32_t bytes{};
};
class ModelLodStore {
public:
    enum class Status : std::uint8_t { ready, missing, malformed, stale, ioError, noMemory };
    ModelLodStore() = default;
    ModelLodStore(const ModelLodStore&) = delete;
    ModelLodStore& operator=(const ModelLodStore&) = delete;
    ~ModelLodStore() { close(); }
    // Reads and checks the header, the index and the trailer; keeps the index
    // (24 bytes per model) and the open file, nothing else.
    Status open(const char* path);
    void close();
    bool ready() const { return file != nullptr; }
    std::uint32_t models() const { return count; }
    std::size_t indexBytes() const { return std::size_t(count) * modelLodIndexBytes; }
    std::uint32_t largestRecord() const { return largest; }
    // Binary search, no I/O.
    bool find(std::uint64_t key, ModelLodEntry& entry) const;
    // Reads one record (one seek and one read). One thread at a time.
    bool read(const ModelLodEntry& entry, void* output, std::size_t size);
    std::uint64_t reads() const { return readCount; }
    std::uint64_t bytesRead() const { return readBytes; }
    static const char* statusText(Status status);
private:
    std::FILE* file = nullptr;
    std::unique_ptr<std::uint8_t[]> index;
    std::uint32_t count = 0, largest = 0;
    std::uint64_t dataOffset = 0, fileBytes = 0, readCount = 0, readBytes = 0;
    Status load();
};
}
