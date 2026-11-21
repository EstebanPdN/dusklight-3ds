/**
 * f_pc_executor.cpp
 * Framework - Process Executor
 */

#if defined(DUSK_TARGET_3DS)
#include "../../platforms/3ds/runtime/gx_profile.hpp"
#include "../../platforms/3ds/runtime/perf_features.hpp"
#endif
#include "f_pc/f_pc_executor.h"
#include "f_pc/f_pc_debug_sv.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_node.h"
#include "f_pc/f_pc_pause.h"
#include "f_pc/f_pc_searcher.h"
#if defined(DUSK_TARGET_3DS)
#include "d/d_native_logic.h"
#include "f_pc/f_pc_id_index.h"
#include "f_op/f_op_actor_tag.h"
#include <dolphin/os.h>
#include <dolphin/ppc_math.h>
#include <dolphin/gd/GDBase.h>
#include <cstdio>

void dBgS_NativeReportChanges(f32 perPicture);
void dBgS_Acch_NativeReportSkip(f32 perPicture);

namespace dNativeLogic {

Census census;
bool g_split;

namespace {
Config l_config;
bool l_configRead;
unsigned l_pictures;

struct OtherJudge {
    void* judge;
    unsigned calls, nodes;
};
OtherJudge l_otherJudges[24];
}  // namespace

void countOtherJudge(void* judge, unsigned nodes) {
    census.actorOther++;
    census.actorNodesOther += nodes;
    for (OtherJudge& entry : l_otherJudges) {
        if (entry.judge == judge || entry.judge == NULL) {
            entry.judge = judge;
            entry.calls++;
            entry.nodes += nodes;
            return;
        }
    }
}

const Config& config() {
    if (!l_configRead) {
        l_configRead = true;
        int refused = 0;
        bool present = false;
        if (FILE* file = std::fopen("sdmc:/3ds/DuskLight/logic.txt", "rb")) {
            char text[256] = {};
            (void)std::fread(text, 1, sizeof(text) - 1, file);
            std::fclose(file);
            present = true;
            refused = parseConfig(text, l_config);
        }
        g_split = l_config.split;
        dusk_native_frsqrte_mode = l_config.frsqrte;
        dusk_native_fmaf_mode = l_config.fmaf;
        dusk_native_rnd_mode = l_config.rnd;
        dusk_native_gd_bytewise = l_config.gdWrite == Off;
        static const char* const names[3] = {"off", "on", "verify"};
        OSReport("Native logic procindex=%s actorindex=%s envelope=%s frsqrte=%s fmaf=%s rnd=%s kytag12=%s "
                 "ccpre=%s crrskip=%s gdwrite=%s census=%s split=%s%s%s (SD switch logic.txt: none, verify, key=0|1|2)\n",
                 names[l_config.procIndex], names[l_config.actorIndex], names[l_config.envelope],
                 names[l_config.frsqrte], names[l_config.fmaf], names[l_config.rnd],
                 names[l_config.kytag12], names[l_config.ccPre], names[l_config.crrSkip], names[l_config.gdWrite],
                 l_config.census ? "on" : "off",
                 l_config.split ? "on" : "off",
                 present ? "" : "; no logic.txt", refused ? "; words refused in logic.txt" : "");
    }
    return l_config;
}

void endPicture() {
    census.ticks++;
    if (!config().census || ++l_pictures < 300) {
        return;
    }
    const Census& c = census;
    const float n = 1.0f / c.ticks;
    unsigned live = 0, peak = 0, broken = 0;
    fpcIdIdx_State(&live, &peak, &broken);
    OSReport("Native logic, last %u pictures, per picture: searches by id %.1f (table %.1f, none found "
             "%.1f) by judge %.1f actor list %.1f create list %.1f, nodes walked %.1f; id table live=%u "
             "peak=%u stopped=%u; verify checks %u mismatches %u\n",
             l_pictures, c.searchById * n, c.searchByIdIndexed * n, c.searchByIdMissing * n,
             c.searchByJudge * n, c.actorListJudge * n, c.createListJudge * n, c.nodesVisited * n, live,
             peak, broken, c.verifyChecks, c.verifyMismatches);
    unsigned actorLive = 0, actorPeak = 0, actorStopped = 0;
    fopAcTg_IndexState(&actorLive, &actorPeak, &actorStopped);
    OSReport("Native logic actor list, per picture: by id %.1f (table %.1f, walked nodes %.1f) by name "
             "%.1f (table %.1f, walked nodes %.1f) other judges %.1f (nodes %.1f); actor table live=%u "
             "peak=%u stopped=%u; verify checks %u mismatches %u\n",
             c.actorById * n, c.actorByIdIndexed * n, c.actorNodesById * n, c.actorByName * n,
             c.actorByNameIndexed * n, c.actorNodesByName * n, c.actorOther * n, c.actorNodesOther * n,
             actorLive, actorPeak, actorStopped, c.actorVerifyChecks, c.actorVerifyMismatches);
    {
        // The other judges, most nodes first (addr2line the addresses).
        char line[320];
        int at = std::snprintf(line, sizeof(line), "Native logic other actor judges (calls/nodes per picture):");
        for (int rank = 0; rank < 8 && at < int(sizeof(line)) - 40; rank++) {
            OtherJudge* best = NULL;
            for (OtherJudge& entry : l_otherJudges) {
                if (entry.judge != NULL && entry.calls != 0 && (best == NULL || entry.nodes > best->nodes)) {
                    best = &entry;
                }
            }
            if (best == NULL) {
                break;
            }
            at += std::snprintf(line + at, sizeof(line) - at, " %p=%.1f/%.0f", best->judge,
                                best->calls * n, best->nodes * n);
            best->calls = 0;
        }
        OSReport("%s\n", line);
        for (OtherJudge& entry : l_otherJudges) {
            entry = OtherJudge{};
        }
    }
    OSReport("Native logic math, per picture: random numbers %.1f; mist ticks %u particles %.1f; "
             "verify math checks %u mismatches %u, mist checks %u mismatches %u\n",
             c.rndCalls * n, c.kytagTicks, c.kytagParticles * n, dusk_native_math_checks,
             dusk_native_math_mismatches, c.kytagChecks, c.kytagMismatches);
    dusk_native_math_checks = dusk_native_math_mismatches = 0;
    OSReport("Native logic collision, per picture: kcl calls/triangles line %.1f/%.0f gnd %.1f/%.0f "
             "wallSort %.1f/%.0f wall %.1f/%.0f roof %.1f/%.0f spl %.1f/%.0f, corner rebuilds %.0f; "
             "collider pairs co %.0f (area %.0f, tested %.0f) at-tg %.0f (area %.0f, tested %.0f); "
             "joints %.0f\n",
             c.kclCalls[0] * n, c.kclPrisms[0] * n, c.kclCalls[1] * n, c.kclPrisms[1] * n,
             c.kclCalls[2] * n, c.kclPrisms[2] * n, c.kclCalls[3] * n, c.kclPrisms[3] * n,
             c.kclCalls[4] * n, c.kclPrisms[4] * n, c.kclCalls[5] * n, c.kclPrisms[5] * n,
             c.kclTriPnt * n, c.ccCoPairs * n, c.ccCoArea * n, c.ccCoShape * n, c.ccAtTgPairs * n,
             c.ccAtTgArea * n, c.ccAtTgShape * n, c.j3dJoints * n);
    OSReport("Native logic collision objects, per picture: walks/slots line %.1f/%.0f gnd %.1f/%.0f wall "
             "%.1f/%.0f roof %.1f/%.0f spl %.1f/%.0f; push pairs hit %.1f, pre-check left out %.1f "
             "(verify checks %u mismatches %u)\n",
             c.bgWalks[0] * n, c.bgScanned[0] * n, c.bgWalks[1] * n, c.bgScanned[1] * n,
             c.bgWalks[2] * n, c.bgScanned[2] * n, c.bgWalks[3] * n, c.bgScanned[3] * n,
             c.bgWalks[4] * n, c.bgScanned[4] * n, c.ccCoHits * n, c.ccPreSkipped * n, c.ccPreChecks,
             c.ccPreMismatches);
    OSReport("Native logic crrskip, per picture: eligible %.1f same input and world %.1f skipped %.1f; "
             "collision object changes %.1f; verify checks %u mismatches %u\n",
             c.crrEligible * n, c.crrHits * n, c.crrSkipped * n, c.bgChanges * n, c.crrChecks,
             c.crrMismatches);
    dBgS_NativeReportChanges(n);
    dBgS_Acch_NativeReportSkip(n);
    OSReport("Native logic skin, per picture: models %.1f envelopes %.1f joint products %.1f; verify "
             "checks %u mismatches %u\n",
             c.envelopeModels * n, c.envelopeEntries * n, c.envelopeProducts * n, c.envelopeChecks,
             c.envelopeMismatches);
    OSReport("Native logic far, per picture: culled actors of the reviewed profiles %.1f, rested %.1f (features "
             "logic=%d)\n",
             c.farCandidates * n, c.farRested * n,
             int(dusk::native::features::on(dusk::native::features::Id::farLogic)));
    census = Census{};
    l_pictures = 0;
}

}  // namespace dNativeLogic
#endif

