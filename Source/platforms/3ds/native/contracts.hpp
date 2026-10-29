#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

// Native asset/draw ABI. No command stream, console register bank or source
// platform enums cross this boundary. Matrices and UVs use PICA conventions.
namespace dusk::pica {
constexpr unsigned paletteSize=8, resourceSlots=64;
using Row=std::array<float,4>;
using Matrix=std::array<Row,4>;
struct Vertex {
    std::array<float,3> position{},normal{};
    std::array<std::array<float,2>,3> uv{};
    std::array<std::uint8_t,4> color{255,255,255,255};
    // Byte row offsets 0,3,...21, prepared by the asset converter.
    std::array<std::uint8_t,4> boneRows{};
    std::array<std::uint8_t,4> weights{255,0,0,0};
};
static_assert(sizeof(Vertex)==60 && offsetof(Vertex,color)==48 && offsetof(Vertex,boneRows)==52);
static_assert(sizeof(float)==4 && offsetof(Vertex,uv)==24 && offsetof(Vertex,weights)==56);
static_assert(std::is_trivially_copyable_v<Vertex> && std::is_standard_layout_v<Vertex>);
struct Palette {
    std::array<Row,paletteSize*3> position{},normal{};
    unsigned count=1;
    static Palette identity();
};
struct Light { std::array<float,3> toward{},color{}; };
struct UvTransform {
    // Native affine (S,T,1) -> (S',T'). UV channel selection is native data,
    // not a source-platform texgen enum or runtime material conversion.
    std::array<std::array<float,3>,2> rows{{{1,0,0},{0,1,0}}};
    unsigned source=0;
    static constexpr UvTransform identity(unsigned channel){UvTransform t;t.source=channel;return t;}
    bool passthrough(unsigned channel)const{return source==channel&&rows==identity(channel).rows;}
};
enum class PositionSource:std::uint8_t { object,posed };
struct ProjectiveTransform {
    // Complete native (X,Y,Z,1)->(S,T,Q) matrix for texture unit0. PICA performs
    // projection; the CPU does not transform vertices or divide coordinates.
    // Caller owns the Q-domain/content contract. Signed/near-zero Q behavior
    // and physical sampler precision remain unverified until GPU validation.
    std::array<Row,3> rows{{{1,0,0,0},{0,1,0,0},{0,0,0,1}}};
    PositionSource source=PositionSource::posed;
    bool enabled=false;
};
struct MaterialParameters {
    // Native RGBA8 parameter vectors, low byte red. Scene/animation producers
    // supply values directly; there are no source-platform register semantics.
    std::array<std::uint32_t,8> colors{};
    std::uint8_t suppliedMask=0;
};
enum class Wrap:std::uint8_t { clamp,repeat,mirror };
enum class Filter:std::uint8_t { nearest,linear };
struct Sampler {
    Wrap wrapS=Wrap::clamp,wrapT=Wrap::clamp;
    Filter magnify=Filter::linear,minify=Filter::linear,mipmap=Filter::nearest;
    std::uint8_t firstLevel=0,lastLevel=0;
    // Native signed8.8, exact integer transport; PICA accepts -4096..4095.
    std::int16_t lodBias=0;
};
struct DrawUniforms {
    Palette palette=Palette::identity();
    Matrix viewProjection{}; // Row-major; native clip depth [-W,0].
    std::array<float,3> ambient{1,1,1};
    std::array<Light,2> lights{}; // Normalized directions, same space as normals.
    bool lit=false;
    std::array<UvTransform,3> uvTransforms{UvTransform::identity(0),UvTransform::identity(1),UvTransform::identity(2)};
    ProjectiveTransform projective{};
    MaterialParameters material{};
    std::array<Sampler,3> samplers{};
    std::uint8_t samplerMask=0; // Explicit overrides for consumed texture units.
};
enum class Source:std::uint8_t { primary,texture0,texture1,texture2,constant,previous,buffer };
enum class Combine:std::uint8_t { replace,modulate,add,addSigned,interpolate,subtract,dot3,multiplyAdd,addMultiply };
enum class ColorOperand:std::uint8_t { color,oneMinusColor,alpha,oneMinusAlpha,red,oneMinusRed,green,oneMinusGreen,blue,oneMinusBlue };
enum class AlphaOperand:std::uint8_t { alpha,oneMinusAlpha,red,oneMinusRed,green,oneMinusGreen,blue,oneMinusBlue };
struct Stage {
    std::array<Source,3> rgb{Source::previous,Source::primary,Source::primary},alpha=rgb;
    std::array<ColorOperand,3> rgbOperand{};
    std::array<AlphaOperand,3> alphaOperand{};
    Combine rgbFunction=Combine::replace,alphaFunction=Combine::replace;
    std::uint32_t constant=0xffffffff; // Native packed RGBA8, low byte red.
    // Per RGBA destination component: 0 retains the authored literal byte;
    // 1..32 selects component (selector-1)%4 of parameter (selector-1)/4.
    // Fixed offline wiring, not a runtime material expression interpreter.
    std::array<std::uint8_t,4> constantBindings{};
    std::uint8_t rgbScale=0,alphaScale=0; // Native 1x,2x,4x.
};
enum class Compare:std::uint8_t { never,always,equal,notEqual,less,lessEqual,greater,greaterEqual };
enum class Blend:std::uint8_t { opaque,alpha,additive,custom };
enum class BlendEquation:std::uint8_t { add,subtract,reverseSubtract,minimum,maximum };
enum class BlendFactor:std::uint8_t { zero,one,sourceColor,oneMinusSourceColor,destinationColor,oneMinusDestinationColor,sourceAlpha,oneMinusSourceAlpha,destinationAlpha,oneMinusDestinationAlpha };
struct BlendState {
    BlendEquation color=BlendEquation::add,alpha=BlendEquation::add;
    BlendFactor colorSource=BlendFactor::one,colorDestination=BlendFactor::zero;
    BlendFactor alphaSource=BlendFactor::one,alphaDestination=BlendFactor::zero;
};
enum class Cull:std::uint8_t { none,front,back };
struct Material {
    std::array<Stage,6> stages{};
    unsigned stageCount=1;
    unsigned textureMask=0;
    Blend blend=Blend::opaque;
    BlendState customBlend{};
    Cull cull=Cull::back;
    Compare depth=Compare::greaterEqual,alpha=Compare::always;
    bool depthTest=true,depthWrite=true,colorWrite=true,alphaWrite=true;
    std::uint8_t alphaReference=0,bufferRgbMask=0,bufferAlphaMask=0;
    std::uint32_t bufferColor=0;
    // Already authored native depth LUT. No runtime source-fog conversion.
    bool fog=false,fogDepthFlip=false;
    std::uint32_t fogColor=0;
    std::array<float,256> fogValues{};
    static Material vertexColor();
    static Material textured(unsigned unit=0);
};
enum class TextureFormat:std::uint8_t { rgba8,rgb565,etc1,etc1a4 };
struct TextureInfo {
    std::uint16_t width{},height{};
    TextureFormat format{};
    Wrap wrapS=Wrap::clamp,wrapT=Wrap::clamp;
    bool linear=true;
    unsigned mipLevels=1; // Includes level0; every level >=8x8.
};
enum class Error { none,wrongThread,busy,protocol,invalidAsset,invalidHandle,budget,allocation,publication,backend };
struct GeometryInfo { unsigned highestBone{},vertices{},indices{};std::size_t bytes{};bool rigidFirst{}; };
bool inspectGeometry(std::span<const Vertex>,std::span<const std::uint16_t>,GeometryInfo&);
bool inspectPackedGeometry(std::span<const std::byte> vertices,std::span<const std::byte> indices,GeometryInfo&);
bool validateUniforms(const DrawUniforms&,unsigned requiredBone);
bool validateSampler(const Sampler&,unsigned residentMipLevels);
bool validateMaterial(const Material&);
std::size_t textureBytes(const TextureInfo&);
constexpr std::size_t align16(std::size_t n){return (n+15)&~std::size_t(15);}
template<class Tag> struct Handle {std::uint32_t value{};explicit operator bool()const{return value!=0;} };
using MeshHandle=Handle<struct MeshTag>;
using TextureHandle=Handle<struct TextureTag>;
using MaterialHandle=Handle<struct MaterialTag>;
}
