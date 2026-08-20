#include "catch_amalgamated.hpp"

#include "AppHdr.h"

#include "ability-type.h"
#include "branch.h"
#include "cloud.h"
#include "dgn-overview.h"
#include "directn.h"
#include "dungeon.h"
#include "env.h"
#include "feature.h"
#include "files.h"
#include "housing.h"
#include "item-status-flag-type.h"
#include "items.h"
#include "jobs.h"
#include "losparam.h"
#include "mapmark.h"
#include "menu.h"
#include "mgen-data.h"
#include "mon-death.h"
#include "mon-place.h"
#include "mon-util.h"
#include "monster.h"
#include "notes.h"
#include "options.h"
#include "package.h"
#include "player.h"
#include "state.h"
#include "status.h"
#include "tags.h"
#include "shopping.h"
#include "unwind.h"
#include "terrain.h"
#include "tile-env.h"
#include "viewgeom.h"
#include "wizard.h"
#include "zot.h"

extern map<level_id, string> level_uniques;
extern set<pair<string, level_id>> auto_unique_annotations;

#ifndef TARGET_OS_WINDOWS
#include <sys/stat.h>

#include "syscalls.h"
#endif

TEST_CASE("Housing Delvers start on the single Housing floor",
          "[single-file]")
{
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<job_type> saved_job(you.char_class, JOB_DELVER);

    REQUIRE(starting_absdepth() == 0);

    crawl_state.type = GAME_TYPE_NORMAL;
    REQUIRE(starting_absdepth() == 4);
}

TEST_CASE("Legacy Housing Delver depth is normalized narrowly",
          "[single-file]")
{
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<job_type> saved_job(you.char_class, JOB_DELVER);
    unwind_var<branch_type> saved_branch(you.where_are_you, BRANCH_DUNGEON);
    unwind_var<int> saved_depth(you.depth, 5);
    unwind_var<int> saved_dungeon_depth(brdepth[BRANCH_DUNGEON], 1);

    housing_normalize_legacy_delver_depth();
    REQUIRE(you.depth == 1);

    // Do not turn the hook into a general corrupted-save repair path.
    you.depth = 4;
    housing_normalize_legacy_delver_depth();
    REQUIRE(you.depth == 4);

    you.depth = 5;
    you.char_class = JOB_FIGHTER;
    housing_normalize_legacy_delver_depth();
    REQUIRE(you.depth == 5);

    you.char_class = JOB_DELVER;
    crawl_state.type = GAME_TYPE_NORMAL;
    housing_normalize_legacy_delver_depth();
    REQUIRE(you.depth == 5);
}

TEST_CASE("Housing map target validation is strict ASCII", "[single-file]")
{
    REQUIRE(housing_valid_map_id("_"));
    REQUIRE(housing_valid_map_id("this_is_test_MAP07"));
    REQUIRE(housing_valid_map_id("12345678901234567890"));
    REQUIRE_FALSE(housing_valid_map_id(""));
    REQUIRE_FALSE(housing_valid_map_id("map-name"));
    REQUIRE_FALSE(housing_valid_map_id("123456789012345678901"));

    REQUIRE(housing_valid_map_target("ASCIIPhilia:this_is_test_MAP07"));
    REQUIRE(housing_valid_map_target("abc:_"));
    REQUIRE(housing_valid_map_target("Account123:12345678901234567890"));

    REQUIRE_FALSE(housing_valid_map_target("ab:main"));
    REQUIRE_FALSE(housing_valid_map_target("abc:"));
    REQUIRE_FALSE(housing_valid_map_target("abc:main:extra"));
    REQUIRE_FALSE(housing_valid_map_target("abc:../main"));
    REQUIRE_FALSE(housing_valid_map_target("abc:map-name"));
    REQUIRE_FALSE(housing_valid_map_target("abc:mäp"));
    REQUIRE_FALSE(housing_valid_map_target("abc:123456789012345678901"));
}

TEST_CASE("Housing-created monsters are inert only for their owner",
          "[single-file]")
{
    monster placed;
    placed.type = MONS_RAT;
    placed.hit_points = placed.max_hit_points = 1;
    placed.props["housing_created_monster"] = true;
    REQUIRE(housing_monster_was_created(placed));

    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    REQUIRE(housing_monster_is_owner_inert(placed));

    crawl_state.type = GAME_TYPE_NORMAL;
    REQUIRE_FALSE(housing_monster_is_owner_inert(placed));

    crawl_state.type = GAME_TYPE_HOUSING;
    placed.props.erase("housing_created_monster");
    REQUIRE_FALSE(housing_monster_was_created(placed));
    REQUIRE_FALSE(housing_monster_is_owner_inert(placed));
}

TEST_CASE("Housing disables explore mode but preserves wizard mode",
          "[single-file]")
{
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<wizard_option_type> saved_wiz_option(Options.wiz_mode,
                                                     WIZ_YES);
    unwind_var<wizard_option_type> saved_explore_option(Options.explore_mode,
                                                         WIZ_YES);
    unwind_var<bool> saved_wizard(you.wizard, true);
    unwind_var<bool> saved_suppress(you.suppress_wizard, true);
    unwind_var<bool> saved_explore(you.explore, true);
    unwind_var<bool> saved_wizard_vision(you.wizard_vision, true);

    housing_enforce_explore_mode();
    REQUIRE(Options.wiz_mode == WIZ_YES);
    REQUIRE(Options.explore_mode == WIZ_NEVER);
    REQUIRE(you.wizard);
    REQUIRE(you.suppress_wizard);
    REQUIRE_FALSE(you.explore);
    REQUIRE(you.wizard_vision);

#ifdef WIZARD
    you.explore = true;
    enter_explore_mode();
    REQUIRE_FALSE(you.explore);
    REQUIRE(you.wizard);
#endif

    crawl_state.type = GAME_TYPE_NORMAL;
    Options.explore_mode = WIZ_YES;
    you.explore = true;
    housing_enforce_explore_mode();
    REQUIRE(Options.explore_mode == WIZ_YES);
    REQUIRE(you.explore);
}

TEST_CASE("Housing has no Zot clock", "[single-file]")
{
    const CrawlHashTable saved_properties = you.props;
    unwinder restore_properties = [saved_properties]() {
        you.props = saved_properties;
    };
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<branch_type> saved_branch(you.where_are_you, BRANCH_DUNGEON);
    unwind_var<int> saved_time_taken(you.time_taken, BASELINE_DELAY);
    unwind_var<bool> saved_show_zot(Options.always_show_zot, true);
    unwind_var<int> saved_zigs_completed(you.zigs_completed, 0);
    unwind_var<game_chapter> saved_chapter(you.chapter,
                                           CHAPTER_ORB_HUNTING);
    unwind_var<god_type> saved_religion(you.religion, GOD_NO_GOD);

    you.props.erase("ZOT_AUTS");
    REQUIRE_FALSE(zot_clock_active());
    REQUIRE(bezotting_level() == 0);
    REQUIRE_FALSE(bezotted());
    REQUIRE_FALSE(should_fear_zot());

    incr_zot_clock();
    decr_zot_clock();
    set_turns_until_zot(1);
    REQUIRE_FALSE(you.props.exists("ZOT_AUTS"));

    // A clock persisted by an old Housing build remains inert rather than
    // being deleted or silently reset.
    crawl_state.type = GAME_TYPE_NORMAL;
    set_turns_until_zot(1234);
    REQUIRE(you.props.exists("ZOT_AUTS"));
    REQUIRE(turns_until_zot() == 1234);
    REQUIRE(zot_clock_active());
    incr_zot_clock();
    REQUIRE(turns_until_zot() == 1233);
    set_turns_until_zot(1234);
    crawl_state.type = GAME_TYPE_HOUSING;
    REQUIRE(gem_clock_active());
    incr_zot_clock();
    decr_zot_clock();
    set_turns_until_zot(1);
    REQUIRE(turns_until_zot() == 1234);

    status_info zot_status;
    fill_status_info(STATUS_ZOT, zot_status);
    REQUIRE(zot_status.light_text.empty());
    REQUIRE(zot_status.short_text.empty());
    REQUIRE(zot_status.long_text.empty());
}

TEST_CASE("Housing branch theme ids and menu order are stable",
          "[single-file]")
{
    REQUIRE(housing_branch_theme_count() == 37);
    REQUIRE(housing_branch_theme_catalog_valid());

    // These ids already exist in canonical multi-map saves and are therefore
    // append-only even though the display order follows Crawl's branch order.
    const char *legacy[] = {
        "Dungeon", "Lair of Beasts", "Orcish Mines", "Swamp", "Vaults",
        "Crypt", "Depths", "Realm of Zot",
    };
    for (int id = 0; id < static_cast<int>(ARRAYSZ(legacy)); ++id)
        REQUIRE(string(housing_branch_theme_name(id)) == legacy[id]);

    const int expected_order[] = {
        0, 8, 1, 3, 10, 11, 12, 13, 2, 9, 4, 5, 14, 6, 15, 16, 17,
        18, 19, 7, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36,
    };
    for (int position = 0;
         position < static_cast<int>(ARRAYSZ(expected_order)); ++position)
    {
        REQUIRE(housing_branch_theme_menu_id(position)
                == expected_order[position]);
    }
    REQUIRE(housing_branch_theme_menu_id(-1) == -1);
    REQUIRE(housing_branch_theme_menu_id(ARRAYSZ(expected_order)) == -1);
    REQUIRE(housing_branch_theme_name(-1) == nullptr);
    REQUIRE(housing_branch_theme_name(housing_branch_theme_count()) == nullptr);

    menu_letter hotkey('a');
    string hotkeys;
    for (int i = 0; i < housing_branch_theme_count(); ++i)
        hotkeys += static_cast<char>(hotkey++);
    REQUIRE(hotkeys == "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK");
}

