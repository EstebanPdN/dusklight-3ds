#include "gpu_list_capture.hpp"
#include "gpu_canary.hpp"
#include "bootstrap.hpp"
#include "clock.hpp"
#include <3ds.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

extern "C" void OSReport(const char*,...);
extern "C" unsigned duskGxJobRingCopy(u32*,unsigned,u32*);

namespace dusk::native {
namespace {
constexpr unsigned maxJobs = 512, jobWords = 14, maxCommandWords = 128 * 1024;
constexpr unsigned listStart = maxJobs * jobWords;

// Fixed prefix of DLPC001. Only complete register and uniform snapshots may be published.
struct FileHeader {
    char magic[8] = "DLPC001";
    u32 frameLo, frameHi, tickLo, tickHi;
    std::int32_t room;
    u32 firstJob, jobCount, listCount = 0, registerCount, uniformCount;
    char stage[16];
    std::array<u32, 0x300> registers;
    std::array<u32, 96 * 4> uniforms;
};
static_assert(offsetof(FileHeader, stage) == 48 && offsetof(FileHeader, registers) == 64);
static_assert(sizeof(FileHeader) == 64 + (0x300 + 96 * 4) * 4);

struct ListCapture {
    FileHeader header;
    char path[160];
    std::uint64_t frame;
    u64 armTicks{}, lockTicks{}, copyTicks{}, readyTick{}, retireTicks{}, pressureTicks{};
    unsigned words = 0;
    bool failed = false, ended = false;
    // List entries are born in file order. At retirement, copy just this picture's
    // job records into the space immediately before them. No command is moved again.
    std::array<u32, listStart + 2 * maxJobs + maxCommandWords> body;
    unsigned listEnd() const { return listStart + 2 * header.listCount + words; }
    unsigned jobStart() const { return listStart - jobWords * header.jobCount; }
};
// The game's global new uses the current JKR scene heap. A write can outlive
// that scene, so both owners use the application C heap.
using OwnedCapture = std::unique_ptr<ListCapture, decltype(&std::free)>;
static_assert(std::is_trivially_destructible_v<ListCapture>);
OwnedCapture recording{nullptr, &std::free};
std::atomic<ListCapture*> pendingWrite{}; // Non-null until the sole consumer closes, reports and frees it.
char pendingPath[160]{}, pendingStage[16]{};
int pendingRoom{};
std::atomic<bool> accepting{};
LightLock listLock=1;

void appendList(ListCapture& c, const u32* list, u32 bytes) {
    if (c.failed) return;
    if (!list || !bytes || (bytes & 15) || c.header.listCount == maxJobs || bytes / 4 > maxCommandWords - c.words) {
        c.failed = true;
        return;
    }
    auto* entry = c.body.data() + c.listEnd();
    entry[0] = u32(reinterpret_cast<std::uintptr_t>(list));
    entry[1] = bytes;
    std::memcpy(entry + 2, list, bytes);
    ++c.header.listCount;
    c.words += bytes / 4;
}

bool copyCompletedJobs(ListCapture& c) {
    u32 total;
    duskGxJobRingCopy(nullptr, 0, &total);
    auto& h = c.header;
    h.jobCount = total - h.firstJob;
    if (c.failed || !c.ended || !h.listCount || !h.jobCount || h.jobCount > maxJobs) return false;
    auto* job = c.body.data() + c.jobStart();
    u32 observed;
    if (duskGxJobRingCopy(job, h.jobCount, &observed) != h.jobCount || observed != total) return false;
    if ((job[0] & 0xff) != 2 || (job[(h.jobCount - 1) * jobWords] & 0xff) != 3) return false;
    unsigned at = listStart;
    u64 previous = 0;
    for (unsigned n = 0; n < h.jobCount; ++n, job += jobWords) {
        const u64 start = u64(job[8]) | (u64(job[9]) << 32);
        const u64 end = u64(job[10]) | (u64(job[11]) << 32);
        if (!start || end < start || start < previous || job[13]) return false;
        previous = end;
        if ((job[0] & 0xff) != 1) continue;
        if (at == c.listEnd() || job[12] != GSPGPU_EVENT_P3D) return false;
        if (job[1] != c.body[at] || job[2] != c.body[at + 1]) return false;
        at += 2 + c.body[at + 1] / 4;
    }
    return at == c.listEnd();
}

bool writeListCapture(const ListCapture& c) {
    char tmp[sizeof(c.path) + 4];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", c.path);
    auto* file = std::fopen(tmp, "wb");
    if (!file) return false;
    std::setvbuf(file, nullptr, _IONBF, 0);
    const auto first = c.jobStart();
    const auto bodyBytes = (c.listEnd() - first) * sizeof(u32);
    const bool complete = std::fwrite(&c.header, 1, sizeof(c.header), file) == sizeof(c.header) &&
        std::fwrite(c.body.data() + first, 1, bodyBytes, file) == bodyBytes;
    if (std::fclose(file) == 0 && complete && std::rename(tmp, c.path) == 0) return true;
    std::remove(tmp);
    return false;
}

void reportResult(const ListCapture& c, bool written) {
    OSReport("Native GPU list capture %s frame=%llu stage=%.8s room=%d lists=%u words=%u jobs=%lu %s\n",
        c.path, (unsigned long long)c.frame, c.header.stage, int(c.header.room), unsigned(c.header.listCount), c.words,
        static_cast<unsigned long>(c.header.jobCount), written ? "written" : "omitted (incomplete or write error)");
}

void retireCapture() {
    const auto start = svcGetSystemTick();
    accepting.store(false, std::memory_order_release);
    // The GPU is idle. A submitter may still be returning from appendList.
    LightLock_Lock(&listLock);
    LightLock_Unlock(&listLock);
    if (!copyCompletedJobs(*recording)) {
        reportResult(*recording, false);
        recording.reset();
        return;
    }
    recording->readyTick = svcGetSystemTick();
    recording->retireTicks = recording->readyTick - start;
    pendingWrite.store(recording.release(), std::memory_order_release);
    if (!kickNativeSdWriterAsync()) writePendingGpuListCapture(); // No consumer exists.
}
}

bool gpuListCaptureWritePending() { return pendingWrite.load(std::memory_order_acquire) != nullptr; }

void writePendingGpuListCapture() {
    // Only the SD thread calls this while a consumer exists. Keep the slot
    // occupied throughout the write, including cleanup after a storage error.
    OwnedCapture capture{pendingWrite.load(std::memory_order_acquire), &std::free};
    if (!capture) return;
    const auto start = svcGetSystemTick();
    const bool written = writeListCapture(*capture);
    const auto end = svcGetSystemTick();
    reportResult(*capture, written);
    const auto us = [](u64 ticks){ return double(ticks) * 1e6 / nativeTickFrequency; };
    OSReport("Native GPU sidecar wall us: frame=%llu arm=%.0f lock=%.0f copy=%.0f retire=%.0f pressure=%.0f queued=%.0f file=%.0f\n",
        (unsigned long long)capture->frame, us(capture->armTicks), us(capture->lockTicks), us(capture->copyTicks),
        us(capture->retireTicks), us(capture->pressureTicks), us(start - capture->readyTick), us(end - start));
    capture.reset();
    pendingWrite.store(nullptr, std::memory_order_release);
    signalNativeCaptureProgress();
}

void waitForGpuListCaptureWrite() {
    while (gpuListCaptureWritePending()) waitForNativeCaptureProgress();
}

void requestGpuListCapture(const char* directory,const char* phase,const char* stage,int room){
    if(pendingPath[0])return; // One request for the next picture, including while the current one retires.
    if(std::snprintf(pendingPath,sizeof(pendingPath),"%s/gpu-%s.bin",directory,phase)>=int(sizeof(pendingPath))){
        pendingPath[0]=0;
        return;
    }
    std::memset(pendingStage,0,sizeof(pendingStage)); // All 16 bytes go to disk.
    std::snprintf(pendingStage,sizeof(pendingStage),"%.15s",stage);
    pendingRoom=room;
}

void recordSubmittedGpuList(const std::uint32_t* list,std::uint32_t bytes){
    if(!accepting.load(std::memory_order_acquire))return;
    const auto start = svcGetSystemTick();
    LightLock_Lock(&listLock);
    if(accepting.load(std::memory_order_relaxed)){
        const auto locked = svcGetSystemTick();
        appendList(*recording,list,bytes);
        recording->lockTicks += locked - start;
        recording->copyTicks += svcGetSystemTick() - locked;
    }
    LightLock_Unlock(&listLock);
}

bool gpuListCaptureBoundary(std::uint64_t frame) {
    const bool retired = bool(recording);
    if (retired) retireCapture();
    if (!pendingPath[0]) return retired;
    // One capture allocation total. Only a new scheduled capture can encounter
    // backpressure here, before it samples the new picture's initial state.
    u64 pressure = 0;
    if (gpuListCaptureWritePending()) {
        const auto start = svcGetSystemTick();
        waitForGpuListCaptureWrite();
        pressure = svcGetSystemTick() - start;
    }
    const auto start = svcGetSystemTick();
    if (void* storage = std::malloc(sizeof(ListCapture))) recording.reset(::new(storage) ListCapture);
    if (!recording) {
        OSReport("Native GPU list capture %s omitted (no scratch memory)\n", pendingPath);
        pendingPath[0] = 0;
        return true;
    }
    auto& c = *recording;
    std::memcpy(c.path, pendingPath, sizeof(c.path));
    pendingPath[0] = 0;
    auto& h = c.header;
    std::memcpy(h.stage, pendingStage, sizeof(h.stage));
    h.room = pendingRoom;
    c.frame = frame;
    h.frameLo = u32(frame);
    h.frameHi = u32(frame >> 32);
    duskGxJobRingCopy(nullptr, 0, &h.firstJob);
    const u32* regs{};
    const u32* uniforms{};
    std::size_t nr{}, nu{};
    gpuCanaryShadowState(regs, nr, uniforms, nu);
    c.failed = !regs || nr != h.registers.size() || !uniforms || nu != h.uniforms.size();
    if (!c.failed) {
        h.registerCount = nr;
        h.uniformCount = nu;
        std::memcpy(h.registers.data(), regs, nr * 4);
        std::memcpy(h.uniforms.data(), uniforms, nu * 4);
    }
    c.armTicks = svcGetSystemTick() - start;
    c.pressureTicks = pressure;
    accepting.store(!c.failed, std::memory_order_release);
    return true;
}

void closeGpuListCapture(std::uint64_t frame,std::uint64_t submitTick,const char* stage,int room){
    if(!recording||recording->frame!=frame)return;
    auto& h = recording->header;
    h.tickLo = u32(submitTick);
    h.tickHi = u32(submitTick >> 32);
    recording->ended = stage && std::strcmp(stage, h.stage) == 0 && room == h.room;
}

void finishGpuListCaptures() {
    if (recording) retireCapture(); // The last submitted picture has no next FrameBegin.
    if (pendingPath[0]) OSReport("Native GPU list capture %s omitted (no submitted picture)\n",pendingPath);
    pendingPath[0] = 0;
    waitForGpuListCaptureWrite();
}
}