base_process_class* fpcEx_Search(fpcLyIt_JudgeFunc i_judgeFunc, void* i_data) {
#if defined(DUSK_TARGET_3DS)
    dNativeLogic::census.searchByJudge++;
#endif
    return (base_process_class*)fpcLyIt_AllJudge(i_judgeFunc, i_data);
}

base_process_class* fpcEx_SearchByID(fpc_ProcID i_id) {
    if (!(i_id == fpcM_UNK_PROCESS_ID_e || i_id == fpcM_ERROR_PROCESS_ID_e)) {
#if defined(DUSK_TARGET_3DS)
        dNativeLogic::Census& census = dNativeLogic::census;
        census.searchById++;
        const unsigned mode = dNativeLogic::config().procIndex;
        base_process_class* found;
        if (mode != dNativeLogic::Off && fpcIdIdx_Find(i_id, &found)) {
            census.searchByIdIndexed++;
            if (mode == dNativeLogic::Verify) {
                // The original walk, kept as the answer.
                base_process_class* walked =
                    (base_process_class*)fpcLyIt_AllJudge(fpcSch_JudgeByID, &i_id);
                census.verifyChecks++;
                if (walked != found) {
                    if (census.verifyMismatches++ < 8) {
                        OSReport("Native logic procindex mismatch: id=%u table=%p walk=%p\n", i_id,
                                 (void*)found, (void*)walked);
                    }
                    found = walked;
                }
            }
            if (found == NULL) {
                census.searchByIdMissing++;
            }
            return found;
        }
        return (base_process_class*)fpcLyIt_AllJudge(fpcSch_JudgeByID, &i_id);
#else
        return fpcEx_Search(fpcSch_JudgeByID, &i_id);
#endif
    }
    return NULL;
}

