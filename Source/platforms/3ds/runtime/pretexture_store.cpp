#include "pretexture_store.hpp"
#include <array>
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
static_assert(std::endian::native == std::endian::little, "pretexture records are little endian");
constexpr u32 P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
inline u32 rotl(u32 x, int r) { return (x << r) | (x >> (32 - r)); }
inline u32 load32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
inline u32 avalanche(u32 h) { h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16; return h; }
u32 le16(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8); }
u32 le32(const u8* p) { return le16(p) | (le16(p + 2) << 16); }
u64 le64(const u8* p) { return u64(le32(p)) | (u64(le32(p + 4)) << 32); }
unsigned padded(unsigned n) { unsigned p = 8; while (p < n) p *= 2; return p; }
bool contains(u64 limit, u64 offset, u64 size) { return offset <= limit && size <= limit - offset; }
constexpr u32 maxEntries = 1u << 20;
}

// Four xxHash32 lanes over 16-byte stripes; two different finalizations of the
// 128-bit lane state give the 64-bit result. Images are multiples of 32 bytes,
// so the tail loops only run for palettes.
std::uint64_t pretextureImageHash(std::span<const std::uint8_t> bytes) {
    const u8* p = bytes.data();
    const std::size_t n = bytes.size();
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
    return (u64(avalanche(h)) << 32) | avalanche(g ^ (h >> 7));
}
std::uint32_t pretexturePaletteHash(std::span<const std::uint8_t> bytes) {
    return bytes.empty() ? 0 : u32(pretextureImageHash(bytes)) | 1u;
}
bool pretextureKey(std::span<const std::uint8_t> bti, PretextureKey& key) {
    key = {};
    NativeTexturePlan plan;
    if (!inspectNativeTexture(bti, plan)) return false;
    key.imageHash = pretextureImageHash(bti.subspan(plan.dataOffset, plan.sourceBytes));
    if (plan.paletteEntries)
        key.paletteHash = pretexturePaletteHash(bti.subspan(plan.paletteOffset, std::size_t(plan.paletteEntries) * 2));
    key.sourceBytes = u32(plan.sourceBytes);
    key.width = std::uint16_t(plan.originalWidth); key.height = std::uint16_t(plan.originalHeight);
    key.paletteEntries = plan.paletteEntries;
    key.gxFormat = plan.gxFormat; key.mipCount = plan.originalMipCount; key.paletteFormat = plan.paletteFormat;
    return true;
}
bool pretextureServes(const PretextureEntry& entry, const NativeTexturePlan& plan, std::size_t& offset, std::size_t& bytes) {
    offset = bytes = 0;
    const auto& k = entry.key;
    if (entry.variant != PretextureVariant::lossless || unsigned(entry.format) > 4 ||
        plan.gxFormat != k.gxFormat || plan.originalWidth != k.width || plan.originalHeight != k.height ||
        plan.originalMipCount != k.mipCount || plan.paletteEntries != k.paletteEntries ||
        std::uint8_t(plan.format) != std::uint8_t(entry.format)) return false;
    if (plan.sourceMip == 0) {
        if (plan.mipCount != k.mipCount || plan.width != k.width || plan.height != k.height ||
            plan.outputBytes != entry.bytes) return false;
        bytes = entry.bytes; return true;
    }
    if (plan.sourceMip == 1) {
        // Authored mip 1 onward: the tail of the full chain (power-of-two
        // extents, so every level keeps its own padded size).
        const std::size_t level0 = std::size_t(padded(k.width)) * padded(k.height) *
                                   nativeTextureBytesPerPixel(plan.format);
        if (k.mipCount < 2 || plan.mipCount != k.mipCount - 1 || level0 + plan.outputBytes != entry.bytes) return false;
        offset = level0; bytes = plan.outputBytes; return true;
    }
    return false; // box half (sourceMip 2) has other pixels
}
std::uint32_t pretextureCrc32(std::uint32_t crc, const void* data, std::size_t size) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256; ++n) { u32 c = n; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u))); t[n] = c; }
        return t;
    }();
    const auto* p = static_cast<const u8*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

