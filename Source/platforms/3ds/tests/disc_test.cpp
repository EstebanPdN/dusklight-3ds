#include "../runtime/disc.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace dusk::native;
    const std::string path = std::string(argv[1]) + "/synthetic-disc.iso";
    std::array<unsigned char, 0x1000> image{};
    auto word = [&](unsigned offset, unsigned value) {
        for (unsigned i = 0; i < 4; ++i) image[offset + i] = value >> (24 - 8 * i);
    };
    std::memcpy(image.data(), "GZ2E01", 6);
    word(0x1c, 0xc2339f3d); word(0x420, 0x500); word(0x424, 0x800); word(0x428, 64);
    word(0x500, 0x100); word(0x590, 16); // DOL text extent
    word(0x800, 0x01000000); word(0x808, 2);
    word(0x80c, 0); word(0x810, 0xf00); word(0x814, 4);
    std::memcpy(image.data() + 0x818, "test", 5);
    auto check = [&](DiscStatus expected, size_t bytes = 0x1000) {
        FILE* f = std::fopen(path.c_str(), "wb"); assert(f);
        assert(std::fwrite(image.data(), 1, bytes, f) == bytes); assert(!std::fclose(f));
        Disc d; assert(d.open(path.c_str()) == expected);
        if (expected == DiscStatus::ready) {
            std::array<unsigned char, 4> out;
            assert(d.read(0, out.data(), 4) && !std::memcmp(out.data(), image.data(), 4));
            assert(!d.read(UINT64_MAX, out.data(), 4));
            assert(!d.read(0xfff, out.data(), 4));
            assert(!d.read(0, nullptr, 1));
            assert(d.read(0x1000, nullptr, 0));
        }
    };
    check(DiscStatus::ready);
    check(DiscStatus::malformed, 12);
    image[7] = 1; check(DiscStatus::unsupported); image[7] = 0;
    image[3] = 'P'; check(DiscStatus::ready);
    image[7] = 1; check(DiscStatus::unsupported); image[7] = 0;
    image[6] = 1; check(DiscStatus::unsupported); image[6] = 0;
    image[3] = 'J'; check(DiscStatus::unsupported); image[3] = 'E';
    word(0x428, 0xffffffff); check(DiscStatus::malformed); word(0x428, 64);
    word(0x808, 0xffffffff); check(DiscStatus::malformed); word(0x808, 2);
    word(0x810, 0xfffffffe); check(DiscStatus::malformed); word(0x810, 0xf00);
    word(0x590, 0xffffffff); check(DiscStatus::malformed); word(0x590, 16);
    image[0x818] = '/'; check(DiscStatus::malformed); image[0x818] = 't';
    std::memset(image.data() + 0x818, 'a', 40); check(DiscStatus::malformed);
    std::memcpy(image.data() + 0x818, "test", 5);
    word(0x80c, 0x01000000); word(0x810, 1); word(0x814, 2); check(DiscStatus::malformed);
    word(0x810, 0); check(DiscStatus::ready); // valid empty child directory
    word(0x814, 3); check(DiscStatus::malformed);
    Disc missing; assert(missing.open(nullptr) == DiscStatus::missing);
    assert(std::remove(path.c_str()) == 0);
    assert(missing.open(path.c_str()) == DiscStatus::missing);
    std::puts("PASS: disc identity, truncation, DOL/FST extents, names, nesting, bounded reads");
}
