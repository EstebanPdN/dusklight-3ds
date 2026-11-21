#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace dusk::native {

// Startup settings are read once per launch. A complete directory listing
// avoids sending an FS request to the SD for every absent switch. A hash
// match still opens the actual file and checks its access and contents.
class BootOptions {
public:
    explicit BootOptions(const char* directory = "sdmc:/3ds/DuskLight") : root(directory) {
        auto* dir = opendir(root);
        if (!dir) return;
        while (count < names.size()) {
            errno = 0;
            auto* entry = readdir(dir);
            if (!entry) {
                complete = errno == 0;
                break;
            }
            names[count++] = hashName(entry->d_name);
        }
        if (closedir(dir) != 0) complete = false;
    }

    std::FILE* open(const char* name) const {
        if (complete && !std::strchr(name, '/') &&
            std::find(names.begin(), names.begin() + count, hashName(name)) == names.begin() + count) {
            errno = ENOENT;
            return nullptr;
        }
        char path[512];
        const int length = std::snprintf(path, sizeof(path), "%s/%s", root, name);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
            errno = ENAMETOOLONG;
            return nullptr;
        }
        return std::fopen(path, "rb");
    }

    bool present(const char* name) const {
        auto* file = open(name);
        if (!file) return false;
        std::fclose(file);
        return true;
    }

private:
    static std::uint32_t hashName(const char* name) {
        std::uint32_t value = 2166136261u;
        for (; *name; ++name) {
            unsigned char c = static_cast<unsigned char>(*name);
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            value = (value ^ c) * 16777619u;
        }
        return value;
    }

    const char* root;
    std::array<std::uint32_t, 256> names{};
    std::size_t count{};
    bool complete{};
};

}