BOOL fpcEx_IsExist(fpc_ProcID i_id) {
    if (fpcEx_SearchByID(i_id) != NULL) {
        return TRUE;
    } else {
        return FALSE;
    }
}

int fpcEx_Execute(base_process_class* i_proc) {
    if (i_proc->state.init_state != 2 || fpcPause_IsEnable(i_proc, 1) == TRUE)
        return 0;

#if defined(DUSK_TARGET_3DS)
    dusk::native::GxProcessProfileScope processProfile(0, i_proc->profname);
#endif
    return fpcBs_Execute(i_proc);
}

int fpcEx_ToLineQ(base_process_class* i_proc) {
    base_process_class* process = &i_proc->layer_tag.layer->process_node->base;

    if (i_proc->layer_tag.layer->layer_id == fpcLy_ROOT_e || cTg_IsUse(&process->line_tag_.base) == TRUE) {
        int var_r28 = i_proc->priority.current_info.list_id;
        if (fpcLnTg_ToQueue(&i_proc->line_tag_, var_r28) == 0) {
            fpcLyTg_QueueTo(&i_proc->layer_tag);
            return 0;
        }

#if DEBUG
        if (g_fpcDbSv_service[1] != NULL) {
            g_fpcDbSv_service[1](i_proc);
        }
#endif

        i_proc->state.init_state = 2;
        if (fpcBs_Is_JustOfType(g_fpcNd_type, i_proc->subtype)) {
            fpcLyIt_OnlyHere(&((process_node_class*)i_proc)->layer, (fpcLyIt_OnlyHereFunc)fpcEx_ToLineQ, i_proc);
        }

        return 1;
    }

    return 0;
}

int fpcEx_ExecuteQTo(base_process_class* i_proc) {
    if (fpcLyTg_QueueTo(&i_proc->layer_tag) == 1) {
        i_proc->state.init_state = 3;
        return 1;
    }
    
    return 0;
}

int fpcEx_ToExecuteQ(base_process_class* i_proc) {
    process_priority_class* priority = &i_proc->priority;
    if (fpcLyTg_ToQueue(&i_proc->layer_tag, priority->current_info.layer_id, priority->current_info.list_id, priority->current_info.list_priority) == 1) {
        fpcEx_ToLineQ(i_proc);
        return 1;
    }
    
    return 0;
}

void fpcEx_Handler(fpcLnIt_QueueFunc i_queueFunc) {
    fpcLnIt_Queue(i_queueFunc);
}
