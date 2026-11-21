// Mac-only: walks every texture of a GameCube disc image or zelda.twilight
// bundle with the runtime's own readers (disc.cpp, resource_archive.cpp), so
// the converter and its test see exactly the bytes the console loads.
//
// Sources: every FST file and every RARC member, Yaz0 peeled at any depth;
// stand-alone .bti files, TEX1 of any J3D file (bmd/bdl/bmt), JPC particle
// textures (JPAC). TEX1 headers whose image offset is zero (named external
// textures) are skipped: the runtime resolves them to archive .bti files,
// which are walked themselves. Fonts (.bfn), DOL-embedded images and images
// built at run time (EFB copies, map captures, ramps) are not on the disc as
// BTIs and always take the runtime path.
#pragma once
#include "../../Source/platforms/3ds/runtime/disc.hpp"
#include "../../Source/platforms/3ds/runtime/resource_archive.hpp"
#include "../../Source/platforms/3ds/runtime/texture_gx.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pretexture {
using Bytes = std::vector<std::uint8_t>;
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }

struct Found {
    std::string path;                      // disc path / member path / #index
    std::span<const std::uint8_t> view;    // header-relative view inside its container (as archived)
    Bytes normalized;                      // texture_resources.cpp retain() copy: header, image at 32, palette after
};
struct WalkStats {
    std::uint64_t files = 0, archives = 0, yaz0 = 0, yaz0Bytes = 0, j3d = 0, jpc = 0, btiFiles = 0;
    std::uint64_t headers = 0, external = 0, refused = 0;
};

// Mirrors texture_resources.cpp retain(): inspectBti + inspectNativeTexture,
// the image copied right after the 32-byte header, the palette after it.
inline bool normalize(std::span<const std::uint8_t> view, Bytes& out) {
    using namespace dusk::native;
    BtiInfo info{};
    NativeTexturePlan plan{};
    if (inspectBti({view.data(), view.size()}, 1024, info) != ResourceStatus::ready ||
        !inspectNativeTexture(view, plan) || plan.sourceBytes != info.image.size) return false;
    out.assign(32 + info.image.size + info.palette.size, 0);
    std::memcpy(out.data(), view.data(), 32);
    out[28] = out[29] = out[30] = 0; out[31] = 32;
    std::memcpy(out.data() + 32, info.image.data, info.image.size);
    if (info.palette.size) {
        const auto offset = std::uint32_t(32 + info.image.size);
        for (unsigned n = 0; n < 4; ++n) out[12 + n] = std::uint8_t(offset >> (24 - 8 * n));
        std::memcpy(out.data() + offset, info.palette.data, info.palette.size);
    }
    return true;
}

