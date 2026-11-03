#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

// Native asset/draw ABI. No command stream, console register bank or source
// platform enums cross this boundary. Matrices and UVs use PICA conventions.
namespace dusk::pica {
constexpr unsigned paletteSize=4, resourceSlots=128, maxModelParts=1024, maxFrameDraws=4096;
using Row=std::array<float,4>;
using Matrix=std::array<Row,4>;
struct Vertex {
    std::array<float,3> position{},normal{};
    std::array<std::array<float,2>,3> uv{};
    std::array<std::uint8_t,4> color{255,255,255,255};
    std::array<std::uint8_t,4> color1{255,255,255,255};
    // Byte row offsets 0,3,6,9, prepared by the asset converter.
    std::array<std::uint8_t,4> boneRows{};
    std::array<std::uint8_t,4> weights{255,0,0,0};
};
static_assert(sizeof(Vertex)==64 && offsetof(Vertex,color)==48 && offsetof(Vertex,color1)==52);
static_assert(sizeof(float)==4 && offsetof(Vertex,uv)==24 && offsetof(Vertex,boneRows)==56 && offsetof(Vertex,weights)==60);
static_assert(std::is_trivially_copyable_v<Vertex> && std::is_standard_layout_v<Vertex>);
struct Palette {
    std::array<Row,paletteSize*3> position{},normal{};
    unsigned count=1;
    static Palette identity() noexcept;
};
enum class LightKind:std::uint8_t { point,directional };
enum class Diffuse:std::uint8_t { none,signedDot,clamped };
enum class Attenuation:std::uint8_t { specular,spot,none };
struct Light {
    std::array<float,3> position{},direction{};
    std::array<float,4> color{};
    std::array<float,3> cosine{1,0,0},distance{1,0,0};
    LightKind kind=LightKind::point;
};
struct ColorChannel {
    bool enabled=false,materialVertex=true,ambientVertex=false;
    std::uint8_t lightMask=0;
    Diffuse diffuse=Diffuse::clamped;
    Attenuation attenuation=Attenuation::none;
};
enum class FogFunction:std::uint8_t { linear,exp,expSquared,reverseExp,reverseExpSquared };
struct FogUniforms {
    bool provided=false,enabled=false,rangeEnabled=false,orthographic=false;
    FogFunction function=FogFunction::linear;
    float a=0,b=1,c=0,rangeScale=0,rangeBias=0;
    std::array<float,10> range{1,1,1,1,1,1,1,1,1,1};
    std::uint32_t color=0;
};
struct UvTransform {
    // Sources0..2 use (S,T,1,1);3/4 object/posed position;5/6/7
    // object normal, posed inverse-transpose normal, posed linear normal.
    // Direction palette transforms omit translation; the final matrix does not.
    std::array<std::array<float,3>,2> rows{{{1,0,0},{0,1,0}}};
    unsigned source=0;
    std::array<float,2> translation{};
    bool normalize=false;
    static constexpr UvTransform identity(unsigned channel) noexcept {UvTransform t;t.source=channel;return t;}
    bool passthrough(unsigned channel)const{return source==channel&&!normalize&&rows==identity(channel).rows&&translation==std::array<float,2>{};}
};
enum class PositionSource:std::uint8_t { object,posed,objectNormal,posedNormal,posedLinearNormal };
struct ProjectiveTransform {
    // Complete native (X,Y,Z,1)->(S,T,Q) matrix for texture unit0. PICA performs
    // projection; the CPU does not transform vertices or divide coordinates.
    // Caller owns the Q-domain/content contract. Signed/near-zero Q behavior
    // and physical sampler precision remain unverified until GPU validation.
    std::array<Row,3> rows{{{1,0,0,0},{0,1,0,0},{0,0,0,1}}};
    PositionSource source=PositionSource::posed;
    bool enabled=false;
    bool normalize=false; // Before the complete matrix; caller guarantees finite nonzero length.
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
// Only the mirror's source front-cull override is admitted (native back).
enum class CullOverride:std::uint8_t { material,back };
struct DrawUniforms {
    Palette palette=Palette::identity();
    Matrix viewProjection{}; // Row-major; native clip depth [-W,0].
    std::array<Light,8> lights{};
    unsigned lightCount=0;
    std::array<ColorChannel,4> channels{}; // RGB0, RGB1, ALPHA0, ALPHA1.
    std::array<Row,2> materialColors{Row{1,1,1,1},Row{1,1,1,1}},ambientColors{};
    FogUniforms fog{};
    std::array<std::uint32_t,3> imageOverrides{};
    std::uint8_t imageOverrideMask=0;
    std::array<UvTransform,3> uvTransforms{UvTransform::identity(0),UvTransform::identity(1),UvTransform::identity(2)};
    ProjectiveTransform projective{};
    MaterialParameters material{};
    std::array<Sampler,3> samplers{};
    std::uint8_t samplerMask=0; // Explicit overrides for consumed texture units.
    CullOverride cullOverride=CullOverride::material;
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
enum class FogCarrier:std::uint8_t { none,primaryAlpha,primaryRed,secondaryAlpha,secondaryRed,texture0,texture1,texture2 };
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
    bool rasterLookup=false;
    std::uint8_t lookupBase=0;
    FogCarrier fogCarrier=FogCarrier::none;
    // Already authored native depth LUT. No runtime source-fog conversion.
    bool fog=false,fogDepthFlip=false;
    std::uint32_t fogColor=0;
    std::array<float,256> fogValues{};
    static Material vertexColor();
    static Material textured(unsigned unit=0);
};
enum class TextureFormat:std::uint8_t { rgba8,rgb565,etc1,etc1a4,rgba5551=5 };
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
using MeshBankHandle=Handle<struct MeshBankTag>;
using TextureHandle=Handle<struct TextureTag>;
using MaterialHandle=Handle<struct MaterialTag>;
}
