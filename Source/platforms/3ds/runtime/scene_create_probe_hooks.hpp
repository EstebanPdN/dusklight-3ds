#ifndef DUSK_SCENE_CREATE_PROBE_HOOKS_HPP
#define DUSK_SCENE_CREATE_PROBE_HOOKS_HPP
#include "scene_create_probe.hpp"
#include "f_pc/f_pc_create_req.h"
#include "f_pc/f_pc_node.h"
#include "f_pc/f_pc_name.h"
namespace dusk::native {
// Borrowed pointers are read synchronously at their creation owner; none are retained.
inline SceneProbeIdentity sceneProbeIdentity(const create_request& r, int requested) {
    SceneProbeIdentity s{r.id, ~0u, ~0u, 0, 0, requested, -1, -1, -1, -1, -1, -1};
    if (r.layer) {
        s.layer = r.layer->layer_id;
        if (r.layer->process_node) {
            const auto& parent = r.layer->process_node->base;
            s.parent = parent.id; s.parentProfile = parent.profname; s.parentParam = parent.parameters;
            if (parent.profname == fpcNm_ROOM_SCENE_e) s.parentRoom = static_cast<int>(parent.parameters);
        }
    }
    if (r.process) {
        s.name = r.process->name; s.profile = r.process->profname;
        s.param = r.process->parameters; s.init = r.process->state.init_state;
        if (s.profile == fpcNm_ROOM_SCENE_e) s.room = static_cast<int>(s.param);
    }
    return s;
}
}
#endif