TEST_CASE("Housing terrain permits decorative hazards and altars safely",
          "[single-file]")
{
    REQUIRE(housing_feature_allowed(DNGN_SHALLOW_WATER));
    REQUIRE(housing_feature_allowed(DNGN_DEEP_WATER));
    REQUIRE(housing_feature_allowed(DNGN_LAVA));
    REQUIRE(housing_feature_allowed(DNGN_ALTAR_ZIN));
    REQUIRE(housing_feature_allowed(DNGN_SLIMY_WALL));
    REQUIRE(housing_feature_allowed(DNGN_GRATE));
    REQUIRE(housing_feature_allowed(DNGN_STONE_ARCH));
    REQUIRE(housing_feature_allowed(DNGN_EXPIRED_PORTAL));

    // A runelight is reserved for an authenticated Housing spawn fixture;
    // generic terrain editing must not be able to forge one.
    REQUIRE_FALSE(housing_feature_allowed(DNGN_RUNELIGHT));
    REQUIRE_FALSE(housing_feature_allowed(DNGN_UNKNOWN_ALTAR));
    REQUIRE_FALSE(housing_feature_allowed(DNGN_TRAP_TELEPORT));
    REQUIRE_FALSE(housing_feature_allowed(DNGN_ENTER_LAIR));
    REQUIRE_FALSE(housing_feature_allowed(DNGN_ORB_DAIS));
    REQUIRE_FALSE(housing_feature_allowed(DNGN_MOULD_PATCH));
}

TEST_CASE("Housing monster policy rejects only unsafe actor payloads",
          "[single-file]")
{
    init_monsters();
    REQUIRE(housing_monster_type_allowed(MONS_RAT));
    REQUIRE(housing_monster_type_allowed(MONS_PLANT));
    REQUIRE(housing_monster_type_allowed(MONS_SIGMUND));

    REQUIRE_FALSE(housing_monster_type_allowed(MONS_PROGRAM_BUG));
    REQUIRE_FALSE(housing_monster_type_allowed(MONS_TEST_SPAWNER));
    REQUIRE_FALSE(housing_monster_type_allowed(MONS_ZOMBIE));
}

TEST_CASE("Housing generation preserves unique creature history",
          "[single-file]")
{
    init_show_table();
    init_monsters();
    const coord_def player_pos(20, 20);
    const coord_def first_pos(24, 20);
    const coord_def second_pos(25, 20);
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<bool> saved_on_level(you.on_current_level, true);
    unwind_var<coord_def> saved_player_pos(you.position, player_pos);
    unwind_var<unique_creature_list> saved_uniques(you.unique_creatures);
    const auto saved_unique_items = you.unique_items;
    unwinder restore_unique_items = [saved_unique_items]() {
        you.unique_items = saved_unique_items;
    };
    const auto saved_level_uniques = level_uniques;
    const auto saved_auto_unique_annotations = auto_unique_annotations;
    unwinder restore_unique_annotations = [saved_level_uniques,
                                            saved_auto_unique_annotations]() {
        level_uniques = saved_level_uniques;
        auto_unique_annotations = saved_auto_unique_annotations;
    };
    unwind_var<dungeon_feature_type> saved_first_feat(env.grid(first_pos),
                                                       DNGN_FLOOR);
    unwind_var<dungeon_feature_type> saved_second_feat(env.grid(second_pos),
                                                        DNGN_FLOOR);
    unwind_var<unsigned short> saved_first_mon(env.mgrid(first_pos),
                                                NON_MONSTER);
    unwind_var<unsigned short> saved_second_mon(env.mgrid(second_pos),
                                                 NON_MONSTER);
    unwind_var<int> saved_first_item(env.igrid(first_pos), NON_ITEM);
    unwind_var<int> saved_second_item(env.igrid(second_pos), NON_ITEM);

    REQUIRE(env.grid(first_pos) == DNGN_FLOOR);
    REQUIRE(env.grid(second_pos) == DNGN_FLOOR);
    REQUIRE(mons_class_can_pass(MONS_MAGGIE, env.grid(first_pos)));
    REQUIRE(mons_class_can_pass(MONS_MARGERY, env.grid(second_pos)));
    REQUIRE(monster_at(first_pos) == nullptr);
    REQUIRE(monster_at(second_pos) == nullptr);

    vector<monster*> generated;
    unwinder remove_generated = [&generated]() {
        for (monster *mons : generated)
            if (mons && mons->alive())
                monster_die(*mons, KILL_RESET, NON_MONSTER, true, false,
                            true);
    };

    auto unique_history = []() {
        vector<bool> result;
        result.reserve(NUM_MONSTERS);
        for (int type = 0; type < NUM_MONSTERS; ++type)
            result.push_back(you.unique_creatures[type]);
        return result;
    };
    auto place_unique = [&](monster_type type, const coord_def &pos,
                            bool ignore_unique_status) {
        mgen_flags flags = MG_FORBID_BANDS | MG_FORCE_PLACE;
        if (ignore_unique_status)
            flags |= MG_IGNORE_UNIQUE_STATUS;
        mgen_data mg(type, BEH_HOSTILE, pos, MHITYOU, flags);
        monster *created = create_monster(mg);
        REQUIRE(created != nullptr);
        generated.push_back(created);
        return created;
    };

    SECTION("fresh history permits both ages and respawning")
    {
        you.unique_creatures.reset();
        you.unique_creatures.set(MONS_SIGMUND);
        const vector<bool> expected = unique_history();

        monster *maggie = place_unique(MONS_MAGGIE, first_pos, true);
        REQUIRE(unique_history() == expected);
        REQUIRE(place_unique(MONS_MARGERY, second_pos, true) != nullptr);
        REQUIRE(unique_history() == expected);

        monster_die(*maggie, KILL_RESET, NON_MONSTER, true, false, true);
        REQUIRE_FALSE(maggie->alive());
        REQUIRE(place_unique(MONS_MAGGIE, first_pos, true) != nullptr);
        REQUIRE(unique_history() == expected);
    }

    SECTION("legacy paired history remains bit-identical")
    {
        you.unique_creatures.reset();
        you.unique_creatures.set(MONS_MAGGIE);
        you.unique_creatures.set(MONS_SIGMUND);
        const vector<bool> expected = unique_history();
        REQUIRE(you.unique_creatures[MONS_MAGGIE]);
        REQUIRE(you.unique_creatures[MONS_MARGERY]);

        REQUIRE(place_unique(MONS_MAGGIE, first_pos, true) != nullptr);
        REQUIRE(place_unique(MONS_MARGERY, second_pos, true) != nullptr);
        REQUIRE(unique_history() == expected);
    }

    SECTION("normal generation still records the paired unique")
    {
        crawl_state.type = GAME_TYPE_NORMAL;
        you.unique_creatures.reset();
        REQUIRE(place_unique(MONS_MAGGIE, first_pos, false) != nullptr);
        REQUIRE(you.unique_creatures[MONS_MAGGIE]);
        REQUIRE(you.unique_creatures[MONS_MARGERY]);
    }
}

TEST_CASE("Housing rider defeat paths do not synthesize persistent history",
          "[single-file]")
{
    init_show_table();
    init_monsters();
    const coord_def player_pos(20, 20);
    const coord_def rider_pos(24, 20);
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<bool> saved_need_save(crawl_state.need_save, true);
    unwind_var<bool> saved_on_level(you.on_current_level, true);
    unwind_var<coord_def> saved_player_pos(you.position, player_pos);
    unwind_var<branch_type> saved_branch(you.where_are_you, BRANCH_DUNGEON);
    unwind_var<int> saved_depth(you.depth, 1);
    unwind_var<unique_creature_list> saved_uniques(you.unique_creatures);
    unwind_var<KillMaster> saved_kills(you.kills, KillMaster());
    unwind_var<vector<text_pattern>> saved_note_monsters(
        Options.note_monsters,
        vector<text_pattern>{ text_pattern("ghost moth"),
                              text_pattern("Goji") });
    unwind_var<dungeon_feature_type> saved_feat(env.grid(rider_pos),
                                                 DNGN_FLOOR);
    unwind_var<unsigned short> saved_mon(env.mgrid(rider_pos), NON_MONSTER);
    unwind_var<int> saved_item(env.igrid(rider_pos), NON_ITEM);
    const auto saved_unique_items = you.unique_items;
    unwinder restore_unique_items = [saved_unique_items]() {
        you.unique_items = saved_unique_items;
    };
    const CrawlHashTable saved_properties = you.props;
    unwinder restore_properties = [saved_properties]() {
        you.props = saved_properties;
    };
    const vector<Note> saved_notes = note_list;
    const bool saved_notes_active = notes_are_active();
    unwinder restore_notes = [saved_notes, saved_notes_active]() {
        note_list = saved_notes;
        activate_notes(saved_notes_active);
    };
    const auto saved_level_uniques = level_uniques;
    const auto saved_auto_unique_annotations = auto_unique_annotations;
    unwinder restore_unique_annotations = [saved_level_uniques,
                                            saved_auto_unique_annotations]() {
        level_uniques = saved_level_uniques;
        auto_unique_annotations = saved_auto_unique_annotations;
    };

    REQUIRE(env.grid(rider_pos) == DNGN_FLOOR);
    REQUIRE(mons_class_can_pass(MONS_GOJI, env.grid(rider_pos)));
    REQUIRE(monster_at(rider_pos) == nullptr);

    you.unique_creatures.reset();
    you.unique_creatures.set(MONS_SIGMUND);
    auto unique_history = []() {
        vector<bool> result;
        result.reserve(NUM_MONSTERS);
        for (int type = 0; type < NUM_MONSTERS; ++type)
            result.push_back(you.unique_creatures[type]);
        return result;
    };
    const vector<bool> expected_uniques = unique_history();

    mgen_data mg(MONS_GOJI, BEH_HOSTILE, rider_pos, MHITYOU,
                 MG_FORBID_BANDS | MG_FORCE_PLACE
                 | MG_IGNORE_UNIQUE_STATUS);
    mg.extra_flags |= MF_NO_REWARD;
    monster *goji = create_monster(mg);
    REQUIRE(goji != nullptr);
    unwinder remove_goji = [&goji]() {
        if (goji && goji->alive())
            monster_die(*goji, KILL_RESET, NON_MONSTER, true, false, true);
    };
    REQUIRE(mons_is_rider(goji->type));
    REQUIRE(mons_mount_type(goji->type) == MONS_GHOST_MOTH);
    goji->props["housing_created_monster"] = true;
    goji->props[NEVER_CORPSE_KEY] = true;
    for (mon_inv_iterator item(*goji); item; ++item)
        if (!is_unrandom_artefact(*item))
            item->flags |= ISFLAG_SUMMONED;
    REQUIRE(housing_monster_was_created(*goji));
    REQUIRE(unique_history() == expected_uniques);

    note_list.clear();
    activate_notes(true);
    you.props["last_milestone"] = "housing rider sentinel";
    you.props["last_milestone_type"] = "housing.test";
    you.props["last_milestone_turn"] = you.num_turns - 1;

    auto require_history_unchanged = [&]() {
        REQUIRE(you.kills.empty());
        REQUIRE(note_list.empty());
        REQUIRE(you.props["last_milestone"].get_string()
                == "housing rider sentinel");
        REQUIRE(you.props["last_milestone_type"].get_string()
                == "housing.test");
        REQUIRE(you.props["last_milestone_turn"].get_int()
                == you.num_turns - 1);
        REQUIRE(unique_history() == expected_uniques);
    };

    SECTION("full death skips the synthetic mount death")
    {
        monster_die(*goji, KILL_YOU, NON_MONSTER, true, false, true);
        REQUIRE_FALSE(goji->alive());
        require_history_unchanged();
    }

    SECTION("mid-combat split skips the synthetic rider death")
    {
        const level_id level = level_id::current();
        const auto annotation = make_pair(string("Goji"), level);
        set_unique_annotation(goji, level);
        REQUIRE(auto_unique_annotations.count(annotation) == 1);
        REQUIRE(level_uniques.count(level) == 1);
        REQUIRE(level_uniques[level].find("Goji") != string::npos);

        const int starting_hp = goji->hit_points;
        REQUIRE(starting_hp == goji->max_hit_points);
        REQUIRE(starting_hp > 5);
        const int requested_damage = starting_hp * 3 / 5 + 1;
        REQUIRE(requested_damage > 0);
        REQUIRE(requested_damage < starting_hp);
        const int damage = goji->hurt(nullptr, requested_damage,
                                      BEAM_MMISSILE, KILLED_BY_SOMETHING,
                                      "", "", false, false);
        REQUIRE(damage > 0);
        REQUIRE(damage < starting_hp);
        REQUIRE(goji->alive());
        REQUIRE(goji->type == MONS_GHOST_MOTH);
        REQUIRE(housing_monster_was_created(*goji));
        REQUIRE(auto_unique_annotations.count(annotation) == 0);
        const auto remaining = level_uniques.find(level);
        REQUIRE((remaining == level_uniques.end()
                 || remaining->second.find("Goji") == string::npos));
        require_history_unchanged();
    }
}

