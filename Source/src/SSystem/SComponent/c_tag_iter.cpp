/**
 * c_tag_iter.cpp
 *
 */

#include "SSystem/SComponent/c_tag_iter.h"
#include "SSystem/SComponent/c_tag.h"
#if defined(DUSK_TARGET_3DS)
#include "d/d_native_logic.h"
#endif

int cTgIt_MethodCall(create_tag_class* tag, method_filter* filter) {
    return filter->mpMethodFunc((node_class*)tag->mpTagData, filter->mpUserData);
}

void* cTgIt_JudgeFilter(create_tag_class* tag, judge_filter* filter) {
#if defined(DUSK_TARGET_3DS)
    dNativeLogic::census.nodesVisited++;
#endif
    return filter->mpJudgeFunc((node_class*)tag->mpTagData, filter->mpUserData);
}