class Walker {
public:
    using Visit = std::function<void(const Found&)>;
    WalkStats stats;
    // Optional: every whole J3D file before its TEX1 images are visited
    // (mip_inventory.cpp reads which materials sample each image, and how).
    std::function<void(const std::string&, std::span<const std::uint8_t>)> j3dFile;
    explicit Walker(Visit visit) : visit(std::move(visit)) {}
    bool walkDisc(dusk::native::Disc& disc) {
        dusk::native::DiscDirectory root;
        if (disc.findDirectory("/", root) != dusk::native::DiscFileStatus::ready) return false;
        return walkDirectory(disc, root, "");
    }
    // One in-memory file (tests).
    void walkBytes(const std::string& name, std::span<const std::uint8_t> bytes, int depth = 0) { classify(name, bytes, depth); }
private:
    Visit visit;
    bool walkDirectory(dusk::native::Disc& disc, dusk::native::DiscDirectory& dir, const std::string& prefix) {
        dusk::native::DiscDirectoryEntry entry;
        while (disc.readDirectory(dir, entry) == dusk::native::DiscFileStatus::ready) {
            const std::string path = prefix + "/" + entry.name.data();
            if (entry.directory) {
                dusk::native::DiscDirectory child;
                if (disc.openDirectory(entry.index, child) != dusk::native::DiscFileStatus::ready) return false;
                if (!walkDirectory(disc, child, path)) return false;
                continue;
            }
            dusk::native::DiscFile file;
            if (disc.openFile(entry.index, file) != dusk::native::DiscFileStatus::ready) return false;
            Bytes data(file.size());
            if (file.size() && !disc.readFile(file, 0, data.data(), data.size())) return false;
            ++stats.files;
            classify(path, data, 0);
        }
        return true;
    }
    static bool endsWith(const std::string& s, const char* suffix) {
        const auto n = std::strlen(suffix);
        if (s.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            char c = s[s.size() - n + i];
            if (c >= 'A' && c <= 'Z') c = char(c + 32);
            if (c != suffix[i]) return false;
        }
        return true;
    }
    void emit(const std::string& path, std::span<const std::uint8_t> view) {
        ++stats.headers;
        Found found{path, view, {}};
        if (!normalize(view, found.normalized)) { ++stats.refused; return; }
        visit(found);
    }
    void classify(const std::string& path, std::span<const std::uint8_t> data, int depth) {
        using namespace dusk::native;
        if (depth > 8 || data.size() < 16) return;
        if (!std::memcmp(data.data(), "Yaz0", 4)) {
            std::uint32_t expanded = 0;
            if (yaz0ExpandedSize(data.data(), data.size(), 256u << 20, expanded) != ResourceStatus::ready) return;
            Bytes out(expanded);
            std::size_t written = 0;
            if (decodeYaz0(data.data(), data.size(), out.data(), out.size(), 256u << 20, written) != ResourceStatus::ready) return;
            out.resize(written);
            ++stats.yaz0; stats.yaz0Bytes += written;
            classify(path, out, depth + 1);
            return;
        }
        if (!std::memcmp(data.data(), "RARC", 4)) {
            RarcArchive archive;
            if (archive.open(data.data(), data.size(), {256u << 20, 4096, 65536, 255}) != ResourceStatus::ready) return;
            ++stats.archives;
            for (std::uint32_t i = 0; i < archive.entryCount(); ++i) {
                RarcEntry e;
                if (archive.entry(i, e) != ResourceStatus::ready || e.directory || !e.contents.data) continue;
                if (!std::strcmp(e.name, ".") || !std::strcmp(e.name, "..")) continue;
                classify(path + ":" + e.name, {e.contents.data, e.contents.size}, depth + 1);
            }
            return;
        }
        if (!std::memcmp(data.data(), "J3D", 3) && data.size() >= 0x20) {
            ++stats.j3d;
            if (j3dFile) j3dFile(path, data);
            const auto sections = be32(data.data() + 12);
            std::size_t at = 0x20;
            for (std::uint32_t s = 0; s < sections && at + 8 <= data.size(); ++s) {
                const auto size = be32(data.data() + at + 4);
                if (size < 8 || size > data.size() - at) break;
                if (!std::memcmp(data.data() + at, "TEX1", 4) && size >= 20) {
                    const auto* t = data.data() + at;
                    const auto count = be16(t + 8);
                    const auto headers = be32(t + 12);
                    for (unsigned i = 0; i < count; ++i) {
                        const std::size_t h = at + headers + std::size_t(i) * 32;
                        if (headers < 20 || h + 32 > at + size) break;
                        if (!be32(data.data() + h + 28)) { ++stats.headers; ++stats.external; continue; }
                        // Same view as the runtime's TEX1 span: header to the end of the block.
                        emit(path + "#" + std::to_string(i), data.subspan(h, at + size - h));
                    }
                }
                at += size;
            }
            return;
        }
        if (!std::memcmp(data.data(), "JPAC", 4) && data.size() >= 16) {
            ++stats.jpc;
            const auto count = be16(data.data() + 10);
            std::size_t at = be32(data.data() + 12);
            for (unsigned i = 0; i < count && at + 0x40 <= data.size(); ++i) {
                const auto size = be32(data.data() + at + 4);
                if (size < 0x40 || size > data.size() - at) break;
                emit(path + "#tex" + std::to_string(i), data.subspan(at + 0x20, size - 0x20));
                at += size;
            }
            return;
        }
        if (endsWith(path, ".bti")) { ++stats.btiFiles; emit(path, data); }
    }
};
}
