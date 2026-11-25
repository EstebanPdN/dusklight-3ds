// Mac-only test: every texture of the disc, read back through the runtime
// reader (runtime/pretexture_store.cpp), must equal the runtime decoder
// (runtime/texture_gx.cpp) byte for byte, for the plans the runtime uses.
//   pretexture_store_test --disc zelda.twilight --store section-or-bundle [--scratch dir]
// Exit 0 only when every check passes. Prints one summary line per check.
#include "pretexture_walk.hpp"
#include "../../Source/platforms/3ds/runtime/pretexture_store.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace dusk::native;
using pretexture::Bytes;
namespace {
unsigned failures = 0;
void fail(const std::string& what, const std::string& path) {
    if (++failures <= 20) std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), path.c_str());
}
unsigned paddedSize(unsigned n) { unsigned p = 8; while (p < n) p *= 2; return p; }
std::size_t etcBytes(const PretextureKey& k, bool alpha) {
    std::size_t total = 0; unsigned w = k.width, h = k.height;
    for (unsigned l = 0; l < k.mipCount; ++l) { total += std::size_t(paddedSize(w)) * paddedSize(h) / (alpha ? 1 : 2); w = std::max(1u, w / 2); h = std::max(1u, h / 2); }
    return total;
}
bool copyFile(const std::string& from, const std::string& to, std::size_t keepTail, const std::function<void(Bytes&)>& edit) {
    // Copies only the last keepTail bytes (the section), then edits them.
    std::FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    fseeko(in, 0, SEEK_END);
    const auto size = std::size_t(ftello(in));
    const auto take = std::min(size, keepTail);
    Bytes tail(take);
    fseeko(in, off_t(size - take), SEEK_SET);
    const bool ok = std::fread(tail.data(), 1, take, in) == take;
    std::fclose(in);
    if (!ok) return false;
    edit(tail);
    std::FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) return false;
    const bool w = std::fwrite(tail.data(), 1, tail.size(), out) == tail.size();
    return std::fclose(out) == 0 && w;
}
}

