// Mac-only survey (built and run by Tools/3ds/mip_inventory.py): which disc
// textures are authored without mip levels, their bytes, and how the game
// samples them, to decide which may get generated levels (worldMips) and which
// never may. Never linked on the 3DS.
//
// Every texture is counted once (pretextureKey: same image, palette and
// format), wherever it occurs. A TEX1 image is classified by the materials of
// its J3D file that sample it (MAT3, read as J3DMaterialFactory reads it):
//   uv      a TEV stage samples it through a GX_TG_MTX2x4 texgen of a vertex
//           texture coordinate, in a material without indirect stages: the
//           world use worldMips accepts at run time;
//   uv3x4   the same through GX_TG_MTX3x4 (the runtime refuses a projective
//           texgen);
//   env     texgen from position, normal or binormals (environment and
//           projected maps);
//   ramp    texgen from a colour channel (SRTG) or an emboss (bump) texgen;
//   indirect an indirect map, or any texture of a material with indirect
//           stages (warped);
//   unused  no TEV stage of its file samples it.
// Stand-alone .bti files (2D layouts, external model images, weather) and
// particle textures (JPAC) carry no material: they are listed by origin.
// Fonts, DOL images and images made at run time are not on the disc as BTIs.
#include "pretexture_walk.hpp"
#include "../../Source/platforms/3ds/runtime/pretexture_store.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace dusk::native;
using pretexture::be16;
using pretexture::be32;
namespace {
enum Use : unsigned { uv = 1, uv3x4 = 2, env = 4, ramp = 8, indirect = 16, unused = 32 };
enum Origin : unsigned { roomModel = 1, objectModel = 2, otherModel = 4, particle = 8, layoutBti = 16, otherBti = 32 };
struct Tex {
    unsigned gxFormat = 0, width = 0, height = 0, mips = 0, minFilter = 0;
    NativeTexturePlan plan{};       // storage as the runtime plans an immutable image (compact)
    unsigned uses = 0, origins = 0, occurrences = 0;
    bool alphaTested = false, blended = false;
    std::string example;
};
auto tupleOf(const PretextureKey& k) {
    return std::make_tuple(k.imageHash, k.paletteHash, k.sourceBytes, k.width, k.height, k.paletteEntries, k.gxFormat,
                           k.mipCount, k.paletteFormat);
}
struct KeyLess { bool operator()(const PretextureKey& a, const PretextureKey& b) const { return tupleOf(a) < tupleOf(b); } };
std::map<PretextureKey, Tex, KeyLess> textures;
std::uint64_t j3dFiles = 0, j3dRefused = 0, externalImages = 0;

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
unsigned modelOrigin(const std::string& path) {
    if (contains(path, "/res/Stage/")) return roomModel;
    if (contains(path, "/res/Object/")) return objectModel;
    return otherModel;
}
const std::uint8_t* block(std::span<const std::uint8_t> f, const char* tag, std::uint32_t& size) {
    const auto sections = be32(f.data() + 12);
    std::size_t at = 0x20;
    for (std::uint32_t s = 0; s < sections && at + 8 <= f.size(); ++s) {
        const auto n = be32(f.data() + at + 4);
        if (n < 8 || n > f.size() - at) return nullptr;
        if (!std::memcmp(f.data() + at, tag, 4)) { size = n; return f.data() + at; }
        at += n;
    }
    return nullptr;
}
// Levels a full chain would have (last level >= 8x8, the PICA's smallest),
// on power-of-two extents without clamped padding. 1 = none possible.
unsigned chainLevels(const NativeTexturePlan& p) {
    if (p.width != p.paddedWidth || p.height != p.paddedHeight || p.width < 16 || p.height < 16) return 1;
    unsigned levels = 1;
    for (unsigned w = p.width, h = p.height; w >= 16 && h >= 16; w /= 2, h /= 2) ++levels;
    return levels;
}
std::size_t chainExtra(const NativeTexturePlan& p) {
    std::size_t extra = 0;
    const unsigned levels = chainLevels(p);
    for (unsigned n = 1; n < levels; ++n)
        extra += std::size_t(p.width >> n) * (p.height >> n) * nativeTextureBytesPerPixel(p.format);
    return extra;
}
Tex* note(std::span<const std::uint8_t> view, const std::string& path, unsigned origin) {
    PretextureKey key;
    if (!pretextureKey(view, key)) return nullptr;
    auto [it, fresh] = textures.try_emplace(key);
    auto& t = it->second;
    if (fresh) {
        t.gxFormat = view[0]; t.width = be16(view.data() + 2); t.height = be16(view.data() + 4);
        t.mips = view[24]; t.minFilter = view[20]; t.example = path;
        if (!inspectNativeTexture(view, t.plan, false, false, true)) t.plan = {};
    }
    t.origins |= origin;
    ++t.occurrences;
    return &t;
}
// One J3D file: its TEX1 images and how its MAT3 materials sample them.
void j3d(const std::string& path, std::span<const std::uint8_t> f) {
    ++j3dFiles;
    std::uint32_t texN = 0, matN = 0;
    const auto* tex1 = block(f, "TEX1", texN);
    const auto* mat3 = block(f, "MAT3", matN);
    if (!tex1 || texN < 20) return;
    const unsigned count = be16(tex1 + 8);
    const auto headers = be32(tex1 + 12);
    std::vector<Tex*> images(count, nullptr);
    std::vector<unsigned> uses(count, 0);
    std::vector<bool> tested(count, false), blended(count, false);
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t h = std::size_t(tex1 - f.data()) + headers + std::size_t(i) * 32;
        if (headers < 20 || h + 32 > std::size_t(tex1 - f.data()) + texN) break;
        if (!be32(f.data() + h + 28)) { ++externalImages; continue; }
        images[i] = note(f.subspan(h, std::size_t(tex1 - f.data()) + texN - h), path + "#" + std::to_string(i), modelOrigin(path));
    }
    if (!mat3 || matN < 0x84) { ++j3dRefused; return; }
    auto table = [&](unsigned offset) { return be32(mat3 + offset); };
    const unsigned materials = be16(mat3 + 8);
    const auto initData = table(0x0C), remap = table(0x10), ind = table(0x18), texGenNum = table(0x34),
               texCoord = table(0x38), texNo = table(0x48), tevOrder = table(0x4C), tevStageNum = table(0x58),
               alphaComp = table(0x6C), blend = table(0x70);
    auto in = [&](std::uint64_t at, std::uint64_t bytes) { return at + bytes <= matN; };
    for (unsigned m = 0; m < materials; ++m) {
        if (!in(remap + 2u * m, 2)) { ++j3dRefused; return; }
        const std::uint64_t init = initData + std::uint64_t(be16(mat3 + remap + 2 * m)) * 0x14C;
        if (!in(init, 0x14C)) { ++j3dRefused; return; }
        const auto* d = mat3 + init;
        const unsigned stages = in(tevStageNum + d[4], 1) ? mat3[tevStageNum + d[4]] : 0;
        (void)texGenNum;
        bool indirectMaterial = false;
        std::set<unsigned> indirectMaps;
        if (ind && in(ind + std::uint64_t(m) * 0x138, 0x138)) {
            const auto* e = mat3 + ind + std::uint64_t(m) * 0x138;
            if (e[0] == 1 && e[1]) {
                indirectMaterial = true;
                for (unsigned n = 0; n < e[1] && n < 3; ++n) indirectMaps.insert(e[4 + 4 * n + 1]);
            }
        }
        bool test = false, blending = false;
        if (const auto a = be16(d + 0x146); a != 0xFFFF && in(alphaComp + 8u * a, 8)) {
            const auto* c = mat3 + alphaComp + 8u * a;
            // Passes always: (ALWAYS op x) for OR, (ALWAYS and ALWAYS) for AND.
            const bool always0 = c[0] == 7, always1 = c[3] == 7;
            test = !(c[2] == 1 ? (always0 || always1) : c[2] == 0 ? (always0 && always1) : false);
        }
        if (const auto b = be16(d + 0x148); b != 0xFFFF && in(blend + 4u * b, 4)) blending = mat3[blend + 4u * b] == 1;
        auto textureOf = [&](unsigned map) -> int {
            if (map > 7) return -1;
            const auto slot = be16(d + 0x84 + 2 * map);
            if (slot == 0xFFFF || !in(texNo + 2u * slot, 2)) return -1;
            const auto index = be16(mat3 + texNo + 2u * slot);
            return index < count ? int(index) : -1;
        };
        for (const auto map : indirectMaps) if (const int t = textureOf(map); t >= 0) uses[t] |= indirect;
        for (unsigned s = 0; s < stages && s < 16; ++s) {
            const auto o = be16(d + 0xBC + 2 * s);
            if (o == 0xFFFF || !in(tevOrder + 4u * o, 4)) continue;
            const auto* order = mat3 + tevOrder + 4u * o;
            const int t = textureOf(order[1]);
            if (t < 0) continue;
            unsigned use = indirect;
            if (!indirectMaterial) {
                use = ramp;
                if (order[0] < 8) {
                    const auto c = be16(d + 0x28 + 2 * order[0]);
                    if (c != 0xFFFF && in(texCoord + 4u * c, 4)) {
                        const auto* g = mat3 + texCoord + 4u * c;
                        const unsigned type = g[0], source = g[1];
                        if (source >= 4 && source <= 11 && (type == 0 || type == 1)) use = type == 1 ? uv : uv3x4;
                        else if (source <= 3 && (type == 0 || type == 1)) use = env;
                    }
                }
            }
            uses[t] |= use;
            if (use == uv) { tested[t] = tested[t] || test; blended[t] = blended[t] || blending; }
        }
    }
    for (unsigned i = 0; i < count; ++i) if (images[i]) {
        images[i]->uses |= uses[i] ? uses[i] : unused;
        images[i]->alphaTested |= tested[i];
        images[i]->blended |= blended[i];
    }
}
const char* gxName(unsigned f) {
    switch (f) { case 0: return "I4"; case 1: return "I8"; case 2: return "IA4"; case 3: return "IA8"; case 4: return "RGB565";
    case 5: return "RGB5A3"; case 6: return "RGBA8"; case 8: return "CI4"; case 9: return "CI8"; case 10: return "CI14"; case 14: return "CMPR"; }
    return "?";
}
const char* picaName(NativeTextureFormat f) {
    switch (f) { case NativeTextureFormat::rgba8: return "RGBA8"; case NativeTextureFormat::la8: return "LA8";
    case NativeTextureFormat::la4: return "LA4"; case NativeTextureFormat::rgb565: return "RGB565"; case NativeTextureFormat::rgba5551: return "RGBA5551"; }
    return "?";
}
struct Sum { std::uint64_t count = 0, bytes = 0, extra = 0;
    void add(const Tex& t) { ++count; bytes += t.plan.outputBytes; extra += chainExtra(t.plan); } };
