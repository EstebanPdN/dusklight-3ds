// Mac-only converter: every BTI of the disc -> pre-converted PICA images in a
// texture section (runtime/pretexture_store.hpp layout). Never linked on 3DS.
// Built and run by Tools/3ds/pretexture_pack.py.
//
// Lossless entries are decodeNativeTexture(normalized BTI, compact plan,
// out, leanPadding=false) from the runtime's own texture_gx.cpp: the same
// bytes the console would compute, all padding filled. Optional ETC1/ETC1A4
// variants (--etc) use rg_etc1 (zlib licence, Upstream/tex3ds-host-lab).
#include "pretexture_walk.hpp"
#include "../../Source/platforms/3ds/runtime/pretexture_store.hpp"
#include "rg_etc1.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace dusk::native;
using pretexture::Bytes;
namespace {
struct Options {
    std::string disc, section, report;
    bool etc = false, etcAll = false;
    unsigned etcMinTexels = 4096;
    double etcMinPsnr = 34.0;
    unsigned threads = 0;
};
auto tupleOf(const PretextureKey& k) {
    return std::make_tuple(k.imageHash, k.paletteHash, k.sourceBytes, k.width, k.height, k.paletteEntries,
                           k.gxFormat, k.mipCount, k.paletteFormat);
}
struct KeyLess { bool operator()(const PretextureKey& a, const PretextureKey& b) const { return tupleOf(a) < tupleOf(b); } };
struct Item {
    PretextureKey key;
    std::string path;
    unsigned occurrences = 0;
    bool stage = false;
    NativeTexturePlan plan{};
    Bytes normalized;             // kept only for ETC candidates
};
struct Record {
    PretextureKey key;
    PretextureVariant variant;
    PretextureFormat format;
    std::uint8_t quality;
    std::uint64_t data;           // byte offset inside the data area
    std::uint32_t bytes, crc;
};
unsigned paddedSize(unsigned n) { unsigned p = 8; while (p < n) p *= 2; return p; }
const char* gxName(unsigned f) {
    switch (f) { case 0: return "I4"; case 1: return "I8"; case 2: return "IA4"; case 3: return "IA8"; case 4: return "RGB565";
    case 5: return "RGB5A3"; case 6: return "RGBA8"; case 8: return "CI4"; case 9: return "CI8"; case 10: return "CI14"; case 14: return "CMPR"; }
    return "?";
}
const char* picaName(unsigned f) {
    switch (f) { case 0: return "RGBA8"; case 1: return "LA8"; case 2: return "LA4"; case 3: return "RGB565"; case 4: return "RGBA5551";
    case 16: return "ETC1"; case 17: return "ETC1A4"; }
    return "?";
}
bool stagePath(const std::string& p) {
    std::string lower = p;
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return lower.rfind("/res/stage/", 0) == 0;
}
// Morton output of one level -> row-major RGBA (padded extent).
void untile(const std::uint8_t* in, NativeTextureFormat f, unsigned pw, unsigned ph, std::vector<std::uint8_t>& out) {
    out.assign(std::size_t(pw) * ph * 4, 0);
    const unsigned bpp = nativeTextureBytesPerPixel(f);
    for (unsigned y = 0; y < ph; ++y) for (unsigned x = 0; x < pw; ++x) {
        const unsigned m = ((y / 8) * (pw / 8) + x / 8) * 64 +
            ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
        const std::uint8_t* t = in + std::size_t(m) * bpp;
        std::uint8_t* o = out.data() + (std::size_t(y) * pw + x) * 4;
        switch (f) {
        case NativeTextureFormat::rgba8: o[0] = t[3]; o[1] = t[2]; o[2] = t[1]; o[3] = t[0]; break;
        case NativeTextureFormat::la8: o[0] = o[1] = o[2] = t[1]; o[3] = t[0]; break;
        case NativeTextureFormat::la4: o[0] = o[1] = o[2] = std::uint8_t((t[0] >> 4) * 17); o[3] = std::uint8_t((t[0] & 15) * 17); break;
        case NativeTextureFormat::rgb565: {
            const unsigned v = t[0] | (t[1] << 8), r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            o[0] = std::uint8_t((r << 3) | (r >> 2)); o[1] = std::uint8_t((g << 2) | (g >> 4)); o[2] = std::uint8_t((b << 3) | (b >> 2)); o[3] = 255; break; }
        case NativeTextureFormat::rgba5551: {
            const unsigned v = t[0] | (t[1] << 8), r = v >> 11, g = (v >> 6) & 31, b = (v >> 1) & 31;
            o[0] = std::uint8_t((r << 3) | (r >> 2)); o[1] = std::uint8_t((g << 3) | (g >> 2)); o[2] = std::uint8_t((b << 3) | (b >> 2)); o[3] = (v & 1) ? 255 : 0; break; }
        }
    }
}
struct EtcResult { bool ok = false; bool alpha = false; Bytes bytes; double psnr = 0; double psnr565 = 0; };
// ETC1 (opaque) or ETC1A4 (alpha, every alpha a multiple of 17) over every
// padded level, PICA order: 8x8 tiles row-major, 4x4 blocks TL TR BL BR,
// rows in the same order as the lossless Morton output (tex3ds layout).
EtcResult encodeEtc(const Item& item) {
    EtcResult r;
    NativeTexturePlan ref;
    const auto& b = item.normalized;
    if (!inspectNativeTexture(b, ref, false, false, false)) return r;
    Bytes reference(ref.outputBytes), compact(item.plan.outputBytes);
    if (!decodeNativeTexture(b, ref, reference) || !decodeNativeTexture(b, item.plan, compact)) return r;
    r.alpha = item.plan.format != NativeTextureFormat::rgb565;
    rg_etc1::etc1_pack_params params; params.clear(); params.m_quality = rg_etc1::cMediumQuality; params.m_dithering = false;
    double se = 0, se565 = 0; std::uint64_t samples = 0;
    std::size_t refAt = 0, compactAt = 0;
    unsigned w = ref.width, h = ref.height;
    std::vector<std::uint8_t> rgba, rgba565;
    for (unsigned level = 0; level < ref.mipCount; ++level) {
        const unsigned pw = paddedSize(w), ph = paddedSize(h);
        untile(reference.data() + refAt, ref.format, pw, ph, rgba);
        untile(compact.data() + compactAt, item.plan.format, pw, ph, rgba565);
        refAt += std::size_t(pw) * ph * nativeTextureBytesPerPixel(ref.format);
        compactAt += std::size_t(pw) * ph * nativeTextureBytesPerPixel(item.plan.format);
        if (r.alpha) for (std::size_t i = 3; i < rgba.size(); i += 4) if (rgba[i] % 17) return r;
        for (unsigned ty = 0; ty < ph; ty += 8) for (unsigned tx = 0; tx < pw; tx += 8)
            for (unsigned blk = 0; blk < 4; ++blk) {
                const unsigned bx = tx + (blk & 1) * 4, by = ty + (blk >> 1) * 4;
                unsigned pixels[16]; std::uint64_t alpha = 0;
                for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
                    const std::uint8_t* p = rgba.data() + (std::size_t(by + y) * pw + bx + x) * 4;
                    pixels[y * 4 + x] = unsigned(p[0]) | (unsigned(p[1]) << 8) | (unsigned(p[2]) << 16) | 0xff000000u;
                    alpha |= std::uint64_t(p[3] / 17) << (4 * (x * 4 + y));
                }
                std::uint8_t block[8];
                rg_etc1::pack_etc1_block(block, pixels, params);
                if (r.alpha) for (unsigned n = 0; n < 8; ++n) r.bytes.push_back(std::uint8_t(alpha >> (8 * n)));
                for (unsigned n = 0; n < 8; ++n) r.bytes.push_back(block[7 - n]);
                unsigned decoded[16];
                rg_etc1::unpack_etc1_block(block, decoded);
                for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
                    if (bx + x >= w || by + y >= h) continue; // logical texels only
                    const std::uint8_t* p = rgba.data() + (std::size_t(by + y) * pw + bx + x) * 4;
                    const std::uint8_t* q = rgba565.data() + (std::size_t(by + y) * pw + bx + x) * 4;
                    const unsigned d = decoded[y * 4 + x];
                    for (unsigned c = 0; c < 3; ++c) {
                        const double e = double(p[c]) - double((d >> (8 * c)) & 255);
                        const double e2 = double(p[c]) - double(q[c]);
                        se += e * e; se565 += e2 * e2;
                    }
                    samples += 3;
                }
            }
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    }
    const double mse = samples ? se / samples : 0, mse565 = samples ? se565 / samples : 0;
    r.psnr = mse ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
    r.psnr565 = mse565 ? 10 * std::log10(255.0 * 255.0 / mse565) : 99;
    r.ok = true;
    return r;
}
void put16(Bytes& o, unsigned v) { o.push_back(std::uint8_t(v)); o.push_back(std::uint8_t(v >> 8)); }
void put32(Bytes& o, std::uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }
void put64(Bytes& o, std::uint64_t v) { put32(o, std::uint32_t(v)); put32(o, std::uint32_t(v >> 32)); }
std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (std::uint8_t(c) < 32) { o += '?'; continue; } o += c; }
    return o;
}
}

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--disc") opt.disc = next();
        else if (a == "--section") opt.section = next();
        else if (a == "--report") opt.report = next();
        else if (a == "--etc") opt.etc = true;
        else if (a == "--etc-scope") { const auto s = next(); if (s != "stage" && s != "all") return 2; opt.etcAll = s == "all"; }
        else if (a == "--etc-min-texels") opt.etcMinTexels = unsigned(std::stoul(next()));
        else if (a == "--etc-min-psnr") opt.etcMinPsnr = std::stod(next());
        else if (a == "--threads") opt.threads = unsigned(std::stoul(next()));
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (opt.disc.empty() || opt.section.empty()) {
        std::fprintf(stderr, "usage: pretexture_build --disc zelda.twilight|game.iso --section out.dlptex [--report r.json] [--etc] ...\n");
        return 2;
    }
    if (!opt.threads) opt.threads = std::max(1u, std::thread::hardware_concurrency());
    const auto start = std::chrono::steady_clock::now();
    Disc disc;
    if (const auto s = disc.open(opt.disc.c_str()); s != DiscStatus::ready) {
        std::fprintf(stderr, "disc: %s\n", discStatusText(s)); return 3;
    }
    std::FILE* data = std::fopen((opt.section + ".data.partial").c_str(), "w+b");
    if (!data) { std::perror("data"); return 4; }
    std::map<PretextureKey, Item, KeyLess> items;
    std::vector<Record> records;
    std::uint64_t dataBytes = 0, occurrences = 0, keyRefused = 0, planRefused = 0, decodeRefused = 0, halfServed = 0;
    struct Hist { std::uint64_t count = 0, source = 0, rgba8Era = 0, output = 0; };
    std::map<std::pair<unsigned, unsigned>, Hist> hist;
    Hist total;
    // Per stage directory (/res/Stage/<name>), deduplicated inside the stage:
    // what one zone's own archives hold (objects and Link come on top).
    struct StageSum { std::set<PretextureKey, KeyLess> keys; std::uint64_t source = 0, output = 0; };
    std::map<std::string, StageSum> stages;
    auto append = [&](const Bytes& bytes) {
        const std::uint64_t at = dataBytes;
        if (std::fwrite(bytes.data(), 1, bytes.size(), data) != bytes.size()) { std::perror("write"); std::exit(5); }
        dataBytes += bytes.size();
        const auto pad = (64 - dataBytes % 64) % 64;
        static const std::uint8_t zeros[64] = {};
        if (pad && std::fwrite(zeros, 1, pad, data) != pad) std::exit(5);
        dataBytes += pad;
        return at;
    };
    pretexture::Walker walker([&](const pretexture::Found& f) {
        ++occurrences;
        PretextureKey key, viewKey;
        if (!pretextureKey(f.normalized, key)) { ++keyRefused; return; }
        // The key ignores header offsets: the archived view gives the same key.
        if (!pretextureKey(f.view, viewKey) || !(viewKey == key)) { std::fprintf(stderr, "key differs between views: %s\n", f.path.c_str()); std::exit(6); }
        std::string stageDir;
        if (stagePath(f.path)) { const auto end = f.path.find('/', 11); stageDir = f.path.substr(0, end); }
        auto [it, fresh] = items.try_emplace(key);
        auto& item = it->second;
        ++item.occurrences;
        item.stage |= stagePath(f.path);
        auto noteStage = [&](const Item& i) {
            if (stageDir.empty() || !i.plan.outputBytes) return;
            auto& st = stages[stageDir];
            if (st.keys.insert(key).second) { st.source += i.plan.sourceBytes + std::uint64_t(i.plan.paletteEntries) * 2; st.output += i.plan.outputBytes; }
        };
        if (!fresh) { noteStage(item); return; }
        item.key = key; item.path = f.path;
        if (!inspectNativeTexture(f.normalized, item.plan, false, false, true)) { ++planRefused; return; }
        Bytes out(item.plan.outputBytes);
        if (!decodeNativeTexture(f.normalized, item.plan, out, false)) { ++decodeRefused; return; }
        NativeTexturePlan half, era;
        if (inspectNativeTexture(f.normalized, half, false, true, true)) ++halfServed;
        inspectNativeTexture(f.normalized, era, false, false, false);
        Record rec{key, PretextureVariant::lossless, PretextureFormat(item.plan.format), 255,
                   append(out), std::uint32_t(out.size()), pretextureCrc32(0, out.data(), out.size())};
        records.push_back(rec);
        auto& h = hist[{item.plan.gxFormat, unsigned(item.plan.format)}];
        const std::uint64_t source = item.plan.sourceBytes + std::uint64_t(item.plan.paletteEntries) * 2;
        for (Hist* x : {&h, &total}) { ++x->count; x->source += source; x->rgba8Era += era.outputBytes; x->output += out.size(); }
        const bool candidate = opt.etc &&
            std::uint64_t(item.plan.originalWidth) * item.plan.originalHeight >= opt.etcMinTexels &&
            (item.plan.format == NativeTextureFormat::rgb565 || item.plan.format == NativeTextureFormat::rgba5551 ||
             (item.plan.format == NativeTextureFormat::rgba8 && item.plan.gxFormat != 4));
        if (candidate) item.normalized = f.normalized;
        noteStage(item);
    });
    if (!walker.walkDisc(disc)) { std::fprintf(stderr, "disc walk failed\n"); return 7; }
    const double walkSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // ETC variants, in parallel. Stage scope is decided after the walk (a
    // texture also used outside /res/Stage stays a candidate: the flag means
    // "seen in a stage archive").
    struct EtcJob { Item* item; EtcResult result; };
    std::vector<EtcJob> jobs;
    for (auto& [k, item] : items) if (!item.normalized.empty() && (opt.etcAll || item.stage)) jobs.push_back({&item, {}});
    std::atomic<std::size_t> nextJob{0};
    if (!jobs.empty()) {
        rg_etc1::pack_etc1_block_init();
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < opt.threads; ++t) pool.emplace_back([&] {
            for (std::size_t n; (n = nextJob.fetch_add(1)) < jobs.size();) jobs[n].result = encodeEtc(*jobs[n].item);
        });
        for (auto& t : pool) t.join();
    }
    struct EtcStats { std::uint64_t tried = 0, encoded = 0, accepted = 0, alphaRefused = 0, replacedBytes = 0, etcBytes = 0; std::map<int, unsigned> psnr; };
    EtcStats es;
    for (auto& job : jobs) {
        ++es.tried;
        if (!job.result.ok) { ++es.alphaRefused; continue; }
        ++es.encoded;
        es.psnr[std::min(60, int(job.result.psnr) / 2 * 2)]++;
        if (job.result.psnr < opt.etcMinPsnr) continue;
        ++es.accepted;
        es.replacedBytes += job.item->plan.outputBytes; es.etcBytes += job.result.bytes.size();
        Record rec{job.item->key, PretextureVariant::etc, job.result.alpha ? PretextureFormat::etc1a4 : PretextureFormat::etc1,
                   std::uint8_t(std::min(254.0, std::floor(job.result.psnr))), append(job.result.bytes),
                   std::uint32_t(job.result.bytes.size()), pretextureCrc32(0, job.result.bytes.data(), job.result.bytes.size())};
        records.push_back(rec);
    }
    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return std::tuple_cat(tupleOf(a.key), std::make_tuple(unsigned(a.variant))) < std::tuple_cat(tupleOf(b.key), std::make_tuple(unsigned(b.variant)));
    });
    // Section: header, index, pixels (64-aligned), trailer.
    Bytes index;
    for (const auto& r : records) {
        put64(index, r.key.imageHash); put32(index, r.key.paletteHash); put32(index, r.key.sourceBytes);
        put16(index, r.key.width); put16(index, r.key.height); put16(index, r.key.paletteEntries);
        index.push_back(r.key.gxFormat); index.push_back(r.key.mipCount); index.push_back(r.key.paletteFormat);
        index.push_back(std::uint8_t(r.variant)); index.push_back(std::uint8_t(r.format)); index.push_back(r.quality);
        if (r.data % 64 || r.data / 64 > 0xffffffffu) return 8;
        put32(index, std::uint32_t(r.data / 64)); put32(index, r.bytes); put32(index, r.crc);
    }
    const std::uint64_t dataOffset = (pretextureHeaderBytes + index.size() + 63) / 64 * 64;
    const std::uint64_t sectionBytes = dataOffset + dataBytes + pretextureTrailerBytes;
    Bytes header;
    for (char c : pretextureMagic) header.push_back(std::uint8_t(c));
    put32(header, pretextureFormatVersion); put32(header, pretextureDecoderVersion); put32(header, pretextureHeaderBytes);
    put32(header, std::uint32_t(records.size())); put32(header, pretextureRecordBytes); put32(header, es.accepted ? 1u : 0u);
    put64(header, pretextureHeaderBytes); put64(header, dataOffset); put64(header, sectionBytes);
    put32(header, pretextureCrc32(0, index.data(), index.size())); put32(header, 0);
    const auto headerCrc = pretextureCrc32(0, header.data(), header.size());
    std::memcpy(header.data() + 60, &headerCrc, 4);
    Bytes trailer;
    for (char c : pretextureTrailerMagic) trailer.push_back(std::uint8_t(c));
    put32(trailer, pretextureFormatVersion); put32(trailer, 0); put64(trailer, sectionBytes);
    trailer.resize(60, 0); put32(trailer, 0);
    const auto trailerCrc = pretextureCrc32(0, trailer.data(), trailer.size());
    std::memcpy(trailer.data() + 60, &trailerCrc, 4);
    std::FILE* out = std::fopen(opt.section.c_str(), "wbx");
    if (!out) { std::perror("section (refusing to overwrite)"); return 9; }
    Bytes pad(dataOffset - pretextureHeaderBytes - index.size(), 0);
    bool ok = std::fwrite(header.data(), 1, header.size(), out) == header.size() &&
              std::fwrite(index.data(), 1, index.size(), out) == index.size() &&
              std::fwrite(pad.data(), 1, pad.size(), out) == pad.size();
    std::fflush(data); std::rewind(data);
    std::vector<std::uint8_t> chunk(1 << 20);
    for (std::uint64_t left = dataBytes; ok && left;) {
        const auto n = std::size_t(std::min<std::uint64_t>(left, chunk.size()));
        ok = std::fread(chunk.data(), 1, n, data) == n && std::fwrite(chunk.data(), 1, n, out) == n;
        left -= n;
    }
    ok = ok && std::fwrite(trailer.data(), 1, trailer.size(), out) == trailer.size();
    ok = std::fclose(out) == 0 && ok;
    std::fclose(data); std::remove((opt.section + ".data.partial").c_str());
    if (!ok) { std::fprintf(stderr, "section write failed\n"); std::remove(opt.section.c_str()); return 10; }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // Report (stdout summary + JSON).
    const auto& ws = walker.stats;
    std::printf("walk: %llu files, %llu archives, %llu Yaz0 (%.1f MiB expanded), %llu J3D, %llu JPC, %llu .bti, %llu texture headers (%llu external, %llu refused by the runtime checks)\n",
        (unsigned long long)ws.files, (unsigned long long)ws.archives, (unsigned long long)ws.yaz0, ws.yaz0Bytes / 1048576.0,
        (unsigned long long)ws.j3d, (unsigned long long)ws.jpc, (unsigned long long)ws.btiFiles, (unsigned long long)ws.headers,
        (unsigned long long)ws.external, (unsigned long long)ws.refused);
    std::printf("textures: %llu occurrences, %zu unique, %llu lossless entries, plan refused %llu, decode refused %llu, half-servable %llu\n",
        (unsigned long long)occurrences, items.size(), (unsigned long long)total.count, (unsigned long long)planRefused,
        (unsigned long long)decodeRefused, (unsigned long long)halfServed);
    std::printf("bytes (unique): GX source %.1f MiB, runtime RGBA8-era %.1f MiB, compact lossless %.1f MiB\n",
        total.source / 1048576.0, total.rgba8Era / 1048576.0, total.output / 1048576.0);
    std::printf("%-8s %-9s %7s %12s %12s %12s\n", "GX", "PICA", "count", "GX MiB", "RGBA8-era", "stored MiB");
    for (const auto& [k, h] : hist)
        std::printf("%-8s %-9s %7llu %12.2f %12.2f %12.2f\n", gxName(k.first), picaName(k.second), (unsigned long long)h.count,
                    h.source / 1048576.0, h.rgba8Era / 1048576.0, h.output / 1048576.0);
    if (opt.etc) {
        std::printf("etc: %llu candidates, %llu encoded, %llu alpha not 4-bit exact, %llu accepted (PSNR >= %.1f dB): %.2f MiB lossless -> %.2f MiB ETC\n",
            (unsigned long long)es.tried, (unsigned long long)es.encoded, (unsigned long long)es.alphaRefused, (unsigned long long)es.accepted,
            opt.etcMinPsnr, es.replacedBytes / 1048576.0, es.etcBytes / 1048576.0);
        std::printf("etc PSNR histogram (dB bucket: count):");
        for (const auto& [b, n] : es.psnr) std::printf(" %d:%u", b, n);
        std::printf("\n");
    }
    std::printf("section: %s, %zu records, index %.1f KiB, %.1f MiB total; walk %.1f s, total %.1f s\n", opt.section.c_str(),
        records.size(), index.size() / 1024.0, sectionBytes / 1048576.0, walkSeconds, seconds);
    if (!opt.report.empty()) {
        std::FILE* j = std::fopen(opt.report.c_str(), "wx");
        if (!j) { std::perror("report"); return 11; }
        std::fprintf(j, "{\n \"section_bytes\": %llu,\n \"records\": %zu,\n \"index_bytes\": %zu,\n \"unique_textures\": %zu,\n \"occurrences\": %llu,\n",
            (unsigned long long)sectionBytes, records.size(), index.size(), items.size(), (unsigned long long)occurrences);
        std::fprintf(j, " \"walk\": {\"files\": %llu, \"archives\": %llu, \"yaz0\": %llu, \"j3d\": %llu, \"jpc\": %llu, \"bti_files\": %llu, \"headers\": %llu, \"external\": %llu, \"refused\": %llu},\n",
            (unsigned long long)ws.files, (unsigned long long)ws.archives, (unsigned long long)ws.yaz0, (unsigned long long)ws.j3d,
            (unsigned long long)ws.jpc, (unsigned long long)ws.btiFiles, (unsigned long long)ws.headers, (unsigned long long)ws.external, (unsigned long long)ws.refused);
        std::fprintf(j, " \"plan_refused\": %llu, \"decode_refused\": %llu, \"half_servable\": %llu,\n",
            (unsigned long long)planRefused, (unsigned long long)decodeRefused, (unsigned long long)halfServed);
        std::fprintf(j, " \"totals\": {\"count\": %llu, \"gx_source_bytes\": %llu, \"rgba8_era_bytes\": %llu, \"lossless_bytes\": %llu},\n",
            (unsigned long long)total.count, (unsigned long long)total.source, (unsigned long long)total.rgba8Era, (unsigned long long)total.output);
        std::fprintf(j, " \"formats\": [");
        bool first = true;
        for (const auto& [k, h] : hist) {
            std::fprintf(j, "%s\n  {\"gx\": \"%s\", \"pica\": \"%s\", \"count\": %llu, \"gx_source_bytes\": %llu, \"rgba8_era_bytes\": %llu, \"stored_bytes\": %llu}",
                first ? "" : ",", gxName(k.first), picaName(k.second), (unsigned long long)h.count, (unsigned long long)h.source,
                (unsigned long long)h.rgba8Era, (unsigned long long)h.output);
            first = false;
        }
        std::fprintf(j, "\n ],\n \"etc\": {\"enabled\": %s, \"scope\": \"%s\", \"min_texels\": %u, \"min_psnr\": %.2f, \"candidates\": %llu, \"encoded\": %llu, \"alpha_refused\": %llu, \"accepted\": %llu, \"replaced_lossless_bytes\": %llu, \"etc_bytes\": %llu, \"psnr_histogram\": {",
            opt.etc ? "true" : "false", opt.etcAll ? "all" : "stage", opt.etcMinTexels, opt.etcMinPsnr, (unsigned long long)es.tried,
            (unsigned long long)es.encoded, (unsigned long long)es.alphaRefused, (unsigned long long)es.accepted,
            (unsigned long long)es.replacedBytes, (unsigned long long)es.etcBytes);
        first = true;
        for (const auto& [b, n] : es.psnr) { std::fprintf(j, "%s\"%d\": %u", first ? "" : ", ", b, n); first = false; }
        std::fprintf(j, "}},\n \"etc_samples\": [");
        first = true;
        unsigned shown = 0;
        for (const auto& job : jobs) {
            if (!job.result.ok || shown >= 400) continue;
            ++shown;
            const auto& p = job.item->plan;
            std::fprintf(j, "%s\n  {\"path\": \"%s\", \"gx\": \"%s\", \"w\": %u, \"h\": %u, \"mips\": %u, \"lossless\": \"%s\", \"etc\": \"%s\", \"psnr_etc\": %.2f, \"psnr_lossless\": %.2f, \"lossless_bytes\": %zu, \"etc_bytes\": %zu}",
                first ? "" : ",", jsonEscape(job.item->path).c_str(), gxName(p.gxFormat), p.originalWidth, p.originalHeight, p.originalMipCount,
                picaName(unsigned(p.format)), job.result.alpha ? "ETC1A4" : "ETC1", job.result.psnr, job.result.psnr565, p.outputBytes, job.result.bytes.size());
            first = false;
        }
        std::fprintf(j, "\n ],\n \"stages\": {");
        first = true;
        for (const auto& [name, st] : stages) {
            std::fprintf(j, "%s\n  \"%s\": {\"textures\": %zu, \"gx_source_bytes\": %llu, \"lossless_bytes\": %llu}", first ? "" : ",",
                jsonEscape(name).c_str(), st.keys.size(), (unsigned long long)st.source, (unsigned long long)st.output);
            first = false;
        }
        std::fprintf(j, "\n },\n \"seconds\": %.2f\n}\n", seconds);
        std::fclose(j);
    }
    return 0;
}
