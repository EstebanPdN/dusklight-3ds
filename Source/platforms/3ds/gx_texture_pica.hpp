#pragma once
#include "runtime/gx_state.hpp"
#include "runtime/gx_world_mip_use.hpp"
#include "runtime/texture_gx.hpp"
namespace dusk::native {
// Configure an empty cache before original startup. Hard maximum 12 MiB/256
// images stays inside the existing16 MiB linear heap. Defaults4 MiB/64 for
// diagnostics. No live storage, binding or budget may change through this call.
// Reference mode changes GX RGB565 storage only, for same-binary A/B runs.
// It is accepted only while the cache is empty, like the budget selection.
// Optional fourth argument copies <=64 SHA-256 source identities into the
// empty cache. Empty means off. Each entry must be audited as world-only,
// immutable, normalized-UV, nonprojective content across ALL its uses/aliases;
// immutable opaque pixels alone do not prove this. Projected admitted content
// fails closed before allocation/binding. GX4 or all-mip opaque GX14 only;
// CMPR remains decoded RGBA8. No wildcard or scene/handle policy.
// Before configureGxTextureCache: false keeps RGBA8 for every converted
// format (SD switch texture-compact-off.txt; Alpha 12 default on).
void setGxTextureCompact(bool on);
bool configureGxTextureCache(std::size_t bytes,unsigned images,bool rgb565Rgba8Reference=false,
                             std::span<const NativeTextureFingerprint> halfResolutionAllowlist={},
                             bool enableFusedColorCopy=false);
// A draw protects all of its consumed images before any binding/eviction.
// Pressure may retire unprotected least-recently-used GPU conversions only
// after actual queue completion. Their live CPU resource/token stays intact.
// The second (ping-pong) storage of a dynamic image is released first, with
// no wait once no queued draw can read it.
class GxTextureDrawScope {
    bool active{};
public:
    explicit GxTextureDrawScope(std::span<const gx::Texture> textures);
    ~GxTextureDrawScope();
    explicit operator bool()const{return active;}
    GxTextureDrawScope(const GxTextureDrawScope&)=delete;
    GxTextureDrawScope& operator=(const GxTextureDrawScope&)=delete;
};
// An ordered GX invalidation forces reconversion at the next binding.
// Units 0..2 retain separate sampler metadata while sharing cached pixel data.
// The caller supplies the matching shader UV output and handles inactive units.
// GPU-resident EFB capture: the cached RGBA8 texture of a copy image (null when
// not resident/eligible) and the commit that marks it current without a decode.
void* gpuCaptureTexture(gx::TextureHandle,unsigned& width,unsigned& height); // C3D_Tex*
// The same texture, moved once into VRAM so the PICA can render into it.
void* gpuCaptureVramTexture(gx::TextureHandle,unsigned& width,unsigned& height); // C3D_Tex*
void gpuCaptureCommit(gx::TextureHandle);
// Diagnostic: copy the resident texture bytes (after the GPU finished).
bool gpuCaptureRead(gx::TextureHandle,std::span<std::uint8_t> out,unsigned paddedWidth,unsigned paddedHeight);
// Forget cached unit bindings (frame begin, or any foreign C3D_TexBind).
void invalidateGxTextureBindings();
// Profiled report: recent decodes, then two lines since the previous report,
// re-decode GPU waits and re-decode paths/spares (each under the 255-character
// console OSReport limit).
void gxTextureDecodeReport();
// Changed re-decodes since startup, by how the CPU write was kept away from
// pixels that queued draws may still read: a completion since the last
// invalidation (unfenced), storage last bound in an earlier frame (idle), the
// image's second storage (spare, ping-pong), or a full GPU wait (fenced).
// Unchanged sources are skipped with no decode. Spares count in gxTextureBytes.
// alpha6Waits counts the waits the Alpha 6 rule took (the first changed
// re-decode after each invalidation). spareHeldWaits counts eviction waits
// taken while spares in service held the missing memory, which Alpha 6 might
// not have taken. fenceTicks is the time of the fenced and spare-held waits.
struct GxTextureRedecodeStatistics {
    std::uint64_t unfenced{},idle{},spare{},fenced{},unchanged{};
    std::uint64_t alpha6Waits{},spareHeldWaits{},fenceTicks{};
    std::uint64_t spareAllocations{},spareReleases{},spareRefusals{};
    unsigned spares{};std::size_t spareBytes{};
};
GxTextureRedecodeStatistics gxTextureRedecodeStatistics();
void gxTextureBindStatistics(std::uint64_t& skips,std::uint64_t& applies);
// mipUse (worldMips, runtime/perf_features.hpp): a unit of a world-model draw
// may sample generated mip levels (runtime/gx_world_mips.hpp says which).
bool bindGxTexture(const gx::Texture& texture, float& scaleU, float& scaleV,unsigned unit=0,bool projective=false,
                   GxWorldMipUse mipUse={});
// worldMips memory and decisions since start (tour report, measured dump).
struct GxWorldMipStatistics {
    unsigned images{};        // resident images with generated levels
    std::size_t bytes{};      // their generated levels' bytes (linear memory)
    std::uint64_t made{};     // images given levels
    std::uint64_t worldBinds{},sampledBinds{}; // world-use binds; of them sampling generated levels
    // Binds of images that carry generated levels at their base level while
    // the improvement is on: other uses of a world image (rare), or a world
    // draw path that does not pass its use (Alpha 21: the worker's record
    // replays). Compare with sampledBinds.
    std::uint64_t levelBaseBinds{};
    std::uint64_t refusedShape{},refusedAlpha{},refusedCap{},refusedLinear{},refusedAllocation{};
    std::uint64_t residentWithout{}; // world-use binds of an image already stored without levels
};
GxWorldMipStatistics gxWorldMipStatistics();
// The checksum the cache keeps of an image the game can rewrite (header,
// palette and texels), taken now. False when the image cannot be inspected.
bool gxTextureSourceSum(const gx::Texture& texture,std::uint64_t& sum);
// Unit 0 after bindGxTexture: PICA projection mode (per-pixel divide by the
// third coordinate) or plain 2D. False when no texture is bound to unit 0.
bool setGxTextureProjection(bool projective);
const char* gxTextureFailure();
std::size_t gxTextureBytes(); // Live PICA allocations in the bounded cache.
// Cached images released because the linear heap refused a new image.
std::uint64_t gxTextureLinearEvictions();
// Render owner: release the cached image the current frame needs least (one
// GPU fence first, tracked by the caller's flag). Spare storage goes first: an
// idle one with no wait, one still in service after that fence. False when
// nothing can go.
bool releaseLeastNeededTexture(bool& fenced);
std::size_t gxTextureIdleBytes(); // At most 256 bytes for retired, unused unit bindings.
struct GxTextureHalfStatus {
    std::size_t policyEntries{}, residentImages{}, residentBytes{}, sameImagesFullBytes{};
};
// Render owner only. A bounded metadata scan, with no pixel reads/allocation.
// Full bytes are a counterfactual for exactly these resident images, not a
// measured whole-cache/whole-RAM saving (eviction can change the resident set).
GxTextureHalfStatus gxTextureHalfStatus();
// Render owner only. No pixels are read or changed here; the next binding
// validates its current borrowed view, decodes into GPU storage no queued draw
// reads (or waits for the GPU) and flushes it before that image is sampled.
// Also releases spare storage unused for a while once it is idle.
void invalidateGxTextures();
bool invalidateGxTexture(gx::TextureHandle); // Render owner, after completion.
bool releaseGxTexture(gx::TextureHandle); // Render owner, after completion.
void destroyGxTextures(); // Only after C3D_Fini has drained GPU work.
}

namespace dusk::native {
struct ColorCopyPlan;
// Experimental resident refresh. Only configureGxTextureCache may set the flag;
// it is OFF by default and reset at teardown. No startup I/O/live option parser.
extern bool fusedColorCopyEnabled;
enum class FusedColorCopyResult { skipped, published, failed };
// Internal synchronous GXCopyTex color/nonindexed branch only, after completed
// readback. No pointer/lease escapes. Skipped guarantees neither output changed.
FusedColorCopyResult refreshResidentColorCopy(gx::TextureHandle,const ColorCopyPlan&,
    std::span<const std::byte> completedReadback,std::span<std::uint8_t> ownedBti);
}
