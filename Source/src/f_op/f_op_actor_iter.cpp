/**
 * f_op_actor_iter.cpp
 * Actor Process Iterator
 */

#include "f_op/f_op_actor_iter.h"
#include "SSystem/SComponent/c_list_iter.h"
#include "SSystem/SComponent/c_tag_iter.h"
#include "f_op/f_op_actor_tag.h"
#if defined(DUSK_TARGET_3DS)
#include "d/d_native_logic.h"
#include "f_pc/f_pc_searcher.h"
#include <dolphin/os.h>
#endif

int fopAcIt_Executor(fopAcIt_ExecutorFunc i_execFunc, void* i_data) {
    struct {
        fopAcIt_ExecutorFunc func;
        void* data;
    } userData;
    userData.func = i_execFunc;
    userData.data = i_data;

    return cLsIt_Method(&g_fopAcTg_Queue, (cNdIt_MethodFunc)cTgIt_MethodCall, &userData);
}

#if defined(DUSK_TARGET_3DS)
static void* fopAcIt_JudgeWalk(fopAcIt_JudgeFunc i_judgeFunc, void* i_data) {
    struct {
        fopAcIt_JudgeFunc func;
        void* data;
    } userData;
    userData.func = i_judgeFunc;
    userData.data = i_data;

    return cLsIt_Judge(&g_fopAcTg_Queue, (cNdIt_JudgeFunc)cTgIt_JudgeFilter, &userData);
}

// Native New 3DS (logic.txt actorindex): fopAcM_SearchByID and
// fopAcM_SearchByName (fpcSch_JudgeByID / fpcSch_JudgeForPName on the actor
// list) read the tables of f_op_actor_tag.cpp; every other judge walks.
void* fopAcIt_Judge(fopAcIt_JudgeFunc i_judgeFunc, void* i_data) {
    dNativeLogic::Census& census = dNativeLogic::census;
    census.actorListJudge++;
    const bool byId = i_judgeFunc == (fopAcIt_JudgeFunc)fpcSch_JudgeByID;
    const bool byName = !byId && i_judgeFunc == (fopAcIt_JudgeFunc)fpcSch_JudgeForPName;
    if (byId || byName) {
        const unsigned mode = dNativeLogic::config().actorIndex;
        void* found = NULL;
        const int answered =
            mode == dNativeLogic::Off
                ? 0
                : (byId ? fopAcTg_FindById(*(unsigned int*)i_data, &found)
                        : fopAcTg_FindByName(*(s16*)i_data, &found));
        if (byId) {
            census.actorById++;
        } else {
            census.actorByName++;
        }
        if (answered) {
            if (byId) {
                census.actorByIdIndexed++;
            } else {
                census.actorByNameIndexed++;
            }
            if (mode == dNativeLogic::Verify) {
                void* walked = fopAcIt_JudgeWalk(i_judgeFunc, i_data);
                census.actorVerifyChecks++;
                if (walked != found) {
                    if (census.actorVerifyMismatches++ < 8) {
                        OSReport("Native logic actorindex mismatch: %s=%d table=%p walk=%p\n",
                                 byId ? "id" : "name",
                                 byId ? int(*(unsigned int*)i_data) : int(*(s16*)i_data), found,
                                 walked);
                    }
                    found = walked;
                }
            }
            return found;
        }
        const unsigned before = census.nodesVisited;
        void* walked = fopAcIt_JudgeWalk(i_judgeFunc, i_data);
        (byId ? census.actorNodesById : census.actorNodesByName) += census.nodesVisited - before;
        return walked;
    }
    const unsigned before = census.nodesVisited;
    void* walked = fopAcIt_JudgeWalk(i_judgeFunc, i_data);
    dNativeLogic::countOtherJudge((void*)i_judgeFunc, census.nodesVisited - before);
    return walked;
}
#else
void* fopAcIt_Judge(fopAcIt_JudgeFunc i_judgeFunc, void* i_data) {
    struct {
        fopAcIt_JudgeFunc func;
        void* data;
    } userData;
    userData.func = i_judgeFunc;
    userData.data = i_data;

    return cLsIt_Judge(&g_fopAcTg_Queue, (cNdIt_JudgeFunc)cTgIt_JudgeFilter, &userData);
}
#endif
