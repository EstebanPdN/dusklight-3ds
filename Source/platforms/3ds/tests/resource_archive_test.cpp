#include "../runtime/resource_archive.hpp"
#include "../runtime/disc.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dusk::native;
namespace {
void word(unsigned char* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = value >> (24 - 8 * i);
}
void half(unsigned char* p, std::uint16_t value) { p[0] = value >> 8; p[1] = value; }
std::vector<unsigned char> yaz0(std::uint32_t expanded, std::initializer_list<unsigned char> stream) {
    std::vector<unsigned char> result(16);
    std::memcpy(result.data(), "Yaz0", 4); word(result.data() + 4, expanded);
    result.insert(result.end(), stream.begin(), stream.end());
    return result;
}
std::array<unsigned char, 256> rarc() {
    std::array<unsigned char, 256> data{};
    auto w = [&](unsigned offset, unsigned value) { word(data.data() + offset, value); };
    std::memcpy(data.data(), "RARC", 4);
    w(4, 256); w(8, 32); w(12, 160); w(16, 64); w(20, 64);
    w(32, 1); w(36, 32); w(40, 3); w(44, 48); w(48, 32); w(52, 128);
    std::memcpy(data.data() + 64, "ROOT", 4);
    w(68, 0); half(data.data() + 74, 3); w(76, 0);
    w(84, 0x02000005); w(88, 0); // '.' -> current node
    w(104, 0x02000007); w(108, UINT32_MAX); // root '..' sentinel
    w(124, 0x1100000a); w(128, 0); w(132, 64);
    const char strings[] = "root\0.\0..\0sample.bti\0";
    std::memcpy(data.data() + 160, strings, sizeof(strings));
    data[192] = 3; half(data.data() + 194, 4); half(data.data() + 196, 4);
    data[192 + 24] = 1; w(192 + 28, 32);
    return data;
}
void yazTests() {
    auto encoded = yaz0(12, {0xe0, 'a', 'b', 'c', 0x70, 2});
    std::array<unsigned char, 40> buffer; buffer.fill(0xcd);
    std::size_t written = 123;
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data() + 4, 32, 32, written) == ResourceStatus::ready);
    assert(written == 12 && !std::memcmp(buffer.data() + 4, "abcabcabcabc", 12));
    assert(buffer[3] == 0xcd && buffer[16] == 0xcd);
    for (std::size_t prefix = 0; prefix < encoded.size(); ++prefix) {
        written = 99;
        assert(decodeYaz0(encoded.data(), prefix, buffer.data(), buffer.size(), 32, written) == ResourceStatus::truncated);
        assert(written == 0);
    }
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), 11, 32, written) == ResourceStatus::outputTooSmall);
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), buffer.size(), 11, written) == ResourceStatus::limitExceeded);
    assert(decodeYaz0(encoded.data(), encoded.size(), nullptr, 12, 12, written) == ResourceStatus::outputTooSmall);
    assert(decodeYaz0(encoded.data(), encoded.size(), encoded.data() + 1, 12, 12, written) == ResourceStatus::overlappingBuffers);
    encoded = yaz0(19, {0x80, 'x', 0, 0, 0});
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), 19, 19, written) == ResourceStatus::ready);
    assert(written == 19 && std::all_of(buffer.begin(), buffer.begin() + 19, [](auto v) { return v == 'x'; }));
    encoded = yaz0(3, {0, 0x10, 0});
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), 3, 3, written) == ResourceStatus::malformed);
    encoded = yaz0(11, {0xe0, 'a', 'b', 'c', 0x70, 2});
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), 11, 11, written) == ResourceStatus::malformed);
    encoded = yaz0(19, {0x80, 'x', 0, 0});
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), 19, 19, written) == ResourceStatus::truncated);
    encoded = yaz0(0, {});
    assert(decodeYaz0(encoded.data(), encoded.size(), nullptr, 0, 0, written) == ResourceStatus::ready && written == 0);
    encoded[0] = 'X';
    assert(decodeYaz0(encoded.data(), encoded.size(), nullptr, 0, 0, written) == ResourceStatus::unsupported);
    encoded = yaz0(UINT32_MAX, {});
    assert(decodeYaz0(encoded.data(), encoded.size(), buffer.data(), buffer.size(), 512 * 1024, written) == ResourceStatus::limitExceeded);
}
void archiveTests() {
    const RarcLimits budget{256, 1, 3, 255};
    auto source = rarc();
    RarcArchive archive; RarcEntry entry;
    assert(archive.findFile("sample.bti", entry) == ResourceStatus::notReady);
    assert(archive.open(source.data(), source.size(), budget) == ResourceStatus::ready);
    assert(archive.nodeCount() == 1 && archive.entryCount() == 3);
    assert(archive.entry(0, entry) == ResourceStatus::ready && entry.directory && !entry.contents.data);
    assert(archive.entry(3, entry) == ResourceStatus::notFound);
    assert(archive.findFile("SAMPLE.BTI", entry) == ResourceStatus::ready && entry.contents.size == 64);
    BtiInfo bti;
    assert(inspectBti(entry.contents, 1024, bti) == ResourceStatus::ready);
    assert(bti.width == 4 && bti.height == 4 && bti.format == 3 && bti.mipCount == 1 && bti.image.size == 32);
    assert(archive.findFile("missing.bti", entry) == ResourceStatus::notFound);
    for (std::size_t prefix = 0; prefix < source.size(); ++prefix) {
        assert(archive.open(source.data(), prefix, budget) == ResourceStatus::truncated);
        assert(archive.entryCount() == 0 && archive.entry(0, entry) == ResourceStatus::notReady);
    }
    assert(archive.open(source.data(), source.size(), {255, 1, 3, 255}) == ResourceStatus::limitExceeded);
    assert(archive.open(source.data(), source.size(), {256, 1, 2, 255}) == ResourceStatus::limitExceeded);
    auto reject = [&](unsigned at, unsigned value, ResourceStatus expected = ResourceStatus::malformed) {
        auto corrupt = source; word(corrupt.data() + at, value);
        assert(archive.open(corrupt.data(), corrupt.size(), budget) == expected);
        assert(archive.entryCount() == 0);
    };
    reject(8, UINT32_MAX); reject(12, UINT32_MAX); reject(16, 1000);
    reject(36, 0); reject(44, 32); reject(52, 48); // outside/overlapping tables
    reject(68, 32); reject(76, 3); reject(88, 3); // names/node extent/directory reference
    reject(124, 0x0000000a); reject(128, 63); reject(132, 65);
    reject(32, 2, ResourceStatus::limitExceeded);
    auto corrupt = source; std::fill(corrupt.begin() + 170, corrupt.begin() + 192, 'x');
    assert(archive.open(corrupt.data(), corrupt.size(), budget) == ResourceStatus::malformed);
    corrupt = source; corrupt[170] = '/';
    assert(archive.open(corrupt.data(), corrupt.size(), budget) == ResourceStatus::malformed);
    corrupt = source; // Two real files with the same name must not silently select either.
    word(corrupt.data() + 84, 0x1100000a); word(corrupt.data() + 88, 0); word(corrupt.data() + 92, 64);
    assert(archive.open(corrupt.data(), corrupt.size(), budget) == ResourceStatus::ready);
    assert(archive.findFile("sample.bti", entry) == ResourceStatus::ambiguous && !entry.contents.data);
    // Deterministic corruption sweep, bounded by tiny explicit table budgets.
    for (unsigned byte = 0; byte < source.size(); ++byte) {
        corrupt = source; corrupt[byte] ^= 0xff;
        if (archive.open(corrupt.data(), corrupt.size(), budget) == ResourceStatus::ready) {
            for (unsigned i = 0; i < archive.entryCount(); ++i) {
                assert(archive.entry(i, entry) == ResourceStatus::ready);
                if (!entry.directory) inspectBti(entry.contents, 1024, bti);
            }
        }
    }
}
void textureTests() {
    auto archive = rarc();
    auto* bytes = archive.data() + 192;
    BtiInfo output;
    assert(inspectBti({bytes, 31}, 1024, output) == ResourceStatus::truncated);
    assert(inspectBti({bytes, 63}, 1024, output) == ResourceStatus::truncated);
    assert(inspectBti({bytes, 64}, 3, output) == ResourceStatus::limitExceeded);
    bytes[24] = 0; assert(inspectBti({bytes, 64}, 1024, output) == ResourceStatus::malformed); bytes[24] = 1;
    bytes[0] = 7; assert(inspectBti({bytes, 64}, 1024, output) == ResourceStatus::unsupported); bytes[0] = 3;
    word(bytes + 28, UINT32_MAX); assert(inspectBti({bytes, 64}, 1024, output) == ResourceStatus::truncated);
    word(bytes + 28, 32); half(bytes + 2, 0);
    assert(inspectBti({bytes, 64}, 1024, output) == ResourceStatus::malformed);
    std::array<unsigned char, 96> paletted{};
    paletted[0] = 8; half(paletted.data() + 2, 8); half(paletted.data() + 4, 8);
    paletted[8] = 1; paletted[9] = 2; half(paletted.data() + 10, 16);
    word(paletted.data() + 12, 32); paletted[24] = 1; word(paletted.data() + 28, 64);
    assert(inspectBti({paletted.data(), paletted.size()}, 1024, output) == ResourceStatus::ready);
    assert(output.paletteEntries == 16 && output.palette.size == 32 && output.image.size == 32);
    word(paletted.data() + 12, 64);
    assert(inspectBti({paletted.data(), paletted.size()}, 1024, output) == ResourceStatus::malformed);
}
int privateProbe(const char* path) {
    using Clock = std::chrono::steady_clock;
    Disc disc; DiscFile file;
    if (disc.open(path) != DiscStatus::ready ||
        disc.findFile("/res/Object/LogoUs.arc", file) != DiscFileStatus::ready || file.size() > 64 * 1024) return 2;
    std::vector<unsigned char> encoded(file.size());
    if (!disc.readFile(file, 0, encoded.data(), encoded.size())) return 3;
    std::uint32_t size;
    if (yaz0ExpandedSize(encoded.data(), encoded.size(), 512 * 1024, size) != ResourceStatus::ready) return 4;
    std::vector<unsigned char> decoded(size);
    std::size_t written;
    const auto start = Clock::now();
    if (decodeYaz0(encoded.data(), encoded.size(), decoded.data(), decoded.size(), 512 * 1024, written) != ResourceStatus::ready) return 5;
    RarcArchive archive;
    if (archive.open(decoded.data(), written, {512 * 1024, 4096, 65536, 255}) != ResourceStatus::ready) return 6;
    RarcEntry entry;
    if (archive.findFile("nintendo_376x104.bti", entry) != ResourceStatus::ready || entry.flags & 4) return 7;
    BtiInfo info;
    if (inspectBti(entry.contents, 1024, info) != ResourceStatus::ready) return 8;
    const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("PASS: private Yaz0/RARC/BTI metadata; encoded_bytes=%zu expanded_bytes=%zu nodes=%u entries=%u "
                "format=%u width=%u height=%u mips=%u image_bytes=%zu decode_parse_ms=%.3f; no payload saved\n",
                encoded.size(), written, archive.nodeCount(), archive.entryCount(), info.format, info.width,
                info.height, info.mipCount, info.image.size, milliseconds);
    return 0;
}
}
int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--private-disc")) return privateProbe(argv[2]);
    assert(argc == 1);
    yazTests(); archiveTests(); textureTests();
    std::puts("PASS: Yaz0 literals/short+long overlapping backrefs/truncation/budgets; RARC tables/names/" 
              "directory+file extents/corruption; BTI GX layout/mips/palette bounds");
}
