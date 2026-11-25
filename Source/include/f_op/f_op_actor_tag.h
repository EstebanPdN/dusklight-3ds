#ifndef F_OP_ACTOR_TAG_H_
#define F_OP_ACTOR_TAG_H_

#include "SSystem/SComponent/c_tag.h"

void fopAcTg_ActorQTo(create_tag_class* i_createTag);
int fopAcTg_Init(create_tag_class* i_createTag, void* i_data);
int fopAcTg_ToActorQ(create_tag_class* i_createTag);

DUSK_GAME_EXTERN node_list_class g_fopAcTg_Queue;

#if defined(DUSK_TARGET_3DS)
// Native New 3DS (f_op/f_op_actor_index.h, logic.txt actorindex): 1 and the
// actor the walk would find (null when none), or 0 when the table cannot
// answer and the caller walks.
int fopAcTg_FindById(unsigned int i_id, void** o_actor);
int fopAcTg_FindByName(s16 i_name, void** o_actor);
void fopAcTg_IndexState(unsigned* o_live, unsigned* o_peak, unsigned* o_stopped);
#endif

#endif
