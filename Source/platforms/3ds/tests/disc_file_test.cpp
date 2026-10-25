#include "../runtime/disc.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dusk::native;
namespace {
struct Fixture {
    std::array<unsigned char, 0x2000> image{};
    std::uint32_t nameCursor = 0;
    static constexpr std::uint32_t fst = 0x800, count = 9, strings = fst + count * 12;
    void word(std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) image.at(at + i) = value >> (24 - 8 * i);
    }
    void entry(unsigned index, const char* name, bool directory, unsigned offset, unsigned size) {
        const auto length = std::strlen(name) + 1;
        assert(strings + nameCursor + length < 0xc00);
        word(fst + index * 12, (directory ? 0x01000000 : 0) | nameCursor);
        word(fst + index * 12 + 4, offset); word(fst + index * 12 + 8, size);
        std::memcpy(image.data() + strings + nameCursor, name, length);
        nameCursor += length;
    }
    Fixture() {
        std::memcpy(image.data(), "GZ2E01", 6);
        word(0x1c, 0xc2339f3d); word(0x420, 0x500); word(0x424, fst); word(0x428, 0x400);
        word(0x500, 0x100); word(0x590, 16);
        word(fst, 0x01000000); word(fst + 8, count);
        entry(1, "Other", true, 0, 3);
        entry(2, "LogoUs.arc", false, 0x1000, 3);
        entry(3, "Res", true, 0, 8);
        entry(4, "Object", true, 3, 7);
        entry(5, "LogoUs.arc", false, 0x1010, 9);
        entry(6, "empty", false, image.size(), 0);
        entry(7, "sibling", false, 0x1020, 5);
        entry(8, "top.bin", false, 0x1030, 7);
        std::memcpy(image.data() + 0x1000, "bad", 3);
        std::memcpy(image.data() + 0x1010, "synthetic", 9);
        std::memcpy(image.data() + 0x1020, "hello", 5);
        std::memcpy(image.data() + 0x1030, "topfile", 7);
    }
    void save(const std::string& path, std::size_t size = 0x2000) const {
        auto* f = std::fopen(path.c_str(), "wb"); assert(f);
        assert(std::fwrite(image.data(), 1, size, f) == size); assert(!std::fclose(f));
    }
};