TEST_CASE("Housing ability ids remain append-only", "[single-file]")
{
    REQUIRE(static_cast<int>(ABIL_HOUSING_RETURN_HOME) == 9005);
    REQUIRE(static_cast<int>(ABIL_HOUSING_MANAGE_MAPS) == 9006);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CREATE_MONSTER) == 9007);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CALL_MERCHANT) == 9008);
    REQUIRE(static_cast<int>(ABIL_HOUSING_TRAVEL_TO_MAP) == 9009);
    REQUIRE(static_cast<int>(ABIL_HOUSING_TOGGLE_VISITOR_WALL) == 9010);
    REQUIRE(static_cast<int>(ABIL_HOUSING_MANAGE_SPAWNS) == 9011);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CREATE_VISITOR_STRIP) == 9012);
    REQUIRE(static_cast<int>(ABIL_HOUSING_SHOW_COORDINATES) == 9013);
    REQUIRE(ABIL_LAST_HOUSING == ABIL_HOUSING_SHOW_COORDINATES);
}

TEST_CASE("Housing editor self-targeting rejects without cancelling",
          "[single-file]")
{
    REQUIRE(housing_editor_self_target_policy()
            == confirm_prompt_type::none);
}

TEST_CASE("Housing snapshot schema is explicit and backwards compatible",
          "[single-file]")
{
    REQUIRE(housing_snapshot_schema_version() == 5);
    REQUIRE(housing_snapshot_schema_supported(1));
    REQUIRE(housing_snapshot_schema_supported(2));
    REQUIRE(housing_snapshot_schema_supported(3));
    REQUIRE(housing_snapshot_schema_supported(4));
    REQUIRE(housing_snapshot_schema_supported(5));
    REQUIRE_FALSE(housing_snapshot_schema_supported(0));
    REQUIRE_FALSE(housing_snapshot_schema_supported(6));
    REQUIRE_FALSE(housing_snapshot_schema_supported(INT_MAX));
}

TEST_CASE("Housing restore can scrub a freshly reset shopping list",
          "[single-file]")
{
    const CrawlHashTable saved_properties = you.props;
    unwinder restore = [saved_properties]() {
        you.props = saved_properties;
        shopping_list.refresh();
    };
    you.props.erase("shopping_list_key");
    you.props.erase("shopping_list_cost_key");

    const level_id housing_level(BRANCH_DUNGEON, 1);
    const level_id other_level(BRANCH_DUNGEON, 2);

    SECTION("empty restored properties")
    {
        shopping_list = ShoppingList();
        shopping_list.del_things_from(housing_level);
        REQUIRE(shopping_list.empty());
        REQUIRE(you.props.exists("shopping_list_key"));
    }

    SECTION("populated restored properties")
    {
        CrawlVector &stored =
            you.props["shopping_list_key"].new_vector(SV_HASH,
                                                       SFLAG_CONST_TYPE);
        auto add_entry = [&](const level_id &level, int cost) {
            CrawlHashTable entry;
            entry["cost_key"] = cost;
            entry["pos_key"] = level_pos(level, coord_def(10, 10));
            entry["desc_key"] = string("test item");
            entry["verb_key"] = string("buy");
            stored.push_back(CrawlStoreValue(entry));
        };
        add_entry(housing_level, 10);
        add_entry(other_level, 20);

        shopping_list = ShoppingList();
        shopping_list.del_things_from(housing_level);
        REQUIRE(shopping_list.size() == 1);
        const CrawlVector &remaining =
            you.props["shopping_list_key"].get_vector();
        REQUIRE(remaining.size() == 1);
        REQUIRE(remaining[0].get_table()["pos_key"].get_level_pos().id
                == other_level);
    }
}

struct housing_wall_cell_fixture
{
    explicit housing_wall_cell_fixture(const coord_def &cell) : pos(cell)
    {
        feature = env.grid(pos);
        monster_index = env.mgrid(pos);
        item_index = env.igrid(pos);
        for (map_marker *marker : env.markers.get_markers_at(pos))
            saved_markers.push_back(marker->clone());
        env.markers.remove_markers_at(pos);
        env.grid(pos) = DNGN_CLEAR_PERMAROCK_WALL;
        env.mgrid(pos) = NON_MONSTER;
        env.igrid(pos) = NON_ITEM;
    }

    ~housing_wall_cell_fixture()
    {
        env.markers.remove_markers_at(pos);
        for (map_marker *marker : saved_markers)
            env.markers.add(marker);
        env.grid(pos) = feature;
        env.mgrid(pos) = monster_index;
        env.igrid(pos) = item_index;
    }

    map_wiz_props_marker *add_wall_marker(bool veto = true)
    {
        auto *marker = new map_wiz_props_marker(pos);
        marker->set_property("housing_visitor_wall", "yes");
        marker->set_property("feature_description", "owner-only barrier");
        if (veto)
            marker->set_property("veto_destroy", "veto");
        env.markers.add(marker);
        return marker;
    }

    coord_def pos;
    dungeon_feature_type feature;
    unsigned short monster_index;
    int item_index;
    vector<map_marker*> saved_markers;
};

TEST_CASE("Housing visitor wall requires exact marker and terrain pairing",
          "[single-file]")
{
    init_show_table();
    const coord_def pos(20, 20);
    housing_wall_cell_fixture cell(pos);
    REQUIRE_FALSE(housing_visitor_wall_is_valid(coord_def(-1, -1)));

    SECTION("new translucent wall opens to floor")
    {
        cell.add_wall_marker();
        REQUIRE(housing_visitor_wall_is_valid(pos));
        unwind_var<game_type> saved_game_type(crawl_state.type,
                                              GAME_TYPE_HOUSING);
        // The exact marker protects this generally editable terrain pair.
        REQUIRE(housing_feature_allowed(DNGN_CLEAR_PERMAROCK_WALL));
        REQUIRE(feat_is_solid(DNGN_CLEAR_PERMAROCK_WALL));
        REQUIRE(feat_is_wall(DNGN_CLEAR_PERMAROCK_WALL));
        REQUIRE(feat_is_permarock(DNGN_CLEAR_PERMAROCK_WALL));
        REQUIRE_FALSE(feat_is_diggable(DNGN_CLEAR_PERMAROCK_WALL));
        REQUIRE_FALSE(feat_is_opaque(DNGN_CLEAR_PERMAROCK_WALL));
        REQUIRE(opc_default(pos) == OPC_CLEAR);
        REQUIRE(opc_no_trans(pos) == OPC_OPAQUE);
        REQUIRE(opc_solid(pos) == OPC_OPAQUE);
        REQUIRE_FALSE(housing_can_edit(pos));
        housing_open_visitor_wall(pos);
        REQUIRE(env.grid(pos) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(pos).empty());
    }

    SECTION("legacy metal wall remains readable")
    {
        env.grid(pos) = DNGN_METAL_WALL;
        cell.add_wall_marker();
        REQUIRE(housing_visitor_wall_is_valid(pos));
        housing_open_visitor_wall(pos);
        REQUIRE(env.grid(pos) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(pos).empty());
    }

    SECTION("wrong terrain fails closed")
    {
        cell.add_wall_marker();
        env.grid(pos) = DNGN_STONE_WALL;
        REQUIRE_FALSE(housing_visitor_wall_is_valid(pos));
        REQUIRE_THROWS(housing_open_visitor_wall(pos));
    }

    SECTION("missing veto fails closed")
    {
        cell.add_wall_marker(false);
        REQUIRE_FALSE(housing_visitor_wall_is_valid(pos));
        REQUIRE_THROWS(housing_open_visitor_wall(pos));
    }

    SECTION("an extra marker fails closed")
    {
        cell.add_wall_marker();
        env.markers.add(new map_wiz_props_marker(pos));
        REQUIRE_FALSE(housing_visitor_wall_is_valid(pos));
        REQUIRE_THROWS(housing_open_visitor_wall(pos));
    }

    SECTION("a conflicting reserved marker role fails closed")
    {
        map_wiz_props_marker *marker = cell.add_wall_marker();
        marker->set_property("housing_spawn", "yes");
        REQUIRE_FALSE(housing_visitor_wall_is_valid(pos));
        REQUIRE_THROWS(housing_open_visitor_wall(pos));
    }

    SECTION("an extra wizard property fails closed")
    {
        map_wiz_props_marker *marker = cell.add_wall_marker();
        marker->set_property("post_init_remove", "yes");
        REQUIRE_FALSE(housing_visitor_wall_is_valid(pos));
        REQUIRE_THROWS(housing_open_visitor_wall(pos));
    }
}

