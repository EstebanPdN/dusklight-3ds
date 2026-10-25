/**
 * f_pc_profile.cpp
 * Framework - Process Profile
 */

#include "f_pc/f_pc_profile.h"
#if defined(DUSK_PROFILE_LINK_PROBE)
#include "os_report.h"
#include <cstdlib>
#endif

#if TARGET_PC
#include "f_pc/f_pc_name.h"
#if !defined(DUSK_TARGET_3DS)
#include "dusk/mods/svc/actor.hpp"
#endif
#endif

#ifndef __MWERKS__
// Forward declare the static list from f_pc_profile_lst.cpp
DUSK_GAME_EXTERN process_profile_definition DUSK_CONST* DUSK_CONST g_fpcPfLst_ProfileList[];
// On PC: Direct pointer to static array
DUSK_GAME_DATA process_profile_definition DUSK_CONST* DUSK_CONST* DUSK_CONST g_fpcPf_ProfileList_p =
    g_fpcPfLst_ProfileList;
#else
// On Console: Pointer initialized by REL module prolog
process_profile_definition** g_fpcPf_ProfileList_p;
#endif

process_profile_definition DUSK_CONST* fpcPf_Get(s16 i_profname) {
#if TARGET_PC
#if !defined(DUSK_TARGET_3DS)
    // Check if a mod has registered an actor with i_profname. Fallback to the profile list if it
    // doesn't exist.
    process_profile_definition* profile =
        dusk::mods::svc::actor_impl::get_profile_from_proc_name(i_profname);
    if (profile != nullptr) {
        return profile;
    }
#endif
    if (i_profname < 0 || i_profname >= fpcNm_MAX_NUM) {
        return nullptr;
    }
#endif

    int index = i_profname;
#if defined(DUSK_PROFILE_LINK_PROBE)
    // A limited link diagnostic must never silently discard a required actor.
    if (g_fpcPf_ProfileList_p[index] == nullptr) {
        OSReport_Error("Bounded profile LINK probe requested unsupported process %d\n", index);
        std::abort();
    }
#endif
    return g_fpcPf_ProfileList_p[index];
}