void line(const char* name, const Sum& s) {
    std::printf("  %-58s %6llu  %9.2f MiB  +%7.2f MiB\n", name, (unsigned long long)s.count, s.bytes / 1048576.0, s.extra / 1048576.0);
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: mip_inventory <zelda.twilight|game.iso> [--list N]\n"); return 2; }
    const unsigned list = argc > 3 && !std::strcmp(argv[2], "--list") ? unsigned(std::stoul(argv[3])) : 0;
    Disc disc;
    if (const auto s = disc.open(argv[1]); s != DiscStatus::ready) { std::fprintf(stderr, "disc: %s\n", discStatusText(s)); return 3; }
    pretexture::Walker walker([&](const pretexture::Found& f) {
        const auto hash = f.path.rfind('#');
        if (hash != std::string::npos && hash + 1 < f.path.size() && f.path[hash + 1] >= '0' && f.path[hash + 1] <= '9') return; // TEX1: j3d()
        const bool jpc = hash != std::string::npos && !f.path.compare(hash, 4, "#tex");
        note(f.view, f.path, jpc ? particle : contains(f.path, "/res/Layout/") || contains(f.path, "timg/") ? layoutBti : otherBti);
    });
    walker.j3dFile = j3d;
    if (!walker.walkDisc(disc)) { std::fprintf(stderr, "disc walk failed\n"); return 7; }

    Sum all, authored, mipless, world, worldRoom, worldObject, worldOther, worldNoChain, worldTested, worldBlended,
        uv3, envs, ramps, indirects, unusedJ3d, particles, layouts, others, worldIndirectToo;
    std::map<std::tuple<unsigned, unsigned, unsigned>, Sum> worldShapes;
    std::map<unsigned, std::uint64_t> authoredFilters, worldFilters;
    for (const auto& [key, t] : textures) {
        if (!t.plan.outputBytes) continue;
        all.add(t);
        if (t.mips > 1) { authored.add(t); ++authoredFilters[t.minFilter]; continue; }
        mipless.add(t);
        const bool model = t.origins & (roomModel | objectModel | otherModel);
        if (model && (t.uses & uv)) {
            // A texture with any other use too is still sampled through the
            // world rule for its plain uses; its other uses keep level 0.
            world.add(t);
            if (t.origins & roomModel) worldRoom.add(t);
            if (t.origins & objectModel) worldObject.add(t);
            if (!(t.origins & (roomModel | objectModel))) worldOther.add(t);
            if (chainLevels(t.plan) == 1) worldNoChain.add(t);
            if (t.alphaTested) worldTested.add(t);
            if (t.blended) worldBlended.add(t);
            if (t.uses & (env | ramp | indirect | uv3x4)) worldIndirectToo.add(t);
            ++worldFilters[t.minFilter];
            worldShapes[{t.gxFormat, std::min(t.width, t.height) >= 16 && chainLevels(t.plan) > 1, unsigned(t.plan.format)}].add(t);
            continue;
        }
        if (model && (t.uses & uv3x4)) { uv3.add(t); continue; }
        if (model && (t.uses & indirect)) { indirects.add(t); continue; }
        if (model && (t.uses & env)) { envs.add(t); continue; }
        if (model && (t.uses & ramp)) { ramps.add(t); continue; }
        if (model) { unusedJ3d.add(t); continue; }
        if (t.origins & particle) { particles.add(t); continue; }
        if (t.origins & layoutBti) { layouts.add(t); continue; }
        others.add(t);
    }
    std::printf("Disc: %llu J3D files (%llu without a readable MAT3), %llu external TEX1 entries (their .bti is counted as a file)\n",
                (unsigned long long)j3dFiles, (unsigned long long)j3dRefused, (unsigned long long)externalImages);
    std::printf("Unique textures, PICA storage as the runtime plans immutable images (compact); last column: a full chain\n");
    std::printf("  %-58s %6s  %13s  %12s\n", "", "count", "level 0", "chain adds");
    line("all textures", all);
    line("authored with mip levels", authored);
    line("authored without mip levels", mipless);
    std::printf(" without mips, by how the game samples them:\n");
    line("J3D, plain uv (MTX2x4 of a vertex coordinate): worldMips", world);
    line("  of them in stage (room) archives", worldRoom);
    line("  of them in object archives", worldObject);
    line("  of them elsewhere (Link, items, NPC data...)", worldOther);
    line("  of them alpha-tested by a material", worldTested);
    line("  of them blended by a material", worldBlended);
    line("  of them also sampled another way (that use keeps level 0)", worldIndirectToo);
    line("  of them NPOT, padded or < 16 px: no chain, left alone", worldNoChain);
    line("J3D, MTX3x4 only (projective: refused)", uv3);
    line("J3D, indirect map or warped by an indirect stage (never)", indirects);
    line("J3D, environment or projected (position/normal source: never)", envs);
    line("J3D, colour-channel ramp or emboss (never)", ramps);
    line("J3D, no TEV stage samples it", unusedJ3d);
    line("particles (JPAC: immediate draws, never)", particles);
    line("2D layouts (/res/Layout, timg: never)", layouts);
    line("other stand-alone BTI (weather, external, misc: never)", others);
    std::printf(" world candidates by GX format, chain possible, PICA storage:\n");
    for (const auto& [k, s] : worldShapes) {
        char name[96];
        std::snprintf(name, sizeof name, "%s, %s, %s", gxName(std::get<0>(k)), std::get<1>(k) ? "chain" : "no chain",
                      picaName(NativeTextureFormat(std::get<2>(k))));
        line(name, s);
    }
    std::printf(" GX minification filter (0 near, 1 linear, 2 near-mip-near, 3 lin-mip-near, 4 near-mip-lin, 5 lin-mip-lin):\n");
    std::printf("  authored mips:");
    for (const auto& [f, n] : authoredFilters) std::printf("  %u: %llu", f, (unsigned long long)n);
    std::printf("\n  world candidates:");
    for (const auto& [f, n] : worldFilters) std::printf("  %u: %llu", f, (unsigned long long)n);
    std::printf("\n");
    if (list) {
        std::printf(" largest world candidates:\n");
        std::vector<const Tex*> sorted;
        for (const auto& [k, t] : textures) if (t.plan.outputBytes && t.mips <= 1 && (t.uses & uv) && (t.origins & (roomModel | objectModel | otherModel))) sorted.push_back(&t);
        std::sort(sorted.begin(), sorted.end(), [](const Tex* a, const Tex* b) { return a->plan.outputBytes > b->plan.outputBytes; });
        for (unsigned n = 0; n < list && n < sorted.size(); ++n) {
            const auto& t = *sorted[n];
            std::printf("  %-7s %4ux%-4u -> %-8s %7zu B +%6zu B x%-3u %s%s %s\n", gxName(t.gxFormat), t.width, t.height, picaName(t.plan.format),
                        t.plan.outputBytes, chainExtra(t.plan), t.occurrences, t.alphaTested ? "atest " : "", t.blended ? "blend" : "", t.example.c_str());
        }
    }
    return 0;
}
