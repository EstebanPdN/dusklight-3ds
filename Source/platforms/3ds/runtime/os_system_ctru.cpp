#include "os_system.hpp"
#include <3ds.h>
namespace dusk::native {
std::uint8_t platformGameLanguage(){
    // Translate the supported native console language to the GCN language
    // numbering used by original PAL resources. Unsupported scripts use English.
    static const std::uint8_t language=[](){
        u8 raw=CFG_LANGUAGE_EN;
        if(R_FAILED(cfguInit()))return std::uint8_t{0};
        const auto result=CFGU_GetSystemLanguage(&raw);cfguExit();
        if(R_FAILED(result))return std::uint8_t{0};
        switch(raw){case CFG_LANGUAGE_DE:return std::uint8_t{1};case CFG_LANGUAGE_FR:return std::uint8_t{2};
        case CFG_LANGUAGE_ES:return std::uint8_t{3};case CFG_LANGUAGE_IT:return std::uint8_t{4};case CFG_LANGUAGE_NL:return std::uint8_t{5};
        default:return std::uint8_t{0};}
    }();
    return language;
}
bool platformDigitalVideo(){return true;}
}
