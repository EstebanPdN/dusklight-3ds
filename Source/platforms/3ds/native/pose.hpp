#pragma once
#include "pack.hpp"
namespace dusk::pica {
using Affine=std::array<Row,3>;
enum class NormalMode:std::uint32_t {inverseTranspose,linear};
struct PoseMatrix {Affine position{},normal{};};
class PoseView {
public:
    bool palette(std::span<const std::uint32_t> nativeMatrixIds,Palette&)const;
    std::span<const Affine> nodes()const{return world;}
    std::span<const PoseMatrix> matrices()const{return draws;}
private:
    friend class PosePack;
    std::span<const Affine> world{};std::span<const PoseMatrix> draws{};
};
// Borrowed immutable native graph. Caller retains file bytes through evaluation.
// No source model/animation/command parser or CPU vertex skinning.
class PosePack {
public:
    static PackError open(std::span<const std::byte>,PosePack&);
    unsigned nodeCount()const{return count;}
    unsigned matrixCount()const{return draws;}
    unsigned anchorNode(std::uint32_t id)const; // Resolve once when binding, 0=missing.
    bool restLocal(unsigned node,Affine&)const;
    // Empty locals selects authored native rest pose; otherwise all nodes.
    // Workspaces must be disjoint from each other, file, root and local input.
    // Failure invalidates out; workspace may contain an incomplete evaluation.
    bool evaluate(const Affine& root,std::span<const Affine> locals,
                  std::span<Affine> world,std::span<PoseMatrix> matrices,PoseView& out,
                  std::span<const NormalMode> normalModes={})const;
    static Affine identity();
private:
    std::span<const std::byte> bytes{};
    unsigned count{},draws{},terms{},drawAt{},termAt{};
};
}
