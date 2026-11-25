#include "../runtime/packed_dump_index.hpp"
#include <array>
#include <cassert>
#include <filesystem>
#include <string>

using dusk::native::appdump::PackedIndex;

int main(int argc, char** argv) {
    assert(argc == 2);
    std::array<char, 65536> buffer;
    std::array<char, 4096> scratch;
    {
        PackedIndex index;
        index.text = buffer.data();
        index.capacity = buffer.size();
        std::snprintf(index.spillPath, sizeof(index.spillPath), "%s", argv[1]);
        std::string expected;
        auto appendLine = [&](unsigned i) {
            const unsigned address = 0x10000000 + i * 65536;
            char record[128];
            std::snprintf(record, sizeof(record), "block %08x 00010000 raw %u 65536 fnv1a32=01234567\n", address, i * 65536 + 16);
            expected += record;
            index.line("%s", record);
        };
        // A capture small enough for RAM never opens the spill file.
        appendLine(0);
        assert(!std::filesystem::exists(argv[1]));
        for (unsigned i = 1; i < 4096; ++i) appendLine(i);
        assert(expected.size() > 3 * buffer.size());
        assert(index.size() == expected.size() && std::filesystem::exists(argv[1]));
        std::string output;
        assert(index.appendTo(scratch.data(), scratch.size(), [&](const void* data, std::size_t size) {
            output.append(static_cast<const char*>(data), size);
            return true;
        }));
        assert(output == expected);
        assert(!index.appendTo(scratch.data(), scratch.size(), [](const void*, std::size_t) { return false; }));
    }
    assert(!std::filesystem::exists(argv[1]));
    PackedIndex noFile;
    noFile.text = buffer.data();
    noFile.capacity = 12;
    noFile.line("12345678");
    noFile.line("abcdefg");
    assert(noFile.failed);
}