TEST_CASE("Housing chargen adopts and starts on the visible template spawn",
          "[single-file]")
{
    init_show_table();
    const coord_def old_start(20, 20);
    const coord_def template_spawn(21, 20);
    const coord_def ambiguous_spawn(22, 20);
    const coord_def third_passage(23, 20);
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<bool> saved_on_level(you.on_current_level, true);
    unwinder restore_view = [&]() {
        crawl_view.set_player_at(you.position);
    };
    unwind_var<coord_def> saved_position(you.position, old_start);
    unwind_var<CrawlHashTable> saved_properties(env.properties);
    const auto saved_shops = env.shop;
    const auto saved_clouds = env.cloud;
    unwinder restore_publication_state = [saved_shops, saved_clouds]() {
        env.shop = saved_shops;
        env.cloud = saved_clouds;
    };
    env.shop.clear();
    env.cloud.clear();

    const dungeon_feature_type old_start_feat = env.grid(old_start);
    const dungeon_feature_type old_spawn_feat = env.grid(template_spawn);
    const dungeon_feature_type old_ambiguous_feat =
        env.grid(ambiguous_spawn);
    const dungeon_feature_type old_third_feat = env.grid(third_passage);
    const unsigned short old_start_monster = env.mgrid(old_start);
    const unsigned short old_spawn_monster = env.mgrid(template_spawn);
    const unsigned short old_ambiguous_monster =
        env.mgrid(ambiguous_spawn);
    const unsigned short old_third_monster = env.mgrid(third_passage);
    const int old_start_item = env.igrid(old_start);
    const int old_spawn_item = env.igrid(template_spawn);
    const int old_ambiguous_item = env.igrid(ambiguous_spawn);
    const int old_third_item = env.igrid(third_passage);
    const tile_flavour old_start_flavour = tile_env.flv(old_start);
    const tile_flavour old_spawn_flavour = tile_env.flv(template_spawn);
    const tile_flavour old_ambiguous_flavour =
        tile_env.flv(ambiguous_spawn);
    const tile_flavour old_third_flavour = tile_env.flv(third_passage);
    const vector<string> old_tile_names = tile_env.names;
    const map_cell old_start_knowledge = env.map_knowledge(old_start);
    const map_cell old_spawn_knowledge = env.map_knowledge(template_spawn);
    const map_cell old_ambiguous_knowledge =
        env.map_knowledge(ambiguous_spawn);
    const map_cell old_third_knowledge = env.map_knowledge(third_passage);
    const tileidx_t old_start_remembered =
        tile_env.remembered_flavour.feat_flavour(old_start);
    const tileidx_t old_spawn_remembered =
        tile_env.remembered_flavour.feat_flavour(template_spawn);
    const tileidx_t old_ambiguous_remembered =
        tile_env.remembered_flavour.feat_flavour(ambiguous_spawn);
    const tileidx_t old_third_remembered =
        tile_env.remembered_flavour.feat_flavour(third_passage);
    const unsigned short old_start_remembered_idx =
        tile_env.remembered_flavour.feat_flavour_idx(old_start);
    const unsigned short old_spawn_remembered_idx =
        tile_env.remembered_flavour.feat_flavour_idx(template_spawn);
    const unsigned short old_ambiguous_remembered_idx =
        tile_env.remembered_flavour.feat_flavour_idx(ambiguous_spawn);
    const unsigned short old_third_remembered_idx =
        tile_env.remembered_flavour.feat_flavour_idx(third_passage);
    const unsigned short old_start_colour = env.grid_colours(old_start);
    const unsigned short old_spawn_colour = env.grid_colours(template_spawn);
    const unsigned short old_ambiguous_colour =
        env.grid_colours(ambiguous_spawn);
    const unsigned short old_third_colour = env.grid_colours(third_passage);
    vector<map_marker*> old_start_markers;
    vector<map_marker*> old_spawn_markers;
    vector<map_marker*> old_ambiguous_markers;
    vector<map_marker*> old_third_markers;
    for (map_marker *marker : env.markers.get_markers_at(old_start))
        old_start_markers.push_back(marker->clone());
    for (map_marker *marker : env.markers.get_markers_at(template_spawn))
        old_spawn_markers.push_back(marker->clone());
    for (map_marker *marker : env.markers.get_markers_at(ambiguous_spawn))
        old_ambiguous_markers.push_back(marker->clone());
    for (map_marker *marker : env.markers.get_markers_at(third_passage))
        old_third_markers.push_back(marker->clone());
    unwinder restore_cells = [&]() {
        env.markers.remove_markers_at(old_start);
        env.markers.remove_markers_at(template_spawn);
        env.markers.remove_markers_at(ambiguous_spawn);
        env.markers.remove_markers_at(third_passage);
        for (map_marker *marker : old_start_markers)
            env.markers.add(marker);
        for (map_marker *marker : old_spawn_markers)
            env.markers.add(marker);
        for (map_marker *marker : old_ambiguous_markers)
            env.markers.add(marker);
        for (map_marker *marker : old_third_markers)
            env.markers.add(marker);
        env.grid(old_start) = old_start_feat;
        env.grid(template_spawn) = old_spawn_feat;
        env.grid(ambiguous_spawn) = old_ambiguous_feat;
        env.grid(third_passage) = old_third_feat;
        env.mgrid(old_start) = old_start_monster;
        env.mgrid(template_spawn) = old_spawn_monster;
        env.mgrid(ambiguous_spawn) = old_ambiguous_monster;
        env.mgrid(third_passage) = old_third_monster;
        env.igrid(old_start) = old_start_item;
        env.igrid(template_spawn) = old_spawn_item;
        env.igrid(ambiguous_spawn) = old_ambiguous_item;
        env.igrid(third_passage) = old_third_item;
        tile_env.flv(old_start) = old_start_flavour;
        tile_env.flv(template_spawn) = old_spawn_flavour;
        tile_env.flv(ambiguous_spawn) = old_ambiguous_flavour;
        tile_env.flv(third_passage) = old_third_flavour;
        tile_env.names = old_tile_names;
        env.map_knowledge(old_start) = old_start_knowledge;
        env.map_knowledge(template_spawn) = old_spawn_knowledge;
        env.map_knowledge(ambiguous_spawn) = old_ambiguous_knowledge;
        env.map_knowledge(third_passage) = old_third_knowledge;
        tile_env.remembered_flavour.set_feat_flavour(
            old_start, old_start_remembered, old_start_remembered_idx);
        tile_env.remembered_flavour.set_feat_flavour(
            template_spawn, old_spawn_remembered, old_spawn_remembered_idx);
        tile_env.remembered_flavour.set_feat_flavour(
            ambiguous_spawn, old_ambiguous_remembered,
            old_ambiguous_remembered_idx);
        tile_env.remembered_flavour.set_feat_flavour(
            third_passage, old_third_remembered, old_third_remembered_idx);
        env.grid_colours(old_start) = old_start_colour;
        env.grid_colours(template_spawn) = old_spawn_colour;
        env.grid_colours(ambiguous_spawn) = old_ambiguous_colour;
        env.grid_colours(third_passage) = old_third_colour;
    };

    env.properties.erase("housing_spawn_points");
    env.markers.remove_markers_at(old_start);
    env.markers.remove_markers_at(template_spawn);
    env.markers.remove_markers_at(ambiguous_spawn);
    env.markers.remove_markers_at(third_passage);
    env.grid(old_start) = DNGN_FLOOR;
    env.grid(template_spawn) = DNGN_RUNELIGHT;
    env.grid(ambiguous_spawn) = DNGN_FLOOR;
    env.grid(third_passage) = DNGN_FLOOR;
    env.mgrid(old_start) = NON_MONSTER;
    env.mgrid(template_spawn) = NON_MONSTER;
    env.mgrid(ambiguous_spawn) = NON_MONSTER;
    env.mgrid(third_passage) = NON_MONSTER;
    env.igrid(old_start) = NON_ITEM;
    env.igrid(template_spawn) = NON_ITEM;
    env.igrid(ambiguous_spawn) = NON_ITEM;
    env.igrid(third_passage) = NON_ITEM;
    crawl_view.set_player_at(old_start);
    REQUIRE(cloud_at(template_spawn) == nullptr);

    SECTION("editor self-selection reaches domain validation")
    {
        direction_chooser_args args;
        args.restricts = DIR_ENFORCE_RANGE;
        args.mode = TARG_NON_ACTOR;
        args.range = LOS_MAX_RANGE;
        args.needs_path = false;
        args.self = housing_editor_self_target_policy();
        dist selected;
        selected.target = you.pos();

        direction(selected, args);
        REQUIRE(selected.isValid);
        REQUIRE_FALSE(selected.isCancel);
        REQUIRE_FALSE(selected.interactive);
    }

    SECTION("one bare runelight is authenticated and used")
    {
        housing_ensure_level(true);
        REQUIRE(you.pos() == template_spawn);
        REQUIRE(housing_is_spawn(template_spawn));
        const vector<map_marker*> markers =
            env.markers.get_markers_at(template_spawn);
        REQUIRE(markers.size() == 1);
        REQUIRE(markers.front()->property("housing_spawn") == "yes");
        REQUIRE(markers.front()->property("veto_destroy") == "veto");
        REQUIRE(env.grid(old_start) == DNGN_FLOOR);
    }

    SECTION("multiple unauthenticated runelights fail closed")
    {
        env.grid(ambiguous_spawn) = DNGN_RUNELIGHT;
        REQUIRE_THROWS(housing_ensure_level(false));
        REQUIRE(you.pos() == old_start);
        REQUIRE(env.markers.get_markers_at(template_spawn).empty());
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
    }

    SECTION("a malformed runelight fixture fails closed")
    {
        env.grid(ambiguous_spawn) = DNGN_RUNELIGHT;
        env.markers.add(new map_wiz_props_marker(ambiguous_spawn));
        REQUIRE_THROWS(housing_ensure_level(false));
        REQUIRE(you.pos() == old_start);
        REQUIRE(env.markers.get_markers_at(template_spawn).empty());
    }

    SECTION("spawn management adds and removes exact visible fixtures")
    {
        housing_ensure_level(false);
        REQUIRE(housing_toggle_spawn_point(ambiguous_spawn));
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(housing_is_spawn(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_RUNELIGHT);
        const vector<map_marker*> added =
            env.markers.get_markers_at(ambiguous_spawn);
        REQUIRE(added.size() == 1);
        REQUIRE(added.front()->property("housing_spawn") == "yes");
        REQUIRE(added.front()->property("veto_destroy") == "veto");

        REQUIRE(housing_toggle_spawn_point(template_spawn));
        REQUIRE_FALSE(housing_is_spawn(template_spawn));
        REQUIRE(env.grid(template_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(template_spawn).empty());

        // The remaining 1/N respawn set can never become empty.
        REQUIRE_FALSE(housing_toggle_spawn_point(ambiguous_spawn));
        REQUIRE(housing_is_spawn(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_RUNELIGHT);
    }

    SECTION("spawn management rejects occupied or marked floor")
    {
        housing_ensure_level(false);
        env.markers.add(new map_wiz_props_marker(ambiguous_spawn));
        REQUIRE_FALSE(housing_toggle_spawn_point(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE_FALSE(housing_is_spawn(ambiguous_spawn));
    }

    SECTION("spawn management rejects special terrain and items")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_STONE_WALL;
        REQUIRE_FALSE(housing_toggle_spawn_point(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_STONE_WALL);
        REQUIRE_FALSE(housing_is_spawn(ambiguous_spawn));

        env.grid(ambiguous_spawn) = DNGN_FLOOR;
        env.igrid(ambiguous_spawn) = 0;
        REQUIRE_FALSE(housing_toggle_spawn_point(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE_FALSE(housing_is_spawn(ambiguous_spawn));
    }

    SECTION("publication reports the exact damaged spawn coordinate")
    {
        housing_ensure_level(false);
        vector<map_marker*> markers =
            env.markers.get_markers_at(template_spawn);
        REQUIRE(markers.size() == 1);
        auto *wiz = static_cast<map_wiz_props_marker *>(markers.front());
        wiz->properties.erase("veto_destroy");

        const housing_publish_validation result =
            housing_validate_current_map();
        REQUIRE_FALSE(result.valid());
        REQUIRE(result.problem
                == housing_publish_problem_type::invalid_spawn);
        REQUIRE(result.has_position);
        REQUIRE(result.position == template_spawn);
        REQUIRE(result.detail == "spawn point");
        REQUIRE(result.message.find("(21,20)") != string::npos);
        REQUIRE(result.message.find("Clear and recreate") != string::npos);

        // Clear must be able to act on the coordinate it reports. Since this
        // is the final spawn, it repairs the exact fixture instead of leaving
        // the map spawn-less.
        REQUIRE(housing_clear_terrain(template_spawn));
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("publication handles an unknown terrain enum without crashing")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) =
            static_cast<dungeon_feature_type>(NUM_FEATURES);

        const housing_publish_validation result =
            housing_validate_current_map();
        REQUIRE_FALSE(result.valid());
        REQUIRE(result.problem
                == housing_publish_problem_type::unsupported_feature);
        REQUIRE(result.has_position);
        REQUIRE(result.position == ambiguous_spawn);
        REQUIRE(result.detail == "unknown");
        REQUIRE(result.message.find("feature ") != string::npos);
        REQUIRE(result.message.find("(22,20)") != string::npos);

        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("clear repairs an authenticated runelight missing from spawn data")
    {
        housing_ensure_level(false);
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(env.markers.get_markers_at(template_spawn).size() == 1);
        env.properties.erase("housing_spawn_points");

        const housing_publish_validation before =
            housing_validate_current_map();
        REQUIRE_FALSE(before.valid());
        REQUIRE(before.problem
                == housing_publish_problem_type::invalid_spawn);
        REQUIRE(before.position == template_spawn);

        REQUIRE(housing_clear_terrain(template_spawn));
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("publication rejects and Clear repairs a stored floor spawn")
    {
        housing_ensure_level(false);
        env.grid(template_spawn) = DNGN_FLOOR;

        const housing_publish_validation before =
            housing_validate_current_map();
        REQUIRE_FALSE(before.valid());
        REQUIRE(before.problem
                == housing_publish_problem_type::invalid_spawn);
        REQUIRE(before.position == template_spawn);

        REQUIRE(housing_clear_terrain(template_spawn));
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("Clear repairs a final stored spawn with damaged terrain")
    {
        housing_ensure_level(false);
        env.grid(template_spawn) = DNGN_ORB_DAIS;

        const housing_publish_validation before =
            housing_validate_current_map();
        REQUIRE_FALSE(before.valid());
        REQUIRE(before.problem
                == housing_publish_problem_type::unsupported_feature);
        REQUIRE(before.position == template_spawn);

        // Raw spawn membership must route around strict migration even though
        // the filtered spawn view no longer considers this terrain valid.
        REQUIRE(housing_clear_terrain(template_spawn));
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("spawn placement repairs an invalid empty spawn property")
    {
        housing_ensure_level(false);
        env.properties.erase("housing_spawn_points");
        env.properties["housing_spawn_points"] = 17;

        REQUIRE(housing_toggle_spawn_point(ambiguous_spawn));
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(housing_is_spawn(ambiguous_spawn));
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("publication reports an inconsistent shop entry coordinate")
    {
        housing_ensure_level(false);
        shop_struct orphan;
        orphan.pos = ambiguous_spawn;
        orphan.type = SHOP_GENERAL;
        orphan.level = 1;
        env.shop[ambiguous_spawn] = orphan;

        const housing_publish_validation result =
            housing_validate_current_map();
        REQUIRE_FALSE(result.valid());
        REQUIRE(result.problem
                == housing_publish_problem_type::inconsistent_shop);
        REQUIRE(result.has_position);
        REQUIRE(result.position == ambiguous_spawn);
        REQUIRE(result.message.find("(22,20)") != string::npos);
    }

    SECTION("publication reports the first cloud coordinate and count")
    {
        housing_ensure_level(false);
        cloud_struct first;
        first.pos = old_start;
        first.type = CLOUD_FIRE;
        cloud_struct second;
        second.pos = ambiguous_spawn;
        second.type = CLOUD_STEAM;
        env.cloud[old_start] = first;
        env.cloud[ambiguous_spawn] = second;

        const housing_publish_validation result =
            housing_validate_current_map();
        REQUIRE_FALSE(result.valid());
        REQUIRE(result.problem
                == housing_publish_problem_type::cloud_state);
        REQUIRE(result.has_position);
        REQUIRE(result.position == old_start);
        REQUIRE(result.count == 2);
        REQUIRE(result.message.find("2 clouds") != string::npos);
        REQUIRE(result.message.find("(20,20)") != string::npos);
    }

    SECTION("publication identifies an unsafe monster by name and position")
    {
        housing_ensure_level(false);
        init_monsters();
        mgen_data mg(MONS_RAT, BEH_HOSTILE, ambiguous_spawn, MHITYOU,
                     MG_FORBID_BANDS | MG_FORCE_PLACE);
        monster *created = create_monster(mg);
        REQUIRE(created != nullptr);
        unwinder remove_created = [&created]() {
            if (created && created->alive())
            {
                env.mgrid(created->pos()) = NON_MONSTER;
                created->reset();
            }
        };

        const housing_publish_validation result =
            housing_validate_current_map();
        REQUIRE_FALSE(result.valid());
        REQUIRE(result.problem
                == housing_publish_problem_type::unsafe_monster);
        REQUIRE(result.has_position);
        REQUIRE(result.position == ambiguous_spawn);
        REQUIRE(result.detail.find("rat") != string::npos);
        REQUIRE(result.message.find("(22,20)") != string::npos);
        REQUIRE(result.message.find("type") != string::npos);
    }

    SECTION("terrain clear removes a spawn but preserves the final one")
    {
        housing_ensure_level(false);
        REQUIRE(housing_toggle_spawn_point(ambiguous_spawn));
        you.position = ambiguous_spawn;
        crawl_view.set_player_at(ambiguous_spawn);
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE_FALSE(housing_is_spawn(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());

        item_def gold;
        gold.clear();
        gold.base_type = OBJ_GOLD;
        gold.quantity = 1;
        const int gold_index = copy_item_to_grid(gold, template_spawn);
        REQUIRE(gold_index != NON_ITEM);

        // Clear still destroys the selected stack even though the final spawn
        // fixture itself must remain protected.
        REQUIRE(housing_clear_terrain(template_spawn));
        REQUIRE_FALSE(env.item[gold_index].defined());
        REQUIRE(housing_is_spawn(template_spawn));
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(env.markers.get_markers_at(template_spawn).size() == 1);
        REQUIRE_FALSE(housing_clear_terrain(template_spawn));
    }

    SECTION("owner-only barrier toggle creates a solid see-through wall")
    {
        housing_ensure_level(false);
        REQUIRE(housing_toggle_visitor_wall(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_CLEAR_PERMAROCK_WALL);
        REQUIRE(feat_is_solid(env.grid(ambiguous_spawn)));
        REQUIRE_FALSE(feat_is_opaque(env.grid(ambiguous_spawn)));
        REQUIRE(housing_visitor_wall_is_valid(ambiguous_spawn));
        const vector<map_marker*> markers =
            env.markers.get_markers_at(ambiguous_spawn);
        REQUIRE(markers.size() == 1);
        REQUIRE(markers.front()->property("housing_visitor_wall") == "yes");
        REQUIRE(markers.front()->property("veto_destroy") == "veto");

        REQUIRE(housing_toggle_visitor_wall(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
    }

    SECTION("terrain clear removes an authenticated owner-only barrier")
    {
        housing_ensure_level(false);
        REQUIRE(housing_toggle_visitor_wall(ambiguous_spawn));
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
    }

    SECTION("terrain clear removes a Housing portal and its item stack")
    {
        housing_ensure_level(false);
        REQUIRE(housing_create_portal(ambiguous_spawn, "Owner:main"));
        REQUIRE(housing_portal_is_valid(ambiguous_spawn));

        item_def gold;
        gold.clear();
        gold.base_type = OBJ_GOLD;
        gold.quantity = 7;
        const int gold_index = copy_item_to_grid(gold, ambiguous_spawn);
        REQUIRE(gold_index != NON_ITEM);

        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE_FALSE(env.item[gold_index].defined());
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
        REQUIRE_FALSE(housing_portal_is_valid(ambiguous_spawn));
    }

    SECTION("named Housing passages persist and route matching pairs")
    {
        housing_ensure_level(false);
        you.position = template_spawn;
        crawl_view.set_player_at(template_spawn);

        REQUIRE(housing_create_portal(old_start, "gallery_1"));
        REQUIRE(housing_create_portal(ambiguous_spawn, "gallery_1"));
        REQUIRE(housing_local_portal_is_valid(old_start));
        REQUIRE(housing_local_portal_is_valid(ambiguous_spawn));
        REQUIRE(env.grid(old_start) == DNGN_STONE_ARCH);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_STONE_ARCH);

        coord_def destination;
        REQUIRE(housing_local_portal_destination(old_start, destination));
        REQUIRE(destination == ambiguous_spawn);
        REQUIRE(housing_local_portal_is_valid(old_start));
        REQUIRE(housing_local_portal_is_valid(ambiguous_spawn));

        // Removing one endpoint leaves the singleton authenticated but inert.
        REQUIRE(housing_clear_terrain(old_start));
        REQUIRE(env.grid(old_start) == DNGN_FLOOR);
        REQUIRE_FALSE(housing_local_portal_destination(ambiguous_spawn,
                                                       destination));
        REQUIRE(housing_local_portal_is_valid(ambiguous_spawn));
    }

    SECTION("different named Housing passages never mix")
    {
        housing_ensure_level(false);
        you.position = template_spawn;
        crawl_view.set_player_at(template_spawn);

        REQUIRE(housing_create_portal(old_start, "north"));
        REQUIRE(housing_create_portal(ambiguous_spawn, "south"));
        coord_def destination;
        REQUIRE_FALSE(housing_local_portal_destination(old_start,
                                                       destination));
    }

    SECTION("three matching passage endpoints choose another endpoint")
    {
        housing_ensure_level(false);
        you.position = template_spawn;
        crawl_view.set_player_at(template_spawn);

        REQUIRE(housing_create_portal(old_start, "hub"));
        REQUIRE(housing_create_portal(ambiguous_spawn, "hub"));
        REQUIRE(housing_create_portal(third_passage, "hub"));
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            coord_def destination;
            REQUIRE(housing_local_portal_destination(old_start,
                                                     destination));
            REQUIRE((destination == ambiguous_spawn
                     || destination == third_passage));
        }
        REQUIRE(housing_local_portal_is_valid(old_start));
        REQUIRE(housing_local_portal_is_valid(ambiguous_spawn));
        REQUIRE(housing_local_portal_is_valid(third_passage));
    }

    SECTION("visitor inventory tiles are exact fixtures removable by Clear")
    {
        housing_ensure_level(false);
        REQUIRE(housing_create_visitor_strip(ambiguous_spawn));
        REQUIRE(housing_visitor_strip_is_valid(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_TRANSPORTER_LANDING);
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
        REQUIRE_FALSE(housing_visitor_strip_is_valid(ambiguous_spawn));
    }

    SECTION("Clear removes a fixture with conflicting roles")
    {
        housing_ensure_level(false);
        REQUIRE(housing_create_portal(ambiguous_spawn, "gallery"));
        vector<map_marker*> markers =
            env.markers.get_markers_at(ambiguous_spawn);
        REQUIRE(markers.size() == 1);
        auto *wiz = static_cast<map_wiz_props_marker *>(markers.front());
        wiz->set_property("housing_visitor_strip", "yes");
        REQUIRE_FALSE(housing_local_portal_is_valid(ambiguous_spawn));
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
    }

    SECTION("malformed passage markers never fall through to native traps")
    {
        housing_ensure_level(false);
        you.position = template_spawn;
        crawl_view.set_player_at(template_spawn);
        REQUIRE(housing_create_portal(ambiguous_spawn, "broken"));

        env.grid(ambiguous_spawn) = DNGN_PASSAGE_OF_GOLUBRIA;
        REQUIRE_FALSE(housing_local_portal_is_valid(ambiguous_spawn));
        you.position = ambiguous_spawn;
        crawl_view.set_player_at(ambiguous_spawn);
        REQUIRE(housing_movement_fixture_is_reserved(ambiguous_spawn));
        REQUIRE(housing_trigger_local_portal(you));
        REQUIRE(you.pos() == ambiguous_spawn);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_PASSAGE_OF_GOLUBRIA);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).size() == 1);
    }

    SECTION("malformed strip markers never fall through to native traps")
    {
        housing_ensure_level(false);
        you.position = template_spawn;
        crawl_view.set_player_at(template_spawn);
        REQUIRE(housing_create_visitor_strip(ambiguous_spawn));

        env.grid(ambiguous_spawn) = DNGN_TRAP_DISPERSAL;
        REQUIRE_FALSE(housing_visitor_strip_is_valid(ambiguous_spawn));
        you.position = ambiguous_spawn;
        crawl_view.set_player_at(ambiguous_spawn);
        REQUIRE(housing_movement_fixture_is_reserved(ambiguous_spawn));
        REQUIRE(housing_trigger_visitor_strip(you));
        REQUIRE(you.pos() == ambiguous_spawn);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_TRAP_DISPERSAL);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).size() == 1);
    }

    SECTION("terrain clear removes a malformed portal and its items")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_ENTER_PORTAL_VAULT;

        item_def gold;
        gold.clear();
        gold.base_type = OBJ_GOLD;
        gold.quantity = 3;
        const int gold_index = copy_item_to_grid(gold, ambiguous_spawn);
        REQUIRE(gold_index != NON_ITEM);

        // The selected cell is explicitly destructive: its item stack and
        // publisher-rejected reserved terrain are repaired together.
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE_FALSE(env.item[gold_index].defined());
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
    }

    SECTION("terrain clear removes an active shop under the owner")
    {
        housing_ensure_level(false);
        REQUIRE(env.shop.find(old_start) == env.shop.end());
        const size_t original_shop_count = env.shop.size();
        shop_struct merchant;
        merchant.pos = old_start;
        merchant.type = SHOP_GENERAL;
        merchant.level = 1;
        env.shop[old_start] = merchant;
        env.grid(old_start) = DNGN_ENTER_SHOP;

        REQUIRE(housing_clear_terrain(old_start));
        REQUIRE(env.shop.size() == original_shop_count);
        REQUIRE(env.shop.find(old_start) == env.shop.end());
        REQUIRE(env.grid(old_start) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(old_start).empty());
    }

    SECTION("terrain clear also repairs inconsistent shops")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_ENTER_SHOP;
        REQUIRE(env.shop.find(ambiguous_spawn) == env.shop.end());
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);

        shop_struct orphan;
        orphan.pos = ambiguous_spawn;
        orphan.type = SHOP_GENERAL;
        orphan.level = 1;
        env.grid(ambiguous_spawn) = DNGN_FLOOR;
        env.shop[ambiguous_spawn] = orphan;
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.shop.find(ambiguous_spawn) == env.shop.end());
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);

        env.grid(ambiguous_spawn) = DNGN_ENTER_SHOP;
        env.shop[ambiguous_spawn] = orphan;
        env.markers.add(new map_wiz_props_marker(ambiguous_spawn));
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.shop.find(ambiguous_spawn) == env.shop.end());
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());

        env.grid(ambiguous_spawn) = DNGN_ABANDONED_SHOP;
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
    }

    SECTION("terrain clear still handles ordinary editable terrain")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_STONE_WALL;
        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
    }

    SECTION("terrain clear forcibly repairs unsupported protected terrain")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_ORB_DAIS;
        const housing_publish_validation before =
            housing_validate_current_map();
        REQUIRE_FALSE(before.valid());
        REQUIRE(before.problem
                == housing_publish_problem_type::unsupported_feature);
        REQUIRE(before.position == ambiguous_spawn);

        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(housing_validate_current_map().valid());
    }

    SECTION("terrain clear removes every item on the selected square")
    {
        housing_ensure_level(false);

        item_def skeleton;
        skeleton.clear();
        skeleton.base_type = OBJ_CORPSES;
        skeleton.sub_type = CORPSE_SKELETON;
        skeleton.quantity = 1;
        skeleton.mon_type = MONS_RAT;
        skeleton.orig_monnum = MONS_RAT;
        const int skeleton_index =
            copy_item_to_grid(skeleton, ambiguous_spawn);
        REQUIRE(skeleton_index != NON_ITEM);

        item_def gold;
        gold.clear();
        gold.base_type = OBJ_GOLD;
        gold.sub_type = 0;
        gold.quantity = 17;
        const int gold_index = copy_item_to_grid(gold, ambiguous_spawn);
        REQUIRE(gold_index != NON_ITEM);

        REQUIRE(housing_clear_terrain(ambiguous_spawn));
        REQUIRE_FALSE(env.item[skeleton_index].defined());
        REQUIRE_FALSE(env.item[gold_index].defined());
        REQUIRE(env.igrid(ambiguous_spawn) == NON_ITEM);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
    }

    SECTION("legacy metal owner-only barrier upgrades on owner load")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_METAL_WALL;
        auto *marker = new map_wiz_props_marker(ambiguous_spawn);
        marker->set_property("housing_visitor_wall", "yes");
        marker->set_property("feature_description", "owner-only barrier");
        marker->set_property("veto_destroy", "veto");
        env.markers.add(marker);
        REQUIRE(housing_visitor_wall_is_valid(ambiguous_spawn));

        housing_ensure_level(false);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_CLEAR_PERMAROCK_WALL);
        REQUIRE_FALSE(feat_is_opaque(env.grid(ambiguous_spawn)));
        REQUIRE(housing_visitor_wall_is_valid(ambiguous_spawn));
    }

    SECTION("owner migration removes only unmarked legacy runelight residue")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_RUNELIGHT;
        housing_ensure_level(false);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).empty());
        REQUIRE(housing_is_spawn(template_spawn));
    }

    SECTION("a reset player position does not skip fixture migration")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_RUNELIGHT;
        you.position.reset();
        housing_ensure_level(false);
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_FLOOR);
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(env.markers.get_markers_at(template_spawn).size() == 1);
    }

    SECTION("a stored floor spawn upgrades with a reset player position")
    {
        env.grid(template_spawn) = DNGN_FLOOR;
        env.properties.erase("housing_spawn_points");
        CrawlVector &stored =
            env.properties["housing_spawn_points"].new_vector(SV_COORD);
        stored.push_back(template_spawn);
        you.position.reset();
        housing_ensure_level(false);
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        const vector<map_marker*> markers =
            env.markers.get_markers_at(template_spawn);
        REQUIRE(markers.size() == 1);
        REQUIRE(markers.front()->property("housing_spawn") == "yes");
    }

    SECTION("invalid stored spawn data never adopts a bare runelight")
    {
        env.properties.erase("housing_spawn_points");
        CrawlVector &stored =
            env.properties["housing_spawn_points"].new_vector(SV_COORD);
        stored.push_back(coord_def(-1, -1));
        you.position.reset();
        REQUIRE_THROWS(housing_ensure_level(false));
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(env.markers.get_markers_at(template_spawn).empty());
    }

    SECTION("marker-bearing extra runelight fails closed")
    {
        housing_ensure_level(false);
        env.grid(ambiguous_spawn) = DNGN_RUNELIGHT;
        env.markers.add(new map_wiz_props_marker(ambiguous_spawn));
        REQUIRE_THROWS(housing_ensure_level(false));
        REQUIRE(env.grid(ambiguous_spawn) == DNGN_RUNELIGHT);
        REQUIRE(env.markers.get_markers_at(ambiguous_spawn).size() == 1);
        REQUIRE(env.grid(template_spawn) == DNGN_RUNELIGHT);
        REQUIRE(env.markers.get_markers_at(template_spawn).size() == 1);
    }
}

