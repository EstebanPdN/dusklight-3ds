#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dusk::native::gx {
inline constexpr std::size_t maxVertices = 4096;
struct Vertex {
    float x, y, z;
    std::uint8_t r, g, b, a;
    float u, v;
};
enum class Primitive : std::uint8_t { quads, triangles, triangleStrip, triangleFan, lines, lineStrip };
struct Viewport { float x, y, width, height, nearZ, farZ; };
struct Scissor { std::uint32_t x, y, width, height; };
// Raw numeric values below are the pinned GX enums, not PICA enums:
// Blend mode: 0 none, 1 blend, 2 logic, 3 subtract.
// Factors: 0 zero, 1 one, 2 srcColor/dstColor, 3 inverse thereof,
// 4 srcAlpha, 5 inverse srcAlpha, 6 dstAlpha, 7 inverse dstAlpha.
struct Blend { std::uint8_t mode, src, dst, logic; };
// GX compare: 0 never, 1 less, 2 equal, 3 lequal, 4 greater,
// 5 nequal, 6 gequal, 7 always. GX alpha op: 0 and, 1 or, 2 xor, 3 xnor.
struct Depth { bool enabled; std::uint8_t compare; bool write; };
struct AlphaCompare { std::uint8_t compare0, reference0, op, compare1, reference1; };
using TextureHandle = std::uint64_t;
struct Texture {
    TextureHandle handle;
    std::span<const std::uint8_t> bti, image; // Validated header-relative view and exact GX tiled texels.
    std::uint16_t width, height;
    std::uint8_t format, wrapS, wrapT, minFilter, magFilter;
    std::uint8_t mipCount{1},minLod{},maxLod{};
    float lodBias{};
    // Retained private copies never mutate their texels/palette. Global GX
    // cache invalidation cannot change these backing bytes; dynamic providers
    // keep the default false and their explicit image invalidation path.
    bool immutablePixels{};
};
struct Rgba { std::uint8_t r, g, b, a; };
struct TevOperation { std::uint8_t operation, bias, scale, clamp, output; };
struct TevStage {
    // Raw GX_CC_* / GX_CA_* arguments, in a,b,c,d order. ADD computes d+(1-c)*a+c*b.
    std::uint8_t color[4], alpha[4];
    TevOperation colorOp, alphaOp;
    std::uint8_t coordinate, texture, channel, konstColor, konstAlpha;
};
struct DrawPacket {
    std::span<const Vertex> vertices;
    Primitive primitive;
    // Row-major GX matrices, column-vector multiplication: projection * modelView * position.
    // No viewport transform, depth remapping or 3DS screen rotation is applied here.
    float projection[16], modelView[12];
    Viewport viewport;
    Scissor scissor;
    Blend blend;
    Depth depth;
    AlphaCompare alpha;
    std::uint8_t cull; // GX: 0 none, 1 front, 2 back, 3 all.
    float lineWidthPixels; // GX line-width register is in sixths of a pixel.
    bool zCompareBeforeTexture;
    bool textured;
    Texture texture;
    std::uint8_t tevCount;
    TevStage tev[2];
    Rgba registers[4], konst[4]; // GX_TEVPREV, REG0, REG1, REG2; KCOLOR0..3.
};
enum class Error : std::uint8_t {
    none, invalidParameter, unsupportedState, vertexProtocol, vertexOverflow,
    incompleteDraw, missingSink, backendRejected
};
struct Statistics {
    std::uint64_t drawsSubmitted, drawsDropped, verticesSubmitted, errors;
    Error firstError;
    const char* firstErrorCall; // Static literal, never a game/resource pointer.
};
// The packet and its vertices remain immutable only until this synchronous
// callback returns. A backend must consume/copy them immediately, never retain pointers.
using DrawSink = bool (*)(const DrawPacket&, void*);
void setDrawSink(DrawSink sink, void* user = nullptr);
// Optional render-owner reset barrier. TLS routing installs a program-lifetime
// callback; this resource registry retains no stream/resource pointer or SDK dependency.
// Called before mutation, so a throwing/terminating guard preserves prior state.
void setStateResetObserver(void (*observer)());
void resetState(); // Resets capture state and counters, preserving the installed sink.
// Single render thread, no reentrant GX calls from the sink. The caller keeps
// registered BTIs alive and unchanged until unregister. Reset preserves live
// texture registrations/objects, but clears draw bindings along with GX state.
// Diagnostics: 64 registrations/128 objects. Original game startup: bounded
// 1024 registrations/2048 objects. Descriptor storage only; GPU texels retain
// their independent cache budget. No allocation in this registry.
#ifdef DUSK_GX_LOOKUP_TEST
bool advanceTextureIdentityCountersForTest(TextureHandle nextImageId,std::uint64_t nextGeneration);
#endif
// Only image registration consumes the monotonic 23-bit command ID space.
// Exhaustion fails without publishing a registration; IDs are never recycled.
bool registerTexture(std::span<const std::uint8_t> bti, TextureHandle& handle,bool immutablePixels=false);
bool unregisterTexture(TextureHandle handle);
// EFB copies use an opaque original Aurora destination identity. The copy
// owner retains the real validated BTI; the identity is never dereferenced.
bool aliasTextureImage(TextureHandle handle,const void* identity);
// A shared immutable palette can retain multiple original header identities.
// All aliases retire with their image; the fixed table rejects overflow.
bool aliasTexturePalette(TextureHandle handle,const void* identity);
// Bounded immutable embedded palette identity, never an unbounded GX pointer.
bool paletteCommandToken(const void* palette,std::uint32_t& token);
bool texturePaletteMatches(const Texture& texture,const Texture& palette);
// TEX1 headers may share an enclosing resource span. Actual header/pixel ranges
// must be disjoint. Caller retains the immutable block until release; destroy
// original J3D texture objects before releasing, and release before archive free.
// The output owns unique images (exact aliases within this TEX1 share a handle).
// Optional external records are not registered: the caller must resolve them
// through their original named resource before drawing. Default rejects them.
// The output must be empty. Registration rolls back completely on failure.
struct TextureBlockResources {
    std::array<TextureHandle,64> handles{};
    std::uint16_t count{};
};
bool registerTextureBlock(std::span<const std::uint8_t> tex1,TextureBlockResources&,bool allowExternal=false);
bool releaseTextureBlock(TextureBlockResources&);
// Snapshot an existing GX texture-map binding; clears output on missing/stale
// state. No raw pointer dereference, allocation or mutation of GX state.
bool textureBinding(unsigned unit,Texture& output);
// Private diagnostic snapshots on the render owner, between frames only.
// Borrowed spans remain valid until the next registry mutation. Empty slots
// clear output; this does not change bindings, counters or registration order.
std::size_t textureRegistrationCapacity();
bool registeredTextureAt(std::size_t slot,Texture& output);
// Reflect initialized dimensions and wrap modes even before image registration.
// This never reads the image pointer or claims that texels are renderable.
// Unknown/destroyed objects return false and clear the snapshot.
// The original mirror's explicit native null/zero-size/RGBA8 sentinel is a
// known empty descriptor: width/height queries return zero, binding still fails.
bool queryTextureObject(const void* object,Texture& output);
// Native display lists encode a bounded registration identity, never an ARM
// address. Tokens are not reused; owner unregister invalidates every old list.
bool textureCommandToken(const void* image,std::uint32_t& token);
// Original image identity aliased to a retained private copy (null if none).
const void* textureAliasImage(TextureHandle handle);
bool textureFromCommandToken(std::uint32_t token,Texture& output);
Statistics statistics();
// Query actual immediate diagnostic state; missing/unready state returns false.
bool queryViewport(Viewport& output);
bool queryScissor(Scissor& output);
bool queryProjection(float* sevenValues);
const char* errorText(Error error);

