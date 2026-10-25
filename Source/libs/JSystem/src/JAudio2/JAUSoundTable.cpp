//
// JAUSoundTable
//

#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JAudio2/JAUSoundTable.h"

#if !defined(DUSK_TARGET_3DS)
#include "dusk/mods/svc/audio_res/bst.hpp"
#endif

void JAUSoundTable::init(void const* param_0) {
    if (param_0 != NULL) {
        field_0x0.init(param_0);
    } else {
        field_0x0.reset();
    }
}

u8 JAUSoundTable::getTypeID(JAISoundID param_0 IF_DUSK_ARG(SoundTableReplacementSlot const* replacement)) const {
#if DUSK_AUDIO_DISABLED
    if (this == NULL) {
        return 0xff;
    }
#endif
#if defined(DUSK_TARGET_3DS)
    if (replacement) OSPanic(__FILE__,__LINE__,"Native audio expects original sound table");
#elif TARGET_PC
    if (replacement) {
        return replacement->get_type_id();
    }
#endif
    if (param_0.isAnonymous()) {
        return 0xff;
    }
    JAUSoundTableSection* section = field_0x0.getSection(param_0.id_.info.type.parts.sectionID);
    if (section == NULL) {
        return 0xff;
    }
    JAUSoundTableGroup* group = field_0x0.getGroup(section, param_0.id_.info.type.parts.groupID);
    if (group == NULL) {
        return 0xff;
    }
    return group->getTypeID(param_0.id_.info.waveID);
}

JAUSoundTableItem DUSK_CONST* JAUSoundTable::getData(JAISoundID param_0 IF_DUSK_ARG(SoundTableReplacementSlot const* replacement)) const {
#if DUSK_AUDIO_DISABLED
    if (this == NULL) {
        return NULL;
    }
#endif
#if defined(DUSK_TARGET_3DS)
    if (replacement) OSPanic(__FILE__,__LINE__,"Native audio expects original sound table");
#elif TARGET_PC
    if (replacement) {
        return &replacement->item;
    }
#endif
    if (param_0.isAnonymous()) {
        return NULL;
    }
    JAUSoundTableSection* section = field_0x0.getSection(param_0.id_.info.type.parts.sectionID);
    if (section == NULL) {
        return NULL;
    }
    JAUSoundTableGroup* group = field_0x0.getGroup(section, param_0.id_.info.type.parts.groupID);
    if (group == NULL) {
        return NULL;
    }
    return getItem(group, param_0.id_.info.waveID);
}

void JAUSoundNameTable::init(void const* param_0) {
    if (param_0 != NULL) {
        field_0x0.init(param_0);
    } else {
        field_0x0.reset();
    }
}