#ifndef TARGET_OS_WINDOWS
TEST_CASE("Housing publication distinguishes console from broken binding",
          "[single-file]")
{
    const char *old_account = getenv("CRAWL_HOUSING_ACCOUNT_ID");
    const char *old_public_dir = getenv("CRAWL_HOUSING_PUBLIC_DIR");
    const string saved_account = old_account ? old_account : "";
    const string saved_public_dir = old_public_dir ? old_public_dir : "";
    const bool had_account = old_account;
    const bool had_public_dir = old_public_dir;
    unwinder restore_environment = [&]() {
        if (had_account)
            setenv("CRAWL_HOUSING_ACCOUNT_ID", saved_account.c_str(), 1);
        else
            unsetenv("CRAWL_HOUSING_ACCOUNT_ID");
        if (had_public_dir)
            setenv("CRAWL_HOUSING_PUBLIC_DIR", saved_public_dir.c_str(), 1);
        else
            unsetenv("CRAWL_HOUSING_PUBLIC_DIR");
    };

    package save;
    {
        chunk_writer output(&save, "D");
        const char payload = 'x';
        output.write(&payload, sizeof(payload));
    }
    save.commit();
    unwind_var<package*> saved_save(you.save, &save);
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<bool> saved_on_level(you.on_current_level, true);

    unsetenv("CRAWL_HOUSING_ACCOUNT_ID");
    unsetenv("CRAWL_HOUSING_PUBLIC_DIR");
    REQUIRE(housing_publish_current_map());

    setenv("CRAWL_HOUSING_ACCOUNT_ID", "17", 1);
    REQUIRE_FALSE(housing_publish_current_map());
}
#endif

