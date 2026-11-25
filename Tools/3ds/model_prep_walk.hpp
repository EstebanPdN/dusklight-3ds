// Mac-only: every room model of the disc (the files daBg_c::createHeap asks
// the room archive for: model.bmd/model1.bmd .. model5.bmd, or the .bdl of
// the same number), read with the runtime's own disc and archive readers, with
// the btk/brk files beside them.
#pragma once
#include "model_prep_bmd.hpp"
#include "../../Source/platforms/3ds/runtime/disc.hpp"
#include "../../Source/platforms/3ds/runtime/resource_archive.hpp"
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace modelprep {
struct RoomModel {
    std::string stage;            // e.g. F_SP103
    unsigned room = 0;            // R<room>_00.arc
    unsigned slot = 0;            // 0..5 (model.bmd, model1.bmd, ...)
    std::string file;             // basename inside the archive
    Bytes bytes;                  // the model file as loaded (Yaz0 peeled)
    std::set<std::string> animatedMaterials;   // names a btk/brk of the same slot updates
    bool hasBtk = false, hasBrk = false;
};

inline bool readDiscFile(dusk::native::Disc& disc, std::uint32_t index, Bytes& out) {
    dusk::native::DiscFile file;
    if (disc.openFile(index, file) != dusk::native::DiscFileStatus::ready) return false;
    out.resize(file.size());
    return !file.size() || disc.readFile(file, 0, out.data(), out.size());
}
inline bool peelYaz0(Bytes& data) {
    using namespace dusk::native;
    if (data.size() < 16 || std::memcmp(data.data(), "Yaz0", 4)) return true;
    std::uint32_t expanded = 0;
    if (yaz0ExpandedSize(data.data(), data.size(), 256u << 20, expanded) != ResourceStatus::ready) return false;
    Bytes out(expanded);
    std::size_t written = 0;
    if (decodeYaz0(data.data(), data.size(), out.data(), out.size(), 256u << 20, written) != ResourceStatus::ready) return false;
    out.resize(written);
    data.swap(out);
    return true;
}
// Material names of a J3D animation name table block (TTK1 / TRK1).
inline void animationNames(const Bytes& f, std::set<std::string>& names) {
    if (f.size() < 32 || std::memcmp(f.data(), "J3D1", 4)) return;
    const auto blocks = be32(f.data() + 12);
    std::size_t at = 32;
    for (std::uint32_t b = 0; b < blocks && at + 8 <= f.size(); ++b) {
        const auto n = be32(f.data() + at + 4);
        if (n < 8 || n > f.size() - at) return;
        const auto* blk = f.data() + at;
        std::vector<std::uint32_t> tabs;
        if (!std::memcmp(blk, "TTK1", 4) && n >= 0x60) tabs = {be32(blk + 0x1C), be32(blk + 0x44)};
        if (!std::memcmp(blk, "TRK1", 4) && n >= 0x40) tabs = {be32(blk + 0x30), be32(blk + 0x34)};
        for (const auto off : tabs) {
            if (!off || off + 4 > n) continue;
            const unsigned count = be16(blk + off);
            for (unsigned i = 0; i < count; ++i) {
                auto s = nameAt(blk + off, n - off, i);
                if (!s.empty()) names.insert(s);
            }
        }
        at += n;
    }
}

// Calls visit for every room model; returns false on a disc error.
inline bool walkRoomModels(dusk::native::Disc& disc, const std::function<void(RoomModel&)>& visit,
                           unsigned& archives) {
    using namespace dusk::native;
    DiscDirectory stages;
    if (disc.findDirectory("/res/Stage", stages) != DiscFileStatus::ready) return false;
    DiscDirectoryEntry stageEntry;
    while (disc.readDirectory(stages, stageEntry) == DiscFileStatus::ready) {
        if (!stageEntry.directory) continue;
        DiscDirectory dir;
        if (disc.openDirectory(stageEntry.index, dir) != DiscFileStatus::ready) return false;
        DiscDirectoryEntry e;
        while (disc.readDirectory(dir, e) == DiscFileStatus::ready) {
            if (e.directory) continue;
            const std::string name = e.name.data();
            unsigned room = 0;
            if (name.size() != 10 || (name[0] != 'R' && name[0] != 'r') || name.substr(3) != "_00.arc") continue;
            if (name[1] < '0' || name[1] > '9' || name[2] < '0' || name[2] > '9') continue;
            room = unsigned((name[1] - '0') * 10 + (name[2] - '0'));
            Bytes arc;
            if (!readDiscFile(disc, e.index, arc) || !peelYaz0(arc)) return false;
            RarcArchive archive;
            if (archive.open(arc.data(), arc.size(), {256u << 20, 4096, 65536, 255}) != ResourceStatus::ready) continue;
            ++archives;
            static const char* const bmd[6] = {"model.bmd", "model1.bmd", "model2.bmd", "model3.bmd", "model4.bmd", "model5.bmd"};
            static const char* const bdl[6] = {"model.bdl", "model1.bdl", "model2.bdl", "model3.bdl", "model4.bdl", "model5.bdl"};
            static const char* const btk[6] = {"model.btk", "model1.btk", "model2.btk", "model3.btk", "model4.btk", "model5.btk"};
            static const char* const brk[6] = {"model.brk", "model1.brk", "model2.brk", "model3.brk", "model4.brk", "model5.brk"};
            for (unsigned slot = 0; slot < 6; ++slot) {
                RarcEntry entry;
                const char* file = bmd[slot];
                if (archive.findFile(file, entry) != ResourceStatus::ready) {
                    file = bdl[slot];
                    if (archive.findFile(file, entry) != ResourceStatus::ready) continue;
                }
                RoomModel model;
                model.stage = stageEntry.name.data();
                model.room = room;
                model.slot = slot;
                model.file = file;
                model.bytes.assign(entry.contents.data, entry.contents.data + entry.contents.size);
                if (!peelYaz0(model.bytes)) continue;
                for (const auto* anim : {btk[slot], brk[slot]}) {
                    RarcEntry a;
                    if (archive.findFile(anim, a) != ResourceStatus::ready) continue;
                    Bytes ab(a.contents.data, a.contents.data + a.contents.size);
                    if (!peelYaz0(ab)) continue;
                    (anim == btk[slot] ? model.hasBtk : model.hasBrk) = true;
                    animationNames(ab, model.animatedMaterials);
                }
                visit(model);
            }
        }
    }
    return true;
}

// Every J3D model file of the disc (any FST file or archive member, Yaz0
// peeled at any depth), with its path ("/res/Object/Cow.arc:cow.bmd").
inline void classifyModels(const std::string& path, Bytes& data, int depth,
                           const std::function<void(const std::string&, const Bytes&)>& visit) {
    using namespace dusk::native;
    if (depth > 6 || data.size() < 16) return;
    if (!std::memcmp(data.data(), "Yaz0", 4)) {
        if (!peelYaz0(data)) return;
        classifyModels(path, data, depth + 1, visit);
        return;
    }
    if (!std::memcmp(data.data(), "RARC", 4)) {
        RarcArchive archive;
        if (archive.open(data.data(), data.size(), {256u << 20, 4096, 65536, 255}) != ResourceStatus::ready) return;
        for (std::uint32_t i = 0; i < archive.entryCount(); ++i) {
            RarcEntry e;
            if (archive.entry(i, e) != ResourceStatus::ready || e.directory || !e.contents.data) continue;
            if (!std::strcmp(e.name, ".") || !std::strcmp(e.name, "..")) continue;
            Bytes member(e.contents.data, e.contents.data + e.contents.size);
            classifyModels(path + ":" + e.name, member, depth + 1, visit);
        }
        return;
    }
    if (!std::memcmp(data.data(), "J3D2bmd3", 8) || !std::memcmp(data.data(), "J3D2bdl4", 8)) visit(path, data);
}
inline bool walkAllModels(dusk::native::Disc& disc, dusk::native::DiscDirectory& dir, const std::string& prefix,
                          const std::function<void(const std::string&, const Bytes&)>& visit) {
    using namespace dusk::native;
    DiscDirectoryEntry e;
    while (disc.readDirectory(dir, e) == DiscFileStatus::ready) {
        const std::string path = prefix + "/" + e.name.data();
        if (e.directory) {
            DiscDirectory child;
            if (disc.openDirectory(e.index, child) != DiscFileStatus::ready || !walkAllModels(disc, child, path, visit)) return false;
            continue;
        }
        Bytes bytes;
        if (!readDiscFile(disc, e.index, bytes)) return false;
        classifyModels(path, bytes, 0, visit);
    }
    return true;
}
}
