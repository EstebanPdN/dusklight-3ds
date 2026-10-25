#pragma once
#include "runtime/gx_state.hpp"
#include "runtime/texture_gx.hpp"
namespace dusk::native {
// Configure an empty cache before original startup. Hard maximum10 MiB/128
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
bool configureGxTextureCache(std::size_t bytes,unsigned images,bool rgb565Rgba8Reference=false,
                             std::span<const NativeTextureFingerprint> halfResolutionAllowlist={},
                             bool enableFusedColorCopy=false);
// A draw protects all of its consumed images before any binding/eviction.
// Pressure may retire unprotected least-recently-used GPU conversions only
// after actual queue completion. Their live CPU resource/token stays intact.
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
void gpuCaptureCommit(gx::TextureHandle);
// Diagnostic: copy the resident texture bytes (after the GPU finished).
bool gpuCaptureRead(gx::TextureHandle,std::span<std::uint8_t> out,unsigned paddedWidth,unsigned paddedHeight);
// Forget cached unit bindings (frame begin, or any foreign C3D_TexBind).
void invalidateGxTextureBindings();
void gxTextureDecodeReport();
void gxTextureBindStatistics(std::uint64_t& skips,std::uint64_t& applies);
bool bindGxTexture(const gx::Texture& texture, float& scaleU, float& scaleV,unsigned unit=0,bool projective=false);
const char* gxTextureFailure();
std::size_t gxTextureBytes(); // Live PICA allocations in the bounded cache.
std::size_t gxTextureIdleBytes(); // At most 256 bytes for retired, unused unit bindings.
struct GxTextureHalfStatus {
    std::size_t policyEntries{}, residentImages{}, residentBytes{}, sameImagesFullBytes{};
};
// Render owner only. A bounded metadata scan, with no pixel reads/allocation.
// Full bytes are a counterfactual for exactly these resident images, not a
// measured whole-cache/whole-RAM saving (eviction can change the resident set).
GxTextureHalfStatus gxTextureHalfStatus();
// Only the render owner after successful GPU completion. No pixels are read or
// changed here; the next binding validates its current borrowed view, decodes
// into the existing GPU image and flushes it before that image is sampled.
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
