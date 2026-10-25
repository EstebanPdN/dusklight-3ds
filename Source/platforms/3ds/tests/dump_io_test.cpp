#include "../runtime/dump_io.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace dusk::native;
    assert(dumpHash("hello", 5) == 0x4f9f2cab);
    assert(!hasDumpSpace(0, 0, 0));
    assert(hasDumpSpace(dumpReserve, 0, 0));
    assert(!hasDumpSpace(dumpReserve, 1, 0));
    assert(!hasDumpSpace(UINT64_MAX, UINT64_MAX, 1));
    assert(!hasDumpSpace(UINT64_MAX, 1, UINT64_MAX));
    assert(hasDumpSpace(dumpReserve + 1024, 768, 256));
    assert(!hasDumpSpace(dumpReserve + 1023, 768, 256));
    std::array<unsigned char, 257> scratch{};
    std::vector<unsigned char> data(512 * 1024);
    for (size_t i = 0; i < data.size(); ++i) data[i] = i * 37;
    std::uint32_t checksum = 0;
    const std::string base = std::string(argv[1]) + "/dump-io-fixture";
    assert(writeVerifiedDumpFile(base.c_str(), data.data(), data.size(), scratch.data(), scratch.size(), checksum));
    assert(checksum == dumpHash(data.data(), data.size()));
    // Never clobber existing evidence, even when a caller mistakenly reuses a name.
    assert(!writeVerifiedDumpFile(base.c_str(), "changed", 7, scratch.data(), scratch.size(), checksum));
    FILE* file = std::fopen(base.c_str(), "rb"); assert(file);
    std::vector<unsigned char> actual(data.size());
    assert(std::fread(actual.data(), 1, actual.size(), file) == actual.size());
    assert(!std::fclose(file) && actual == data);
    assert(!std::remove(base.c_str()));
    // Full dumps use the same buffer for source and readback.
    auto expected = dumpHash(data.data(), data.size());
    assert(writeVerifiedDumpFile(base.c_str(), data.data(), data.size(), data.data(), data.size(), checksum));
    assert(checksum == expected); assert(!std::remove(base.c_str()));
    assert(writeVerifiedDumpFile(base.c_str(), nullptr, 0, scratch.data(), scratch.size(), checksum));
    assert(checksum == 2166136261u); assert(!std::remove(base.c_str()));
    assert(!writeVerifiedDumpFile((base + "/missing/child").c_str(), "x", 1, scratch.data(), scratch.size(), checksum));
    assert(!writeVerifiedDumpFile(base.c_str(), nullptr, 1, scratch.data(), scratch.size(), checksum));
    assert(!writeVerifiedDumpFile(base.c_str(), "x", 1, nullptr, 0, checksum));
    assert(writeStreamedDumpFile(base.c_str(),data.data(),data.size(),scratch.data(),scratch.size(),checksum));
    assert(checksum==dumpHash(data.data(),data.size()));
    assert(!writeStreamedDumpFile(base.c_str(),data.data(),data.size(),scratch.data(),scratch.size(),checksum));
    assert(!std::remove(base.c_str()));
    assert(!writeStreamedDumpFile(base.c_str(),data.data(),data.size(),data.data()+10,257,checksum));
    assert(!writeStreamedDumpFile(base.c_str(),data.data()+10,data.size()-10,data.data(),257,checksum));
    assert(writeStreamedDumpFile(base.c_str(),nullptr,0,scratch.data(),scratch.size(),checksum));
    assert(checksum==2166136261u);assert(!std::remove(base.c_str()));
    std::puts("PASS: reserve/overflow preflight, exclusive creation, 512 KiB flush/readback, aliasing, invalid inputs");
}
