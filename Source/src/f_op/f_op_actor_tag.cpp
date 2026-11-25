/**
 * f_op_actor_tag.cpp
 *
 */

#include "f_op/f_op_actor_tag.h"
#include "SSystem/SComponent/c_list.h"
#if defined(DUSK_TARGET_3DS)
#include "f_pc/f_pc_base.h"
#include "f_pc/f_pc_id_index.h"
#include "f_op/f_op_actor_index.h"

// Native New 3DS: the actor list by id and by profile name (see
// f_op/f_op_actor_index.h, logic.txt actorindex). Kept from the first actor
// on, whatever the switch says; the switch only chooses who answers.
namespace {
fpcIdIndexTable<base_process_class, fpc_ProcID> l_actorIdIndex;
fopAcNameFirstTable<base_process_class> l_actorNameFirst;

inline base_process_class* actorOfTag(create_tag_class* i_createTag) {
    // As cTgIt_JudgeFilter hands it to the judge (void* -> process).
    return (base_process_class*)i_createTag->mpTagData;
}
}  // namespace

int fopAcTg_FindById(fpc_ProcID i_id, void** o_actor) {
    base_process_class* actor;
    if (!l_actorIdIndex.find(i_id, &actor)) {
        return 0;
    }
    if (actor != NULL && actor->id != i_id) {
        // The process's id changed while listed: never expected; walk from now on.
        l_actorIdIndex.stopped = 7;
        return 0;
    }
    *o_actor = actor;
    return 1;
}

int fopAcTg_FindByName(s16 i_name, void** o_actor) {
    base_process_class* actor;
    if (!l_actorNameFirst.find(i_name, &actor)) {
        return 0;
    }
    *o_actor = actor;
    return 1;
}

void fopAcTg_IndexState(unsigned* o_live, unsigned* o_peak, unsigned* o_stopped) {
    *o_live = l_actorIdIndex.live;
    *o_peak = l_actorIdIndex.peak;
    *o_stopped = l_actorIdIndex.stopped;
}
#endif

DUSK_GAME_DATA node_list_class g_fopAcTg_Queue = {NULL, NULL, 0};

int fopAcTg_ToActorQ(create_tag_class* i_createTag) {
#if defined(DUSK_TARGET_3DS)
    const int ret = cTg_Addition(&g_fopAcTg_Queue, i_createTag);
    if (ret != 0) {
        l_actorIdIndex.add(actorOfTag(i_createTag));
        l_actorNameFirst.added(actorOfTag(i_createTag));
    }
    return ret;
#else
    return cTg_Addition(&g_fopAcTg_Queue, i_createTag);
#endif
}

void fopAcTg_ActorQTo(create_tag_class* i_createTag) {
#if defined(DUSK_TARGET_3DS)
    if (i_createTag->mbIsUse == true) {
        // Before the cut: the name table walks on from this node.
        l_actorNameFirst.removing(
            actorOfTag(i_createTag), &i_createTag->mpNode,
            [](node_class* node) { return node->mpNextNode; },
            [](node_class* node) { return actorOfTag((create_tag_class*)node); });
    }
#endif
    int _ = cTg_SingleCutFromTree(i_createTag);
#if defined(DUSK_TARGET_3DS)
    if (_ != 0) {
        l_actorIdIndex.remove(actorOfTag(i_createTag));
    }
#endif
}

int fopAcTg_Init(create_tag_class* i_createTag, void* i_data) {
    cTg_Create(i_createTag, i_data);
    return 1;
}
