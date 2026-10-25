#pragma once

#if defined(DUSK_TARGET_3DS)
#include "d/d_com_inf_game.h"
#include "dusk/version.hpp"

namespace dusk::native {
// Original regional message directories and original save-language codes.
// No desktop configuration, ISO browser or mod SDK is needed by these readers.
inline const char* messageFolder() {
    if (version::isRegionJpn()) return "Msgjp";
    if (version::isRegionPal()) {
        switch (dComIfGs_getPalLanguage()) {
        case dSv_player_config_c::LANGUAGE_GERMAN: return "Msgde";
        case dSv_player_config_c::LANGUAGE_FRENCH: return "Msgfr";
        case dSv_player_config_c::LANGUAGE_SPANISH: return "Msgsp";
        case dSv_player_config_c::LANGUAGE_ITALIAN: return "Msgit";
        default: return "Msguk";
        }
    }
    return "Msgus";
}
} // namespace dusk::native
#endif
