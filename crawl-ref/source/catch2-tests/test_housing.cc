#include "catch_amalgamated.hpp"

#include "AppHdr.h"

#include "ability-type.h"
#include "branch.h"
#include "cloud.h"
#include "dungeon.h"
#include "env.h"
#include "files.h"
#include "housing.h"
#include "jobs.h"
#include "mapmark.h"
#include "mon-util.h"
#include "package.h"
#include "player.h"
#include "state.h"
#include "tags.h"
#include "shopping.h"
#include "unwind.h"
#include "viewgeom.h"

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

TEST_CASE("Housing ability ids remain append-only", "[single-file]")
{
    REQUIRE(static_cast<int>(ABIL_HOUSING_RETURN_HOME) == 9005);
    REQUIRE(static_cast<int>(ABIL_HOUSING_MANAGE_MAPS) == 9006);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CREATE_MONSTER) == 9007);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CALL_MERCHANT) == 9008);
    REQUIRE(static_cast<int>(ABIL_HOUSING_TRAVEL_TO_MAP) == 9009);
    REQUIRE(static_cast<int>(ABIL_HOUSING_TOGGLE_VISITOR_WALL) == 9010);
    REQUIRE(static_cast<int>(ABIL_HOUSING_MANAGE_SPAWNS) == 9011);
}

TEST_CASE("Housing snapshot schema is explicit and backwards compatible",
          "[single-file]")
{
    REQUIRE(housing_snapshot_schema_version() == 3);
    REQUIRE(housing_snapshot_schema_supported(1));
    REQUIRE(housing_snapshot_schema_supported(2));
    REQUIRE(housing_snapshot_schema_supported(3));
    REQUIRE_FALSE(housing_snapshot_schema_supported(0));
    REQUIRE_FALSE(housing_snapshot_schema_supported(4));
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
        env.grid(pos) = DNGN_METAL_WALL;
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
    const coord_def pos(20, 20);
    housing_wall_cell_fixture cell(pos);

    SECTION("exact wall opens to floor")
    {
        cell.add_wall_marker();
        REQUIRE(housing_visitor_wall_is_valid(pos));
        unwind_var<game_type> saved_game_type(crawl_state.type,
                                              GAME_TYPE_HOUSING);
        // Metal wall is generally editable, but its exact visitor-wall marker
        // protects the pair from generic terrain editing.
        REQUIRE(housing_feature_allowed(DNGN_METAL_WALL));
        REQUIRE_FALSE(housing_can_edit(pos));
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
}

TEST_CASE("Housing chargen adopts and starts on the visible template spawn",
          "[single-file]")
{
    const coord_def old_start(20, 20);
    const coord_def template_spawn(21, 20);
    const coord_def ambiguous_spawn(22, 20);
    unwind_var<game_type> saved_game_type(crawl_state.type,
                                          GAME_TYPE_HOUSING);
    unwind_var<bool> saved_on_level(you.on_current_level, true);
    unwinder restore_view = [&]() {
        crawl_view.set_player_at(you.position);
    };
    unwind_var<coord_def> saved_position(you.position, old_start);
    unwind_var<CrawlHashTable> saved_properties(env.properties);

    const dungeon_feature_type old_start_feat = env.grid(old_start);
    const dungeon_feature_type old_spawn_feat = env.grid(template_spawn);
    const dungeon_feature_type old_ambiguous_feat =
        env.grid(ambiguous_spawn);
    const unsigned short old_start_monster = env.mgrid(old_start);
    const unsigned short old_spawn_monster = env.mgrid(template_spawn);
    const unsigned short old_ambiguous_monster =
        env.mgrid(ambiguous_spawn);
    const int old_start_item = env.igrid(old_start);
    const int old_spawn_item = env.igrid(template_spawn);
    const int old_ambiguous_item = env.igrid(ambiguous_spawn);
    vector<map_marker*> old_start_markers;
    vector<map_marker*> old_spawn_markers;
    vector<map_marker*> old_ambiguous_markers;
    for (map_marker *marker : env.markers.get_markers_at(old_start))
        old_start_markers.push_back(marker->clone());
    for (map_marker *marker : env.markers.get_markers_at(template_spawn))
        old_spawn_markers.push_back(marker->clone());
    for (map_marker *marker : env.markers.get_markers_at(ambiguous_spawn))
        old_ambiguous_markers.push_back(marker->clone());
    unwinder restore_cells = [&]() {
        env.markers.remove_markers_at(old_start);
        env.markers.remove_markers_at(template_spawn);
        env.markers.remove_markers_at(ambiguous_spawn);
        for (map_marker *marker : old_start_markers)
            env.markers.add(marker);
        for (map_marker *marker : old_spawn_markers)
            env.markers.add(marker);
        for (map_marker *marker : old_ambiguous_markers)
            env.markers.add(marker);
        env.grid(old_start) = old_start_feat;
        env.grid(template_spawn) = old_spawn_feat;
        env.grid(ambiguous_spawn) = old_ambiguous_feat;
        env.mgrid(old_start) = old_start_monster;
        env.mgrid(template_spawn) = old_spawn_monster;
        env.mgrid(ambiguous_spawn) = old_ambiguous_monster;
        env.igrid(old_start) = old_start_item;
        env.igrid(template_spawn) = old_spawn_item;
        env.igrid(ambiguous_spawn) = old_ambiguous_item;
    };

    env.properties.erase("housing_spawn_points");
    env.markers.remove_markers_at(old_start);
    env.markers.remove_markers_at(template_spawn);
    env.markers.remove_markers_at(ambiguous_spawn);
    env.grid(old_start) = DNGN_FLOOR;
    env.grid(template_spawn) = DNGN_RUNELIGHT;
    env.grid(ambiguous_spawn) = DNGN_FLOOR;
    env.mgrid(old_start) = NON_MONSTER;
    env.mgrid(template_spawn) = NON_MONSTER;
    env.mgrid(ambiguous_spawn) = NON_MONSTER;
    env.igrid(old_start) = NON_ITEM;
    env.igrid(template_spawn) = NON_ITEM;
    env.igrid(ambiguous_spawn) = NON_ITEM;
    crawl_view.set_player_at(old_start);
    REQUIRE(cloud_at(template_spawn) == nullptr);

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
