// Private offline adapter: reuse the runtime Disc, RARC and Yaz0 validators.
// Invoked by prepare_native_assets.py with an inherited private directory FD.
#include "runtime/disc.hpp"
#include "runtime/resource_archive.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace dusk::native;
namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::size_t archiveLimit = 64u << 20, resourceLimit = 16u << 20;
constexpr std::size_t totalLimit = 1u << 30;
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
void ready(ResourceStatus status, const char* operation) {
    if (status != ResourceStatus::ready)
        throw std::runtime_error(std::string(operation) + ": " + resourceStatusText(status));
}
ResourceSpan expand(ResourceSpan source, Bytes& storage, std::size_t limit) {
    require(source.size <= limit, "Encoded resource exceeds budget");
    if (source.size < 4 || std::memcmp(source.data, "Yaz0", 4)) return source;
    std::uint32_t size = 0;
    ready(yaz0ExpandedSize(source.data, source.size, limit, size), "Yaz0 extent");
    storage.resize(size);
    std::size_t written = 0;
    ready(decodeYaz0(source.data, source.size, storage.data(), storage.size(), limit, written), "Yaz0 decode");
    require(written == size, "Yaz0 decoded extent mismatch");
    return {storage.data(), storage.size()};
}
void save(int directory, const char* name, ResourceSpan data) {
    const int fd = ::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    require(fd >= 0, "Exclusive output creation failed");
    bool okay = true;
    std::size_t done = 0;
    while (done < data.size) {
        const auto n = ::write(fd, data.data + done, data.size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { okay = false; break; }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd)) okay = false;
    if (::close(fd)) okay = false;
    require(okay, "Output write/sync failed");
}
bool suffix(const char* name, const char* ending) {
    const auto a = std::strlen(name), b = std::strlen(ending);
    if (a < b) return false;
    for (std::size_t i = 0; i < b; ++i) {
        const auto c = static_cast<unsigned char>(name[a - b + i]);
        if ((c >= 'A' && c <= 'Z' ? c + 32 : c) != static_cast<unsigned char>(ending[i])) return false;
    }
    return true;
}
}
int main(int argc, char** argv) {
    unsigned archiveNumber = 0, entryNumber = 0;
    try {
        require(argc >= 4 && argc <= 19, "Usage: extract-native-inputs INPUT PRIVATE_OUTPUT_FD /res/ARCHIVE...");
        char* end = nullptr;
        const long directory = std::strtol(argv[2], &end, 10);
        require(end != argv[2] && !*end && directory >= 0 && directory <= 0x7fffffff, "Invalid private output FD");
        struct stat info{};
        require(!::fstat(static_cast<int>(directory), &info) && S_ISDIR(info.st_mode) &&
                info.st_uid == ::geteuid() && !(info.st_mode & 077), "Output FD must be an owner-only directory");
        Disc disc;
        const auto status = disc.open(argv[1]);
        if (status != DiscStatus::ready) throw std::runtime_error(discStatusText(status));
        std::array<unsigned char, 8> identity{};
        require(disc.read(0, identity.data(), identity.size()), "Disc identity read failed");
        std::printf("disc\t%.6s\t%u\t%s\n", reinterpret_cast<const char*>(identity.data()),
                    unsigned(identity[7]), disc.isBundle() ? "bundle" : "iso");
        std::size_t total = 0, work = 0;
        unsigned resources = 0;
        for (int arg = 3; arg < argc; ++arg, ++archiveNumber) {
            entryNumber = 0;
            require(!std::strncmp(argv[arg], "/res/", 5), "Archive must be an absolute /res/ path");
            DiscFile file;
            const auto found = disc.findFile(argv[arg], file);
            if (found != DiscFileStatus::ready) throw std::runtime_error(discFileStatusText(found));
            require(file.size() <= archiveLimit, "Archive exceeds encoded budget");
            Bytes encoded(file.size()), expanded;
            require(disc.readFile(file, 0, encoded.data(), encoded.size()), "Archive read failed");
            const auto bytes = expand({encoded.data(), encoded.size()}, expanded, archiveLimit);
            RarcArchive archive;
            ready(archive.open(bytes.data, bytes.size, {archiveLimit, 4096, 65536, 255}), "RARC validation");
            require(bytes.size <= totalLimit - total, "Total extracted byte budget exceeded");
            total += bytes.size;
            char filename[64];
            std::snprintf(filename, sizeof(filename), "archive-%04u.arc", archiveNumber);
            save(static_cast<int>(directory), filename, bytes);
            std::printf("archive\t%u\t%u\t%u\t%u\t%zu\n", archiveNumber, file.entryIndex(), file.discOffset(), file.size(), bytes.size);
            for (; entryNumber < archive.entryCount(); ++entryNumber) {
                RarcEntry entry;
                ready(archive.entry(entryNumber, entry), "RARC entry");
                if (entry.directory) continue;
                Bytes decoded;
                // Uncompressed non-resource entries remain borrowed; no copy.
                const auto resource = expand(entry.contents, decoded, archiveLimit);
                require(resource.size <= totalLimit - work, "Expanded entry work budget exceeded");
                work += resource.size;
                const bool model = (resource.size >= 4 && !std::memcmp(resource.data, "J3D2", 4)) || suffix(entry.name, ".bmd") || suffix(entry.name, ".bdl");
                const bool texture = suffix(entry.name, ".bti");
                if (!model && !texture) continue;
                require(++resources <= 4096, "Extracted resource count budget exceeded");
                require(resource.size <= resourceLimit, "Model/BTI exceeds converter source budget");
                require(resource.size <= totalLimit - total, "Total extracted byte budget exceeded");
                total += resource.size;
                std::snprintf(filename, sizeof(filename), "archive-%04u-entry-%06u.bin", archiveNumber, entryNumber);
                save(static_cast<int>(directory), filename, resource);
                std::printf("entry\t%u\t%u\t%u\t%u\t%zu\t%zu\t%zu\t%s\t", archiveNumber, entryNumber,
                            unsigned(entry.id), unsigned(entry.flags), static_cast<std::size_t>(entry.contents.data - bytes.data), entry.contents.size,
                            resource.size, model ? "model" : "texture");
                for (const auto* p = reinterpret_cast<const unsigned char*>(entry.name); *p; ++p) std::printf("%02x", *p);
                std::putchar('\n');
            }
        }
        require(!std::fflush(stdout) && !std::ferror(stdout), "Metadata output failed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "archive=%u entry=%u: %s\n", archiveNumber, entryNumber, error.what());
        return 1;
    }
}
