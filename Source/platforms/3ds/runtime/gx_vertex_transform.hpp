#pragma once
#include "gx_commands.hpp"
namespace dusk::native::gx {
enum class VertexTransformError { none, missingState, projection, viewport, matrixIndex, texgen, missingAttribute, nonfinite, normalization, normalLengthOverflow };
struct TransformedVertex {
    std::array<float,4> clip{}; // PICA rotated clip position; preserve w for clipping/interpolation.
    std::array<float,3> viewPosition{},viewNormal{};
    bool normalValid{};
    std::array<std::array<float,3>,8> texcoord{}; // s,t,q. No premature projective division.
};
class VertexTransformPlan {
public:
    const std::array<float,16>& clipProjection()const{return projection;}
    std::uint8_t coordinateMask()const{return mask;}
    std::uint8_t projectiveMask()const{return projective;}
private:
    friend struct VertexTransformDiagnostic;
    friend bool planVertexTransform(const CommandRegisters&,std::uint8_t,VertexTransformPlan&,VertexTransformError*);
    friend bool transformVertex(const VertexTransformPlan&,const DecodedVertex&,TransformedVertex&,VertexTransformError*,bool);
    struct Texgen {std::uint8_t row{},type{},post{};bool normalize{},projection{};};
    const CommandRegisters* registers=nullptr;
    std::array<float,16> projection{};
    std::array<Texgen,8> texgen{};
    // Validated lazily once per selected palette slot, within this draw only.
    // No reuse across GX state writes or resource lifetimes.
    struct MatrixCache {std::array<float,12> values{};unsigned rows{};};
    mutable std::array<MatrixCache,20> matrices{},postMatrices{};
    mutable std::array<MatrixCache,10> normals{};
    std::uint8_t mask{},projective{};
};
// Prepare once per synchronous draw. Registers remain borrowed, alive and
// unchanged until all its vertices are transformed; do not cache across writes.
// Requested mask includes only texture coordinates actually consumed by the
// material. Matrix2x4/3x4, SRTG, normalize/post matrices and per-vertex selectors
// match the pinned native-source path. Emboss/line-point offsets reject or stay
// outside this triangle path. No allocation, lighting, skinning or GPU calls.
bool planVertexTransform(const CommandRegisters&,std::uint8_t requestedCoordinates,
                         VertexTransformPlan&,VertexTransformError* = nullptr);
// Position matrices are already produced by the original J3D joint/envelope
// system. This consumes their GX palette; it does not reconstruct joint logic.
// Output clears on failure. Texture q is preserved for later fragment sampling.
bool transformVertex(const VertexTransformPlan&,const DecodedVertex&,TransformedVertex&,VertexTransformError* = nullptr,bool normalRequired = false);
const char* vertexTransformErrorText(VertexTransformError);
}