static void _write_test_chunk(package &save, const string &chunk,
                              const string &payload)
{
    chunk_writer output(&save, chunk);
    output.write(payload.data(), payload.size());
}

static void _write_test_housing_snapshot(const string &path,
                                         const string &account_id,
                                         const string &owner,
                                         const string &map_id,
                                         const string &level_payload)
{
    package snapshot(path.c_str(), true, true);
    {
        writer output(&snapshot, "housing_meta");
        marshallInt(output, housing_snapshot_schema_version());
        marshallString(output, "test-version");
        marshallString(output, account_id);
        marshallString(output, owner);
        marshallString(output, map_id);
    }
    _write_test_chunk(snapshot, "level", level_payload);
    snapshot.commit();
}

static string _read_test_chunk(package &save, const string &chunk)
{
    vector<char> data;
    chunk_reader input(&save, chunk);
    input.read_all(data);
    return string(data.begin(), data.end());
}

TEST_CASE("Housing map index round-trips named map chunks", "[single-file]")
{
    package save;
    _write_test_chunk(save, housing_map_chunk_name("main"), "main level");
    _write_test_chunk(save, housing_map_chunk_name("Side_7"), "side level");

    const vector<string> expected = { "main", "Side_7" };
    housing_write_map_index(save, expected, "Side_7");
    save.commit();

    vector<string> maps;
    string current;
    REQUIRE(housing_read_map_index(save, maps, current));
    REQUIRE(maps == expected);
    REQUIRE(current == "Side_7");
    REQUIRE(housing_map_chunk_name("Side_7") == "housing_map_Side_7");
    REQUIRE(housing_map_chunk_name("bad-id").empty());
}