bool PretextureCanary::check(std::span<const std::uint8_t> bti, const NativeTexturePlan& plan, std::span<const std::uint8_t> stored) {
    if (broken) return false;
    if (plan.gxFormat >= 16 || unsigned(plan.format) >= 8 || plan.sourceMip >= 2) { broken = true; return false; }
    auto& count = seen[plan.gxFormat][unsigned(plan.format)][plan.sourceMip];
    if (count >= samples) return true;
    if (stored.size() != plan.outputBytes) { broken = true; return false; }
    // Two prefills tell the bytes the lean decode writes from those it skips.
    std::unique_ptr<u8[]> a(new (std::nothrow) u8[plan.outputBytes]), b(new (std::nothrow) u8[plan.outputBytes]);
    if (!a || !b) return true; // no memory: skip the sample, keep the store
    std::memset(a.get(), 0x00, plan.outputBytes); std::memset(b.get(), 0xff, plan.outputBytes);
    if (!decodeNativeTexture(bti, plan, {a.get(), plan.outputBytes}, true) ||
        !decodeNativeTexture(bti, plan, {b.get(), plan.outputBytes}, true)) { broken = true; return false; }
    for (std::size_t n = 0; n < plan.outputBytes; ++n)
        if (a[n] == b[n] && a[n] != stored[n]) { broken = true; return false; }
    ++count; ++checks;
    return true;
}

