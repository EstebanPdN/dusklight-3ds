#include "trace.hpp"
#include "pipeline.hpp"
#include "shader_info.hpp"
#include <aurora/gx_trace.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace aurora::gx::trace {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t MaxMaterials = 128, MaxTextures = 512, MaxCopies = 32, QueueSize = 8;
constexpr uint64_t MaxFrames = 1800;
struct Material {
    PipelineConfig program{};
    uint64_t key = 0, draws = 0;
};
struct Texture {
    // Pointer is an in-process identity only. Never serialized or dereferenced.
    const void* identity = nullptr;
    u32 version = 0, width = 0, height = 0, format = 0, mips = 0;
};
struct Copy {
    s32 left = 0, top = 0, width = 0, height = 0;
    u32 dstWidth = 0, dstHeight = 0, format = 0;
};
struct Frame {
    uint64_t number = 0, draws = 0, vertices = 0, triangles = 0, indexedDraws = 0;
    uint64_t indirectDraws = 0, litDraws = 0, efbCopies = 0, targetSwitches = 0;
    uint64_t materialOverflow = 0, textureOverflow = 0, copyOverflow = 0;
    std::array<uint64_t, 17> tevHistogram{};
    std::array<uint64_t, 9> texgenHistogram{};
    std::array<Material, MaxMaterials> materials{};
    std::array<Texture, MaxTextures> textures{};
    std::array<Copy, MaxCopies> copies{};
    size_t materialCount = 0, textureCount = 0, copyCount = 0;
    char scene[9] = {};
    int room = -1;
    double frameIntervalMs = -1, updateMs = -1, drawMs = -1;
};
Frame current;
std::array<Frame, QueueSize> queue;
size_t head = 0, tail = 0, queued = 0;
std::mutex mutex;
std::condition_variable wake;
std::thread writer;
FILE* output = nullptr;
bool stopping = false, active = false, frameActive = false;
std::atomic_bool writeFailed{false};
uint64_t frameNumber = 0, dropped = 0;
Clock::time_point lastBegin{};

uint64_t signature(const PipelineConfig& config) {
    // Versioned, pointer-free canonical Aurora shader/pipeline representation.
    // Uniform values are excluded: this is a program key, not complete GX state.
    static_assert(std::has_unique_object_representations_v<PipelineConfig>);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&config);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(config); ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
