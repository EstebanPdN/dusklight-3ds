#include "../runtime/disc.hpp"
#include "../runtime/stage_data.hpp"
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

// Read-only metadata inventory. This is not a game-data preparer or a player.
using namespace dusk::native;
namespace {
void inspect(Disc& disc, const char* archiveName, const char* resourceName) {
    DiscFile file;
    assert(disc.findFile(archiveName, file) == DiscFileStatus::ready);
    assert(file.size() <= 8 * 1024 * 1024);
    std::vector<unsigned char> archive(file.size());
    assert(disc.readFile(file, 0, archive.data(), archive.size()));
    if (archive.size() >= 4 && !std::memcmp(archive.data(), "Yaz0", 4)) {
        unsigned size = 0;
        assert(yaz0ExpandedSize(archive.data(), archive.size(), 8 * 1024 * 1024, size) == ResourceStatus::ready);
        std::vector<unsigned char> decoded(size);
        std::size_t written = 0;
        assert(decodeYaz0(archive.data(), archive.size(), decoded.data(), decoded.size(),
                          8 * 1024 * 1024, written) == ResourceStatus::ready);
        assert(written == size);
        archive.swap(decoded);
    }
    RarcArchive rarc;
    assert(rarc.open(archive.data(), archive.size(), {8 * 1024 * 1024, 4096, 65536, 255}) == ResourceStatus::ready);
    RarcEntry resource;
    assert(rarc.findFile(resourceName, resource) == ResourceStatus::ready);
    StageData stage;
    assert(stage.open(resource.contents) == ResourceStatus::ready);
    for (unsigned i = 0; i < stage.chunkCount(); ++i) {
        StageChunk chunk;
        assert(stage.chunk(i, chunk) == ResourceStatus::ready);
        const auto tag = chunk.tag;
        // Actual d_stage.h layouts: actor/player 0x20, scaled actor/door 0x24.
        const bool actor = !std::memcmp(tag.data(), "ACTR", 4) ||
                           !std::memcmp(tag.data(), "TGOB", 4) ||
                           !std::memcmp(tag.data(), "PLYR", 4) ||
                           (!std::memcmp(tag.data(), "ACT", 3) && tag[3] != 'R');
        const bool scaled = !std::memcmp(tag.data(), "TGSC", 4) ||
                            !std::memcmp(tag.data(), "TGDR", 4) ||
                            !std::memcmp(tag.data(), "SCOB", 4) ||
                            !std::memcmp(tag.data(), "Door", 4) ||
                            !std::memcmp(tag.data(), "SCO", 3) ||
                            !std::memcmp(tag.data(), "Doo", 3);
        if (!actor && !scaled) continue;
        const unsigned stride = scaled ? 36 : 32;
        const auto status = stage.require(tag, stride, chunk);
        if (status != ResourceStatus::ready)
            std::fprintf(stderr, "%s %.4s count=%u extent=%zu stride=%u status=%s\n",
                         resourceName, tag.data(), chunk.count, chunk.contents.size, stride, resourceStatusText(status));
        assert(status == ResourceStatus::ready);
        for (unsigned record = 0; record < chunk.count; ++record) {
            const auto* data = chunk.contents.data + record * stride;
            char name[9]{};
            std::memcpy(name, data, 8);
            for (const unsigned char* p = reinterpret_cast<unsigned char*>(name); *p; ++p)
                assert(std::isalnum(*p) || *p == '_'); // JSON-safe original actor names.
            std::printf("{\"archive\":\"%s\",\"resource\":\"%s\",\"chunk\":\"%.4s\",\"record\":%u,\"actor\":\"%s\"}\n",
                        archiveName, resourceName, tag.data(), record, name);
        }
    }
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    Disc disc;
    assert(disc.open(argv[1]) == DiscStatus::ready);
    inspect(disc, "/res/Stage/F_SP103/STG_00.arc", "stage.dzs");
    inspect(disc, "/res/Stage/F_SP103/R00_00.arc", "room.dzr");
}
