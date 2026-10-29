#pragma once
#include "model.hpp"
namespace dusk::pica {
struct SceneItem {
    Model* model{};
    const DrawUniforms* parameters{};
    unsigned part{};
    std::uint32_t order{};
    std::uint16_t pass{};
};
// Synchronous, bounded native scene submission. Caller supplies final ordering
// keys: ascending (pass,order), then stable input order for ties. No source
// buckets/material interpreter, camera transforms or implicit alpha sorting.
// Model objects and parameter blocks must remain alive/immutable for this call.
// All resources stay on this renderer's exclusive owner thread. No retained
// pointers, new resident storage, cross-model merging or asynchronous callbacks.
class Scene {
public:
    static constexpr unsigned capacity=1024;
    static Submission draw(Renderer&,std::span<const SceneItem>);
};
}
