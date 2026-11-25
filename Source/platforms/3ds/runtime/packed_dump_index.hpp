#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace dusk::native::appdump {

// DPK2 stores the complete text index after the payload. Keep it in the
// diagnostic workspace until it fills, then spool whole pages in order.
struct PackedIndex {
    char* text{};
    std::size_t capacity{}, used{};
    char spillPath[160]{};
    bool failed{};

    ~PackedIndex() {
        if (spill) {
            std::fclose(spill);
            std::remove(spillPath);
        }
    }

    void line(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (failed) return;
        va_list args;
        va_start(args, format);
        va_list copy;
        va_copy(copy, args);
        int n = std::vsnprintf(text + used, capacity - used, format, copy);
        va_end(copy);
        if (n >= 0 && std::size_t(n) >= capacity - used && std::size_t(n) < capacity) {
            if (!spill) spill = std::fopen(spillPath, "w+b");
            if (spill && std::fwrite(text, 1, used, spill) == used) {
                spilledBytes += used;
                used = 0;
                n = std::vsnprintf(text, capacity, format, args);
            } else {
                failed = true;
            }
        }
        va_end(args);
        if (n < 0 || std::size_t(n) >= capacity - used) failed = true;
        if (!failed) used += std::size_t(n);
    }

    std::size_t size() const { return spilledBytes + used; }

    template<class Append>
    bool appendTo(void* scratch, std::size_t bytes, Append append) {
        if (failed || !bytes) return false;
        if (spill) {
            if (std::fflush(spill) || std::fseek(spill, 0, SEEK_SET)) return false;
            for (std::size_t left = spilledBytes; left;) {
                const auto n = std::min(left, bytes);
                if (std::fread(scratch, 1, n, spill) != n || !append(scratch, n)) return false;
                left -= n;
            }
        }
        for (std::size_t at = 0; at < used;) {
            const auto n = std::min(used - at, bytes);
            if (!append(text + at, n)) return false;
            at += n;
        }
        return true;
    }

private:
    std::FILE* spill{};
    std::size_t spilledBytes{};
};

}
