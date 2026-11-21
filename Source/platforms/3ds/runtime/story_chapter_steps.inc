// Spawn points, scene exits, actor names and named events come from the disc's
// PLYR, SCLS, ACTR and REVT records. An empty anchor appends after Castle Town.
struct ChapterEntry {
    std::string_view before;
    Step step;
};

inline constexpr ChapterEntry chapterEntries[]{
    {"ordon-woods-gate", {"ordon-ranch-ride", Checkpoint::ranch, {"F_SP00",0,99,6}, false, Script::walk, 120,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::horse}}},
    // Room 0's exit 2 enters point 22 on layer 7, which selects Demo06_01.
    {"faron-field", {"castle-prison-midna", Checkpoint::prison, {"R_SP107",0,22,7}, true, Script::cutscene, 900,
        0,0,{"R_SP107",0,2,-1},3,{ChapterGoal::event,"demo06_01"}}},
    {"faron-field", {"castle-sewers-senses", Checkpoint::sewers, {"R_SP107",1,2,14}, true, Script::equipment, 150,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::wolfSense}}},
    {"faron-field", {"castle-sewers-stairs", Checkpoint::sewers, {"R_SP107",2,1,14}, true, Script::route, 200,
        9950,16900,{"R_SP107",3,0,-1},1,{ChapterGoal::exit},RoutePoint{10945,16925}}},
    {"faron-field", {"zelda-first-meeting", Checkpoint::sewers, {"R_SP301",0,20,8}, true, Script::cutscene, 1350,
        0,0,{"F_SP116",0,20,8},1,{ChapterGoal::event,"demo07_02"}}},
    {"faron-field", {"forest-temple", Checkpoint::forestTemple, {"D_MN05",0,0,0}, false, Script::walk, 100,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"faron-field", {"forest-temple-ook", Checkpoint::forestTemple, {"D_MN05B",51,0,0}, false, Script::fight, 420,
        0,-5025,{},-1,{ChapterGoal::bossIntro,"E_mk"}}},
    {"faron-field", {"forest-temple-boomerang", Checkpoint::forestBoomerang, {"D_MN05",1,0,0}, false, Script::equipment, 180,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::boomerang}}},
    {"faron-field", {"forest-temple-diababa", Checkpoint::forestBoomerang, {"D_MN05A",50,0,0}, false, Script::fight, 420,
        150,2150,{},-1,{ChapterGoal::bossIntro,"B_bq"}}},
    {"eldin-field", {"kakariko-spirit-restored", Checkpoint::kakariko, {"F_SP109",0,30,8}, false, Script::cutscene, 1500,
        0,0,{"F_SP109",0,32,9},33,{ChapterGoal::event,"demo14"}}},
    {"eldin-field", {"death-mountain-path", Checkpoint::mines, {"F_SP110",0,0,0}, false, Script::walk, 105,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"eldin-field", {"goron-mines-iron-boots", Checkpoint::mines, {"D_MN04",1,0,0}, false, Script::equipment, 150,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::ironBoots}}},
    {"eldin-field", {"goron-mines-dangoro", Checkpoint::mines, {"D_MN04B",51,0,0}, false, Script::fight, 420,
        0,0,{},-1,{ChapterGoal::bossIntro,"E_gob",Ability::none,IntroInput::dangoro}}},
    {"eldin-field", {"goron-mines-bow", Checkpoint::minesBow, {"D_MN04",1,0,0}, false, Script::equipment, 200,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::bow}}},
    {"eldin-field", {"goron-mines-fyrus", Checkpoint::minesBow, {"D_MN04A",50,0,0}, false, Script::fight, 420,
        0,1475,{},-1,{ChapterGoal::bossIntro,"E_fm"}}},
    {"castle-town-west-gate", {"midna-warp-to-castle-west", Checkpoint::lanayruWestOpened, {"F_SP121",10,3,-1}, true, Script::warp, 850,
        0,0,{"F_SP122",8,playerWarpPoint,-1},-1,{},std::nullopt,castleWestPortal,castleWestWarpSpawn}},
    {"castle-town-west-gate", {"lake-hylia-twilight", Checkpoint::zoraIce, {"F_SP115",0,5,14}, true, Script::cutscene, 900,
        0,0,{},-1,{ChapterGoal::event,"h_lake_info"}}},
    {"castle-town-west-gate", {"zoras-domain-thaw", Checkpoint::zoraIce, {"F_SP113",0,50,8}, true, Script::cutscene, 1200,
        0,0,{"F_SP113",1,98,8},5,{ChapterGoal::event,"demo34"}}},
    // demo18.stb contains 7,670 camera ticks and 7,679 message-track ticks,
    // before player-paced dialogue. Stop immediately on the authored exit.
    {"castle-town-west-gate", {"lanayru-spirit-restored", Checkpoint::lanayruSpirit, {"F_SP115",1,20,8}, false, Script::cutscene, 12000,
        0,0,{"F_SP115",1,1,-1},1,{ChapterGoal::event,"demo18"}}},
    {"", {"kakariko-escort-arrival", Checkpoint::escort, {"F_SP109",0,67,13}, false, Script::cutscene, 1100,
        0,0,{"F_SP109",0,68,4},32,{ChapterGoal::event,"demo35_02"}}},
    {"", {"lake-hylia-zora-swimming", Checkpoint::lakebed, {"F_SP115",0,11,0}, false, Script::equipment, 210,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::zoraSwim}}},
    {"", {"lakebed-temple", Checkpoint::lakebed, {"D_MN01",0,0,0}, false, Script::equipment, 210,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::zoraSwim}}},
    // The room's own return point faces the ceiling encounter from the arena.
    // Point 2 leaves the pull-switch gate between Link and the net trigger.
    {"", {"lakebed-deku-toad", Checkpoint::lakebed, {"D_MN01B",51,3,0}, false, Script::fight, 420,
        0,1400,{},-1,{ChapterGoal::bossIntro,"E_dt",Ability::none,IntroInput::dekuToad}}},
    {"", {"lakebed-clawshot", Checkpoint::lakebedClawshot, {"D_MN01",1,0,0}, false, Script::equipment, 190,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::clawshot}}},
    {"", {"lakebed-morpheel", Checkpoint::lakebedClawshot, {"D_MN01A",50,0,0}, false, Script::fight, 1500,
        0,0,{},-1,{ChapterGoal::bossIntro,"B_ob",Ability::zoraSwim}}},
    {"", {"lake-hylia-zant-ambush", Checkpoint::desperateHour, {"F_SP115",1,21,9}, false, Script::cutscene, 1400,
        0,0,{"F_SP121",10,20,11},2,{ChapterGoal::event,"demo20_01"}}},
    {"", {"zelda-revives-midna", Checkpoint::zeldaReunion, {"R_SP107",3,23,9}, true, Script::cutscene, 1650,
        0,0,{"F_SP122",8,100,9},8,{ChapterGoal::event,"demo21"}}},
    {"", {"sacred-grove-master-sword", Checkpoint::grove, {"F_SP117",1,99,9}, false, Script::cutscene, 1600,
        0,0,{"F_SP117",1,150,0},0,{ChapterGoal::event,"demo22_02"}}},
    {"", {"gerudo-desert-arrival", Checkpoint::desert, {"F_SP124",0,0,8}, false, Script::cutscene, 900,
        0,0,{"D_MN08",0,22,8},3,{ChapterGoal::event,"demo23_01"}}},
    {"", {"gerudo-desert-to-camp", Checkpoint::desert, {"F_SP124",0,2,0}, false, Script::route, 165,
        2278,11192,{"F_SP118",1,0,-1},0,{ChapterGoal::exit}}},
    {"", {"bulblin-camp-skirmish", Checkpoint::desert, {"F_SP118",1,1,0}, false, Script::fight, 420,
        4470,-2950,{},-1,{ChapterGoal::hit,"E_rd"}}},
    // In room 1 E_wb waits for the gate and introduces the king. In room 2
    // the same boar starts in its already-defeated state.
    {"", {"bulblin-camp-king-bulblin", Checkpoint::bulblinCampKey, {"F_SP118",1,1,0}, false, Script::fight, 1050,
        1800,900,{},-1,{ChapterGoal::bossIntro,"E_rdb",Ability::none,IntroInput::kingBulblin}}},
    {"", {"bulblin-camp-to-arbiter", Checkpoint::arbiter, {"F_SP118",3,3,0}, false, Script::route, 240,
        1814,-13327,{"D_MN10",0,0,-1},0,{ChapterGoal::exit}}},
    {"", {"arbiters-grounds", Checkpoint::arbiter, {"D_MN10",0,1,0}, false, Script::walk, 130,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"", {"arbiters-grounds-death-sword", Checkpoint::arbiter, {"D_MN10B",51,0,0}, false, Script::fight, 470,
        0,0,{},-1,{ChapterGoal::bossIntro,"E_vt"}}},
    {"", {"arbiters-grounds-spinner", Checkpoint::arbiterSpinner, {"D_MN10",1,0,0}, false, Script::equipment, 180,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::spinner}}},
    {"", {"arbiters-grounds-stallord", Checkpoint::arbiterSpinner, {"D_MN10A",50,0,0}, false, Script::fight, 450,
        0,3600,{},-1,{ChapterGoal::bossIntro,"B_ds"}}},
    {"", {"mirror-chamber-sages", Checkpoint::mirrorShard, {"F_SP125",4,52,8}, false, Script::cutscene, 1500,
        0,0,{"F_SP102",0,53,9},2,{ChapterGoal::event,"demo32_02"}}},
    {"", {"snowpeak-pass", Checkpoint::snowpeak, {"F_SP114",0,4,0}, true, Script::walk, 120,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"", {"snowpeak-ruins", Checkpoint::snowpeak, {"D_MN11",0,0,0}, false, Script::walk, 160,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"", {"snowpeak-darkhammer", Checkpoint::snowpeak, {"D_MN11B",51,0,0}, false, Script::fight, 430,
        0,-1450,{},-1,{ChapterGoal::bossIntro,"E_th",Ability::none,IntroInput::walkToTrigger}}},
    {"", {"snowpeak-yeta-room", Checkpoint::snowpeakBall, {"D_MN11",1,0,0}, false, Script::walk, 120,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"", {"snowpeak-ball-and-chain", Checkpoint::snowpeakBall, {"D_MN11",3,0,0}, false, Script::equipment, 190,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::ironBall}}},
    {"", {"snowpeak-blizzeta", Checkpoint::snowpeakBall, {"D_MN11A",50,0,0}, false, Script::fight, 500,
        0,1040,{},-1,{ChapterGoal::bossIntro,"B_yo"}}},
    {"", {"sacred-grove-to-temple-of-time", Checkpoint::timeTemple, {"F_SP117",2,1,2}, false, Script::route, 230,
        0,-10762,{"D_MN06",0,0,-1},1,{ChapterGoal::exit}}},
    {"", {"temple-of-time", Checkpoint::timeTemple, {"D_MN06",0,0,0}, false, Script::walk, 120,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"", {"temple-of-time-darknut", Checkpoint::timeTemple, {"D_MN06B",51,0,0}, false, Script::fight, 550,
        0,1300,{},-1,{ChapterGoal::hit,"B_tn"}}},
    {"", {"temple-of-time-dominion-rod", Checkpoint::timeRod, {"D_MN06",1,0,0}, false, Script::equipment, 190,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::dominionRod}}},
    {"", {"temple-of-time-armogohma", Checkpoint::timeRod, {"D_MN06A",50,0,0}, false, Script::fight, 500,
        0,2800,{},-1,{ChapterGoal::bossIntro,"B_gm"}}},
    {"", {"hidden-village-skirmish", Checkpoint::hiddenVillage, {"F_SP128",0,2,0}, false, Script::fight, 900,
        3400,-7700,{},-1,{ChapterGoal::hit,"E_rd",Ability::bow}}},
    {"", {"city-in-the-sky", Checkpoint::sky, {"D_MN07",0,3,0}, false, Script::walk, 120,
        0,0,{},-1,{ChapterGoal::movement}}},
    // Point 2 resumes the battle after its film. Point 0 is the upper entrance.
    // The upper entrance and the fan have their own cameras before the opening.
    {"", {"city-in-the-sky-aeralfos", Checkpoint::skyAeralfos, {"D_MN07B",51,0,0}, false, Script::fight, 820,
        0,850,{},-1,{ChapterGoal::bossIntro,"B_gg",Ability::none,IntroInput::aeralfos}}},
    {"", {"city-in-the-sky-double-clawshots", Checkpoint::skyClawshots, {"D_MN07",1,0,0}, false, Script::equipment, 180,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::doubleClawshots}}},
    {"", {"city-in-the-sky-argorok", Checkpoint::skyClawshots, {"D_MN07A",50,2,0}, false, Script::fight, 530,
        -790,3100,{},-1,{ChapterGoal::bossIntro,"B_dr",Ability::none,IntroInput::argorok}}},
    {"", {"mirror-chamber-restored", Checkpoint::mirrorComplete, {"F_SP125",4,57,12}, false, Script::cutscene, 1450,
        0,0,{"F_SP125",4,7,0},7,{ChapterGoal::event,"demo24_04"}}},
    {"", {"palace-of-twilight", Checkpoint::palace, {"D_MN08",0,1,0}, false, Script::walk, 120,
        0,0,{},-1,{ChapterGoal::movement}}},
    // Leave time for the entrance events and the walk from z=2400
    // to the original opening trigger at z=-1500.
    {"", {"palace-phantom-zant-east", Checkpoint::palace, {"D_MN08B",51,0,0}, false, Script::fight, 1050,
        0,-1900,{},-1,{ChapterGoal::bossIntro,"E_pz",Ability::none,IntroInput::phantomZant}}},
    {"", {"palace-phantom-zant-west", Checkpoint::palace, {"D_MN08C",52,0,0}, false, Script::fight, 1050,
        0,-1900,{},-1,{ChapterGoal::bossIntro,"E_pz",Ability::none,IntroInput::phantomZant}}},
    {"", {"palace-light-infused-master-sword", Checkpoint::palaceLight, {"D_MN08",0,1,0}, false, Script::equipment, 180,
        0,0,{},-1,{ChapterGoal::ability,nullptr,Ability::lightSword}}},
    {"", {"palace-zant", Checkpoint::palaceLight, {"D_MN08D",50,20,0}, false, Script::fight, 550,
        144,1750,{},-1,{ChapterGoal::bossIntro,"B_zant",Ability::none,IntroInput::standard,53}}},
    {"", {"hyrule-castle", Checkpoint::castle, {"D_MN09",1,1,1}, false, Script::walk, 145,
        0,0,{},-1,{ChapterGoal::movement}}},
    {"", {"hyrule-castle-darknut", Checkpoint::castle, {"D_MN09",2,0,1}, false, Script::fight, 530,
        25,-3900,{},-1,{ChapterGoal::hit,"B_tn"}}},
    {"", {"hyrule-castle-puppet-zelda", Checkpoint::castle, {"D_MN09A",50,20,8}, false, Script::cutscene, 1100,
        0,0,{"D_MN09A",50,1,0},4,{ChapterGoal::event,"demo27_01"}}},
    {"", {"hyrule-castle-dark-beast", Checkpoint::castle, {"D_MN09A",50,22,10}, false, Script::cutscene, 1100,
        0,0,{"D_MN09A",50,2,1},5,{ChapterGoal::event,"demo27_02"}}},
    {"", {"ganondorf-light-arrows", Checkpoint::ganonHorse, {"D_MN09C",0,20,8}, false, Script::cutscene, 1000,
        0,0,{"R_SP300",0,20,8},0,{ChapterGoal::event,"demo28_02"}}},
    {"", {"ganondorf-horseback", Checkpoint::ganonHorse, {"D_MN09B",0,0,0}, false, Script::cutscene, 900,
        0,0,{},-1,{ChapterGoal::event,"r00_start"}}},
    {"", {"ganondorf-final-duel", Checkpoint::ganonDuel, {"D_MN09B",0,1,0}, false, Script::fight, 4800,
        0,0,{"D_MN09C",0,22,10},0,{ChapterGoal::duel,"B_gnd"}}},
};

// Preserve the early definitions verbatim while validating every insertion
// anchor and emitting one itinerary for playback, reporting and direct boot.
static_assert([] {
    for (const auto& chapter : chapterEntries) {
        unsigned matches = 0;
        for (const auto& early : earlySteps)
            if (chapter.before == early.id) ++matches;
        if (!chapter.before.empty() && matches != 1) return false;
    }
    return true;
}(), "Invalid early story anchor");

inline constexpr auto steps = [] {
    std::array<Step, std::size(earlySteps) + std::size(chapterEntries)> itinerary{};
    unsigned at = 0;
    for (const auto& early : earlySteps) {
        for (const auto& chapter : chapterEntries)
            if (chapter.before == early.id) itinerary[at++] = chapter.step;
        itinerary[at++] = early;
    }
    for (const auto& chapter : chapterEntries)
        if (chapter.before.empty()) itinerary[at++] = chapter.step;
    return itinerary;
}();

static_assert([] {
    for (unsigned i = 0; i < steps.size(); ++i) {
        for (unsigned j = 0; j < i; ++j)
            if (std::string_view(steps[i].id) == steps[j].id) return false;
    }
    return true;
}(), "Duplicate story step");
