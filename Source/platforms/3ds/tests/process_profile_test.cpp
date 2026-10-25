#include "f_pc/f_pc_profile.h"
#include "f_pc/f_pc_name.h"
#include <cassert>
#include <cstdio>
#if defined(DUSK_PROFILE_LINK_PROBE)
#include <cstdarg>
void OSReport_Error(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
}
#endif

// The original static profile-list provider is substituted only in this host
// test. No actor or game initialization is simulated by these fixtures.
static const process_profile_definition first{0, 0, 0, 0, nullptr, 128, 0, 0};
static const process_profile_definition third{0, 0, 0, 2, nullptr, 512, 0, 0};
extern const process_profile_definition* const g_fpcPfLst_ProfileList[fpcNm_MAX_NUM] = {
    &first, nullptr, &third,
};

int main(int argc, char**) {
#if defined(DUSK_PROFILE_LINK_PROBE)
    if (argc > 1) {
        fpcPf_Get(1); // Must abort on the omitted required profile.
        return 2;
    }
    assert(fpcPf_Get(0) == &first && fpcPf_Get(2) == &third);
#else
    (void)argc;
    for (int id = -32768; id <= 32767; ++id) {
        const auto* expected = id == 0 ? &first : id == 2 ? &third : nullptr;
        assert(fpcPf_Get(static_cast<s16>(id)) == expected);
    }
#endif
    assert(g_fpcPf_ProfileList_p == g_fpcPfLst_ProfileList);
    assert(fpcPf_Get(0)->process_size == 128);
    assert(fpcPf_Get(2)->process_size == 512);
#if defined(DUSK_PROFILE_LINK_PROBE)
    std::puts("PASS bounded original profile pointer identity and payload; no actor execution");
#else
    std::puts("PASS original native profile resolver: all 65536 signed IDs, static pointer identity, null entries, bounds and profile payload; no actor execution");
#endif
}