PretextureStore::~PretextureStore() { close(); }
void PretextureStore::close() {
    if (file) std::fclose(file);
    file = nullptr; index.reset(); count = 0; sectionOffset = sectionBytes = dataOffset = 0;
}
const char* PretextureStore::statusText(Status status) {
    switch (status) {
    case Status::ready: return "ready";
    case Status::missing: return "no texture section";
    case Status::malformed: return "malformed texture section";
    case Status::stale: return "texture section from another decoder version";
    case Status::ioError: return "texture section read failed";
    case Status::noMemory: return "no memory for the texture index";
    }
    return "unknown";
}
namespace {
bool readAt(std::FILE* f, u64 offset, void* output, std::size_t size) {
    if (offset > u64(std::numeric_limits<off_t>::max())) return false;
    if (fseeko(f, off_t(offset), SEEK_SET)) return false;
    auto* out = static_cast<u8*>(output);
    for (std::size_t done = 0; done < size;) {
        const std::size_t n = size - done < 262144 ? size - done : 262144;
        if (std::fread(out + done, 1, n, f) != n) return false;
        done += n;
    }
    return true;
}
}
PretextureStore::Status PretextureStore::open(const char* path) {
    close();
    if (!path) return Status::missing;
    file = std::fopen(path, "rb");
    if (!file) return Status::missing;
    const auto status = load();
    if (status != Status::ready) close();
    return status;
}
PretextureStore::Status PretextureStore::load() {
    if (fseeko(file, 0, SEEK_END)) return Status::ioError;
    const auto end = ftello(file);
    if (end < off_t(pretextureHeaderBytes + pretextureTrailerBytes)) return Status::missing;
    const u64 size = u64(end);
    std::array<u8, pretextureTrailerBytes> trailer{};
    if (!readAt(file, size - trailer.size(), trailer.data(), trailer.size())) return Status::ioError;
    if (std::memcmp(trailer.data(), pretextureTrailerMagic, 8)) return Status::missing;
    const u32 trailerCrc = le32(trailer.data() + 60);
    std::memset(trailer.data() + 60, 0, 4);
    if (pretextureCrc32(0, trailer.data(), trailer.size()) != trailerCrc || le32(trailer.data() + 8) != pretextureFormatVersion)
        return Status::malformed;
    sectionBytes = le64(trailer.data() + 16);
    if (sectionBytes < pretextureHeaderBytes + pretextureTrailerBytes || sectionBytes > size) return Status::malformed;
    sectionOffset = size - sectionBytes;
    std::array<u8, pretextureHeaderBytes> header{};
    if (!readAt(file, sectionOffset, header.data(), header.size())) return Status::ioError;
    const u32 headerCrc = le32(header.data() + 60);
    std::memset(header.data() + 60, 0, 4);
    if (std::memcmp(header.data(), pretextureMagic, 8) || pretextureCrc32(0, header.data(), header.size()) != headerCrc ||
        le32(header.data() + 8) != pretextureFormatVersion || le32(header.data() + 16) != pretextureHeaderBytes ||
        le32(header.data() + 24) != pretextureRecordBytes || le64(header.data() + 32) != pretextureHeaderBytes ||
        le64(header.data() + 48) != sectionBytes) return Status::malformed;
    if (le32(header.data() + 12) != pretextureDecoderVersion) return Status::stale;
    const u32 entries = le32(header.data() + 20);
    dataOffset = le64(header.data() + 40);
    const u64 indexEnd = pretextureHeaderBytes + u64(entries) * pretextureRecordBytes;
    if (!entries || entries > maxEntries || dataOffset < indexEnd || dataOffset % 64 ||
        dataOffset > sectionBytes - pretextureTrailerBytes) return Status::malformed;
    const std::size_t indexSize = std::size_t(entries) * pretextureRecordBytes;
    index.reset(new (std::nothrow) unsigned char[indexSize]);
    if (!index) return Status::noMemory;
    if (!readAt(file, sectionOffset + pretextureHeaderBytes, index.get(), indexSize)) return Status::ioError;
    if (pretextureCrc32(0, index.get(), indexSize) != le32(header.data() + 56)) return Status::malformed;
    count = entries;
    // Every record: valid format, data inside the pixel area, strictly sorted.
    const u64 pixelBytes = sectionBytes - pretextureTrailerBytes - dataOffset;
    PretextureEntry previous{}, current{};
    for (u32 n = 0; n < count; ++n) {
        entryAt(n, current);
        const auto format = unsigned(current.format);
        if ((format > 4 && format != 16 && format != 17) || unsigned(current.variant) > 1 || !current.bytes ||
            !contains(pixelBytes, current.fileOffset - sectionOffset - dataOffset, current.bytes) ||
            current.fileOffset < sectionOffset + dataOffset) return Status::malformed;
        if (n) {
            const auto& a = previous.key; const auto& b = current.key;
            const auto tupleA = std::array<u64, 5>{a.imageHash, (u64(a.paletteHash) << 32) | a.sourceBytes,
                (u64(a.width) << 32) | (u64(a.height) << 16) | a.paletteEntries,
                (u64(a.gxFormat) << 16) | (u64(a.mipCount) << 8) | a.paletteFormat, u64(previous.variant)};
            const auto tupleB = std::array<u64, 5>{b.imageHash, (u64(b.paletteHash) << 32) | b.sourceBytes,
                (u64(b.width) << 32) | (u64(b.height) << 16) | b.paletteEntries,
                (u64(b.gxFormat) << 16) | (u64(b.mipCount) << 8) | b.paletteFormat, u64(current.variant)};
            if (!(tupleA < tupleB)) return Status::malformed;
        }
        previous = current;
    }
    return Status::ready;
}
void PretextureStore::entryAt(std::uint32_t at, PretextureEntry& e) const {
    const u8* r = index.get() + std::size_t(at) * pretextureRecordBytes;
    e.key.imageHash = le64(r); e.key.paletteHash = le32(r + 8); e.key.sourceBytes = le32(r + 12);
    e.key.width = std::uint16_t(le16(r + 16)); e.key.height = std::uint16_t(le16(r + 18));
    e.key.paletteEntries = std::uint16_t(le16(r + 20));
    e.key.gxFormat = r[22]; e.key.mipCount = r[23]; e.key.paletteFormat = r[24];
    e.variant = PretextureVariant(r[25]); e.format = PretextureFormat(r[26]); e.quality = r[27];
    e.fileOffset = sectionOffset + dataOffset + u64(le32(r + 28)) * 64;
    e.bytes = le32(r + 32); e.crc = le32(r + 36);
}
bool PretextureStore::find(const PretextureKey& key, PretextureVariant variant, PretextureEntry& entry) const {
    entry = {};
    if (!file || !count) return false;
    const auto target = std::array<u64, 5>{key.imageHash, (u64(key.paletteHash) << 32) | key.sourceBytes,
        (u64(key.width) << 32) | (u64(key.height) << 16) | key.paletteEntries,
        (u64(key.gxFormat) << 16) | (u64(key.mipCount) << 8) | key.paletteFormat, u64(variant)};
    u32 first = 0, last = count;
    while (first < last) {
        const u32 mid = first + (last - first) / 2;
        const u8* r = index.get() + std::size_t(mid) * pretextureRecordBytes;
        const auto probe = std::array<u64, 5>{le64(r), (u64(le32(r + 8)) << 32) | le32(r + 12),
            (u64(le16(r + 16)) << 32) | (u64(le16(r + 18)) << 16) | le16(r + 20),
            (u64(r[22]) << 16) | (u64(r[23]) << 8) | r[24], u64(r[25])};
        if (probe < target) first = mid + 1;
        else if (target < probe) last = mid;
        else { entryAt(mid, entry); return true; }
    }
    return false;
}
bool PretextureStore::read(const PretextureEntry& entry, std::size_t offset, void* output, std::size_t size) {
    if (!file || (!output && size) || offset > entry.bytes || size > entry.bytes - offset ||
        entry.fileOffset < sectionOffset || entry.fileOffset + entry.bytes > sectionOffset + sectionBytes) return false;
    if (!size) return true;
    if (!readAt(file, entry.fileOffset + offset, output, size)) return false;
    ++reads; bytesRead += size;
    return true;
}
}
