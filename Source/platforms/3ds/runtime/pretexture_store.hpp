#pragma once
// Pre-converted textures stored by the Mac converter inside zelda.twilight
// (Build/alpha12/pretexture-design.md). The runtime keeps only the index in
// RAM; pixels stay on the SD and are read on demand into the texture buffer.
//
// The key is computed from the bytes the runtime already holds (a header-
// relative BTI view: the normalized copy of texture_resources.cpp or the
// original archive header), so no archive path or load order is involved.
// Header offsets, sampler fields (wrap, filter, LOD) and padding outside the
// image/palette never enter the key: TEX1 headers that share texels share it.
//
// Lossless entries hold exactly decodeNativeTexture(bti, plan, out, false) for
// the compact plan (inspectNativeTexture(..., compact=true)), all mips, with
// full clamp padding (a superset of every byte the lean decode writes). ETC
// entries are optional lossy variants that the runtime cannot produce itself.
#include "texture_gx.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

namespace dusk::native {
// Container layout version, and the version of the decoder output the
// lossless pixels were produced with. Bump pretextureDecoderVersion whenever
// texture_gx.cpp changes any byte decodeNativeTexture writes or any compact
// plan choice: a store built with another value is refused (stale), and the
// runtime decodes as before.
inline constexpr std::uint32_t pretextureFormatVersion = 1;
inline constexpr std::uint32_t pretextureDecoderVersion = 1;

enum class PretextureFormat : std::uint8_t {
    rgba8 = 0, la8 = 1, la4 = 2, rgb565 = 3, rgba5551 = 4, // = NativeTextureFormat
    etc1 = 16, etc1a4 = 17,
};
enum class PretextureVariant : std::uint8_t { lossless = 0, etc = 1 };

struct PretextureKey {
    std::uint64_t imageHash{};      // all GX mips, as authored
    std::uint32_t paletteHash{};    // 0 when not indexed
    std::uint32_t sourceBytes{};    // GX image bytes of all mips
    std::uint16_t width{}, height{}, paletteEntries{};
    std::uint8_t gxFormat{}, mipCount{}, paletteFormat{};
    bool operator==(const PretextureKey&) const = default;
};
// Header-relative BTI view (the same span decodeNativeTexture takes). False for
// anything inspectNativeTexture refuses. Cost: one pass over the image bytes
// (four 32-bit lanes, about 1.5 cycles per byte), no allocation.
bool pretextureKey(std::span<const std::uint8_t> bti, PretextureKey& key);
// The hash the key uses, exposed for the converter and the tests.
std::uint64_t pretextureImageHash(std::span<const std::uint8_t> bytes);
std::uint32_t pretexturePaletteHash(std::span<const std::uint8_t> bytes);

struct PretextureEntry {
    PretextureKey key;
    PretextureFormat format{};
    PretextureVariant variant{};
    std::uint8_t quality{};         // RGB PSNR in dB against the GX image; 255 = lossless
    std::uint64_t fileOffset{};     // absolute offset of the pixels in the opened file
    std::uint32_t bytes{};          // stored bytes (all mips, padded)
    std::uint32_t crc{};            // CRC-32 of the stored bytes (offline checks)
};
// Lossless entry against the plan the runtime computed for the same bytes
// (inspectForCache). Serves the full plan and the authored-mip-1 half plan
// (sourceMip 1: the tail of the full chain). Returns the byte range of the
// entry to copy; false when the plan differs (non-compact, RGB565 reference
// mode, box half, another format): the caller decodes as before.
bool pretextureServes(const PretextureEntry& entry, const NativeTexturePlan& plan,
                      std::size_t& offset, std::size_t& bytes);

// Run-time proof that the section matches this build's decoder: the first
// `samples` served images of every (GX format, PICA format, source mip) are
// also decoded and compared byte for byte (only the bytes the lean decode
// writes). One mismatch disables the store for the session (failed() true);
// the caller then decodes that image as before. Costs one decode and one
// scratch allocation per checked image, nothing afterwards.
class PretextureCanary {
public:
    explicit PretextureCanary(unsigned samples = 2) : samples(samples) {}
    // stored: the bytes read for this plan (pretextureServes range).
    bool check(std::span<const std::uint8_t> bti, const NativeTexturePlan& plan, std::span<const std::uint8_t> stored);
    bool failed() const { return broken; }
    unsigned checked() const { return checks; }
private:
    unsigned samples, checks = 0;
    bool broken = false;
    std::uint8_t seen[16][8][2]{}; // [gxFormat][picaFormat][sourceMip]
};

class PretextureStore {
public:
    enum class Status : std::uint8_t { ready, missing, malformed, stale, ioError, noMemory };
    PretextureStore() = default;
    PretextureStore(const PretextureStore&) = delete;
    PretextureStore& operator=(const PretextureStore&) = delete;
    ~PretextureStore();
    // Finds the section through the trailer in the last 64 bytes of the file
    // (zelda.twilight with a texture range, or a stand-alone section file).
    // A file without one is `missing`, never an error. Reads the header and
    // the index once (index bytes = 40 per entry), keeps no other data.
    Status open(const char* path);
    void close();
    bool ready() const { return file != nullptr; }
    std::uint32_t entryCount() const { return count; }
    std::size_t indexBytes() const { return std::size_t(count) * 40; }
    // Binary search, no I/O, no allocation. Thread-safe against other finds.
    bool find(const PretextureKey& key, PretextureVariant variant, PretextureEntry& entry) const;
    // Reads [offset, offset+size) of the entry's pixels. One thread at a time
    // (the FILE position is shared); the caller serializes. Not the Disc FILE.
    bool read(const PretextureEntry& entry, std::size_t offset, void* output, std::size_t size);
    std::uint64_t readOperations() const { return reads; }
    std::uint64_t readBytes() const { return bytesRead; }
    static const char* statusText(Status status);
private:
    struct Record;
    std::FILE* file = nullptr;
    std::uint64_t sectionOffset = 0, sectionBytes = 0, dataOffset = 0;
    std::unique_ptr<unsigned char[]> index;
    std::uint32_t count = 0;
    std::uint64_t reads = 0, bytesRead = 0;
    Status load();
    void entryAt(std::uint32_t at, PretextureEntry& entry) const;
};

// ---- Container constants, shared with the Mac converter (Tools/3ds). ----
// Section: [header 64][records 40*n][pad to 64][pixels, each 64-aligned][trailer 64]
// Header (little endian): magic "DLPTEX01", u32 format version, u32 decoder
// version, u32 header bytes (64), u32 entries, u32 record bytes (40), u32 flags,
// u64 index offset (64), u64 data offset, u64 section bytes (incl. trailer),
// u32 index CRC-32, u32 header CRC-32 (with this field zero).
// Record: u64 imageHash, u32 paletteHash, u32 sourceBytes, u16 width, u16 height,
// u16 paletteEntries, u8 gxFormat, u8 mipCount, u8 paletteFormat, u8 variant,
// u8 picaFormat, u8 quality, u32 data offset in 64-byte units from the data
// offset, u32 stored bytes, u32 CRC-32 of the stored bytes. Sorted by
// (key fields in that order, variant); keys are unique per variant.
// Trailer (last 64 bytes of the section and of the file): magic "DLPTEXTR",
// u32 format version, u32 0, u64 section bytes, u64 0, ..., u32 CRC-32 of the
// trailer with this field zero at byte 60. The section starts at
// fileSize - sectionBytes (it is the last bundle range of zelda.twilight).
inline constexpr char pretextureMagic[8] = {'D','L','P','T','E','X','0','1'};
inline constexpr char pretextureTrailerMagic[8] = {'D','L','P','T','E','X','T','R'};
inline constexpr std::size_t pretextureHeaderBytes = 64, pretextureRecordBytes = 40, pretextureTrailerBytes = 64;
std::uint32_t pretextureCrc32(std::uint32_t crc, const void* data, std::size_t size);
}