void number_or_null(double value) {
    if (value < 0) std::fputs("null", output); else std::fprintf(output, "%.6f", value);
}
void write_frame(const Frame& f) {
    std::fprintf(output, "{\"type\":\"frame\",\"frame\":%llu,\"scene\":", (unsigned long long)f.number);
    if (f.scene[0]) std::fprintf(output, "\"%s\"", f.scene); else std::fputs("null", output);
    std::fprintf(output, ",\"room\":%d,\"frame_interval_ms\":", f.room); number_or_null(f.frameIntervalMs);
    std::fputs(",\"cpu_update_ms\":", output); number_or_null(f.updateMs);
    std::fputs(",\"cpu_draw_prepare_ms\":", output); number_or_null(f.drawMs);
    std::fputs(",\"gpu_wait_ms\":null,\"audio_ms\":null,\"io_ms\":null,\"debug_ui_visible\":null", output);
    std::fprintf(output, ",\"draws\":%llu,\"vertices\":%llu,\"triangles\":%llu,\"indexed_draws\":%llu,\"indirect_draws\":%llu,\"lit_draws\":%llu,\"efb_copies\":%llu,\"target_switches\":%llu",
        (unsigned long long)f.draws, (unsigned long long)f.vertices, (unsigned long long)f.triangles,
        (unsigned long long)f.indexedDraws, (unsigned long long)f.indirectDraws, (unsigned long long)f.litDraws,
        (unsigned long long)f.efbCopies, (unsigned long long)f.targetSwitches);
    std::fputs(",\"tev_histogram\":[", output);
    for (size_t i = 0; i < f.tevHistogram.size(); ++i) std::fprintf(output, "%s%llu", i ? "," : "", (unsigned long long)f.tevHistogram[i]);
    std::fputs("],\"texgen_histogram\":[", output);
    for (size_t i = 0; i < f.texgenHistogram.size(); ++i) std::fprintf(output, "%s%llu", i ? "," : "", (unsigned long long)f.texgenHistogram[i]);
    std::fputs("],\"materials\":[", output);
    for (size_t i = 0; i < f.materialCount; ++i) {
        const auto& m = f.materials[i]; const auto& p = m.program;
        std::fprintf(output, "%s{\"program_key\":\"%016llx\",\"draws\":%llu,\"tev_stages\":%u,\"indirect_stages\":%u,\"fog\":%u,\"blend\":[%u,%u,%u,%u],\"depth\":[%u,%u,%u],\"alpha\":[%u,%u,%u,%u,%u],\"canonical_program_hex\":\"",
            i ? "," : "", (unsigned long long)m.key, (unsigned long long)m.draws,
            p.shaderConfig.tevStageCount, p.shaderConfig.numIndStages, (unsigned)p.shaderConfig.fogType,
            (unsigned)p.blendMode, (unsigned)p.blendFacSrc, (unsigned)p.blendFacDst, (unsigned)p.blendOp,
            (unsigned)p.depthFunc, p.depthCompare, p.depthUpdate,
            (unsigned)p.shaderConfig.alphaCompare.comp0, p.shaderConfig.alphaCompare.ref0,
            (unsigned)p.shaderConfig.alphaCompare.op, (unsigned)p.shaderConfig.alphaCompare.comp1, p.shaderConfig.alphaCompare.ref1);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&p);
        for (size_t j = 0; j < sizeof(p); ++j) std::fprintf(output, "%02x", bytes[j]);
        std::fputs("\"}", output);
    }
    std::fputs("],\"textures\":[", output);
    uint64_t decodedMip0 = 0;
    for (size_t i = 0; i < f.textureCount; ++i) {
        const auto& t = f.textures[i]; decodedMip0 += uint64_t(t.width) * t.height * 4;
        std::fprintf(output, "%s{\"frame_local_id\":%zu,\"width\":%u,\"height\":%u,\"format\":%u,\"mips\":%u}",
            i ? "," : "", i, t.width, t.height, t.format, t.mips);
    }
    std::fprintf(output, "],\"decoded_rgba8_mip0_estimate_bytes\":%llu,\"copies\":[", (unsigned long long)decodedMip0);
    for (size_t i = 0; i < f.copyCount; ++i) {
        const auto& c = f.copies[i];
        std::fprintf(output, "%s{\"src\":[%d,%d,%d,%d],\"dst\":[%u,%u],\"format\":%u}", i ? "," : "", c.left,c.top,c.width,c.height,c.dstWidth,c.dstHeight,c.format);
    }
    std::fprintf(output, "],\"overflow\":{\"material_draws\":%llu,\"texture_observations\":%llu,\"copies\":%llu}}\n",
        (unsigned long long)f.materialOverflow,(unsigned long long)f.textureOverflow,(unsigned long long)f.copyOverflow);
    if (std::fflush(output) != 0 || std::ferror(output)) writeFailed.store(true);
}
void worker() {
    for (;;) {
        std::unique_lock lock(mutex);
        wake.wait(lock, []{ return queued || stopping; });
        if (!queued && stopping) return;
        // Keep this queue slot owned until disk I/O finishes. Producer uses try_lock.
        const size_t slot = tail;
        lock.unlock();
        write_frame(queue[slot]);
        lock.lock(); tail = (tail + 1) % QueueSize; --queued;
    }
}
}
void initialize() {
    if (writer.joinable()) return;
    const char* path = std::getenv("DUSK_GX_TRACE_PATH");
    if (!path || !*path) return;
    output = std::fopen(path, "wx"); // Never truncate an earlier capture.
    if (!output) { std::fputs("DUSK_GX_TRACE: cannot create exclusive output\n", stderr); return; }
    std::fprintf(output, "{\"type\":\"schema\",\"schema\":1,\"origin\":\"host\",\"complete_g1\":false,\"pipeline_version\":%u,\"pipeline_bytes\":%zu,\"byte_order\":\"little\",\"max_frames\":%llu,\"key_semantics\":\"Aurora canonical program; uniforms excluded\"}\n",
        GXPipelineConfigVersion, sizeof(PipelineConfig), (unsigned long long)MaxFrames);
    static_assert(std::endian::native == std::endian::little);
    stopping = false; active = true; writeFailed = false;
    frameNumber = dropped = head = tail = queued = 0; lastBegin = {};
    writer = std::thread(worker);
}
void shutdown() {
    active = frameActive = false;
    if (!writer.joinable()) return;
    { std::lock_guard lock(mutex); stopping = true; }
    wake.notify_one(); writer.join();
    std::fprintf(output, "{\"type\":\"end\",\"frames_seen\":%llu,\"frames_dropped\":%llu,\"write_failed\":%s}\n",
        (unsigned long long)frameNumber,(unsigned long long)dropped,writeFailed ? "true" : "false");
    const bool failed = std::fflush(output) != 0 || std::ferror(output);
    if (std::fclose(output) != 0 || failed) std::fputs("DUSK_GX_TRACE: output finalization failed\n",stderr);
    output = nullptr;
}
void begin_frame() {
    frameActive = active && !writeFailed.load() && frameNumber < MaxFrames;
    if (!frameActive) return;
    current = {}; current.number = frameNumber++;
    const auto now = Clock::now();
    if (lastBegin.time_since_epoch().count()) current.frameIntervalMs = std::chrono::duration<double,std::milli>(now-lastBegin).count();
    lastBegin = now;
}
void end_frame() {
    if (!frameActive) return;
    frameActive = false;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || queued == QueueSize) { ++dropped; return; }
    queue[head] = current; head = (head + 1) % QueueSize; ++queued;
    lock.unlock(); wake.notify_one();
}
void draw(GXPrimitive primitive, GXVtxFmt format, u32 vertices, u32 indices) {
    if (!frameActive) return;
    ++current.draws; current.vertices += vertices;
    if (indices) { ++current.indexedDraws; current.triangles += indices / 3; }
    else if (primitive == GX_TRIANGLES) current.triangles += vertices / 3;
    else if (primitive == GX_QUADS) current.triangles += (vertices / 4) * 2;
    else if ((primitive == GX_TRIANGLESTRIP || primitive == GX_TRIANGLEFAN) && vertices >= 3) current.triangles += vertices - 2;
    ++current.tevHistogram[std::min<size_t>(g_gxState.numTevStages,16)];
    ++current.texgenHistogram[std::min<size_t>(g_gxState.numTexGens,8)];
    PipelineConfig config{}; populate_pipeline_config(config, primitive, format);
    const auto info = build_shader_info(config.shaderConfig);
    current.indirectDraws += info.usedIndStages.any(); current.litDraws += info.lightingEnabled;
    const uint64_t key = signature(config);
    size_t i = 0;
    for (; i < current.materialCount; ++i) {
        auto& m = current.materials[i];
        if (m.key == key && std::memcmp(&m.program,&config,sizeof(config)) == 0) { ++m.draws; break; }
    }
    if (i == current.materialCount) {
        if (i < MaxMaterials) current.materials[current.materialCount++] = {config,key,1};
        else ++current.materialOverflow;
    }
    const auto sampled = info.sampledTextures | info.sampledIndTextures;
    for (size_t slot = 0; slot < MaxTextures && slot < sampled.size(); ++slot) {
        if (!sampled.test(slot)) continue;
        const auto& t = g_gxState.loadedTextures[slot];
        Texture value{t.data,t.texDataVersion,t.width(),t.height(),t.format(),t.mip_count()};
        bool found = false;
        for (size_t j = 0; j < current.textureCount; ++j) {
            const auto& old = current.textures[j];
            if (old.identity==value.identity && old.version==value.version && old.width==value.width && old.height==value.height && old.format==value.format && old.mips==value.mips) { found=true; break; }
        }
        if (!found) {
            if (current.textureCount < MaxTextures) current.textures[current.textureCount++] = value;
            else ++current.textureOverflow;
        }
    }
}
void efb_copy() {
    if (!frameActive) return;
    ++current.efbCopies;
    if (current.copyCount == MaxCopies) { ++current.copyOverflow; return; }
    const auto& s = g_gxState;
    current.copies[current.copyCount++] = {s.texCopySrc.x,s.texCopySrc.y,s.texCopySrc.width,s.texCopySrc.height,s.texCopyDstWidth,s.texCopyDstHeight,(u32)s.texCopyFmt};
}
void target_switch() { if (frameActive) ++current.targetSwitches; }
}
extern "C" void dusk_gx_trace_scene(const char* scene, int room) {
    using namespace aurora::gx::trace;
    if (!frameActive) return;
    current.room = room;
    for (size_t i = 0; scene && i < 8 && scene[i]; ++i) {
        const char c = scene[i];
        current.scene[i] = ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_') ? c : '_';
    }
}
extern "C" void dusk_gx_trace_cpu(unsigned stage, double ms) {
    using namespace aurora::gx::trace;
    if (!frameActive || ms < 0) return;
    double* value = stage == 0 ? &current.updateMs : stage == 1 ? &current.drawMs : nullptr;
    if (value) *value = std::max(*value,0.0) + ms;
}
