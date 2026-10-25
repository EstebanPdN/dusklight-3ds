#include "diagnostics.hpp"
#include "runtime/disc.hpp"
#include "runtime/dump_io.hpp"
#include "runtime/resource_archive.hpp"
#include "runtime/archive_bootstrap.hpp"
#include "dusk/os.h"
#include <dolphin/os/OSMessage.h>
#include <3ds.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace dusk::native {
namespace {
constexpr std::size_t chunkBytes = 512 * 1024;
constexpr std::uint64_t reserveBytes = dumpReserve;
constexpr char dumpRoot[] = "sdmc:/3ds/dusklight-3ds/dumps";
enum State { startup, idle, working, complete, failed };
std::atomic<State> state{startup};
std::atomic<bool> stop{false};
std::atomic<std::uint32_t> progress{}, total{};
OSThread worker{};
OSMessageQueue wake{};
OSMessage wakeStorage[1]{};
bool workerCreated;
bool workerOkay;
DiscStatus discStatus = DiscStatus::missing;
int mode;
DiagnosticSnapshot snapshot{};
char destination[160]{}, resultText[96]{};
alignas(32) std::array<unsigned char, chunkBytes> scratch;
alignas(32) std::array<std::uint8_t, 512 * 128 * 4> startupPixels;
TextureExtent startupExtent;
bool startupImageReady;
StartupJ2dResources j2dResources;
char resourceText[64] = "Waiting for ISO validation";
std::array<unsigned char, 400 * 240 * 3> topScreen;
std::array<unsigned char, 320 * 240 * 2> bottomScreen;
struct Range { std::uint32_t address, size; };
std::array<Range, 128> ranges;
unsigned rangeCount;
std::uint64_t memoryBytes, freeBytes;

void loadStartupResource(Disc& disc, ArchiveBootstrap& archive) {
    if (discStatus != DiscStatus::ready) { std::strcpy(resourceText, "Resource skipped: ISO unavailable"); return; }
    auto result = archive.load(disc, "/res/Object/LogoUs.arc", {{chunkBytes, 4096, 65536, 255}, 64 * 1024});
    StartupJ2dResources found;
    if (result == ArchiveStatus::ready) result = archive.findBasename("nintendo_376x104.bti", found.nintendo);
    if (result == ArchiveStatus::ready) result = archive.findBasename("warning.bti", found.warning);
    if (result == ArchiveStatus::ready) result = archive.findBasename("warning_pstart.bti", found.prompt);
    if (result != ArchiveStatus::ready) {
        std::snprintf(resourceText, sizeof(resourceText), "Original JKR: %s", archiveStatusText(result)); return;
    }
    const std::span<const std::uint8_t> bti(found.nintendo.data, found.nintendo.size);
    if (!inspectBtiIa8(bti, startupExtent) || !decodeBtiIa8(bti, startupExtent, startupPixels)) {
        std::strcpy(resourceText, "LogoUs: IA8 conversion refused"); return;
    }
    startupImageReady = true;
    j2dResources = found;
    std::strcpy(resourceText, "LogoUs: original JKR resources ready");
    // The worker retains the original archive until every J2D/GPU consumer has
    // been destroyed. No source bytes are extracted to SD.
}

bool directory(const char* path) {
    if (!mkdir(path, 0777)) return true;
    struct stat info{};
    return errno == EEXIST && !stat(path, &info) && S_ISDIR(info.st_mode);
}
bool space() {
    FS_ArchiveResource info{};
    if (R_FAILED(FSUSER_GetSdmcArchiveResource(&info))) return false;
    freeBytes = std::uint64_t(info.clusterSize) * info.freeClusters;
    return true;
}
bool enumerate() {
    rangeCount = 0; memoryBytes = 0;
    for (std::uint32_t address = 0x00100000; address < 0x40000000;) {
        MemInfo info{}; PageInfo page{};
        if (R_FAILED(svcQueryMemory(&info, &page, address))) return false;
        const std::uint64_t end = std::uint64_t(info.base_addr) + info.size;
        if (end <= address || end > 0x100000000ULL) return false;
        // Private application mappings only. Exclude MMIO, service/shared mappings,
        // aliases, VRAM and kernel ranges. No arbitrary pointer walking.
        if ((info.perm & MEMPERM_READ) &&
            (info.state == MEMSTATE_CODE || info.state == MEMSTATE_PRIVATE ||
             info.state == MEMSTATE_STATIC || info.state == MEMSTATE_CONTINUOUS)) {
            const auto finish = std::min<std::uint64_t>(end, 0x40000000);
            if (rangeCount == ranges.size()) return false;
            const auto size = static_cast<std::uint32_t>(finish - address);
            ranges[rangeCount++] = {address, size};
            memoryBytes += size;
            if (memoryBytes > 124ULL * 1024 * 1024) return false;
        }
        if (end >= 0x40000000) break;
        address = static_cast<std::uint32_t>(end);
    }
    return rangeCount != 0;
}
// Every payload is flushed, closed and read back before its inventory entry is admitted.
bool writeFile(const char* name, const void* data, std::size_t bytes, FILE* inventory) {
    char path[208]; std::snprintf(path, sizeof(path), "%s/%s", destination, name);
    std::uint32_t checksum;
    if (!writeVerifiedDumpFile(path, data, bytes, scratch.data(), scratch.size(), checksum)) return false;
    if (inventory && std::fprintf(inventory, "%s %zu fnv1a32=%08lx\n", name, bytes,
                                  static_cast<unsigned long>(checksum)) < 0) return false;
    return true;
}
bool dump() {
    // Preflight BEFORE creating any output, particularly memory payloads.
    if (mode == 2 && !enumerate()) { std::strcpy(resultText, "Unsafe/oversized memory map refused"); return false; }
    if (mode == 1) { rangeCount = 0; memoryBytes = 0; }
    if (!space() || !hasDumpSpace(freeBytes, memoryBytes, 4 * 1024 * 1024)) {
        std::strcpy(resultText, "Insufficient SD space (8 MiB reserve)"); return false;
    }
    if (!directory("sdmc:/3ds") || !directory("sdmc:/3ds/dusklight-3ds") || !directory(dumpRoot)) return false;
    char stamp[32]; const auto now = std::time(nullptr); std::tm date{};
    gmtime_r(&now, &date); std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &date);
    bool created = false;
    for (unsigned i = 0; i < 1000; ++i) {
        std::snprintf(destination, sizeof(destination), "%s/%03u-%s-%s", dumpRoot, i,
                      mode == 1 ? "quick" : "full", stamp);
        if (!mkdir(destination, 0777)) { created = true; break; }
        if (errno != EEXIST) return false;
    }
    if (!created) return false;
    char manifestPath[208]; std::snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.partial", destination);
    FILE* manifest = std::fopen(manifestPath, "wb");
    if (!manifest) return false;
    std::fprintf(manifest,
        "format=1\nPRIVATE - NEVER UPLOAD OR COMMIT\nmode=%s\nchord=L+R+%s\ntimestamp_utc=%s\n"
        "source=422d7bb1-local-dirty\nprofile=native-bootstrap-O2\ncompiler=" __VERSION__ "\n"
        "new3ds=%u\nclock_l2=requested-not-measured\nfirmware_cfw=n/a\napp_region_bytes=%lu\n"
        "scene=n/a\nroom=n/a\nplayer_camera=n/a\ngame_ticks=n/a\nframe=%llu\n"
        "frame_ms=%.3f\np50=%.3f\np95=%.3f\np99=%.3f\nui_visible=%u\n"
        "gx=bounded-J2D-PICA-subset\naudio=not-implemented\nio=FST-Yaz0-original-JKR-memory-archive\n"
        "app_heap=n/a\njkr_budget=%lu\njkr_used=%lu\njkr_sampled_peak=%lu\njkr_free=%lu\n"
        "linear_free=%lu\nlinear_used_peak=n/a\nsd_free=%llu\nsd_reserve=%llu\n"
        "memory_bytes=%llu\nmemory_consistency=application-paused-not-atomic-system-snapshot\n"
        "screens=raw-rotated-top-back-buffer-BGR8-bottom-RGB565\n",
        mode == 1 ? "quick" : "full", mode == 1 ? "A" : "B", stamp,
        snapshot.new3ds, static_cast<unsigned long>(snapshot.appRegionBytes),
        static_cast<unsigned long long>(snapshot.frame), snapshot.frameMs, snapshot.p50, snapshot.p95, snapshot.p99,
        snapshot.visible, static_cast<unsigned long>(snapshot.gameHeap.budget),
        static_cast<unsigned long>(snapshot.gameHeap.used), static_cast<unsigned long>(snapshot.gameHeap.peak),
        static_cast<unsigned long>(snapshot.gameHeap.free), static_cast<unsigned long>(snapshot.linearFree),
        static_cast<unsigned long long>(freeBytes), static_cast<unsigned long long>(reserveBytes),
        static_cast<unsigned long long>(memoryBytes));
    const char build[] = "Dusklight original J2D resource diagnostic\n422d7bb1-local-dirty\nC++20 devkitARM; libctru 2.7.0; Citro3D 1.7.1\nBounded J2D renderer; no game scene/J3D/actors yet\n";
    const char config[] = "profile=diagnostic\nresolution=400x240\nstereo=off\ngame_input=canonical-private-ISO\nmods=off\nnetwork=off\nsave_writes=off\n";
    char log[256]; std::snprintf(log, sizeof(log), "Allowlisted startup summary only\nJKR=%s\nworker_TLS_heap=%s\ndisc=%s\nresource=%s\npreview_uploaded=%u\n",
        snapshot.kernelOkay ? "pass" : "failed", snapshot.workerOkay ? "pass" : "failed",
        discStatusText(static_cast<DiscStatus>(snapshot.discStatus)), resourceText, snapshot.previewReady);
    bool okay = writeFile("build.txt", build, sizeof(build) - 1, manifest) &&
                writeFile("config.txt", config, sizeof(config) - 1, manifest) &&
                writeFile("startup.txt", log, std::strlen(log), manifest) &&
                writeFile("top-240x400-bgr8.raw", topScreen.data(), topScreen.size(), manifest) &&
                writeFile("bottom-240x320-rgb565.raw", bottomScreen.data(), bottomScreen.size(), manifest);
    unsigned chunk = 0; std::uint64_t completed = 0;
    total.store(static_cast<std::uint32_t>(memoryBytes / 1024));
    for (unsigned i = 0; okay && i < rangeCount; ++i) {
        auto range = ranges[i];
        for (std::uint32_t offset = 0; okay && offset < range.size;) {
            const auto bytes = std::min<std::size_t>(chunkBytes, range.size - offset);
            MemInfo current{}; PageInfo page{};
            const auto address = range.address + offset;
            if (R_FAILED(svcQueryMemory(&current, &page, address)) || !(current.perm & MEMPERM_READ) ||
                std::uint64_t(address) + bytes > std::uint64_t(current.base_addr) + current.size ||
                !space() || !hasDumpSpace(freeBytes, bytes, 1024 * 1024) || stop.load()) { okay = false; break; }
            // memmove is intentional: the dump buffer itself may be in this private range.
            std::memmove(scratch.data(), reinterpret_cast<const void*>(address), bytes);
            char name[40]; std::snprintf(name, sizeof(name), "memory-%04u.bin", chunk++);
            std::fprintf(manifest, "range %s address=%08lx size=%zu\n", name, static_cast<unsigned long>(address), bytes);
            okay = writeFile(name, scratch.data(), bytes, manifest);
            offset += bytes; completed += bytes; progress.store(completed / 1024);
        }
    }
    if (okay) std::fputs("payloads_verified=yes\n", manifest);
    if (!flushDumpFile(manifest)) okay = false;
    if (!okay) return false; // manifest.partial deliberately cannot be mistaken for completion.
    // Checksum the manifest separately; no circular self-checksum claim.
    FILE* input = std::fopen(manifestPath, "rb"); if (!input) return false;
    std::uint32_t digest = 2166136261u;
    for (;;) {
        const auto count = std::fread(scratch.data(), 1, scratch.size(), input);
        digest = dumpHash(scratch.data(), count, digest);
        if (count < scratch.size()) break;
    }
    okay = !std::ferror(input); if (std::fclose(input)) okay = false;
    char checksum[64]; std::snprintf(checksum, sizeof(checksum), "manifest.txt fnv1a32=%08lx\n", static_cast<unsigned long>(digest));
    if (!okay || !writeFile("manifest.checksum", checksum, std::strlen(checksum), nullptr)) return false;
    char finalPath[208]; std::snprintf(finalPath, sizeof(finalPath), "%s/manifest.txt", destination);
    return !std::rename(manifestPath, finalPath);
}
void* run(void*) {
    if (OSGetCurrentThread() != &worker) fatal("Native loader thread record was not bound");
    OSSetCurrentThreadName("Dusklight loading worker");
    workerOkay = kernelWorkerCheck();
    const bool attached = workerOkay && attachKernelWorker();
    workerOkay = workerOkay && attached;
    ArchiveBootstrap archive;
    { Disc image; discStatus = image.open(gameImagePath);
      if (attached) loadStartupResource(image, archive);
      else std::strcpy(resourceText, "Loading-worker heap unavailable"); }
    state.store(idle, std::memory_order_release);
    while (!stop.load()) {
        if (!OSReceiveMessage(&wake, nullptr, OS_MESSAGE_BLOCK)) fatal("Native loader wake queue closed");
        if (stop.load()) break;
        bool okay = dump();
        if (!okay && !resultText[0]) std::strcpy(resultText, "SD write/readback failed; partial kept");
        state.store(okay ? complete : failed, std::memory_order_release);
    }
    if (archive.close() != ArchiveStatus::ready) fatal("Loading-worker archive close failed");
    if (attached) detachKernelWorker();
    return &worker;
}
}
bool startDiagnosticWorker() {
    OSInitMessageQueue(&wake, wakeStorage, 1);
    // Original priority 20 maps to libctru 0x31. Its actual worker/stack/TLS
    // now uses the same OS lifecycle as the original JKR/JAS game workers.
    workerCreated = OSCreateThread(&worker, run, nullptr, nullptr, 64 * 1024, 20, 0);
    if (!workerCreated) {
        std::strcpy(resultText, "Native loading-worker allocation failed");
        state.store(failed, std::memory_order_release);
        return false;
    }
    if (OSResumeThread(&worker) != 1) fatal("Native loader did not start suspended");
    return true;
}
void stopDiagnosticWorker() {
    if (!workerCreated) return;
    stop.store(true);
    if (!OSSendMessage(&wake, nullptr, OS_MESSAGE_BLOCK)) fatal("Native loader stop wake failed");
    void* result{};
    if (!OSJoinThread(&worker, &result) || result != &worker) fatal("Native loader join failed");
    workerCreated = false;
    logText("Native loading worker: actual OS join and stack/TLS release complete\n");
}
bool startupFinished() { return state.load(std::memory_order_acquire) != startup; }
bool startupWorkerOkay() { return startupFinished() && workerOkay; }
int startupDiscStatus() { return startupFinished() ? static_cast<int>(discStatus) : -1; }
const char* startupResourceStatus() { return startupFinished() ? resourceText : "Startup resource probe pending"; }
bool startupPreviewImage(PreviewImage& output) {
    output = {};
    if (!startupFinished() || !startupImageReady) return false;
    output = {startupPixels.data(), startupExtent};
    return true;
}
bool startupJ2dResources(StartupJ2dResources& output) {
    output = {};
    if (!startupFinished() || !j2dResources.nintendo.data) return false;
    output = j2dResources;
    return true;
}
bool dumpBusy() { return state.load(std::memory_order_acquire) == working; }
bool requestDump(int requested, const DiagnosticSnapshot& capture) {
    if (!startupFinished() || dumpBusy() || (requested != 1 && requested != 2)) return false;
    // Main thread has already waited for GPU completion. Snapshot screens before progress UI.
    if (gfxGetScreenFormat(GFX_TOP) != GSP_BGR8_OES || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return false;
    auto* top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    GSPGPU_InvalidateDataCache(top, topScreen.size());
    std::memcpy(topScreen.data(), top, topScreen.size());
    std::memcpy(bottomScreen.data(), gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr), bottomScreen.size());
    mode = requested; snapshot = capture; destination[0] = resultText[0] = 0;
    progress.store(0); total.store(0); state.store(working, std::memory_order_release);
    if (!OSSendMessage(&wake, nullptr, OS_MESSAGE_NOBLOCK)) fatal("Native loader request wake failed");
    return true;
}
const char* dumpStateText() {
    switch (state.load(std::memory_order_acquire)) {
    case startup: return "Startup validation on I/O worker";
    case idle: return "Idle";
    case working: return "PRIVATE capture in progress";
    case complete: return "Complete; checksums verified";
    case failed: return resultText;
    }
    return "n/a";
}
std::uint32_t dumpProgressKiB() { return progress.load(); }
std::uint32_t dumpTotalKiB() { return total.load(); }
const char* completedDumpPath() { return state.load(std::memory_order_acquire) == complete ? destination : ""; }
}