TEST_CASE("Housing map index rejects duplicate and missing maps",
          "[single-file]")
{
    package save;
    _write_test_chunk(save, housing_map_chunk_name("main"), "main level");
    {
        writer output(&save, "housing_index");
        marshallInt(output, 1);
        marshallString(output, "main");
        marshallInt(output, 2);
        marshallString(output, "main");
        marshallString(output, "main");
    }

    vector<string> maps;
    string current;
    REQUIRE_THROWS(housing_read_map_index(save, maps, current));

    package legacy;
    REQUIRE_FALSE(housing_read_map_index(legacy, maps, current));
    legacy.abort();
}

TEST_CASE("Housing visitor capsules omit canonical private map chunks",
          "[single-file]")
{
    package source;
    _write_test_chunk(source, "you", "portable character");
    _write_test_chunk(source, "chr", "portable summary");
    _write_test_chunk(source, "D", "active private level");
    _write_test_chunk(source, "housing_index", "private index");
    _write_test_chunk(source, "housing_template", "private template");
    _write_test_chunk(source, housing_map_chunk_name("main"), "private main");
    _write_test_chunk(source, housing_map_chunk_name("Side_7"),
                      "private side");
    _write_test_chunk(source, "housing_theme_Side_7", "private theme");
    source.commit();

    package capsule;
    housing_copy_visitor_state(source, capsule);
    capsule.commit();

    REQUIRE(capsule.list_chunks() == vector<string>{ "chr", "you" });
    REQUIRE_FALSE(capsule.has_chunk("D"));
    REQUIRE_FALSE(capsule.has_chunk("housing_index"));
    REQUIRE_FALSE(capsule.has_chunk("housing_template"));
    REQUIRE_FALSE(capsule.has_chunk(housing_map_chunk_name("main")));
    REQUIRE_FALSE(capsule.has_chunk(housing_map_chunk_name("Side_7")));
    REQUIRE_FALSE(capsule.has_chunk("housing_theme_Side_7"));
}

TEST_CASE("Housing migrates only an exact stale template clone",
          "[single-file]")
{
    package save;
    _write_test_chunk(save, "D", "bare starter level");
    _write_test_chunk(save, housing_map_chunk_name("Side_7"),
                      "bare starter level");
    _write_test_chunk(save, "housing_template", "bare starter level");
    save.commit();

    REQUIRE(housing_legacy_template_is_exact_clone(save, "Side_7"));
    REQUIRE_FALSE(housing_legacy_template_is_exact_clone(save, "bad-id"));

    _write_test_chunk(save, housing_map_chunk_name("Side_7"),
                      "owner-edited level");
    save.commit();
    REQUIRE_FALSE(housing_legacy_template_is_exact_clone(save, "Side_7"));

    _write_test_chunk(save, housing_map_chunk_name("Side_7"),
                      "bare starter level");
    _write_test_chunk(save, "housing_template", "different template");
    save.commit();
    REQUIRE_FALSE(housing_legacy_template_is_exact_clone(save, "Side_7"));
}

#ifndef TARGET_OS_WINDOWS
struct housing_binding_test_tree
{
    housing_binding_test_tree()
    {
        char root_template[] = "/tmp/crawl-housing-binding-XXXXXX";
        char *created_root = mkdtemp(root_template);
        REQUIRE(created_root != nullptr);

        root = created_root;
        account_dir = catpath(root, "17");
        by_name_dir = catpath(root, "by-name");
        owner_dir = catpath(by_name_dir, "owner");
        numeric_file = catpath(account_dir, "Side_7.hmap");
        legacy_file = catpath(owner_dir, "Side_7.hmap");
        binding_file = catpath(owner_dir, ".account-id");
        REQUIRE(mkdir_u(account_dir.c_str(), 0700) == 0);
        REQUIRE(mkdir_u(by_name_dir.c_str(), 0700) == 0);
        REQUIRE(mkdir_u(owner_dir.c_str(), 0700) == 0);
    }

    string root;
    string account_dir;
    string by_name_dir;
    string owner_dir;
    string numeric_file;
    string legacy_file;
    string binding_file;

    ~housing_binding_test_tree()
    {
        unlink_u(numeric_file.c_str());
        unlink_u(legacy_file.c_str());
        unlink_u(binding_file.c_str());
        rmdir(owner_dir.c_str());
        rmdir(by_name_dir.c_str());
        rmdir(account_dir.c_str());
        rmdir(root.c_str());
    }
};

TEST_CASE("Housing public owner binding cannot be reassigned",
          "[single-file]")
{
    housing_binding_test_tree tree;
    REQUIRE(housing_bind_public_owner(tree.root, "Owner", "17"));
    REQUIRE(housing_bind_public_owner(tree.root, "OWNER", "17"));
    REQUIRE_FALSE(housing_bind_public_owner(tree.root, "Owner", "18"));

    FILE *binding = fopen_u(tree.binding_file.c_str(), "rb");
    REQUIRE(binding != nullptr);
    char value[8] = {};
    REQUIRE(fread(value, 1, sizeof(value), binding) == 2);
    REQUIRE(fclose(binding) == 0);
    REQUIRE(string(value, 2) == "17");
}

TEST_CASE("Housing direct resolver uses one authoritative numeric generation",
          "[single-file]")
{
    housing_binding_test_tree tree;
    _write_test_housing_snapshot(tree.legacy_file, "17", "Owner",
                                 "Side_7", "legacy-old");
    _write_test_housing_snapshot(tree.numeric_file, "17", "Owner",
                                 "Side_7", "numeric-new");

    string resolved;
    REQUIRE(housing_resolve_public_snapshot_path(
        tree.root, "Owner", "Side_7", resolved));
    REQUIRE(resolved == tree.numeric_file);
    // Visitor resolution is read-only; the authenticated owner publisher is
    // the only component allowed to install the stable global name binding.
    REQUIRE_FALSE(file_exists(tree.binding_file));
    {
        package chosen(resolved.c_str(), false);
        REQUIRE(_read_test_chunk(chosen, "level") == "numeric-new");
    }

    // Updating only the authoritative payload is immediately what the direct
    // resolver sees; the stale duplicate is never a competing generation.
    _write_test_housing_snapshot(tree.numeric_file, "17", "Owner",
                                 "Side_7", "numeric-newer");
    REQUIRE(housing_resolve_public_snapshot_path(
        tree.root, "Owner", "Side_7", resolved));
    REQUIRE(resolved == tree.numeric_file);
    package chosen(resolved.c_str(), false);
    REQUIRE(_read_test_chunk(chosen, "level") == "numeric-newer");
}