// Adapter-facing state interface, independent of Dolphin and libctru headers.
// Enum arguments carry the pinned raw GX values; gx_api.cpp provides typed GX ABI entry points.
void setViewport(float x, float y, float width, float height, float nearZ, float farZ);
void setScissor(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
void setProjection(const float* matrix, unsigned type);
void loadPositionMatrix(const float* matrix, std::uint32_t id);
void loadTextureMatrix(const float* matrix, std::uint32_t id, unsigned type);
void setCurrentMatrix(std::uint32_t id);
void setBlend(unsigned mode, unsigned source, unsigned destination, unsigned logic);
void setDepth(unsigned enabled, unsigned compare, unsigned write);
void setAlpha(unsigned compare0, std::uint8_t reference0, unsigned op, unsigned compare1, std::uint8_t reference1);
void setCull(unsigned mode);
void setZCompareLocation(unsigned beforeTexture);
void setLineWidth(std::uint8_t width, unsigned offsets);
void setNumIndirect(std::uint8_t count);
void setTevDirect(unsigned stage);
void setTevOperation(unsigned stage, unsigned operation);
void setTevOrder(unsigned stage, unsigned coordinate, unsigned texture, unsigned channel);
void setTevInputs(unsigned stage, bool alpha, unsigned a, unsigned b, unsigned c, unsigned d);
void setTevOperator(unsigned stage, bool alpha, unsigned operation, unsigned bias, unsigned scale,
                    unsigned clamp, unsigned output);
void setTevRegister(unsigned id, Rgba color, bool konst);
void setTevKonstSelection(unsigned stage, unsigned selection, bool alpha);
void setNumTevStages(std::uint8_t count);
void setNumChannels(std::uint8_t count);
void setChannelControl(unsigned channel, unsigned enabled, unsigned ambientSource, unsigned materialSource,
                       std::uint32_t lights, unsigned diffuse, unsigned attenuation);
void setNumTexGens(std::uint8_t count);
void setTexCoordGen(unsigned destination, unsigned function, unsigned source, std::uint32_t matrix,
                    unsigned normalize, std::uint32_t postMatrix);
void clearVertexDescriptors();
void setVertexDescriptor(unsigned attribute, unsigned type);
void setVertexFormat(unsigned format, unsigned attribute, unsigned count, unsigned type, std::uint8_t fraction);
void begin(unsigned primitive, unsigned format, std::uint16_t count);
void position(float x, float y, float z);
void color(std::uint32_t rgba);
void textureCoordinate(float u, float v);
void textureCoordinateShort(std::int16_t u, std::int16_t v);
void initTextureObject(void* object, const void* image, std::uint16_t width, std::uint16_t height,
                        unsigned format, unsigned wrapS, unsigned wrapT, unsigned mipmap, bool palette,unsigned tlut=0);
void setTextureObjectLod(void* object, unsigned minFilter, unsigned magFilter, float minLod, float maxLod,
                         float lodBias, unsigned biasClamp, unsigned edgeLod, unsigned anisotropy);
void loadTextureObject(void* object, unsigned unit);
void destroyTextureObject(void* object);
void setTextureObjectTlut(void* object,unsigned tlut);
void initTlutObject(void* object,const void* palette,unsigned format,unsigned entries);
void setTlutObjectData(void* object,const void* palette);
void loadTlutObject(const void* object,unsigned tlut);
void destroyTlutObject(void* object);
void unsupportedOperation(const char* call);
void unsupportedPalette(const char* call);
void end();
}
