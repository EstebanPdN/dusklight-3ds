#pragma once

#include "story_benchmark.hpp"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include <initializer_list>

namespace dusk::native::story {

// Reuse the early loadout where it precedes the slice. The dungeon entry
// slices use an earlier base so their own dungeon clear stays unset.
inline Checkpoint chapterBase(Checkpoint checkpoint) {
    if (checkpoint == Checkpoint::lanayruWestOpened) return Checkpoint::lanayruTwilight;
    if (checkpoint < Checkpoint::ranch) return checkpoint;
    if (checkpoint == Checkpoint::ranch) return Checkpoint::opening;
    if (checkpoint <= Checkpoint::forestBoomerang) return Checkpoint::faron;
    if (checkpoint <= Checkpoint::minesBow) return Checkpoint::faronCleared;
    if (checkpoint <= Checkpoint::lanayruSpirit) return Checkpoint::lanayruTwilight;
    return Checkpoint::lanayruCleared;
}

inline void applyChapterCheckpoint(Checkpoint checkpoint) {
    if (checkpoint < Checkpoint::ranch) return;
    if (checkpoint == Checkpoint::bulblinCampKey) {
        // Resume after the key guard on the preceding camp skirmish street.
        dComIfGs_setKeyNum(dStage_SaveTbl_DESERT, 1);
        checkpoint = Checkpoint::desert;
    }
    if (checkpoint == Checkpoint::lanayruWestOpened) {
        // Restore the world progress reached by the preceding benchmark slice.
        // Only its original encounter can set this switch in castle-west-portal.
        dComIfGs_onSaveSwitch(dStage_SaveTbl_FIELD, castleWestSwitch);
        return;
    }
    const bool aeralfos = checkpoint == Checkpoint::skyAeralfos;
    if (aeralfos) checkpoint = Checkpoint::sky;

    using Flag = dSv_event_flag_c;
    const auto mark = [](std::initializer_list<u16> flags) {
        for (u16 flag : flags) dComIfGs_onEventBit(flag);
    };
    const auto bossCleared = [](int dungeon) {
        dComIfGs_getSaveInfo()->getSavedata().getSave(dungeon).getBit().onStageBossEnemy();
    };
    const auto selectX = [](u8 slot) { dComIfGs_setSelectItemIndex(SELECT_ITEM_X, slot); };

    if (checkpoint == Checkpoint::ranch) {
        mark({Flag::M_001, Flag::M_002, Flag::F_0574}); // First herding session.
        dComIfGs_setTime(247.0f);
        return;
    }
    if (checkpoint <= Checkpoint::sewers) {
        mark({Flag::M_008, Flag::F_0630});
        dComIfGs_onTransformLV(0);
        dComIfGs_offTransformLV(1);
        if (checkpoint == Checkpoint::prison) {
            dComIfGs_offEventBit(Flag::M_067);
        } else {
            mark({Flag::M_009, Flag::M_010, Flag::M_011, Flag::F_0550}); // Midna has taught wolf senses.
        }
        return;
    }

    if (checkpoint <= Checkpoint::forestBoomerang) {
        mark({Flag::M_008, Flag::M_009, Flag::M_010, Flag::M_011, Flag::M_012, Flag::M_013,
            Flag::M_014, Flag::M_015, Flag::M_016, Flag::M_017, Flag::M_019, Flag::M_072,
            Flag::F_0363});
        dComIfGs_onTransformLV(0);
        dComIfGs_onDarkClearLV(0);
        execItemGet(dItemNo_WEAR_KOKIRI_e);
        execItemGet(dItemNo_SWORD_e);
        execItemGet(dItemNo_WOOD_SHIELD_e);
        if (checkpoint == Checkpoint::forestBoomerang) {
            execItemGet(dItemNo_BOOMERANG_e);
            selectX(SLOT_0);
        }
        return;
    }

    bossCleared(dStage_SaveTbl_LV1);
    dComIfGs_onTransformLV(0);
    dComIfGs_onTransformLV(1);
    mark({Flag::M_018, Flag::M_021, Flag::M_027});

    if (checkpoint == Checkpoint::kakariko) return;
    dComIfGs_onDarkClearLV(1);
    mark({Flag::M_020, Flag::M_023, Flag::M_024, Flag::M_026, Flag::M_028, Flag::M_029, Flag::F_0232});
    execItemGet(dItemNo_HVY_BOOTS_e);
    if (checkpoint == Checkpoint::mines) {
        selectX(SLOT_3);
        return;
    }
    if (checkpoint == Checkpoint::minesBow) {
        execItemGet(dItemNo_BOW_e);
        selectX(SLOT_4);
        return;
    }

    bossCleared(dStage_SaveTbl_LV2);
    dComIfGs_onTransformLV(2);
    if (checkpoint >= Checkpoint::lanayruSpirit) mark({Flag::M_032, Flag::M_064});
    if (checkpoint <= Checkpoint::lanayruSpirit) {
        if (checkpoint == Checkpoint::zoraIce) dComIfGs_offEventBit(Flag::M_032);
        return;
    }

    mark({Flag::M_033, Flag::M_034, Flag::M_036});
    if (checkpoint == Checkpoint::escort) return;
    mark({Flag::M_035, Flag::M_037});
    execItemGet(dItemNo_WEAR_ZORA_e);
    dComIfGs_setEmptyBombBag(dItemNo_WATER_BOMB_e, 15);
    if (checkpoint <= Checkpoint::lakebedClawshot) dComIfGs_setSelectEquipClothes(dItemNo_WEAR_ZORA_e);
    if (checkpoint >= Checkpoint::lakebedClawshot) execItemGet(dItemNo_HOOKSHOT_e);
    if (checkpoint == Checkpoint::lakebedClawshot) {
        selectX(SLOT_9);
        dComIfGs_setSelectItemIndex(SELECT_ITEM_Y, SLOT_3); // Iron Boots for the descent to Morpheel.
    }
    if (checkpoint <= Checkpoint::lakebedClawshot) return;

    bossCleared(dStage_SaveTbl_LV3);
    mark({Flag::M_045});
    if (checkpoint == Checkpoint::desperateHour) return;
    mark({Flag::M_071});
    dComIfGs_onTransformLV(3);
    dComIfGs_offEventBit(Flag::M_067);
    if (checkpoint == Checkpoint::zeldaReunion) return;
    mark({Flag::F_0250});
    if (checkpoint == Checkpoint::grove) return;

    mark({Flag::F_0264, Flag::M_077, Flag::M_067, Flag::F_0305, Flag::F_0306});
    dComIfGs_onDarkClearLV(3);
    execItemGet(dItemNo_MASTER_SWORD_e);
    if (checkpoint >= Checkpoint::arbiter) mark({Flag::M_057});
    if (checkpoint >= Checkpoint::arbiterSpinner) execItemGet(dItemNo_SPINNER_e);
    if (checkpoint == Checkpoint::arbiterSpinner) selectX(SLOT_2);
    if (checkpoint >= Checkpoint::mirrorShard) {
        bossCleared(dStage_SaveTbl_LV4);
        mark({Flag::F_0265});
    }
    if (checkpoint >= Checkpoint::snowpeak) mark({Flag::F_0685, Flag::F_0335});
    if (checkpoint >= Checkpoint::snowpeakBall) execItemGet(dItemNo_IRONBALL_e);
    if (checkpoint == Checkpoint::snowpeakBall) selectX(SLOT_6);
    if (checkpoint >= Checkpoint::timeTemple) {
        bossCleared(dStage_SaveTbl_LV5);
        mark({Flag::F_0266});
        execItemGet(dItemNo_MIRROR_PIECE_2_e);
    }
    if (checkpoint >= Checkpoint::timeRod) execItemGet(dItemNo_COPY_ROD_e);
    if (checkpoint == Checkpoint::timeRod) selectX(SLOT_8);
    if (checkpoint >= Checkpoint::hiddenVillage) {
        bossCleared(dStage_SaveTbl_LV6);
        mark({Flag::F_0267});
        execItemGet(dItemNo_MIRROR_PIECE_3_e);
    }
    if (checkpoint == Checkpoint::hiddenVillage) selectX(SLOT_4);
    if (checkpoint >= Checkpoint::sky) {
        mark({Flag::F_288, Flag::F_0400, Flag::F_0484});
        execItemGet(dItemNo_COPY_ROD_2_e);
    }
    if (checkpoint == Checkpoint::sky) dComIfGs_setSelectItemIndex(SELECT_ITEM_Y, SLOT_3);
    if (aeralfos) selectX(SLOT_9);
    if (checkpoint >= Checkpoint::skyClawshots) execItemGet(dItemNo_W_HOOKSHOT_e);
    if (checkpoint == Checkpoint::skyClawshots) selectX(SLOT_10);
    if (checkpoint >= Checkpoint::mirrorComplete) {
        bossCleared(dStage_SaveTbl_LV7);
        mark({Flag::F_0268});
        execItemGet(dItemNo_MIRROR_PIECE_4_e);
    }
    if (checkpoint >= Checkpoint::palace) mark({Flag::F_0354});
    if (checkpoint >= Checkpoint::palaceLight) execItemGet(dItemNo_LIGHT_SWORD_e);
    if (checkpoint >= Checkpoint::castle) {
        bossCleared(dStage_SaveTbl_LV8);
        mark({Flag::F_0570, Flag::F_0686, Flag::F_0339});
    }
    // B_gnd reads dungeon switch 1 to start its original on-foot rematch.
    // dStage initializes the same (LV9) dungeon bank when it loads the arena.
    if (checkpoint == Checkpoint::ganonDuel) {
        dComIfGs_initDan(dStage_SaveTbl_LV9);
        dComIfGs_onSaveDunSwitch(1);
    }
}

}