TEST_CASE("Housing resolver never serves a legacy-only payload",
          "[single-file]")
{
    housing_binding_test_tree tree;
    _write_test_housing_snapshot(tree.legacy_file, "17", "Owner",
                                 "Side_7", "legacy-only");

    string resolved;
    REQUIRE_FALSE(housing_resolve_public_snapshot_path(
        tree.root, "Owner", "Side_7", resolved));
    REQUIRE_FALSE(file_exists(tree.binding_file));
}

TEST_CASE("Housing resolver rejects a bound snapshot account mismatch",
          "[single-file]")
{
    housing_binding_test_tree tree;
    REQUIRE(housing_bind_public_owner(tree.root, "Owner", "17"));
    _write_test_housing_snapshot(tree.numeric_file, "18", "Owner",
                                 "Side_7", "wrong-account");

    string resolved;
    REQUIRE_THROWS(housing_resolve_public_snapshot_path(
        tree.root, "Owner", "Side_7", resolved));
}

struct housing_public_test_tree
{
    string root;
    string account_dir;
    string by_name_dir;
    string owner_dir;
    string account_file;
    string name_file;
    string outside_dir;
    string outside_file;

    ~housing_public_test_tree()
    {
        unlink_u(account_file.c_str());
        unlink_u(name_file.c_str());
        unlink_u(owner_dir.c_str());
        unlink_u(outside_file.c_str());
        rmdir(owner_dir.c_str());
        rmdir(by_name_dir.c_str());
        rmdir(account_dir.c_str());
        rmdir(root.c_str());
        rmdir(outside_dir.c_str());
    }
};

static bool _touch_test_file(const string &path)
{
    FILE *file = fopen_u(path.c_str(), "wb");
    if (!file)
        return false;
    const bool wrote = fputs("test", file) >= 0;
    const bool closed = fclose(file) == 0;
    return wrote && closed;
}

TEST_CASE("Housing deletion distinguishes console from broken binding",
          "[single-file]")
{
    REQUIRE(housing_unpublish_map_files("", "", "Owner", "Side_7"));
    REQUIRE_FALSE(housing_unpublish_map_files("", "17", "Owner",
                                               "Side_7"));
}

TEST_CASE("Housing public map deletion retries after one alias fails",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *root = mkdtemp(root_template);
    REQUIRE(root != nullptr);

    housing_public_test_tree tree;
    tree.root = root;
    tree.account_dir = catpath(tree.root, "17");
    tree.by_name_dir = catpath(tree.root, "by-name");
    tree.owner_dir = catpath(tree.by_name_dir, "owner");
    tree.account_file = catpath(tree.account_dir, "Side_7.hmap");
    tree.name_file = catpath(tree.owner_dir, "Side_7.hmap");
    tree.outside_dir = tree.root + ".outside";
    tree.outside_file = catpath(tree.outside_dir, "Side_7.hmap");

    REQUIRE(mkdir_u(tree.account_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.by_name_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.outside_dir.c_str(), 0700) == 0);
    REQUIRE(_touch_test_file(tree.account_file));
    REQUIRE(_touch_test_file(tree.outside_file));
    REQUIRE(symlink(tree.outside_dir.c_str(), tree.owner_dir.c_str()) == 0);

    // The ordinary account-id alias is removed, but a parent symlink resolving
    // outside the public tree is rejected before unlink. The operation as a
    // whole fails so callers retain the canonical map for a safe retry.
    REQUIRE_FALSE(housing_unpublish_map_files(tree.root, "17", "Owner",
                                               "Side_7"));
    REQUIRE_FALSE(file_exists(tree.account_file));
    REQUIRE(file_exists(tree.name_file));
    REQUIRE(file_exists(tree.outside_file));

    // Retrying is idempotent: ENOENT for the first alias is accepted and the
    // now-safe second alias is removed.
    REQUIRE(unlink_u(tree.owner_dir.c_str()) == 0);
    REQUIRE(mkdir_u(tree.owner_dir.c_str(), 0700) == 0);
    REQUIRE(_touch_test_file(tree.name_file));
    REQUIRE(housing_unpublish_map_files(tree.root, "17", "Owner", "Side_7"));
    REQUIRE_FALSE(file_exists(tree.name_file));
    REQUIRE(file_exists(tree.outside_file));
}

TEST_CASE("Housing public deletion unlinks a dangling alias symlink",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *root = mkdtemp(root_template);
    REQUIRE(root != nullptr);

    housing_public_test_tree tree;
    tree.root = root;
    tree.account_dir = catpath(tree.root, "17");
    tree.by_name_dir = catpath(tree.root, "by-name");
    tree.owner_dir = catpath(tree.by_name_dir, "owner");
    tree.account_file = catpath(tree.account_dir, "Side_7.hmap");
    tree.name_file = catpath(tree.owner_dir, "Side_7.hmap");
    tree.outside_file = tree.root + ".missing";

    REQUIRE(mkdir_u(tree.account_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.by_name_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.owner_dir.c_str(), 0700) == 0);
    REQUIRE(symlink(tree.outside_file.c_str(), tree.name_file.c_str()) == 0);

    struct stat before;
    REQUIRE(lstat(tree.name_file.c_str(), &before) == 0);
    REQUIRE(S_ISLNK(before.st_mode));
    REQUIRE_FALSE(file_exists(tree.name_file));

    // The account alias is ENOENT and the by-name alias is dangling. Direct
    // unlink after parent containment validation handles both idempotently.
    REQUIRE(housing_unpublish_map_files(tree.root, "17", "Owner", "Side_7"));
    struct stat after;
    errno = 0;
    REQUIRE(lstat(tree.name_file.c_str(), &after) == -1);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("Housing public deletion rejects an internal parent symlink",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *created_root = mkdtemp(root_template);
    REQUIRE(created_root != nullptr);

    const string root = created_root;
    const string account_dir = catpath(root, "17");
    const string other_account_dir = catpath(root, "18");
    const string by_name_dir = catpath(root, "by-name");
    const string owner_dir = catpath(by_name_dir, "owner");
    const string other_map = catpath(other_account_dir, "Side_7.hmap");
    unwinder cleanup = [&]() {
        unlink_u(owner_dir.c_str());
        unlink_u(other_map.c_str());
        rmdir(by_name_dir.c_str());
        rmdir(account_dir.c_str());
        rmdir(other_account_dir.c_str());
        rmdir(root.c_str());
    };

    REQUIRE(mkdir_u(account_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(other_account_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(by_name_dir.c_str(), 0700) == 0);
    REQUIRE(_touch_test_file(other_map));
    REQUIRE(symlink(other_account_dir.c_str(), owner_dir.c_str()) == 0);

    // Realpath containment alone would accept this alias and unlink another
    // account's map. The exact parent must be a real directory, not a symlink.
    REQUIRE_FALSE(housing_unpublish_map_files(root, "17", "Owner",
                                               "Side_7"));
    REQUIRE(file_exists(other_map));
}

TEST_CASE("Housing permits deletion before a map is first published",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *root = mkdtemp(root_template);
    REQUIRE(root != nullptr);

    housing_public_test_tree tree;
    tree.root = root;
    tree.account_dir = catpath(tree.root, "17");
    tree.by_name_dir = catpath(tree.root, "by-name");
    tree.owner_dir = catpath(tree.by_name_dir, "owner");
    tree.account_file = catpath(tree.account_dir, "Side_7.hmap");
    tree.name_file = catpath(tree.owner_dir, "Side_7.hmap");

    // A newly created map can be deleted before its first entry/publication,
    // when neither public alias nor its account/name parent exists.
    REQUIRE(housing_unpublish_map_files(tree.root, "17", "Owner", "Side_7"));
}
#endif

TEST_CASE("Package chunks can be copied without sharing storage",
          "[single-file]")
{
    package source;
    const string payload = "public Housing level payload";
    {
        chunk_writer output(&source, "D:1");
        output.write(payload.data(), payload.size());
    }
    source.commit();

    package destination;
    destination.copy_chunk_from(source, "D:1", "level");
    destination.commit();

    vector<char> copied;
    {
        chunk_reader input(&destination, "level");
        input.read_all(copied);
    }
    REQUIRE(string(copied.begin(), copied.end()) == payload);

    // Replacing the destination later cannot alter the source chunk.
    {
        chunk_writer output(&destination, "level");
        const string replacement = "changed visitor copy";
        output.write(replacement.data(), replacement.size());
    }
    destination.commit();

    vector<char> original;
    {
        chunk_reader input(&source, "D:1");
        input.read_all(original);
    }
    REQUIRE(string(original.begin(), original.end()) == payload);
}

#ifndef TARGET_OS_WINDOWS
TEST_CASE("Anonymous packages can use an explicit writable directory",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-package-directory-XXXXXX";
    char *created_root = mkdtemp(root_template);
    REQUIRE(created_root != nullptr);
    const string root = created_root;

    char original_cwd[4096];
    REQUIRE(getcwd(original_cwd, sizeof(original_cwd)) != nullptr);
    bool cleaned = false;
    unwinder restore_filesystem = [&]() {
        if (!cleaned)
        {
            const int chdir_result = chdir(original_cwd);
            const int rmdir_result = rmdir(root.c_str());
            (void) chdir_result;
            (void) rmdir_result;
        }
    };

#ifdef __linux__
    // DGAMELAUNCH likewise starts Crawl in a directory where its uid cannot
    // create files. procfs makes that property deterministic even for a test
    // process running as root.
    REQUIRE(chdir("/proc/self") == 0);
#endif

    {
        package staged(root);
        _write_test_chunk(staged, "D", "staged level");
        staged.commit();
        REQUIRE(staged.has_chunk("D"));

        package clone(root);
        clone.copy_chunk_from(staged, "D", "D");
        clone.commit();
        REQUIRE(_read_test_chunk(clone, "D") == "staged level");

        // mkstemp's directory entry is removed immediately; only the open fd
        // owns either transactional package until this scope ends.
        REQUIRE(get_dir_files_ext(root, "").empty());
    }

    REQUIRE_THROWS(package(catpath(root, "missing")));
    REQUIRE(get_dir_files_ext(root, "").empty());
    REQUIRE(chdir(original_cwd) == 0);
    REQUIRE(rmdir(root.c_str()) == 0);
    cleaned = true;
}
#endif