int privateProbe(const char* path) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    Disc disc;
    if (disc.open(path) != DiscStatus::ready) return 2;
    const auto opened = Clock::now();
    const auto indexReads = disc.sourceReads();
    if (!disc.fstCacheSize()) return 5;
    DiscFile file, insensitive, indexed;
    if (disc.findFile("/res/Object/LogoUs.arc", file) != DiscFileStatus::ready ||
        disc.findFile("RES/OBJECT/LOGOUS.ARC", insensitive) != DiscFileStatus::ready ||
        disc.openFile(file.entryIndex(), indexed) != DiscFileStatus::ready ||
        file.entryIndex() != insensitive.entryIndex() || file.size() != indexed.size()) return 3;
    if (disc.sourceReads() != indexReads) return 6;
    const auto located = Clock::now();
    std::array<unsigned char, 4096> a{}, b{};
    const auto bytes = std::min<std::size_t>(a.size(), file.size());
    if (!bytes || !disc.readFile(file, 0, a.data(), bytes) ||
        !disc.readFile(indexed, 0, b.data(), bytes) || a != b ||
        disc.readFile(file, file.size(), a.data(), 1)) return 4;
    const auto finished = Clock::now();
    auto milliseconds = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("PASS: private read-only resource lookup/index/bounded-read; resource_bytes=%u read_bytes=%zu "
                "fst_cache_bytes=%zu metadata_source_reads=0 "
                "open_ms=%.3f lookup_ms=%.3f read_ms=%.3f; host timing only; no payload saved\n",
                file.size(), bytes, disc.fstCacheSize(), milliseconds(started, opened), milliseconds(opened, located), milliseconds(located, finished));
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--private-disc")) return privateProbe(argv[2]);
    assert(argc == 2);
    const auto path = std::string(argv[1]) + "/synthetic-disc-files.iso";
    Fixture fixture; fixture.save(path);
    Disc disc; DiscFile file, indexed;
    assert(disc.findFile("res/Object/LogoUs.arc", file) == DiscFileStatus::notReady);
    assert(!disc.read(0, nullptr, 0));
    assert(disc.openFile(5, file) == DiscFileStatus::notReady);
    assert(disc.open(path.c_str()) == DiscStatus::ready);
    assert(disc.fstCacheSize() == 0x400);
    const auto readsBeforeLookup = disc.sourceReads();
    DiscDirectory root, nested;
    DiscDirectoryEntry child;
    assert(disc.findDirectory("/", root) == DiscFileStatus::ready && root.entryIndex() == 0);
    for (auto expected : {1u, 3u, 8u}) {
        assert(disc.readDirectory(root, child) == DiscFileStatus::ready && child.index == expected);
    }
    assert(disc.readDirectory(root, child) == DiscFileStatus::notFound && child.name[0] == 0);
    assert(disc.rewindDirectory(root));
    assert(disc.readDirectory(root, child) == DiscFileStatus::ready && child.index == 1 && child.directory && !std::strcmp(child.name.data(), "Other"));
    assert(disc.findDirectory("RES/OBJECT", nested) == DiscFileStatus::ready && nested.entryIndex() == 4);
    assert(disc.readDirectory(nested, child) == DiscFileStatus::ready && child.index == 5 && !child.directory);
    assert(disc.readDirectory(nested, child) == DiscFileStatus::ready && child.index == 6);
    assert(disc.readDirectory(nested, child) == DiscFileStatus::notFound);
    assert(disc.openDirectory(5, nested) == DiscFileStatus::notFound);
    assert(disc.openDirectory(UINT32_MAX, nested) == DiscFileStatus::notFound);
    assert(disc.findDirectory("res/../Object", nested) == DiscFileStatus::invalidPath);
    assert(!disc.directoryValid(DiscDirectory{}));
    for (unsigned i = 0; i < 100; ++i) {
        assert(disc.findFile("/res/Object/LogoUs.arc", file) == DiscFileStatus::ready);
        assert(disc.findDirectory("/res/Object", nested) == DiscFileStatus::ready);
        assert(disc.readDirectory(nested, child) == DiscFileStatus::ready);
    }
    assert(disc.sourceReads() == readsBeforeLookup); // No SD I/O in metadata lookups.
    for (const auto* name : {"/res/Object/LogoUs.arc", "res/object/logous.arc", "RES/OBJECT/LOGOUS.ARC"}) {
        assert(disc.findFile(name, file) == DiscFileStatus::ready);
        assert(file.entryIndex() == 5 && file.size() == 9);
        std::array<char, 10> bytes{};
        assert(disc.readFile(file, 0, bytes.data(), 9) && !std::strcmp(bytes.data(), "synthetic"));
        assert(disc.readFile(file, 3, bytes.data(), 3) && !std::memcmp(bytes.data(), "the", 3));
        assert(!disc.readFile(file, 9, bytes.data(), 1));
        assert(!disc.readFile(file, 8, bytes.data(), 2));
        assert(!disc.readFile(file, UINT64_MAX, bytes.data(), 1));
        assert(!disc.readFile(file, 0, nullptr, 1));
        assert(!disc.readFile(file, 0, bytes.data(), SIZE_MAX));
        assert(disc.readFile(file, 9, nullptr, 0));
    }
    assert(disc.openFile(file.entryIndex(), indexed) == DiscFileStatus::ready && indexed.size() == file.size());
    for (unsigned index : {0u, 1u, 3u, 4u, 9u, UINT32_MAX})
        assert(disc.openFile(index, indexed) == DiscFileStatus::notFound);
    for (const auto* name : {"res", "res/Object", "res/Object/missing", "res/Object/LogoUs.arc/child",
                             "res/LogoUs.arc", "res/Object/sibling", "other/sibling", "missing"})
        assert(disc.findFile(name, indexed) == DiscFileStatus::notFound);
    for (const auto* name : {"", "/", "//res", "res//Object", "res/", ".", "..", "res/./Object",
                             "res/../Object", "res\\Object", "res/\x1f", "res/\x7f"})
        assert(disc.findFile(name, indexed) == DiscFileStatus::invalidPath);
    assert(disc.findFile(nullptr, indexed) == DiscFileStatus::invalidPath);
    assert(disc.findFile(std::string(1024, 'a').c_str(), indexed) == DiscFileStatus::invalidPath);
    assert(disc.findFile(std::string(256, 'a').c_str(), indexed) == DiscFileStatus::invalidPath);
    std::string tooDeep; for (unsigned i = 0; i < 65; ++i) { if (i) tooDeep += '/'; tooDeep += 'a'; }
    assert(disc.findFile(tooDeep.c_str(), indexed) == DiscFileStatus::invalidPath);
    assert(disc.findFile("/res/Object/empty", indexed) == DiscFileStatus::ready && indexed.size() == 0);
    assert(disc.readFile(indexed, 0, nullptr, 0));
    char byte; assert(!disc.readFile(indexed, 0, &byte, 1));
    assert(disc.findFile("res/sibling", indexed) == DiscFileStatus::ready && indexed.entryIndex() == 7);
    assert(disc.findFile("top.bin", indexed) == DiscFileStatus::ready && indexed.entryIndex() == 8);
    Disc second; assert(second.open(path.c_str()) == DiscStatus::ready);
    assert(second.readDirectory(root, child) == DiscFileStatus::notReady && !second.rewindDirectory(root));
    assert(!second.readFile(file, 0, &byte, 1));
    assert(!disc.readFile(DiscFile{}, 0, &byte, 1));
    assert(disc.open(path.c_str()) == DiscStatus::ready);
    assert(disc.readDirectory(root, child) == DiscFileStatus::notReady && !disc.rewindDirectory(root));
    assert(!disc.readFile(file, 0, &byte, 1));
    assert(disc.findFile("res/Object/LogoUs.arc", file) == DiscFileStatus::ready);
    assert(disc.open(nullptr) == DiscStatus::missing);
    assert(disc.fstCacheSize() == 0);
    assert(!disc.readFile(file, 0, &byte, 1) && disc.size() == 0);
    // Duplicate names under the same parent, including case-only differences,
    // are refused by lookup. The same basename under another parent is valid.
    fixture.entry(6, "LOGOUS.ARC", false, 0x1040, 1); fixture.save(path);
    assert(disc.open(path.c_str()) == DiscStatus::ready);
    assert(disc.findFile("res/Object/LogoUs.arc", file) == DiscFileStatus::malformed);
    fixture = Fixture(); fixture.word(Fixture::fst + 4 * 12 + 8, 10); fixture.save(path);
    assert(disc.open(path.c_str()) == DiscStatus::malformed);
    assert(disc.findFile("res/Object/LogoUs.arc", file) == DiscFileStatus::notReady);
    assert(!disc.read(0, &byte, 1));
    fixture = Fixture(); fixture.image[7] = 1; fixture.save(path);
    assert(disc.open(path.c_str()) == DiscStatus::unsupported && disc.size() == 0);
    assert(!disc.read(0, &byte, 1));
    fixture = Fixture(); fixture.save(path, 0x900);
    assert(disc.open(path.c_str()) == DiscStatus::malformed);
    assert(std::remove(path.c_str()) == 0);
    std::puts("PASS: FST hierarchy, ASCII case folding, indexed opens, extents, empty files, invalid paths, "
              "ambiguous names, directory subtree skipping/rewind, stale/cross-disc handles, malformed/unsupported/truncated input");
}