int main(int argc, char** argv) {
    std::string discPath, storePath, scratch = "/tmp";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--disc") discPath = argv[i + 1];
        else if (a == "--store") storePath = argv[i + 1];
        else if (a == "--scratch") scratch = argv[i + 1];
    }
    if (discPath.empty() || storePath.empty()) { std::fprintf(stderr, "usage: --disc D --store S [--scratch dir]\n"); return 2; }
    Disc disc;
    if (disc.open(discPath.c_str()) != DiscStatus::ready) { std::fprintf(stderr, "disc not ready\n"); return 3; }
    PretextureStore store;
    const auto status = store.open(storePath.c_str());
    std::printf("store open: %s, %u records, index %zu bytes in RAM\n", PretextureStore::statusText(status), store.entryCount(), store.indexBytes());
    if (status != PretextureStore::Status::ready) return 4;

    std::uint64_t occurrences = 0, keyViewEqual = 0, unique = 0, served = 0, exactFull = 0, exactLean = 0, leanUnwritten = 0,
                  exactView = 0, halfChecked = 0, halfExact = 0, nonCompactDiffers = 0, nonCompactRefused = 0,
                  referenceRefused = 0, referenceChecked = 0, etcChecked = 0, etcOk = 0, mutatedMisses = 0, mutatedChecked = 0,
                  crcOk = 0, boxHalfRefused = 0, boxHalfChecked = 0, canaryOk = 0, canaryCaught = 0;
    PretextureCanary canary(~0u);
    std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned>> seen;
    pretexture::Walker walker([&](const pretexture::Found& f) {
        ++occurrences;
        PretextureKey key, viewKey;
        if (!pretextureKey(f.normalized, key)) { fail("key", f.path); return; }
        if (pretextureKey(f.view, viewKey) && viewKey == key) ++keyViewEqual; else fail("view key", f.path);
        const auto id = std::make_tuple(key.imageHash, key.paletteHash, key.sourceBytes, unsigned(key.width), unsigned(key.height),
                                        unsigned(key.paletteEntries), unsigned(key.gxFormat), unsigned(key.mipCount), unsigned(key.paletteFormat));
        if (!seen.insert(id).second) return;
        ++unique;
        PretextureEntry entry;
        if (!store.find(key, PretextureVariant::lossless, entry)) { fail("lossless entry missing", f.path); return; }
        NativeTexturePlan plan;
        if (!inspectNativeTexture(f.normalized, plan, false, false, true)) { fail("compact plan", f.path); return; }
        std::size_t offset, bytes;
        if (!pretextureServes(entry, plan, offset, bytes) || offset || bytes != plan.outputBytes) { fail("serves full", f.path); return; }
        ++served;
        Bytes stored(bytes), full(plan.outputBytes), lean0(plan.outputBytes, 0x00), lean1(plan.outputBytes, 0xff);
        if (!store.read(entry, 0, stored.data(), stored.size())) { fail("read", f.path); return; }
        if (pretextureCrc32(0, stored.data(), stored.size()) == entry.crc) ++crcOk; else fail("crc", f.path);
        if (!decodeNativeTexture(f.normalized, plan, full, false) || !decodeNativeTexture(f.normalized, plan, lean0, true) ||
            !decodeNativeTexture(f.normalized, plan, lean1, true)) { fail("decode", f.path); return; }
        if (stored == full) ++exactFull; else fail("full padding bytes differ", f.path);
        // Lean decode: bytes it writes (equal in both prefills) must match.
        bool leanOk = true;
        for (std::size_t n = 0; n < stored.size(); ++n) {
            if (lean0[n] != lean1[n]) { ++leanUnwritten; continue; }
            leanOk &= lean0[n] == stored[n];
        }
        if (leanOk) ++exactLean; else fail("lean bytes differ", f.path);
        // Run-time canary: accepts the stored bytes; one flipped written byte is caught.
        if (canary.check(f.normalized, plan, stored)) ++canaryOk; else fail("canary", f.path);
        {
            PretextureCanary fresh(1);
            Bytes broken = stored;
            std::size_t at = 0;
            while (at < broken.size() && lean0[at] != lean1[at]) ++at;
            if (at < broken.size()) broken[at] ^= 0x10;
            if (!fresh.check(f.normalized, plan, broken) && fresh.failed()) ++canaryCaught; else fail("canary missed a corrupted byte", f.path);
        }
        // The archived (non-normalized) view decodes to the same bytes.
        NativeTexturePlan viewPlan;
        Bytes fromView(plan.outputBytes);
        if (inspectNativeTexture(f.view, viewPlan, false, false, true) && decodeNativeTexture(f.view, viewPlan, fromView, false) &&
            fromView == stored) ++exactView; else fail("archived view differs", f.path);
        // Authored mip 1 (scenery/allowlist half): served from the tail.
        NativeTexturePlan half;
        if (inspectNativeTexture(f.normalized, half, false, true, true)) {
            ++halfChecked;
            Bytes tail(half.outputBytes), ref(half.outputBytes);
            if (pretextureServes(entry, half, offset, bytes) && bytes == half.outputBytes &&
                store.read(entry, offset, tail.data(), tail.size()) && decodeNativeTexture(f.normalized, half, ref, false) && tail == ref) ++halfExact;
            else fail("half tail", f.path);
        }
        // Box half (sourceMip 2) has other pixels: never served.
        NativeTexturePlan box;
        if (inspectNativeTextureBoxHalf(f.normalized, box, false)) { ++boxHalfChecked; if (!pretextureServes(entry, box, offset, bytes)) ++boxHalfRefused; else fail("box half served", f.path); }
        // Compact off (SD switch): a different format must fall back.
        NativeTexturePlan wide;
        if (inspectNativeTexture(f.normalized, wide, false, false, false) && wide.format != plan.format) {
            ++nonCompactDiffers;
            if (!pretextureServes(entry, wide, offset, bytes)) ++nonCompactRefused; else fail("non-compact served", f.path);
        }
        // RGB565 RGBA8 reference mode: never served.
        NativeTexturePlan reference;
        if (plan.gxFormat == 4 && inspectNativeTexture(f.normalized, reference, true, false, false)) {
            ++referenceChecked;
            if (!pretextureServes(entry, reference, offset, bytes)) ++referenceRefused; else fail("reference served", f.path);
        }
        // One changed texel byte (or palette byte) must miss.
        Bytes mutated = f.normalized;
        mutated[plan.paletteEntries ? plan.paletteOffset : plan.dataOffset + plan.sourceBytes / 2] ^= 0x01;
        PretextureKey mutatedKey; PretextureEntry none;
        ++mutatedChecked;
        if (pretextureKey(mutated, mutatedKey) && !store.find(mutatedKey, PretextureVariant::lossless, none)) ++mutatedMisses;
        else fail("mutated texture still found", f.path);
        // ETC variant, when present: format, size and CRC consistent.
        PretextureEntry etc;
        if (store.find(key, PretextureVariant::etc, etc)) {
            ++etcChecked;
            const bool alpha = etc.format == PretextureFormat::etc1a4;
            Bytes e(etc.bytes);
            if ((alpha || etc.format == PretextureFormat::etc1) && etc.bytes == etcBytes(key, alpha) && etc.quality < 255 &&
                store.read(etc, 0, e.data(), e.size()) && pretextureCrc32(0, e.data(), e.size()) == etc.crc) ++etcOk;
            else fail("etc entry", f.path);
        }
    });
    if (!walker.walkDisc(disc)) { std::fprintf(stderr, "walk failed\n"); return 5; }
    const auto row = [](const char* name, std::uint64_t ok, std::uint64_t of) {
        std::printf("%-58s %llu / %llu%s\n", name, (unsigned long long)ok, (unsigned long long)of, ok == of ? "" : "  <-- FAIL");
        if (ok != of) ++failures;
    };
    row("key(archived view) == key(normalized copy), occurrences", keyViewEqual, occurrences);
    row("unique textures with a lossless entry that serves the plan", served, unique);
    row("stored == decodeNativeTexture(lean=false), byte-exact", exactFull, served);
    row("stored == every byte decodeNativeTexture(lean=true) writes", exactLean, served);
    row("stored == decode of the archived view", exactView, served);
    row("stored CRC-32 matches the record", crcOk, served);
    row("authored-mip-1 half plans served from the tail, exact", halfExact, halfChecked);
    row("box-half plans refused (decode at run time)", boxHalfRefused, boxHalfChecked);
    row("non-compact plans with another format refused", nonCompactRefused, nonCompactDiffers);
    row("RGB565 reference-mode plans refused", referenceRefused, referenceChecked);
    row("one-byte-changed textures miss", mutatedMisses, mutatedChecked);
    row("run-time canary accepts every stored image", canaryOk, served);
    row("run-time canary catches one corrupted byte", canaryCaught, served);
    row("ETC entries consistent (format, size, CRC)", etcOk, etcChecked);
    std::printf("lean decode leaves %llu bytes unwritten (padding never sampled); store reads %llu, %.1f MiB\n",
        (unsigned long long)leanUnwritten, (unsigned long long)store.readOperations(), store.readBytes() / 1048576.0);

    // Container refusals on edited copies of the section (scratch files).
    std::uint64_t sectionBytes = 0;
    {
        std::FILE* f = std::fopen(storePath.c_str(), "rb");
        fseeko(f, -64, SEEK_END);
        unsigned char t[64];
        if (std::fread(t, 1, 64, f) == 64) std::memcpy(&sectionBytes, t + 16, 8);
        std::fclose(f);
    }
    const std::string tmp = scratch + "/pretexture-negative.dlptex";
    struct Case { const char* name; PretextureStore::Status expected; std::function<void(Bytes&)> edit; };
    const std::vector<Case> cases = {
        {"decoder version bumped -> stale", PretextureStore::Status::stale, [](Bytes& b) {
            b[12] ^= 1; std::uint32_t z = 0; std::memcpy(b.data() + 60, &z, 4);
            const auto c = pretextureCrc32(0, b.data(), 64); std::memcpy(b.data() + 60, &c, 4); }},
        {"index byte flipped -> malformed", PretextureStore::Status::malformed, [](Bytes& b) { b[64 + 5] ^= 0x40; }},
        {"trailer byte flipped -> malformed", PretextureStore::Status::malformed, [](Bytes& b) { b[b.size() - 30] ^= 1; }},
        {"no trailer -> missing", PretextureStore::Status::missing, [](Bytes& b) { b.resize(b.size() - 64); }},
    };
    for (const auto& c : cases) {
        if (!copyFile(storePath, tmp, std::size_t(sectionBytes), c.edit)) { fail("scratch copy", c.name); continue; }
        PretextureStore s;
        const auto got = s.open(tmp.c_str());
        std::printf("%-58s %s\n", c.name, got == c.expected ? "ok" : "FAIL");
        if (got != c.expected) ++failures;
        std::remove(tmp.c_str());
    }
    std::printf(failures ? "RESULT: %u failures\n" : "RESULT: all checks passed\n", failures);
    return failures ? 1 : 0;
}
