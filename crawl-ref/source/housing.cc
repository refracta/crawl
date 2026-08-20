/**
 * @file
 * @brief Runtime support for the persistent Housing game mode.
 */

#include "AppHdr.h"

#include "housing.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef TARGET_OS_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "act-iter.h"
#include "actor.h"
#include "artefact.h"
#include "branch.h"
#include "cloud.h"
#include "coord.h"
#include "coordit.h"
#include "delay.h"
#include "dgn-overview.h"
#include "directn.h"
#include "end.h"
#include "env.h"
#include "errors.h"
#include "files.h"
#include "god-companions.h"
#include "god-passive.h"
#include "hiscores.h"
#include "initfile.h"
#include "item-status-flag-type.h"
#include "items.h"
#include "mapdef.h"
#include "mapmark.h"
#include "macro.h"
#include "menu.h"
#include "message.h"
#include "mgen-data.h"
#include "monster.h"
#include "mon-death.h"
#include "mon-place.h"
#include "mon-tentacle.h"
#include "mon-transit.h"
#include "mon-util.h"
#include "options.h"
#include "package.h"
#include "player.h"
#include "player-equip.h"
#include "prompt.h"
#include "random.h"
#include "state.h"
#include "stash.h"
#include "stairs.h"
#include "store.h"
#include "stringutil.h"
#include "shopping.h"
#include "syscalls.h"
#include "tags.h"
#include "target.h"
#include "teleport.h"
#include "terrain.h"
#include "tile-env.h"
#include "tileview.h"
#include "traps.h"
#include "travel.h"
#include "version.h"
#include "rltiles/tiledef-dngn.h"
#ifdef USE_TILE_WEB
#include "tileweb.h"
#endif

using std::string;
using std::vector;
using std::map;

static const char * const HOUSING_SPAWNS_KEY = "housing_spawn_points";
static const char * const HOUSING_LAST_FEATURE_KEY = "housing_last_feature";
static const char * const HOUSING_META_CHUNK = "housing_meta";
static const char * const HOUSING_LEVEL_CHUNK = "level";
static const char * const HOUSING_INDEX_CHUNK = "housing_index";
static const char * const HOUSING_MAP_CHUNK_PREFIX = "housing_map_";
static const char * const HOUSING_THEME_CHUNK_PREFIX = "housing_theme_";
static const char * const HOUSING_TEMPLATE_CHUNK = "housing_template";
static const char * const HOUSING_PORTAL_TARGET_KEY = "housing_target";
static const char * const HOUSING_LOCAL_PORTAL_KEY = "housing_portal_name";
static const char * const HOUSING_VISITOR_STRIP_KEY = "housing_visitor_strip";
static const char * const HOUSING_LOCAL_PORTAL_TILE =
    "dngn_trap_golubria";
static const char * const HOUSING_SPAWN_MARKER_KEY = "housing_spawn";
static const char * const HOUSING_VISITOR_WALL_KEY = "housing_visitor_wall";
static const char * const HOUSING_MONSTER_KEY = "housing_created_monster";
static const int HOUSING_SNAPSHOT_LEGACY_SCHEMA = 1;
// Schema 2 permits strictly validated spawn fixtures, Housing monsters and
// shops. Schema 3 adds visitor-only wall markers; older cores must reject
// these snapshots rather than loading their owner-containment walls intact.
static const int HOUSING_SNAPSHOT_WORLD_SCHEMA = 2;
static const int HOUSING_SNAPSHOT_WALL_SCHEMA = 3;
// Schema 4 renders owner-only barriers as translucent permarock. Readers keep
// accepting schema-3 metal barriers so existing homes can migrate them, while
// older cores reject newly published translucent barriers before loading.
static const int HOUSING_SNAPSHOT_WALL_CURRENT_SCHEMA = 4;
// Schema 5 adds persistent named same-level passages and visitor-only
// inventory stripping tiles. Their inert rollback-safe terrain substrates do
// not corrupt old canonical saves, while the public metadata gate prevents an
// already-running older visitor process from silently ignoring their roles.
static const int HOUSING_SNAPSHOT_SCHEMA = 5;
static const int HOUSING_MAX_MONSTERS = 64;
static const int HOUSING_MAX_SHOPS = 32;
static const int HOUSING_MAX_LOCAL_PORTALS = 128;
static const int HOUSING_INDEX_SCHEMA = 1;
static const int HOUSING_THEME_SCHEMA = 1;
static const int HOUSING_MAX_MAPS = 64;

struct housing_theme_def
{
    const char *name;
    colour_t floor_colour;
    colour_t rock_colour;
    const char *floor_tile;
    const char *rock_tile;
};

static const housing_theme_def HOUSING_THEMES[] =
{
    // Indices 0-7 are persisted in existing multi-map saves. Never reorder
    // them; new themes are append-only and the separate menu order below
    // presents the natural Crawl branch progression.
    { "Dungeon", LIGHTGREY, BROWN, "floor_normal", "wall_normal" },
    { "Lair of Beasts", GREEN, BROWN, "floor_lair", "wall_lair" },
    { "Orcish Mines", BROWN, BROWN, "floor_orc", "wall_orc" },
    { "Swamp", BROWN, GREEN, "floor_swamp", "wall_swamp" },
    { "Vaults", LIGHTGREY, WHITE, "floor_vault", "wall_vault" },
    { "Crypt", DARKGREY, LIGHTGREY, "floor_tomb", "wall_crypt" },
    { "Depths", LIGHTGREY, YELLOW,
      "floor_depthstone", "wall_depths_crystal" },
    { "Realm of Zot", MAGENTA, LIGHTMAGENTA,
      "floor_zot_diamonds", "wall_zot_magenta" },
    { "Ecumenical Temple", LIGHTGREY, BROWN,
      "floor_vines", "wall_vines" },
    { "Elven Halls", WHITE, LIGHTMAGENTA,
      "floor_hall", "wall_hall" },
    { "Shoals", BROWN, BROWN,
      "floor_sand", "wall_shoals" },
    { "Snake Pit", LIGHTGREEN, YELLOW,
      "floor_mosaic", "wall_snake" },
    { "Spider Nest", BROWN, YELLOW,
      "floor_spider", "wall_spider" },
    { "Slime Pits", BROWN, BROWN,
      "floor_slime", "wall_slime" },
    { "Tomb of the Ancients", BROWN, BROWN,
      "floor_tomb", "wall_undead" },
    { "Vestibule of Hell", LIGHTGREY, LIGHTRED,
      "floor_cage", "wall_hell" },
    { "Iron City of Dis", CYAN, BROWN,
      "floor_iron", "wall_zot_cyan" },
    { "Gehenna", BROWN, RED,
      "floor_rough_red", "wall_zot_red" },
    { "Cocytus", LIGHTBLUE, LIGHTCYAN,
      "floor_frozen", "wall_ice" },
    { "Tartarus", MAGENTA, MAGENTA,
      "floor_black_cobalt", "wall_cobalt_rock" },
    // These branches normally choose dynamic per-level colours. Housing uses
    // one stable representative preset for each saved map theme.
    { "Abyss", LIGHTGREY, LIGHTRED,
      "floor_nerves_lightgray", "wall_abyss_lightred" },
    { "Pandemonium", RED, YELLOW,
      "floor_demonic_red", "wall_bars_yellow" },
    { "Ziggurat", LIGHTGREY, BROWN,
      "floor_etched", "wall_vault" },
    { "Bazaar", BLUE, YELLOW,
      "floor_vault", "wall_vault" },
    { "Trove", DARKGREY, BLUE,
      "floor_vault", "wall_vault" },
    { "Sewer", LIGHTGREY, BLUE,
      "floor_slime", "wall_oozing" },
    { "Ossuary", WHITE, YELLOW,
      "floor_sandstone", "wall_sandstone" },
    { "Bailey", WHITE, LIGHTRED,
      "floor_cobble_blood", "wall_brick_brown" },
    { "Ice Cave", BLUE, WHITE,
      "floor_ice", "wall_ice_block" },
    { "Volcano", RED, RED,
      "floor_rough_red", "wall_volcanic" },
    // Wizlabs choose their appearance per vault. This is the representative
    // mystic preset used by Housing.
    { "Wizard's Laboratory", MAGENTA, LIGHTMAGENTA,
      "floor_mystic_chasm", "wall_zot_magenta" },
    { "Desolation of Salt", LIGHTGREY, BROWN,
      "floor_salt", "wall_desolation" },
    { "Gauntlet", LIGHTGREY, BROWN,
      "floor_gauntlet", "wall_lab_rock" },
    { "Arena", LIGHTGREY, CYAN,
      "floor_normal", "wall_normal" },
    { "Crucible of Flesh", LIGHTRED, RED,
      "floor_cage", "wall_crucible" },
    { "Necropolis", MAGENTA, LIGHTGREY,
      "floor_necropolis_squares", "wall_catacombs" },
    { "Gulch", GREEN, LIGHTBLUE,
      "floor_gulch", "wall_gulch_brick" },
};

static const int HOUSING_THEME_MENU_ORDER[] =
{
    // Match Crawl's logical branch order, excluding the four retired TAG 34
    // compatibility branches (Dwarf, Blade, Forest and Labyrinth).
    0, 8, 1, 3, 10, 11, 12, 13, 2, 9, 4, 5, 14, 6, 15, 16, 17,
    18, 19, 7, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36,
};
static_assert(ARRAYSZ(HOUSING_THEME_MENU_ORDER) == ARRAYSZ(HOUSING_THEMES),
              "Every Housing branch theme needs one menu entry");
// The generic chargen map popup reserves uppercase X for immediate exit, so
// keep Housing within a-z,A-W unless that UI learns to skip reserved keys.
static_assert(ARRAYSZ(HOUSING_THEMES) <= 49,
              "Housing branch hotkeys are limited to a-z and A-W");
static int _housing_turn_origin = -1;
static bool _housing_runtime_initialized = false;
static bool _housing_started_as_visitor = false;
static housing_role_type _housing_runtime_role = housing_role_type::none;
// The active package is owned by `you`. The owner package is closed while
// visiting; on return it is reopened briefly and transferred to `you` by the
// ordinary restore path.
static package *_housing_owner_save = nullptr;
static package *_housing_visitor_save = nullptr;
static package *_housing_restore_save = nullptr;
// While an owner map is being loaded, `you.save` points at an anonymous
// staged clone and this keeps the previously committed canonical package.
// Only a fully loaded map is promoted back into the canonical save.
static package *_housing_owner_promotion_save = nullptr;
static package *_housing_staged_owner_save = nullptr;
// A failed in-process owner-map replacement restarts the ordinary game loop
// with the last committed canonical package. Keep this token separate from
// the staged-promotion pointer: _reset_game() clears the player, and WebTiles
// has already re-read the rc file (which clears Options.game) by this point.
// Without an explicit restore token startup can fall through to chargen.
static bool _housing_owner_rollback_restore_pending = false;
// A failed visitor replacement must likewise restart from the exact previous
// disposable package. This token keeps that package out of save discovery and
// chargen after _reset_game clears the runtime game type.
static bool _housing_visitor_rollback_restore_pending = false;
// True only when a staged owner restore actually replaces canonical D with a
// different named map. Returning from a visit to the already-active map must
// preserve its exact saved position and map-bound character references.
static bool _housing_owner_map_changed = false;
// A legacy single-map owner package is staged before housing_finish_map_entry
// upgrades its floor spawn to an authenticated fixture. Refresh the hidden
// template from the post-load D during promotion instead of preserving the
// stale pre-fixture bytes staged for compatibility.
static bool _housing_owner_template_needs_refresh = false;
static string _housing_canonical_save_path;
// The player object is cleared during an in-process restart and a staged
// restore can fail before TAG_YOU repopulates it. Keep the authenticated owner
// identity across that boundary for the one canonical rollback attempt.
static string _housing_restore_name;
static bool _housing_skip_next_checkpoint = false;
static string _housing_current_map_id;
static string _housing_current_map_owner;
static string _housing_pending_notice;
static bool _housing_entry_requires_spawn = false;
static bool _housing_map_entry_finished = false;
// A failed in-process replacement restores the package that was checkpointed
// immediately before the attempt. Its TAG_YOU and D are already paired, so
// cleanup, respawning and resetting the displayed map clock would all make a
// supposedly rolled-back session observably different.
static bool _housing_exact_map_rollback = false;

static const char * const _visitor_start_failure =
    "The Housing visit could not be started safely.";
static const char * const _owner_return_failure =
    "Your canonical Housing home is unavailable.";

static string _lowercase_ascii(string value);
static string _getenv_string(const char *name);
static bool _ensure_owner_map_storage();
static bool _housing_shop_can_be_published(const coord_def &pos);
static bool _current_visitor_wall_marker_at(const coord_def &pos);
static bool _portal_target_at(const coord_def &pos, string *target);
static bool _local_portal_name_at(const coord_def &pos, string *portal_name);
static bool _visitor_strip_marker_at(const coord_def &pos);
static void _restore_housing_fixture_tile_overrides();
static void _promote_staged_owner_map(bool exact_rollback = false,
                                      int rollback_turn_origin = -1);
static void _open_visitor_only_walls();
static bool _move_to_spawn(const vector<coord_def> &spawns,
                           bool secure_choice);

static void _initialize_housing_runtime()
{
    if (_housing_runtime_initialized || !crawl_state.game_is_housing())
        return;

    _housing_runtime_initialized = true;
    const string role = _lowercase_ascii(
        _getenv_string("CRAWL_HOUSING_ROLE"));
    _housing_runtime_role = role.empty() || role == "owner"
                                ? housing_role_type::owner
                                : housing_role_type::visitor;
    _housing_started_as_visitor =
        _housing_runtime_role == housing_role_type::visitor;
    _housing_current_map_id = _getenv_string("CRAWL_HOUSING_TARGET_MAP_ID");
    _housing_current_map_owner =
        _getenv_string("CRAWL_HOUSING_TARGET_OWNER");
    if (_housing_runtime_role == housing_role_type::owner)
    {
        _housing_current_map_id = _getenv_string("CRAWL_HOUSING_MAP_ID");
        if (_housing_current_map_id.empty())
            _housing_current_map_id = "main";
    }
}

static string _housing_save_level_chunk()
{
    // Housing is a one-level branch, whose save chunk is "D" rather than
    // "D:1". During visitor restore this runs before TAG_YOU has restored the
    // branch depths, so force the suffix off instead of consulting brdepth.
    return level_id(BRANCH_DUNGEON, 1).describe(false, false);
}

static string _lowercase_ascii(string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

static string _getenv_string(const char *name)
{
    const char *value = getenv(name);
    return value ? value : "";
}

static bool _is_decimal_id(const string &value)
{
    if (value.empty() || value.size() > 20)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}

static bool _is_ascii_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z');
}

static bool _is_map_id(const string &value)
{
    if (value.empty() || value.size() > 20)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return _is_ascii_alnum(c) || c == '_';
    });
}

bool housing_valid_map_id(const string &map_id)
{
    return _is_map_id(map_id);
}

string housing_map_chunk_name(const string &map_id)
{
    return _is_map_id(map_id) ? HOUSING_MAP_CHUNK_PREFIX + map_id : "";
}

static string _housing_map_theme_chunk_name(const string &map_id)
{
    return _is_map_id(map_id) ? HOUSING_THEME_CHUNK_PREFIX + map_id : "";
}

static bool _valid_housing_theme(int theme)
{
    return theme >= 0 && theme < static_cast<int>(ARRAYSZ(HOUSING_THEMES));
}

int housing_branch_theme_count()
{
    return ARRAYSZ(HOUSING_THEMES);
}

const char *housing_branch_theme_name(int stable_id)
{
    return _valid_housing_theme(stable_id) ? HOUSING_THEMES[stable_id].name
                                           : nullptr;
}

int housing_branch_theme_menu_id(int position)
{
    return position >= 0
           && position < static_cast<int>(ARRAYSZ(HOUSING_THEME_MENU_ORDER))
           ? HOUSING_THEME_MENU_ORDER[position] : -1;
}

bool housing_branch_theme_catalog_valid()
{
    bool seen[ARRAYSZ(HOUSING_THEMES)] = {};
    for (int position = 0;
         position < static_cast<int>(ARRAYSZ(HOUSING_THEME_MENU_ORDER));
         ++position)
    {
        const int theme = HOUSING_THEME_MENU_ORDER[position];
        if (!_valid_housing_theme(theme) || seen[theme])
            return false;
        seen[theme] = true;
    }

    for (int theme = 0; theme < housing_branch_theme_count(); ++theme)
    {
        tileidx_t floor;
        tileidx_t rock;
        const housing_theme_def &definition = HOUSING_THEMES[theme];
        if (!seen[theme] || !definition.name || !*definition.name
            || !tile_dngn_index(definition.floor_tile, &floor)
            || !tile_dngn_index(definition.rock_tile, &rock))
        {
            return false;
        }
    }
    return true;
}

static void _write_housing_map_theme(package &save, const string &map_id,
                                     int theme)
{
    const string chunk = _housing_map_theme_chunk_name(map_id);
    if (chunk.empty() || !_valid_housing_theme(theme))
        fail("invalid Housing map theme");
    writer output(&save, chunk);
    marshallInt(output, HOUSING_THEME_SCHEMA);
    marshallInt(output, theme);
}

// Return -1 for maps created before per-map branch themes existed. A present
// chunk is authoritative and fails closed if malformed.
static int _read_housing_map_theme(package &save, const string &map_id)
{
    const string chunk = _housing_map_theme_chunk_name(map_id);
    if (chunk.empty() || !save.has_chunk(chunk))
        return -1;

    reader input(&save, chunk);
    input.set_safe_read(true);
    const int schema = unmarshallInt(input);
    const int theme = unmarshallInt(input);
    // Stable append-only ids make an expanded theme fail closed on an older
    // core whose catalog ends at 7, while legacy ids remain rollback-safe.
    if (schema != HOUSING_THEME_SCHEMA || !_valid_housing_theme(theme))
        corrupted("Housing map has an invalid branch theme");
    return theme;
}

static void _apply_housing_map_theme(int theme)
{
    if (!_valid_housing_theme(theme))
        fail("invalid Housing map theme");
    const housing_theme_def &definition = HOUSING_THEMES[theme];
    tileidx_t floor;
    tileidx_t rock;
    if (!tile_dngn_index(definition.floor_tile, &floor)
        || !tile_dngn_index(definition.rock_tile, &rock))
    {
        fail("Housing branch theme uses an unknown tile");
    }

    env.floor_colour = definition.floor_colour;
    env.rock_colour = definition.rock_colour;
    tile_env.default_flavour.floor = floor;
    tile_env.default_flavour.floor_idx =
        store_tilename_get_index(definition.floor_tile);
    tile_env.default_flavour.wall = rock;
    tile_env.default_flavour.wall_idx =
        store_tilename_get_index(definition.rock_tile);
    tile_clear_flavour();
    tile_init_flavour();
    // Applying a branch theme deliberately resets every per-cell flavour.
    // Reapply only exact authenticated Housing overrides so named passages
    // keep their Golubria appearance across owner save/reload.
    _restore_housing_fixture_tile_overrides();
}

static void _copy_package_chunk(package &save, const string &source,
                                const string &destination)
{
    if (source.empty() || destination.empty() || source == destination
        || !save.has_chunk(source))
    {
        fail("invalid Housing map chunk copy");
    }

    vector<char> data;
    {
        chunk_reader input(&save, source);
        input.read_all(data);
    }
    if (data.empty())
        corrupted("Housing map chunk \"%s\" is empty", source.c_str());

    chunk_writer output(&save, destination);
    output.write(data.data(), data.size());
}

static bool _package_chunks_equal(package &save, const string &left,
                                  const string &right)
{
    if (!save.has_chunk(left) || !save.has_chunk(right))
        return false;

    vector<char> left_data;
    vector<char> right_data;
    {
        chunk_reader input(&save, left);
        input.read_all(left_data);
    }
    {
        chunk_reader input(&save, right);
        input.read_all(right_data);
    }
    return left_data == right_data;
}

bool housing_legacy_template_is_exact_clone(package &save,
                                            const string &map_id)
{
    const string named_map = housing_map_chunk_name(map_id);
    return !named_map.empty()
        && save.has_chunk(_housing_save_level_chunk())
        && save.has_chunk(named_map)
        && save.has_chunk(HOUSING_TEMPLATE_CHUNK)
        && _package_chunks_equal(save, _housing_save_level_chunk(), named_map)
        && _package_chunks_equal(save, named_map, HOUSING_TEMPLATE_CHUNK);
}

static void _validate_map_index(package &save, const vector<string> &maps,
                                const string &current_map)
{
    if (maps.empty() || maps.size() > HOUSING_MAX_MAPS
        || !_is_map_id(current_map))
    {
        corrupted("Housing map index has invalid bounds");
    }

    std::set<string> unique_maps;
    bool has_main = false;
    bool has_current = false;
    for (const string &map_id : maps)
    {
        const string chunk = housing_map_chunk_name(map_id);
        if (chunk.empty() || !unique_maps.insert(map_id).second
            || !save.has_chunk(chunk))
        {
            corrupted("Housing map index contains an invalid map");
        }
        has_main |= map_id == "main";
        has_current |= map_id == current_map;
    }
    if (!has_main || !has_current)
        corrupted("Housing map index is missing main or its active map");
}

bool housing_read_map_index(package &save, vector<string> &maps,
                            string &current_map)
{
    maps.clear();
    current_map.clear();
    if (!save.has_chunk(HOUSING_INDEX_CHUNK))
        return false;

    reader input(&save, HOUSING_INDEX_CHUNK);
    input.set_safe_read(true);
    const int schema = unmarshallInt(input);
    if (schema != HOUSING_INDEX_SCHEMA)
        corrupted("Housing map index has an unsupported schema");

    current_map = unmarshallString(input);
    const int count = unmarshallInt(input);
    if (count < 1 || count > HOUSING_MAX_MAPS)
        corrupted("Housing map index has an invalid map count");
    for (int i = 0; i < count; ++i)
        maps.push_back(unmarshallString(input));
    input.fail_if_not_eof(HOUSING_INDEX_CHUNK);
    _validate_map_index(save, maps, current_map);
    return true;
}

void housing_write_map_index(package &save, const vector<string> &maps,
                             const string &current_map)
{
    _validate_map_index(save, maps, current_map);
    writer output(&save, HOUSING_INDEX_CHUNK);
    marshallInt(output, HOUSING_INDEX_SCHEMA);
    marshallString(output, current_map);
    marshallInt(output, maps.size());
    for (const string &map_id : maps)
        marshallString(output, map_id);
}

static bool _is_private_housing_save_chunk(const string &chunk)
{
    const string map_prefix = HOUSING_MAP_CHUNK_PREFIX;
    const string theme_prefix = HOUSING_THEME_CHUNK_PREFIX;
    return chunk == _housing_save_level_chunk()
        || chunk == HOUSING_INDEX_CHUNK
        || chunk == HOUSING_TEMPLATE_CHUNK
        || chunk.compare(0, map_prefix.size(), map_prefix) == 0
        || chunk.compare(0, theme_prefix.size(), theme_prefix) == 0;
}

void housing_copy_visitor_state(package &source, package &destination)
{
    for (const string &chunk : source.list_chunks())
        if (!_is_private_housing_save_chunk(chunk))
            destination.copy_chunk_from(source, chunk, chunk);
}

static bool _is_account_name(const string &value)
{
    if (value.size() < 3 || value.size() > 20)
        return false;
    return std::all_of(value.begin(), value.end(), _is_ascii_alnum);
}

bool housing_valid_map_target(const string &target)
{
    const size_t colon = target.find(':');
    return colon != string::npos && colon == target.rfind(':')
        && _is_account_name(target.substr(0, colon))
        && _is_map_id(target.substr(colon + 1));
}

static string _path_without_trailing_separators(string path)
{
    path = canonicalise_file_separator(path);
    while (path.size() > 1 && path.back() == FILE_SEPARATOR)
        path.pop_back();
    return path;
}

static bool _path_is_within(const string &filename, const string &directory)
{
    string file = _path_without_trailing_separators(filename);
    string dir = _path_without_trailing_separators(directory);

#ifndef TARGET_OS_WINDOWS
    char *real_file = realpath(file.c_str(), nullptr);
    char *real_dir = realpath(dir.c_str(), nullptr);
    if (!real_file || !real_dir)
    {
        free(real_file);
        free(real_dir);
        return false;
    }
    file = real_file;
    dir = real_dir;
    free(real_file);
    free(real_dir);
#else
    // realpath() gives the deployed Unix build a symlink-aware containment
    // check. A string-only Windows fallback would accept `..` or junction
    // escapes, so keep visitor restore disabled there until an equivalent
    // handle-based canonicalisation is implemented.
    UNUSED(file);
    UNUSED(dir);
    return false;
#endif

    return file.size() > dir.size()
        && file.compare(0, dir.size(), dir) == 0
        && file[dir.size()] == FILE_SEPARATOR;
}

static NORETURN void _abort_visitor_start(package *save)
{
    if (save)
    {
        save->abort();
        delete save;
    }
    game_ended(game_exit::abort, _visitor_start_failure);
}

struct housing_snapshot_meta
{
    int schema = 0;
    string crawl_version;
    string account_id;
    string owner_name;
    string map_id;
};

static housing_snapshot_meta _read_snapshot_meta(package &snapshot)
{
    if (!snapshot.has_chunk(HOUSING_META_CHUNK)
        || !snapshot.has_chunk(HOUSING_LEVEL_CHUNK))
    {
        fail("Housing snapshot is missing required chunks");
    }

    vector<string> chunks = snapshot.list_chunks();
    std::sort(chunks.begin(), chunks.end());
    const vector<string> expected = { HOUSING_META_CHUNK,
                                      HOUSING_LEVEL_CHUNK };
    if (chunks != expected)
        fail("Housing snapshot contains unexpected chunks");

    reader input(&snapshot, HOUSING_META_CHUNK);
    input.set_safe_read(true);
    housing_snapshot_meta meta;
    meta.schema = unmarshallInt(input);
    meta.crawl_version = unmarshallString(input);
    meta.account_id = unmarshallString(input);
    meta.owner_name = unmarshallString(input);
    meta.map_id = unmarshallString(input);
    input.fail_if_not_eof(HOUSING_META_CHUNK);
    return meta;
}

static bool _supported_snapshot_schema(int schema)
{
    return schema == HOUSING_SNAPSHOT_LEGACY_SCHEMA
           || schema == HOUSING_SNAPSHOT_WORLD_SCHEMA
           || schema == HOUSING_SNAPSHOT_WALL_SCHEMA
           || schema == HOUSING_SNAPSHOT_WALL_CURRENT_SCHEMA
           || schema == HOUSING_SNAPSHOT_SCHEMA;
}

int housing_snapshot_schema_version()
{
    return HOUSING_SNAPSHOT_SCHEMA;
}

bool housing_snapshot_schema_supported(int schema)
{
    return _supported_snapshot_schema(schema);
}

static void _write_snapshot_meta(package &snapshot, const string &account_id,
                                 const string &map_id)
{
    // Keep maps without new fixtures readable by already-running processes
    // during a rolling deployment. Translucent walls require schema 4, while
    // named passages and visitor inventory strips require schema 5.
    int schema = HOUSING_SNAPSHOT_WALL_SCHEMA;
    for (map_marker *marker : env.markers.get_all())
    {
        if (_local_portal_name_at(marker->pos, nullptr)
            || _visitor_strip_marker_at(marker->pos))
        {
            schema = HOUSING_SNAPSHOT_SCHEMA;
            break;
        }
        if (_current_visitor_wall_marker_at(marker->pos))
            schema = HOUSING_SNAPSHOT_WALL_CURRENT_SCHEMA;
    }
    writer output(&snapshot, HOUSING_META_CHUNK);
    marshallInt(output, schema);
    marshallString(output, Version::Long);
    marshallString(output, account_id);
    marshallString(output, you.your_name);
    marshallString(output, map_id);
}

housing_role_type housing_current_role()
{
    if (!crawl_state.game_is_housing())
        return housing_role_type::none;
    _initialize_housing_runtime();
    return _housing_runtime_role;
}

const char *housing_role_name()
{
    switch (housing_current_role())
    {
    case housing_role_type::owner:
        return "owner";
    case housing_role_type::visitor:
        return "visitor";
    case housing_role_type::none:
    default:
        return "none";
    }
}

bool housing_is_owner()
{
    return housing_current_role() == housing_role_type::owner;
}

bool housing_is_visitor()
{
    return housing_current_role() == housing_role_type::visitor;
}

void housing_enforce_explore_mode()
{
    if (!crawl_state.game_is_housing())
        return;

    Options.explore_mode = WIZ_NEVER;
    you.explore = false;
}

const string &housing_current_map_id()
{
    _initialize_housing_runtime();
    return _housing_current_map_id;
}

string housing_place()
{
    if (!crawl_state.game_is_housing())
        return "";
    _initialize_housing_runtime();
    const string &owner = housing_is_owner() ? you.your_name
                                              : _housing_current_map_owner;
    if (owner.empty() || !_is_map_id(_housing_current_map_id))
        return "";
    return owner + ":" + _housing_current_map_id;
}

static bool _ensure_owner_map_storage()
{
    if (!housing_is_owner() || !you.save)
        return false;

    vector<string> maps;
    string current_map;
    if (housing_read_map_index(*you.save, maps, current_map))
    {
        if (!you.save->has_chunk(HOUSING_TEMPLATE_CHUNK))
        {
            // Saves created before the pristine template was introduced use
            // their existing main map as the least-surprising safe fallback.
            _copy_package_chunk(*you.save, housing_map_chunk_name("main"),
                                HOUSING_TEMPLATE_CHUNK);
        }
        _housing_current_map_id = current_map;
        return true;
    }

    // Legacy Housing saves have exactly one loader-facing level chunk. Promote
    // it to the canonical `main` map without regenerating or discarding any
    // terrain, items, monsters, portals, or spawn metadata.
    const string level_chunk = _housing_save_level_chunk();
    if (you.on_current_level)
    {
        // Level generation may already have written D before
        // housing_finish_map_entry() authenticates the visible spawn fixture.
        // Always refresh the loaded level here so main and the pristine
        // template include the post-housing_ensure_level() properties/marker.
        save_level(level_id::current());
    }
    else if (!you.save->has_chunk(level_chunk))
        corrupted("Legacy Housing save is missing its main map");
    _copy_package_chunk(*you.save, level_chunk,
                        housing_map_chunk_name("main"));
    _copy_package_chunk(*you.save, level_chunk, HOUSING_TEMPLATE_CHUNK);
    maps.push_back("main");
    housing_write_map_index(*you.save, maps, "main");
    _housing_current_map_id = "main";
    return true;
}

package *housing_open_save_for_restore(const string &filename)
{
    _initialize_housing_runtime();

    if (_housing_restore_save)
    {
        package *save = _housing_restore_save;
        _housing_restore_save = nullptr;
        // During an in-process restore, _reset_game() has cleared the game
        // type and player. The transaction token, not housing_is_owner(), is
        // therefore the authority until TAG_YOU restores crawl_state. A
        // visitor rollback deliberately remains disposable/read-only.
        if (housing_owner_restore_pending()
            || (!_housing_visitor_rollback_restore_pending
                && housing_is_owner()))
        {
            // A staged owner-map restore deliberately keeps the canonical
            // package separate until the replacement level and spawn are
            // known-good. Otherwise this is an ordinary canonical restart.
            if (!_housing_owner_promotion_save)
                _housing_owner_save = save;
            _housing_visitor_save = nullptr;
        }
        else
        {
            _housing_owner_save = nullptr;
            _housing_visitor_save = save;
        }
        return save;
    }

    if (!housing_is_visitor())
    {
        package *save = new package(filename.c_str(), true);
        _housing_owner_save = save;
        _housing_canonical_save_path = filename;
        return save;
    }

    // This check happens before the save is opened writable. It is a second
    // line of defence behind WebTiles' locked, new-inode session copy: a bad
    // -dir or forged environment must never turn the canonical save into the
    // visitor's destination package.
    const string session_dir = _getenv_string("CRAWL_HOUSING_SESSION_DIR");
    const string snapshot_path = _getenv_string("CRAWL_HOUSING_SNAPSHOT");
    const string own_account_id =
        _getenv_string("CRAWL_HOUSING_ACCOUNT_ID");
    const string target_account_id =
        _getenv_string("CRAWL_HOUSING_TARGET_ACCOUNT_ID");
    const string target_owner =
        _getenv_string("CRAWL_HOUSING_TARGET_OWNER");
    const string target_map =
        _getenv_string("CRAWL_HOUSING_TARGET_MAP_ID");

    if (session_dir.empty() || snapshot_path.empty()
        || !_is_decimal_id(own_account_id)
        || !_is_decimal_id(target_account_id)
        || own_account_id == target_account_id
        || !_is_account_name(target_owner) || !_is_map_id(target_map)
        || !_path_is_within(filename, session_dir)
        || !_path_is_within(snapshot_path, session_dir))
    {
        _abort_visitor_start(nullptr);
    }

    package *save = nullptr;
    try
    {
        save = new package(filename.c_str(), true);
        package snapshot(snapshot_path.c_str(), false);
        const housing_snapshot_meta meta = _read_snapshot_meta(snapshot);
        if (!_supported_snapshot_schema(meta.schema)
            || meta.account_id != target_account_id
            || meta.map_id != target_map
            || _lowercase_ascii(meta.owner_name)
                != _lowercase_ascii(target_owner)
            // The tagged level chunk carries the authoritative save format
            // version. Do not require an exact Version::Long match here: that
            // would invalidate every public map on compatible nightly builds.
            || meta.crawl_version.empty())
        {
            fail("Housing snapshot metadata does not match the request");
        }

        // Keep every character/global chunk from this visitor session and
        // replace only the map. Strip canonical multi-map archives from the
        // disposable package before its first restore as well as on every
        // in-process visitor handoff. HP/inventory/etc. remain portable, while
        // private owner levels are never decompressed into another capsule.
        for (const string &chunk : save->list_chunks())
            if (_is_private_housing_save_chunk(chunk))
                save->delete_chunk(chunk);
        save->copy_chunk_from(snapshot, HOUSING_LEVEL_CHUNK,
                              _housing_save_level_chunk());
        save->commit();
    }
    catch (const game_ended_condition&)
    {
        if (save)
        {
            save->abort();
            delete save;
        }
        throw;
    }
    catch (...)
    {
        _abort_visitor_start(save);
    }

    _housing_visitor_save = save;
    return save;
}

void housing_normalize_legacy_delver_depth()
{
    // Before Housing forced Delvers to its sole D:1 level, their character
    // location was saved as D:5 while the same save correctly recorded the
    // Housing branch depth as 1. Repair only that known legacy tuple; other
    // out-of-range locations should still fail the normal save validation.
    if (crawl_state.game_is_housing()
        && you.char_class == JOB_DELVER
        && you.where_are_you == BRANCH_DUNGEON
        && you.depth == 5
        && brdepth[BRANCH_DUNGEON] == 1)
    {
        you.depth = 1;
    }
}

static void _scrub_housing_map_transition_state()
{
    // These references belong to the previous map, not to the portable player
    // state. This hook runs after the outgoing checkpoint and before TAG_LEVEL
    // replaces the level, for both owner and visitor transitions.
    you.clear_constricted();
    // Do not use clear_beholders() here: it grants a fresh random mesmerise
    // immunity and would mutate otherwise portable visitor state on every hop.
    you.beholders.clear();
    you.duration[DUR_MESMERISED] = 0;
    you.fearmongers.clear();
    you.duration[DUR_AFRAID] = 0;
    you.level_stack.clear();
    you.prev_targ = MID_NOBODY;
    if (you.pet_target != MHITYOU)
        you.pet_target = MHITNOT;

    // These player properties point at actors in the outgoing level. Imported
    // Housing monsters use another owner's MID namespace, so retaining any of
    // them could bind a spell, familiar, or god effect to an unrelated actor.
    you.duration[DUR_DIMENSIONAL_BULLSEYE] = 0;
    you.props.erase(BULLSEYE_TARGET_KEY);
    you.props.erase(BATTLESPHERE_KEY);
    you.props.erase("TELEPORTITIS_SOURCE");
    you.props.erase(DITH_SHADOW_MID_KEY);
    you.props.erase(DITH_SHADOW_LAST_TARGET_KEY);
    you.props.erase(DITH_SHADOW_ATTACK_KEY);
    you.props.erase(DITH_SHADOW_SPELLPOWER_KEY);
    you.props.erase(SOLAR_EMBER_MID_KEY);
    you.props.erase(SOLAR_EMBER_REVIVAL_KEY);
    you.props.erase("canine_familiar_mid");
    if (you.props.exists(WATER_HOLDER_KEY))
        you.props[WATER_HOLDER_KEY].get_int() = MID_NOBODY;
    for (item_def &item : you.inv)
        item.props.erase(SPECTRAL_WEAPON_KEY);

    companion_list.clear();
    the_lost_ones.clear();
    const level_id housing_level(BRANCH_DUNGEON, 1);
    StashTrack.remove_level(housing_level);
    shopping_list.del_things_from(housing_level);
    travel_cache.erase_level_info(housing_level);
    travel_cache.flush_invalid_waypoints();
    overview_clear();
    you.entering_level = false;
}

static bool _housing_transition_changes_map()
{
    return !_housing_exact_map_rollback
        && (housing_is_visitor()
            || (_housing_owner_promotion_save && _housing_owner_map_changed));
}

void housing_prepare_map_transition_restore(bool known_map_change)
{
    if (!known_map_change && !_housing_transition_changes_map())
        return;

    // This is the restore-safe equivalent of the ordinary stairs departure
    // boundary. It runs after TAG_YOU is available but before target
    // TAG_LEVEL, so every helper still sees the outgoing/empty actor table and
    // no delayed reset can bind to an imported target MID.
    stop_delay(true, true);
    heal_flayed_effect(&you, true, true);
    clear_level_bound_player_state(true);
    _scrub_housing_map_transition_state();
    drop_pending_monster_resets();
    crawl_state.potential_pursuers.clear();
    you.position.reset();
}

void housing_prepare_loaded_level()
{
    // Clear optional-death exploration at the TAG_LEVEL boundary, before
    // generic level rescue, marker activation, and redraw. Normal owner
    // restores do not use the early map-entry hook below, so an old save must
    // be sanitised here as well as after rc options are reread during startup.
    housing_enforce_explore_mode();

    if (!crawl_state.game_is_housing() || !housing_is_visitor())
        return;

    // Owner containment walls are data-only fixtures. Remove them before
    // marker activation, travel initialisation and the first redraw so a
    // visitor never observes or collides with the owner-only barrier.
    _open_visitor_only_walls();

    // Public monsters retain the publisher's MID namespace. Do this at the
    // common TAG_LEVEL boundary, including the first URL visitor restore, so
    // no marker/startup hook can allocate a colliding actor id.
    for (const monster &mons : menv_real)
        if (mons.alive())
            you.last_mid = std::max(you.last_mid, mons.mid);
}

static string _housing_points_path()
{
    const char *override_path = getenv("CRAWL_HOUSING_POINTS_FILE");
    if (override_path && *override_path)
        return override_path;

    string account = strip_filename_unsafe_chars(you.your_name);
    if (account.empty())
        account = "unknown";

    return catpath(catpath(Options.shared_dir, "housing-points"),
                   account + ".points");
}

int housing_points()
{
    // The points file is intentionally opened for every call so an external
    // service can change the balance while the Crawl process is running.
    FILE *points_file = fopen_u(_housing_points_path().c_str(), "r");
    if (!points_file)
        return 0;

    char buffer[128];
    const bool read_ok = fgets(buffer, sizeof(buffer), points_file) != nullptr;
    fclose(points_file);
    if (!read_ok)
        return 0;

    errno = 0;
    char *end = nullptr;
    const long long value = strtoll(buffer, &end, 10);
    if (errno || end == buffer)
        return 0;

    while (*end && std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (*end || value <= 0)
        return 0;
    return value > INT_MAX ? INT_MAX : static_cast<int>(value);
}

bool housing_authorize_points(int cost)
{
    const int balance = housing_points();
    return housing_is_owner() && cost >= 0 && balance >= cost;
}

bool housing_authorize_action(const char *action, int cost)
{
    // Keep the read in this public authorization boundary even while all MVP
    // costs are zero. That makes live external balance changes observable as
    // soon as non-zero costs are enabled.
    const int balance = housing_points();
    if (!housing_is_owner())
    {
        mpr("Only the owner can perform housing actions.");
        return false;
    }
    if (cost < 0 || balance < cost)
    {
        mprf("You need %d housing point%s to %s (available: %d).",
             std::max(cost, 0), cost == 1 ? "" : "s",
             action && *action ? action : "do that", balance);
        return false;
    }
    return true;
}

static bool _valid_spawn(const coord_def &pos)
{
    if (!map_bounds(pos))
        return false;
    const dungeon_feature_type feat = env.grid(pos);
    // Never let species-specific habitat rules decide whether a persisted
    // respawn is safe. Legacy floor points are upgraded to a visible runelight
    // by housing_ensure_level(); all new points use runelight directly.
    return feat == DNGN_FLOOR || feat == DNGN_RUNELIGHT;
}

static const char * const HOUSING_FIXTURE_KEYS[] =
{
    HOUSING_PORTAL_TARGET_KEY,
    HOUSING_LOCAL_PORTAL_KEY,
    HOUSING_VISITOR_STRIP_KEY,
    HOUSING_SPAWN_MARKER_KEY,
    HOUSING_VISITOR_WALL_KEY,
};

static const map_wiz_props_marker *_housing_wiz_marker(map_marker *marker)
{
    return marker && marker->get_type() == MAT_WIZ_PROPS
           ? static_cast<const map_wiz_props_marker *>(marker) : nullptr;
}

static bool _marker_has_key(map_marker *marker, const char *key)
{
    const map_wiz_props_marker *wiz = _housing_wiz_marker(marker);
    return wiz && wiz->properties.count(key);
}

static bool _marker_has_only_fixture_role(map_marker *marker,
                                          const char *role)
{
    const map_wiz_props_marker *wiz = _housing_wiz_marker(marker);
    if (!wiz || !wiz->properties.count(role))
        return false;
    for (const char *key : HOUSING_FIXTURE_KEYS)
        if (string(key) != role && wiz->properties.count(key))
            return false;
    return true;
}

static bool _marker_properties_are(map_marker *marker,
                                   const map<string, string> &expected)
{
    const map_wiz_props_marker *wiz = _housing_wiz_marker(marker);
    return wiz && wiz->properties == expected;
}

static bool _feature_tile_override_at(const coord_def &pos,
                                      const char *tile_name)
{
    if (!tile_name || !*tile_name)
        return false;
    const tile_flavour &flavour = tile_env.flv(pos);
    if (!flavour.feat_idx || flavour.feat_idx > tile_env.names.size()
        || tile_env.names[flavour.feat_idx - 1] != tile_name)
    {
        return false;
    }
    tileidx_t expected;
    return tile_dngn_index(tile_name, &expected) && flavour.feat == expected;
}

static bool _set_feature_tile_override(const coord_def &pos,
                                       const char *tile_name)
{
    tileidx_t tile;
    if (!tile_dngn_index(tile_name, &tile))
        return false;
    tile_env.flv(pos).feat = tile;
    tile_env.flv(pos).feat_idx = store_tilename_get_index(tile_name);
    set_terrain_changed(pos);
    return true;
}

static bool _local_portal_marker_at(const coord_def &pos,
                                    string *portal_name)
{
    if (!map_bounds(pos) || env.grid(pos) != DNGN_STONE_ARCH)
        return false;
    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    if (markers.size() != 1
        || !_marker_has_only_fixture_role(markers.front(),
                                          HOUSING_LOCAL_PORTAL_KEY))
    {
        return false;
    }
    const string name = markers.front()->property(HOUSING_LOCAL_PORTAL_KEY);
    const map<string, string> expected =
    {
        { HOUSING_LOCAL_PORTAL_KEY, name },
        { "feature_description", "housing passage " + name },
        { "veto_destroy", "veto" },
    };
    if (!_is_map_id(name)
        || !_marker_properties_are(markers.front(), expected))
    {
        return false;
    }
    if (portal_name)
        *portal_name = name;
    return true;
}

static bool _local_portal_name_at(const coord_def &pos, string *portal_name)
{
    return _local_portal_marker_at(pos, portal_name)
        && _feature_tile_override_at(pos, HOUSING_LOCAL_PORTAL_TILE);
}

static void _restore_housing_fixture_tile_overrides()
{
    for (map_marker *marker : env.markers.get_all())
    {
        if (_local_portal_marker_at(marker->pos, nullptr)
            && !_set_feature_tile_override(marker->pos,
                                           HOUSING_LOCAL_PORTAL_TILE))
        {
            fail("Housing passage tile is unavailable");
        }
    }
}

static bool _visitor_strip_marker_at(const coord_def &pos)
{
    if (!map_bounds(pos) || env.grid(pos) != DNGN_TRANSPORTER_LANDING)
        return false;
    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    const map<string, string> expected =
    {
        { HOUSING_VISITOR_STRIP_KEY, "yes" },
        { "feature_description", "visitor inventory stripping tile" },
        { "veto_destroy", "veto" },
    };
    return markers.size() == 1
        && _marker_has_only_fixture_role(markers.front(),
                                         HOUSING_VISITOR_STRIP_KEY)
        && _marker_properties_are(markers.front(), expected);
}

static bool _fixture_key_state_at(const coord_def &pos, const char *key)
{
    if (!map_bounds(pos))
        return false;
    for (map_marker *marker : env.markers.get_markers_at(pos))
        if (_marker_has_key(marker, key))
            return true;
    return false;
}

static bool _spawn_marker_at(const coord_def &pos)
{
    if (!map_bounds(pos) || env.grid(pos) != DNGN_RUNELIGHT)
        return false;
    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    const map<string, string> expected =
    {
        { HOUSING_SPAWN_MARKER_KEY, "yes" },
        { "feature_description", "housing spawn point" },
        { "veto_destroy", "veto" },
    };
    return markers.size() == 1
        && _marker_has_only_fixture_role(markers.front(),
                                         HOUSING_SPAWN_MARKER_KEY)
        && _marker_properties_are(markers.front(), expected);
}

static bool _visitor_wall_feature(dungeon_feature_type feat)
{
    // Metal is the deployed schema-3 representation. Accept it only for
    // backwards compatibility; owner loads upgrade it to translucent
    // permarock and all new barriers use the latter directly.
    return feat == DNGN_CLEAR_PERMAROCK_WALL || feat == DNGN_METAL_WALL;
}

static bool _visitor_wall_marker_at(const coord_def &pos)
{
    if (!map_bounds(pos) || !_visitor_wall_feature(env.grid(pos)))
        return false;
    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    const map<string, string> expected =
    {
        { HOUSING_VISITOR_WALL_KEY, "yes" },
        { "feature_description", "owner-only barrier" },
        { "veto_destroy", "veto" },
    };
    return markers.size() == 1
        && _marker_has_only_fixture_role(markers.front(),
                                         HOUSING_VISITOR_WALL_KEY)
        && _marker_properties_are(markers.front(), expected);
}

static bool _current_visitor_wall_marker_at(const coord_def &pos)
{
    return map_bounds(pos) && env.grid(pos) == DNGN_CLEAR_PERMAROCK_WALL
           && _visitor_wall_marker_at(pos);
}

bool housing_visitor_wall_is_valid(const coord_def &pos)
{
    return _visitor_wall_marker_at(pos);
}

void housing_open_visitor_wall(const coord_def &pos)
{
    if (!_visitor_wall_marker_at(pos))
        fail("Housing visitor wall marker is malformed");
    map_marker *marker = env.markers.get_markers_at(pos).front();
    env.markers.remove(marker);
    dungeon_terrain_changed(pos, DNGN_FLOOR, false, false, true);
}

static void _open_visitor_only_walls()
{
    if (!housing_is_visitor())
        return;

    // Work from a copy because map_markers::remove deletes each marker.
    const vector<map_marker*> markers = env.markers.get_all();
    for (map_marker *marker : markers)
    {
        if (marker->get_type() != MAT_WIZ_PROPS
            || marker->property(HOUSING_VISITOR_WALL_KEY) != "yes")
        {
            continue;
        }
        const coord_def pos = marker->pos;
        housing_open_visitor_wall(pos);
    }
}

static bool _upgrade_legacy_owner_walls()
{
    if (!housing_is_owner())
        return false;

    bool changed = false;
    // Work from a copy: dungeon_terrain_changed() can notify marker and view
    // subsystems, but the authenticated marker itself remains in place.
    const vector<map_marker*> markers = env.markers.get_all();
    for (map_marker *marker : markers)
    {
        if (marker->get_type() != MAT_WIZ_PROPS
            || marker->property(HOUSING_VISITOR_WALL_KEY) != "yes")
        {
            continue;
        }
        const coord_def pos = marker->pos;
        if (!map_bounds(pos) || env.grid(pos) != DNGN_METAL_WALL
            || !_visitor_wall_marker_at(pos))
        {
            continue;
        }
        dungeon_terrain_changed(pos, DNGN_CLEAR_PERMAROCK_WALL,
                                false, false, true);
        if (!_visitor_wall_marker_at(pos))
            fail("Housing owner-only barrier migration was not atomic");
        changed = true;
    }
    return changed;
}

static bool _housing_created_monster(const monster &mons)
{
    return mons.props.exists(HOUSING_MONSTER_KEY)
           && mons.props[HOUSING_MONSTER_KEY].get_type() == SV_BOOL
           && mons.props[HOUSING_MONSTER_KEY].get_bool();
}

static bool _suppress_legacy_housing_monster_remains()
{
    bool changed = false;
    for (monster &mons : menv_real)
    {
        if (!mons.alive() || !_housing_created_monster(mons)
            || mons.props.exists(NEVER_CORPSE_KEY))
        {
            continue;
        }
        // Old public/canonical maps predate the no-remains invariant. This
        // property is understood by the ordinary death and necromancy paths,
        // and is safe to add to both canonical and disposable visitor levels.
        mons.props[NEVER_CORPSE_KEY] = true;
        changed = true;
    }
    return changed;
}

static bool _ensure_spawn_fixture(const coord_def &pos)
{
    if (!_valid_spawn(pos))
        return false;

    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    if (!markers.empty() && !_spawn_marker_at(pos))
        return false;

    if (env.grid(pos) == DNGN_FLOOR)
        dungeon_terrain_changed(pos, DNGN_RUNELIGHT, false, false, true);

    if (markers.empty())
    {
        auto *marker = new map_wiz_props_marker(pos);
        marker->set_property(HOUSING_SPAWN_MARKER_KEY, "yes");
        marker->set_property("feature_description", "housing spawn point");
        marker->set_property("veto_destroy", "veto");
        env.markers.add(marker);
    }
    return _spawn_marker_at(pos);
}

static vector<coord_def> _stored_spawns()
{
    vector<coord_def> result;
    if (!env.properties.exists(HOUSING_SPAWNS_KEY))
        return result;

    const CrawlStoreValue &stored = env.properties[HOUSING_SPAWNS_KEY];
    if (stored.get_type() != SV_VEC)
        return result;

    const CrawlVector &spawns = stored.get_vector();
    if (spawns.get_type() != SV_COORD)
        return result;

    for (const CrawlStoreValue &spawn : spawns)
    {
        const coord_def pos = spawn.get_coord();
        if (_valid_spawn(pos)
            && std::find(result.begin(), result.end(), pos) == result.end())
        {
            result.push_back(pos);
        }
    }
    return result;
}

static void _store_spawns(const vector<coord_def> &spawns)
{
    env.properties.erase(HOUSING_SPAWNS_KEY);
    CrawlVector &stored =
        env.properties[HOUSING_SPAWNS_KEY].new_vector(SV_COORD);
    for (const coord_def &spawn : spawns)
        stored.push_back(spawn);
}

static bool _stored_spawns_match(const vector<coord_def> &spawns)
{
    if (!env.properties.exists(HOUSING_SPAWNS_KEY))
        return false;
    const CrawlStoreValue &stored = env.properties[HOUSING_SPAWNS_KEY];
    if (stored.get_type() != SV_VEC)
        return false;
    const CrawlVector &values = stored.get_vector();
    if (values.get_type() != SV_COORD || values.size() != spawns.size())
        return false;
    for (size_t i = 0; i < spawns.size(); ++i)
        if (values[i].get_coord() != spawns[i])
            return false;
    return true;
}

// Return true only for the narrowly recognised pre-fixture starter template:
// no persisted spawn list and exactly one marker-free runelight. Callers that
// stage old multi-map saves use this signal together with byte-identical
// D/named/template chunks to perform a one-time lossless migration.
static bool _ensure_housing_level(bool place_new_owner,
                                  bool *level_mutated = nullptr)
{
    if (level_mutated)
        *level_mutated = false;
    if (housing_current_role() == housing_role_type::none
        || !you.on_current_level)
    {
        return false;
    }

    if (_upgrade_legacy_owner_walls() && level_mutated)
        *level_mutated = true;
    if (_suppress_legacy_housing_monster_remains() && level_mutated)
        *level_mutated = true;

    const bool player_position_valid = in_bounds(you.pos());
    const bool had_stored_spawn_data =
        env.properties.exists(HOUSING_SPAWNS_KEY);
    vector<coord_def> spawns = _stored_spawns();
    bool adopted_template_spawn = false;
    if (had_stored_spawn_data && spawns.empty())
        corrupted("Housing map has no valid stored spawn");
    if (spawns.empty())
    {
        if (!housing_is_owner())
            return false;

        // Encompass vault placement chooses an ordinary player square after
        // applying KFEAT, so the starter template's visible runelight is not
        // necessarily under the new character. Adopt that reserved fixture
        // instead of creating a second runelight at the random player square.
        vector<coord_def> authenticated;
        vector<coord_def> bare;
        bool malformed_runelight = false;
        for (rectangle_iterator pos(0); pos; ++pos)
        {
            if (!map_bounds(*pos) || env.grid(*pos) != DNGN_RUNELIGHT)
                continue;
            const vector<map_marker*> markers =
                env.markers.get_markers_at(*pos);
            if (_spawn_marker_at(*pos))
                authenticated.push_back(*pos);
            else if (markers.empty())
                bare.push_back(*pos);
            else
                malformed_runelight = true;
        }

        if (malformed_runelight)
            corrupted("Housing map has a malformed spawn fixture");
        else if (bare.size() == 1 && authenticated.empty())
        {
            spawns = bare;
            adopted_template_spawn = true;
        }
        else if (bare.empty() && !authenticated.empty())
            spawns = authenticated;
        else if (bare.empty() && player_position_valid)
            spawns.push_back(you.pos());
        else if (!bare.empty())
            corrupted("Housing map has ambiguous spawn fixtures");
    }

    if (spawns.empty())
        return false;

    // Upgrade stored legacy floor spawns before classifying any remaining
    // runelights. This remains safe with a reset player position during
    // in-process and startup replacement loads because it depends only on
    // persisted level coordinates.
    for (const coord_def &pos : spawns)
    {
        const bool was_authenticated = _spawn_marker_at(pos);
        if (!_ensure_spawn_fixture(pos))
            corrupted("Housing map has a malformed stored spawn");
        if (level_mutated && !was_authenticated)
            *level_mutated = true;
    }

    vector<coord_def> legacy_runelights;
    for (rectangle_iterator pos(0); pos; ++pos)
    {
        if (!map_bounds(*pos) || env.grid(*pos) != DNGN_RUNELIGHT
            || std::find(spawns.begin(), spawns.end(), *pos) != spawns.end())
        {
            continue;
        }

        // Generic Housing terrain editing has never permitted runelight, so
        // a marker-free one outside the persisted spawn set is unambiguously
        // starter residue. Canonical owners persist the repair; visitors make
        // the same repair only in their disposable level capsule so deployed
        // legacy snapshots remain visitable. Any marker-bearing mismatch
        // continues to fail closed in both roles.
        if (!env.markers.get_markers_at(*pos).empty())
            corrupted("Housing map has an unauthenticated runelight");
        legacy_runelights.push_back(*pos);
    }
    for (const coord_def &pos : legacy_runelights)
        dungeon_terrain_changed(pos, DNGN_FLOOR, false, false, true);
    if (level_mutated && !legacy_runelights.empty())
        *level_mutated = true;

    // Visitor maps are disposable, so recording a legacy fixture migration in
    // their temporary level cannot affect the public/canonical snapshot.
    if (level_mutated && !_stored_spawns_match(spawns))
        *level_mutated = true;
    _store_spawns(spawns);

    // Only chargen opts into relocation. Ordinary legacy-owner restores also
    // adopt a unique bare fixture, but preserve their exact saved position.
    if (place_new_owner && adopted_template_spawn
        && !_move_to_spawn(spawns, false))
    {
        fail("The initial Housing spawn is blocked");
    }

    return adopted_template_spawn;
}

void housing_ensure_level(bool place_new_owner)
{
    (void) _ensure_housing_level(place_new_owner);
}

void housing_reset_map_turns()
{
    if (housing_current_role() == housing_role_type::none)
        return;
    housing_ensure_level();
    _housing_turn_origin = you.num_turns;
}

static size_t _secure_random_index(size_t count)
{
    ASSERT(count > 0);
    uint32_t value = 0;
    const uint64_t range = static_cast<uint64_t>(UINT32_MAX) + 1;
    const uint64_t limit = range - range % count;
    do
    {
        if (!read_urandom(reinterpret_cast<char *>(&value), sizeof(value)))
            return random2(count);
    }
    while (value >= limit);
    return value % count;
}

static bool _move_to_spawn(const vector<coord_def> &spawns,
                           bool secure_choice)
{
    if (spawns.empty())
        return false;

    // Choose before resolving temporary occupancy so every persisted spawn has
    // exactly the same selection probability.
    const size_t index = secure_choice ? _secure_random_index(spawns.size())
                                       : random2(spawns.size());
    const coord_def destination = spawns[index];
    if (monster *blocker = monster_at(destination))
    {
        coord_def displaced;
        const bool moved = find_habitable_spot_near(
                               destination, blocker->type, LOS_RADIUS,
                               displaced, 0)
                        && blocker->move_to(displaced, MV_INTERNAL);
        if (!moved)
            monster_teleport(blocker, true, true);
    }
    if (monster_at(destination) || !_valid_spawn(destination))
        return false;

    delete_cloud(destination);
    destroy_trap(destination);
    you.move_to(destination, MV_INTERNAL);
    return true;
}

void housing_finish_map_entry(bool new_game_entry)
{
    // This hook runs both at the early level-load boundary and again after
    // startup has reread rc options. Keep the restriction ahead of the
    // idempotence guard so neither startup flags nor an old save can re-enable
    // optional-death exploration in Housing.
    housing_enforce_explore_mode();
    if (housing_current_role() == housing_role_type::none
        || _housing_map_entry_finished)
        return;

    // 37db-era packages could contain a hidden template and a newly-created
    // named map copied byte-for-byte before the starter's bare runelight was
    // authenticated. Accept only that exact, unedited triple as migratable;
    // arbitrary spawn-less or edited maps must continue to fail closed.
    const string named_map = housing_map_chunk_name(_housing_current_map_id);
    const bool exact_template_clone = housing_is_owner() && you.save
        && housing_legacy_template_is_exact_clone(
            *you.save, _housing_current_map_id);
    const bool exact_stale_template_clone = exact_template_clone
        && !env.properties.exists(HOUSING_SPAWNS_KEY);
    bool level_mutated = false;
    const bool adopted_bare_spawn =
        _ensure_housing_level(new_game_entry, &level_mutated);
    const bool migrate_stale_template =
        adopted_bare_spawn && exact_stale_template_clone;
    if (housing_is_owner() && _housing_owner_template_needs_refresh)
    {
        if (!you.save)
            fail("The staged Housing owner package is unavailable");

        // A legacy single-map package was cloned before its old floor spawn
        // was upgraded to an authenticated runelight fixture above. Keep the
        // staged named main chunk byte-identical to the loader-facing D before
        // the strict index check; promotion will also refresh the pristine
        // template from these sanitized bytes.
        save_level(level_id::current());
        _copy_package_chunk(*you.save, _housing_save_level_chunk(),
                            housing_map_chunk_name(
                                _housing_current_map_id));
    }
    else if (housing_is_owner() && migrate_stale_template)
    {
        // Materialise the authenticated fixture once, then keep the staged
        // loader alias, named target and hidden pristine template identical.
        // A staged transition tells promotion to copy that template into the
        // canonical generation; an ordinary restart already owns canonical.
        save_level(level_id::current());
        _copy_package_chunk(*you.save, _housing_save_level_chunk(), named_map);
        _copy_package_chunk(*you.save, _housing_save_level_chunk(),
                            HOUSING_TEMPLATE_CHUNK);
        if (_housing_owner_promotion_save)
            _housing_owner_template_needs_refresh = true;
    }
    else if (housing_is_owner() && level_mutated)
    {
        // Persist the sanitized active map before the strict D/named equality
        // check and before any publication attempt. Refresh the hidden
        // pristine template only when it was byte-identical to the active map
        // before migration; an edited owner's distinct template is never
        // overwritten.
        save_level(level_id::current());
        _copy_package_chunk(*you.save, _housing_save_level_chunk(), named_map);
        if (exact_template_clone)
        {
            _copy_package_chunk(*you.save, _housing_save_level_chunk(),
                                HOUSING_TEMPLATE_CHUNK);
            if (_housing_owner_promotion_save)
                _housing_owner_template_needs_refresh = true;
        }
    }
    if (housing_is_owner() && !_ensure_owner_map_storage())
        fail("The canonical Housing map index is unavailable");
    if (housing_is_owner()
        && !_package_chunks_equal(*you.save, _housing_save_level_chunk(),
                                  housing_map_chunk_name(
                                      _housing_current_map_id)))
    {
        corrupted("The active Housing level does not match its map index");
    }
    if (housing_is_owner())
    {
        const int theme = _read_housing_map_theme(
            *you.save, _housing_current_map_id);
        if (theme >= 0)
            _apply_housing_map_theme(theme);
    }
    const bool exact_rollback = _housing_exact_map_rollback;
    const bool place_at_spawn = (housing_is_visitor() && !exact_rollback)
                                || _housing_entry_requires_spawn;
    if (place_at_spawn)
    {
        const vector<coord_def> spawns = _stored_spawns();
        if (spawns.empty())
        {
            _housing_entry_requires_spawn = false;
            if (housing_is_visitor())
                game_ended(game_exit::abort, _visitor_start_failure);
            fail("The Housing owner map has no valid spawn");
        }

        if (!_move_to_spawn(spawns, true))
        {
            _housing_entry_requires_spawn = false;
            if (housing_is_visitor())
                game_ended(game_exit::abort, _visitor_start_failure);
            fail("The Housing owner map spawn is blocked");
        }
    }
    _housing_entry_requires_spawn = false;

    if (!exact_rollback)
        housing_reset_map_turns();
    if (!_housing_pending_notice.empty())
    {
        mprf(MSGCH_ERROR, "%s", _housing_pending_notice.c_str());
        _housing_pending_notice.clear();
    }
    _housing_exact_map_rollback = false;
    _housing_map_entry_finished = true;
}

int housing_map_turns()
{
    if (housing_current_role() == housing_role_type::none)
        return you.num_turns;
    if (_housing_turn_origin < 0)
        _housing_turn_origin = you.num_turns;
    return std::max(0, you.num_turns - _housing_turn_origin);
}

bool housing_is_spawn(const coord_def &pos)
{
    housing_ensure_level();
    const vector<coord_def> spawns = _stored_spawns();
    return std::find(spawns.begin(), spawns.end(), pos) != spawns.end();
}

static void _clear_housing_cell_to_floor(const coord_def &pos)
{
    tile_env.flv(pos).feat = 0;
    tile_env.flv(pos).special = 0;
    env.grid_colours(pos) = 0;
    dungeon_terrain_changed(pos, DNGN_FLOOR, false, false, true);
    tile_init_flavour(pos);
}

static bool _clear_housing_items(const coord_def &pos)
{
    bool removed = false;
    for (stack_iterator item(pos); item; ++item)
    {
        // Clear terrain is deliberately destructive: clearing a square also
        // clears every item on it, including old corpses, skeletons, generated
        // monster equipment, and ordinary player-dropped items.
        item_was_destroyed(*item);
        destroy_item(item.index());
        removed = true;
    }
    if (removed)
    {
        StashTrack.update_stash(pos);
        mpr("The items on that square are cleared away.");
    }
    return removed;
}

static bool _remove_housing_spawn(const coord_def &pos,
                                  vector<coord_def> &spawns)
{
    const auto found = std::find(spawns.begin(), spawns.end(), pos);
    if (found == spawns.end())
        return false;
    if (spawns.size() == 1)
    {
        mpr("Every Housing map must keep at least one spawn point.");
        return false;
    }
    // Replacing a runelight with floor is safe underneath the owner, and is
    // useful when moving the original spawn after creating a second one.
    // Other actors still make the mutation ambiguous.
    if (!_spawn_marker_at(pos) || (actor_at(pos) && pos != you.pos())
        || env.igrid(pos) != NON_ITEM)
    {
        mpr("That Housing spawn point cannot be removed safely.");
        return false;
    }

    map_marker *marker = env.markers.get_markers_at(pos).front();
    env.markers.remove(marker);
    _clear_housing_cell_to_floor(pos);
    spawns.erase(found);
    _store_spawns(spawns);
    mpr("The Housing spawn point is removed.");
    return true;
}

static bool _remove_housing_visitor_wall(const coord_def &pos)
{
    if (!_visitor_wall_marker_at(pos) || actor_at(pos)
        || env.igrid(pos) != NON_ITEM || housing_is_spawn(pos))
    {
        mpr("That owner-only barrier cannot be removed safely.");
        return false;
    }

    map_marker *marker = env.markers.get_markers_at(pos).front();
    env.markers.remove(marker);
    _clear_housing_cell_to_floor(pos);
    mpr("The owner-only barrier is removed.");
    return true;
}

static bool _housing_portal_state_at(const coord_def &pos)
{
    if (env.grid(pos) == DNGN_ENTER_PORTAL_VAULT)
        return true;
    for (map_marker *marker : env.markers.get_markers_at(pos))
        if (!marker->property(HOUSING_PORTAL_TARGET_KEY).empty())
            return true;
    return false;
}

static bool _housing_local_portal_state_at(const coord_def &pos)
{
    return _fixture_key_state_at(pos, HOUSING_LOCAL_PORTAL_KEY);
}

static bool _remove_housing_local_portal(const coord_def &pos)
{
    if (!_local_portal_name_at(pos, nullptr)
        || (actor_at(pos) && pos != you.pos())
        || env.igrid(pos) != NON_ITEM || housing_is_spawn(pos))
    {
        mpr("That named Housing passage cannot be removed safely.");
        return false;
    }

    map_marker *marker = env.markers.get_markers_at(pos).front();
    env.markers.remove(marker);
    _clear_housing_cell_to_floor(pos);
    mpr("The named Housing passage is removed.");
    return true;
}

static bool _housing_visitor_strip_state_at(const coord_def &pos)
{
    return env.grid(pos) == DNGN_TRANSPORTER_LANDING
        || _fixture_key_state_at(pos, HOUSING_VISITOR_STRIP_KEY);
}

bool housing_movement_fixture_is_reserved(const coord_def &pos)
{
    return crawl_state.game_is_housing()
        && (_housing_local_portal_state_at(pos)
            || _housing_visitor_strip_state_at(pos));
}

static bool _remove_housing_visitor_strip(const coord_def &pos)
{
    if (!_visitor_strip_marker_at(pos)
        || (actor_at(pos) && pos != you.pos())
        || env.igrid(pos) != NON_ITEM || housing_is_spawn(pos))
    {
        mpr("That visitor inventory tile cannot be removed safely.");
        return false;
    }

    map_marker *marker = env.markers.get_markers_at(pos).front();
    env.markers.remove(marker);
    _clear_housing_cell_to_floor(pos);
    mpr("The visitor inventory tile is removed.");
    return true;
}

static bool _remove_housing_portal(const coord_def &pos)
{
    if (!_portal_target_at(pos, nullptr)
        || (actor_at(pos) && pos != you.pos())
        || env.igrid(pos) != NON_ITEM || housing_is_spawn(pos))
    {
        mpr("That Housing portal cannot be removed safely.");
        return false;
    }

    map_marker *marker = env.markers.get_markers_at(pos).front();
    env.markers.remove(marker);
    _clear_housing_cell_to_floor(pos);
    mpr("The Housing portal is removed.");
    return true;
}

static bool _remove_housing_shop(const coord_def &pos)
{
    const dungeon_feature_type feat = env.grid(pos);
    const auto found = env.shop.find(pos);
    const bool active = feat == DNGN_ENTER_SHOP;
    const bool abandoned = feat == DNGN_ABANDONED_SHOP;

    // Only erase a complete active shop pair, or an exhausted shop with no
    // remaining shop record. A mismatched grid/table pair is corrupted state,
    // not ordinary terrain which the editor may partially destroy.
    if ((!active && !abandoned)
        || (active && !_housing_shop_can_be_published(pos))
        || (abandoned && found != env.shop.end())
        || !env.markers.get_markers_at(pos).empty()
        || env.igrid(pos) != NON_ITEM
        || (actor_at(pos) && pos != you.pos()))
    {
        mpr("That Housing shop cannot be removed safely.");
        return false;
    }

    if (active)
    {
        destroy_shop_at(pos);
        if (env.shop.find(pos) != env.shop.end()
            || env.grid(pos) != DNGN_ABANDONED_SHOP)
        {
            fail("Housing shop removal was not atomic");
        }
    }
    _clear_housing_cell_to_floor(pos);
    mpr("The Housing shop is removed.");
    return true;
}

bool housing_clear_terrain(const coord_def &pos)
{
    if (!housing_authorize_action("clear terrain", 0) || !map_bounds(pos))
        return false;

    housing_ensure_level();
    const bool items_cleared = _clear_housing_items(pos);
    vector<coord_def> spawns = _stored_spawns();
    if (std::find(spawns.begin(), spawns.end(), pos) != spawns.end())
        return _remove_housing_spawn(pos, spawns) || items_cleared;
    if (_visitor_wall_marker_at(pos))
        return _remove_housing_visitor_wall(pos) || items_cleared;
    if (_housing_local_portal_state_at(pos))
        return _remove_housing_local_portal(pos) || items_cleared;
    if (_housing_visitor_strip_state_at(pos))
        return _remove_housing_visitor_strip(pos) || items_cleared;
    if (_housing_portal_state_at(pos))
        return _remove_housing_portal(pos) || items_cleared;
    if (env.grid(pos) == DNGN_ENTER_SHOP
        || env.grid(pos) == DNGN_ABANDONED_SHOP
        || env.shop.find(pos) != env.shop.end())
    {
        return _remove_housing_shop(pos) || items_cleared;
    }
    if (!housing_can_edit(pos))
    {
        mpr("That square is protected in Housing.");
        return items_cleared;
    }

    _clear_housing_cell_to_floor(pos);
    return true;
}

bool housing_toggle_spawn_point(const coord_def &pos)
{
    if (!housing_authorize_action("manage spawn points", 0)
        || !map_bounds(pos) || !in_bounds(pos))
    {
        return false;
    }

    housing_ensure_level();
    vector<coord_def> spawns = _stored_spawns();
    const auto found = std::find(spawns.begin(), spawns.end(), pos);
    if (found != spawns.end())
        return _remove_housing_spawn(pos, spawns);

    // New spawn fixtures are deliberately stricter than general terrain
    // editing: they may only replace an empty, marker-free ordinary floor.
    // This prevents hidden portals, shops, items, actors or special terrain
    // state from being captured by persistent respawn metadata.
    if (env.grid(pos) != DNGN_FLOOR || actor_at(pos)
        || env.igrid(pos) != NON_ITEM
        || !env.markers.get_markers_at(pos).empty())
    {
        mpr("Choose an empty ordinary floor square for the spawn point.");
        return false;
    }

    dungeon_terrain_changed(pos, DNGN_RUNELIGHT, false, false, true);
    if (!_ensure_spawn_fixture(pos))
    {
        env.markers.remove_markers_at(pos);
        dungeon_terrain_changed(pos, DNGN_FLOOR, false, false, true);
        fail("Housing spawn creation was not atomic");
    }
    spawns.push_back(pos);
    _store_spawns(spawns);
    mpr("A visible Housing spawn point is created.");
    return true;
}

bool housing_can_edit(const coord_def &pos)
{
    if (!housing_is_owner() || !map_bounds(pos) || actor_at(pos)
        || env.igrid(pos) != NON_ITEM
        || !env.markers.get_markers_at(pos).empty())
    {
        return false;
    }
    return !housing_is_spawn(pos)
        && (housing_feature_allowed(env.grid(pos))
            || env.grid(pos) == DNGN_ABANDONED_SHOP);
}

bool housing_feature_allowed(dungeon_feature_type feat)
{
    if (feat_is_altar(feat))
        return true;

    switch (feat)
    {
    case DNGN_FLOOR:
    case DNGN_CLOSED_DOOR:
    case DNGN_RUNED_DOOR:
    case DNGN_SEALED_DOOR:
    case DNGN_OPEN_DOOR:
    case DNGN_ROCK_WALL:
    case DNGN_STONE_WALL:
    case DNGN_METAL_WALL:
    case DNGN_CRYSTAL_WALL:
    case DNGN_SLIMY_WALL:
    case DNGN_PERMAROCK_WALL:
    case DNGN_CLEAR_ROCK_WALL:
    case DNGN_CLEAR_STONE_WALL:
    case DNGN_CLEAR_PERMAROCK_WALL:
    case DNGN_GRATE:
    case DNGN_OPEN_SEA:
    case DNGN_LAVA_SEA:
    case DNGN_ORCISH_IDOL:
    case DNGN_STONE_ARCH:
    case DNGN_EXPIRED_PORTAL:
    case DNGN_TREE:
    case DNGN_GRANITE_STATUE:
    case DNGN_SHALLOW_WATER:
    case DNGN_DEEP_WATER:
    case DNGN_LAVA:
    case DNGN_FOUNTAIN_BLUE:
    case DNGN_FOUNTAIN_SPARKLING:
    case DNGN_FOUNTAIN_BLOOD:
    case DNGN_DRY_FOUNTAIN:
#if TAG_MAJOR_VERSION > 34
    case DNGN_BROKEN_DOOR:
    case DNGN_CLOSED_CLEAR_DOOR:
    case DNGN_BROKEN_CLEAR_DOOR:
    case DNGN_RUNED_CLEAR_DOOR:
    case DNGN_SEALED_CLEAR_DOOR:
    case DNGN_OPEN_CLEAR_DOOR:
    case DNGN_MANGROVE:
    case DNGN_DEMONIC_TREE:
    case DNGN_PETRIFIED_TREE:
    case DNGN_FRIGID_WALL:
    case DNGN_ENDLESS_SALT:
    case DNGN_METAL_STATUE:
    case DNGN_ZOT_STATUE:
    case DNGN_MUD:
    case DNGN_TOXIC_BOG:
    case DNGN_BINDING_SIGIL:
    case DNGN_PURIFIED_MUTATION_CATALYST:
    case DNGN_FOUNTAIN_EYES:
    case DNGN_CACHE_OF_BAKED_GOODS:
    case DNGN_CACHE_OF_FRUIT:
    case DNGN_CACHE_OF_MEAT:
#endif
        return true;
    default:
        return false;
    }
}

dungeon_feature_type housing_last_feature()
{
    if (!you.props.exists(HOUSING_LAST_FEATURE_KEY)
        || you.props[HOUSING_LAST_FEATURE_KEY].get_type() != SV_INT)
    {
        return DNGN_UNSEEN;
    }

    const auto feat = static_cast<dungeon_feature_type>(
        you.props[HOUSING_LAST_FEATURE_KEY].get_int());
    return housing_feature_allowed(feat) ? feat : DNGN_UNSEEN;
}

void housing_set_last_feature(dungeon_feature_type feat)
{
    if (!housing_feature_allowed(feat))
        return;
    you.props.erase(HOUSING_LAST_FEATURE_KEY);
    you.props[HOUSING_LAST_FEATURE_KEY] = static_cast<int>(feat);
}

static bool _portal_target_at(const coord_def &pos, string *target)
{
    if (!map_bounds(pos) || env.grid(pos) != DNGN_ENTER_PORTAL_VAULT)
        return false;

    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    if (markers.size() != 1)
        return false;
    const string value = markers.front()->property(HOUSING_PORTAL_TARGET_KEY);
    const map<string, string> expected =
    {
        { HOUSING_PORTAL_TARGET_KEY, value },
        { "feature_description", "housing portal to " + value },
        { "veto_destroy", "veto" },
    };
    if (!housing_valid_map_target(value)
        || !_marker_has_only_fixture_role(markers.front(),
                                          HOUSING_PORTAL_TARGET_KEY)
        || !_marker_properties_are(markers.front(), expected))
    {
        return false;
    }
    if (target)
        *target = value;
    return true;
}

bool housing_portal_is_valid(const coord_def &pos)
{
    return crawl_state.game_is_housing() && _portal_target_at(pos, nullptr);
}

bool housing_local_portal_is_valid(const coord_def &pos)
{
    return crawl_state.game_is_housing()
        && _local_portal_name_at(pos, nullptr);
}

bool housing_visitor_strip_is_valid(const coord_def &pos)
{
    return crawl_state.game_is_housing() && _visitor_strip_marker_at(pos);
}

static int _housing_local_portal_count()
{
    int count = 0;
    for (map_marker *marker : env.markers.get_all())
        if (_marker_has_key(marker, HOUSING_LOCAL_PORTAL_KEY))
            ++count;
    return count;
}

bool housing_create_local_portal(const coord_def &pos,
                                 const string &portal_name)
{
    if (!_is_map_id(portal_name))
    {
        mprf(MSGCH_PROMPT,
             "Use 1-20 ASCII letters, digits, or _ for a local portal name.");
        return false;
    }
    if (!housing_can_edit(pos) || env.grid(pos) != DNGN_FLOOR)
    {
        mpr("Choose an empty ordinary floor square for the named passage.");
        return false;
    }
    if (_housing_local_portal_count() >= HOUSING_MAX_LOCAL_PORTALS)
    {
        mpr("This Housing map has too many named passage endpoints.");
        return false;
    }
    if (!housing_authorize_action("create a named passage", 0))
        return false;

    dungeon_terrain_changed(pos, DNGN_STONE_ARCH, false, false, true);
    if (!_set_feature_tile_override(pos, HOUSING_LOCAL_PORTAL_TILE))
    {
        _clear_housing_cell_to_floor(pos);
        fail("Housing passage tile is unavailable");
    }
    auto *marker = new map_wiz_props_marker(pos);
    marker->set_property(HOUSING_LOCAL_PORTAL_KEY, portal_name);
    marker->set_property("feature_description",
                         "housing passage " + portal_name);
    marker->set_property("veto_destroy", "veto");
    env.markers.add(marker);
    mprf("A named Housing passage '%s' is created.",
         portal_name.c_str());
    return true;
}

bool housing_create_visitor_strip(const coord_def &pos)
{
    if (!housing_can_edit(pos) || env.grid(pos) != DNGN_FLOOR)
    {
        mpr("Choose an empty ordinary floor square for the visitor inventory tile.");
        return false;
    }
    if (!housing_authorize_action("create a visitor inventory tile", 0))
        return false;

    dungeon_terrain_changed(pos, DNGN_TRANSPORTER_LANDING,
                            false, false, true);
    auto *marker = new map_wiz_props_marker(pos);
    marker->set_property(HOUSING_VISITOR_STRIP_KEY, "yes");
    marker->set_property("feature_description",
                         "visitor inventory stripping tile");
    marker->set_property("veto_destroy", "veto");
    env.markers.add(marker);
    mpr("A visitor inventory stripping tile is created.");
    return true;
}

bool housing_create_portal(const coord_def &pos, const string &target)
{
    if (_is_map_id(target))
        return housing_create_local_portal(pos, target);
    if (!housing_valid_map_target(target))
    {
        mprf(MSGCH_PROMPT,
             "Use account:map or a 1-20 character local portal name "
             "(ASCII letters, digits, and _).");
        return false;
    }
    if (!housing_can_edit(pos))
    {
        mpr("That square is protected in Housing.");
        return false;
    }
    if (!housing_authorize_action("create a portal", 0))
        return false;

    dungeon_terrain_changed(pos, DNGN_ENTER_PORTAL_VAULT,
                            false, false, true);
    auto *marker = new map_wiz_props_marker(pos);
    marker->set_property(HOUSING_PORTAL_TARGET_KEY, target);
    marker->set_property("feature_description",
                         "housing portal to " + target);
    marker->set_property("veto_destroy", "veto");
    env.markers.add(marker);
    return true;
}

bool housing_local_portal_destination(const coord_def &source,
                                      coord_def &destination,
                                      bool *occupied)
{
    string portal_name;
    if (!_local_portal_name_at(source, &portal_name))
        return false;

    vector<coord_def> destinations;
    bool found_occupied = false;
    for (rectangle_iterator pos(0); pos; ++pos)
    {
        if (*pos == source)
            continue;
        string candidate_name;
        if (!_local_portal_name_at(*pos, &candidate_name)
            || candidate_name != portal_name)
        {
            continue;
        }
        if (actor_at(*pos))
            found_occupied = true;
        else
            destinations.push_back(*pos);
    }
    if (occupied)
        *occupied = found_occupied;
    if (destinations.empty())
        return false;
    destination = destinations[random2(destinations.size())];
    return true;
}

bool housing_trigger_local_portal(actor &triggerer)
{
    if (!crawl_state.game_is_housing()
        || !_housing_local_portal_state_at(triggerer.pos()))
    {
        return false;
    }

    string portal_name;
    if (!_local_portal_name_at(triggerer.pos(), &portal_name))
    {
        if (triggerer.is_player())
            mprf(MSGCH_ERROR, "This named Housing passage is malformed.");
        return true;
    }
    if (!you.see_cell_no_trans(triggerer.pos()))
        return true;

    monster *mons = triggerer.as_monster();
    if (mons && mons_is_tentacle_or_tentacle_segment(mons->type))
        return true;

    bool occupied_destination = false;
    coord_def destination;
    if (!housing_local_portal_destination(triggerer.pos(), destination,
                                          &occupied_destination))
    {
        if (triggerer.is_player())
        {
            mprf("This passage %s!", occupied_destination
                 ? "seems to be blocked by something"
                 : "doesn't lead anywhere");
        }
        return true;
    }

    if (triggerer.is_player())
        mprf("You enter the Housing passage '%s'.", portal_name.c_str());
    else
        simple_monster_message(*mons, " enters a Housing passage.");
    if (!triggerer.move_to(destination, MV_TRANSLOCATION | MV_GOLUBRIA))
        fail("Housing passage destination became unavailable");
    return true;
}

bool housing_trigger_visitor_strip(actor &triggerer)
{
    const coord_def pos = triggerer.pos();
    if (!crawl_state.game_is_housing()
        || !_housing_visitor_strip_state_at(pos))
    {
        return false;
    }
    if (!_visitor_strip_marker_at(pos))
    {
        if (triggerer.is_player())
        {
            mprf(MSGCH_ERROR,
                 "This visitor inventory stripping tile is malformed.");
        }
        return true;
    }
    if (!triggerer.is_player() || !housing_is_visitor())
        return true;

    const bool had_items = std::any_of(you.inv.begin(), you.inv.end(),
                                       [](const item_def &item) {
                                           return item.defined();
                                       });
    if (!had_items)
    {
        mpr("The visitor inventory tile finds nothing to remove.");
        return true;
    }
    if (!destroy_player_inventory_for_housing())
    {
        mprf(MSGCH_ERROR,
             "The visitor inventory tile cannot safely remove this inventory.");
        return true;
    }
    mpr("The visitor inventory tile destroys all equipment and carried items "
        "for this visit.");
    return true;
}

bool housing_toggle_visitor_wall(const coord_def &pos)
{
    if (!housing_authorize_action("toggle an owner-only barrier", 0)
        || !map_bounds(pos) || !in_bounds(pos))
    {
        return false;
    }

    if (_visitor_wall_marker_at(pos))
        return _remove_housing_visitor_wall(pos);

    if (!housing_can_edit(pos))
    {
        mpr("That square is protected in Housing.");
        return false;
    }

    // Translucent permarock blocks movement without blocking LOS and cannot
    // be dug or shattered, making it a stable monster enclosure for owners.
    dungeon_terrain_changed(pos, DNGN_CLEAR_PERMAROCK_WALL,
                            false, false, true);
    auto *marker = new map_wiz_props_marker(pos);
    marker->set_property(HOUSING_VISITOR_WALL_KEY, "yes");
    marker->set_property("feature_description", "owner-only barrier");
    marker->set_property("veto_destroy", "veto");
    env.markers.add(marker);
    if (!_visitor_wall_marker_at(pos))
        fail("Housing visitor wall creation was not atomic");
    mpr("A see-through owner-only barrier rises; visitors can pass through "
        "it.");
    return true;
}

bool housing_monster_type_allowed(monster_type type)
{
    if (type <= MONS_PROGRAM_BUG || type >= NUM_MONSTERS
        || mons_is_pghost(type)
        || mons_class_is_test(type) || mons_class_is_zombified(type)
        || mons_is_projectile(type) || mons_is_seeker(type)
        || mons_is_tentacle_head(type)
        || mons_is_tentacle_or_tentacle_segment(type)
        || mons_class_flag(type, M_CANT_SPAWN | M_UNFINISHED | M_UNSTABLE
                                 | M_PERIPHERAL | M_ANCESTOR | M_AVATAR))
    {
        return false;
    }
    return true;
}

bool housing_monster_is_owner_inert(const monster &mons)
{
    return housing_is_owner() && mons.alive()
        && _housing_created_monster(mons);
}

bool housing_monster_was_created(const monster &mons)
{
    return _housing_created_monster(mons);
}

bool housing_create_monster()
{
    if (!housing_authorize_action("create a monster", 0))
        return false;

    const int live_monsters = std::count_if(
        menv_real.begin(), menv_real.end(),
        [](const monster &mons) { return mons.alive(); });
    if (live_monsters >= HOUSING_MAX_MONSTERS)
    {
        mpr("This Housing map already has the maximum number of monsters.");
        return false;
    }

    char name[128];
    mprf(MSGCH_PROMPT, "Enter monster name: ");
    if (cancellable_get_line_autohist(name, sizeof name) || !*name)
    {
        canned_msg(MSG_OK);
        return false;
    }

    monster_type type = get_monster_by_name(name);
    if (type == MONS_PROGRAM_BUG && string(name).size() >= 3)
        type = get_monster_by_name(name, true);
    if (!housing_monster_type_allowed(type))
    {
        mpr("That monster cannot be created in Housing.");
        return false;
    }
    // Match the wizard flow: settle the requested type first, then enter a
    // WebTiles-compatible cell targeter. Quivered activation reaches this same
    // chooser instead of borrowing hostile autofight targeting.
    targeter_smite hitfunc(&you, LOS_MAX_RANGE, 0, 0,
                           true, false, false);
    direction_chooser_args args;
    args.hitfunc = &hitfunc;
    args.restricts = DIR_ENFORCE_RANGE;
    args.mode = TARG_NON_ACTOR;
    args.range = LOS_MAX_RANGE;
    args.needs_path = false;
    args.self = confirm_prompt_type::cancel;
    args.top_prompt = "Place housing monster: <w>" +
                      mons_type_name(type, DESC_PLAIN) + "</w>";
    dist target;
    direction(target, args);
    if (!target.isValid || target.isCancel)
    {
        canned_msg(MSG_OK);
        return false;
    }
    const coord_def place = target.target;

    // The chooser is advisory. Recheck every mutation invariant here so
    // scripted/replayed targets cannot overwrite actors, items, authenticated
    // fixtures, spawns, unseen cells, or map bounds.
    if (!map_bounds(place) || !in_bounds(place)
        || (place - you.pos()).rdist() > LOS_MAX_RANGE
        || !you.see_cell_no_trans(place)
        || actor_at(place) || env.igrid(place) != NON_ITEM
        || !env.markers.get_markers_at(place).empty()
        || housing_is_spawn(place))
    {
        mpr("That square cannot hold a Housing monster.");
        return false;
    }

    if (!monster_habitable_grid(type, place))
    {
        mpr("That monster cannot inhabit the targeted square.");
        return false;
    }

    mgen_data mg(type, BEH_HOSTILE, place, MHITYOU,
                 MG_FORBID_BANDS | MG_FORCE_PLACE
                 | MG_IGNORE_UNIQUE_STATUS);
    mg.extra_flags |= MF_NO_REWARD;
    monster *created = create_monster(mg);
    if (!created)
    {
        mpr("The monster could not be created.");
        return false;
    }
    created->props[HOUSING_MONSTER_KEY] = true;
    created->props[NEVER_CORPSE_KEY] = true;
    // Treat only equipment generated as part of this editor action as
    // disposable. A monster that later acquires a real player item will still
    // drop it normally. Preserve unrands: marking one disposable while its
    // global unique status says it exists would make its lifecycle ambiguous.
    for (mon_inv_iterator item(*created); item; ++item)
    {
        if (!is_unrandom_artefact(*item))
            item->flags |= ISFLAG_SUMMONED;
    }
    mprf("%s appears.", created->name(DESC_A).c_str());
    return true;
}

bool housing_can_create_shop()
{
    return env.shop.size() < HOUSING_MAX_SHOPS;
}

static bool _split_map_target(const string &target, string &owner,
                              string &map_id)
{
    if (!housing_valid_map_target(target))
        return false;
    const size_t colon = target.find(':');
    owner = target.substr(0, colon);
    map_id = target.substr(colon + 1);
    return true;
}

static string _housing_owner_public_dir(const string &public_dir,
                                        const string &owner)
{
    return catpath(catpath(public_dir, "by-name"),
                   _lowercase_ascii(owner));
}

static string _housing_owner_binding_path(const string &public_dir,
                                          const string &owner)
{
    return catpath(_housing_owner_public_dir(public_dir, owner),
                   ".account-id");
}

// Return false only when no binding exists. A present but malformed,
// inaccessible or non-regular binding is a hard error, never an invitation to
// fall back to a potentially different legacy owner payload.
static bool _read_public_owner_binding(const string &public_dir,
                                       const string &owner,
                                       string &account_id)
{
#ifdef TARGET_OS_WINDOWS
    UNUSED(public_dir, owner, account_id);
    return false;
#else
    const string owner_dir = _housing_owner_public_dir(public_dir, owner);
    struct stat owner_info;
    errno = 0;
    if (lstat(owner_dir.c_str(), &owner_info) != 0)
    {
        if (errno == ENOENT)
            return false;
        sysfail("could not inspect Housing owner binding directory");
    }
    if (!S_ISDIR(owner_info.st_mode)
        || !_path_is_within(owner_dir, public_dir))
    {
        fail("Housing owner binding directory is unsafe");
    }

    const string binding = _housing_owner_binding_path(public_dir, owner);
    struct stat binding_info;
    errno = 0;
    if (lstat(binding.c_str(), &binding_info) != 0)
    {
        if (errno == ENOENT)
            return false;
        sysfail("could not inspect Housing owner binding");
    }
    if (!S_ISREG(binding_info.st_mode) || binding_info.st_size < 1
        || binding_info.st_size > 20 || !_path_is_within(binding, public_dir))
    {
        fail("Housing owner binding is malformed");
    }

    FILE *raw = fopen_u(binding.c_str(), "rb");
    if (!raw)
        sysfail("could not open Housing owner binding");
    std::unique_ptr<FILE, int (*)(FILE*)> file(raw, fclose);
    const int fd = fileno(raw);
    if (fd < 0 || !lock_file(fd, false, true))
        fail("could not lock Housing owner binding");

    char value[21] = {};
    const size_t count = fread(value, 1, sizeof(value), raw);
    const bool read_failed = ferror(raw);
    unlock_file(fd);
    if (read_failed || count != static_cast<size_t>(binding_info.st_size)
        || count == sizeof(value))
    {
        fail("could not read Housing owner binding safely");
    }
    account_id.assign(value, count);
    if (!_is_decimal_id(account_id))
        fail("Housing owner binding has an invalid account id");
    return true;
#endif
}

static bool _bind_public_owner(const string &public_dir,
                               const string &owner,
                               const string &account_id)
{
    string existing;
    if (_read_public_owner_binding(public_dir, owner, existing))
        return existing == account_id;

#ifdef TARGET_OS_WINDOWS
    return false;
#else
    const string binding = _housing_owner_binding_path(public_dir, owner);
    string temporary_template = binding + ".tmp.XXXXXX";
    vector<char> temporary_buf(temporary_template.begin(),
                               temporary_template.end());
    temporary_buf.push_back('\0');
    const int fd = mkstemp(temporary_buf.data());
    if (fd < 0)
        return false;
    const string temporary = temporary_buf.data();

    FILE *raw = fdopen(fd, "wb");
    if (!raw)
    {
        close(fd);
        unlink_u(temporary.c_str());
        return false;
    }
    bool success = false;
    if (lock_file(fd, true, true))
    {
        success = fwrite(account_id.data(), 1, account_id.size(), raw)
                      == account_id.size()
                  && fflush(raw) == 0 && fdatasync(fd) == 0;
        unlock_file(fd);
    }
    if (fclose(raw) != 0)
        success = false;
    if (!success)
    {
        unlink_u(temporary.c_str());
        return false;
    }

    // A hard link is the portable Unix no-replace install primitive here: the
    // final name appears only after the complete temp file is durable, and a
    // racing publisher can never overwrite an existing account binding.
    errno = 0;
    const bool installed = link(temporary.c_str(), binding.c_str()) == 0;
    const int install_error = errno;
    unlink_u(temporary.c_str());
    if (installed)
    {
#ifdef O_DIRECTORY
        const string owner_dir = _housing_owner_public_dir(public_dir, owner);
        const int parent_fd = open(owner_dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (parent_fd >= 0)
        {
            fsync(parent_fd);
            close(parent_fd);
        }
#endif
        return true;
    }
    if (install_error != EEXIST)
        return false;
    return _read_public_owner_binding(public_dir, owner, existing)
        && existing == account_id;
#endif
}

bool housing_bind_public_owner(const string &public_dir,
                               const string &owner_name,
                               const string &account_id)
{
    if (public_dir.empty() || !_is_account_name(owner_name)
        || !_is_decimal_id(account_id))
    {
        return false;
    }
    try
    {
        return _bind_public_owner(public_dir, owner_name, account_id);
    }
    catch (const std::exception &error)
    {
        dprf("Housing owner binding failed: %s", error.what());
        return false;
    }
}

static std::unique_ptr<package> _open_valid_public_snapshot(
    const string &path, const string &public_dir, const string &owner,
    const string &map_id, const string &expected_account,
    housing_snapshot_meta &meta)
{
#ifndef TARGET_OS_WINDOWS
    struct stat info;
    errno = 0;
    if (lstat(path.c_str(), &info) != 0)
    {
        if (errno == ENOENT)
            return nullptr;
        sysfail("could not inspect Housing public snapshot");
    }
    if (!S_ISREG(info.st_mode))
        fail("Housing public snapshot is not a regular file");
#endif
    if (!_path_is_within(path, public_dir))
        fail("Housing public snapshot escaped its directory");

    std::unique_ptr<package> snapshot(new package(path.c_str(), false));
    meta = _read_snapshot_meta(*snapshot);
    if (!_supported_snapshot_schema(meta.schema)
        || !_is_decimal_id(meta.account_id)
        || (!expected_account.empty() && meta.account_id != expected_account)
        || meta.map_id != map_id
        || _lowercase_ascii(meta.owner_name) != _lowercase_ascii(owner)
        // TAG_LEVEL's tagged header is the authoritative compatibility gate.
        // Requiring the full nightly git version here would invalidate every
        // published map on each otherwise-compatible upstream update.
        || meta.crawl_version.empty())
    {
        fail("Housing snapshot metadata does not match the portal");
    }
    return snapshot;
}

static std::unique_ptr<package> _resolve_public_snapshot(
    const string &public_dir, const string &owner, const string &map_id,
    string &snapshot_path, housing_snapshot_meta &meta)
{
    string account_id;
    const bool has_binding =
        _read_public_owner_binding(public_dir, owner, account_id);
    const string legacy_path =
        catpath(_housing_owner_public_dir(public_dir, owner),
                map_id + ".hmap");

    if (has_binding)
    {
        const string numeric_path =
            catpath(catpath(public_dir, account_id), map_id + ".hmap");
        std::unique_ptr<package> numeric = _open_valid_public_snapshot(
            numeric_path, public_dir, owner, map_id, account_id, meta);
        if (numeric)
        {
            snapshot_path = numeric_path;
            return numeric;
        }
        return nullptr;
    }

    // Unbound names are old-layout only. Read their metadata to discover the
    // immutable account id, then read the matching numeric payload. Visitor
    // resolution never mutates the public index; authenticated owner publish
    // installs the stable binding later.
    std::unique_ptr<package> legacy = _open_valid_public_snapshot(
        legacy_path, public_dir, owner, map_id, "", meta);
    if (!legacy)
        return nullptr;
    const string numeric_path =
        catpath(catpath(public_dir, meta.account_id), map_id + ".hmap");
    housing_snapshot_meta numeric_meta;
    std::unique_ptr<package> numeric = _open_valid_public_snapshot(
        numeric_path, public_dir, owner, map_id, meta.account_id,
        numeric_meta);
    if (!numeric)
        return nullptr;
    meta = numeric_meta;
    snapshot_path = numeric_path;
    return numeric;
}

bool housing_resolve_public_snapshot_path(const string &public_dir,
                                          const string &owner_name,
                                          const string &map_id,
                                          string &snapshot_path)
{
    if (public_dir.empty() || !_is_account_name(owner_name)
        || !_is_map_id(map_id))
    {
        return false;
    }
    housing_snapshot_meta meta;
    return static_cast<bool>(_resolve_public_snapshot(
        public_dir, owner_name, map_id, snapshot_path, meta));
}

static std::unique_ptr<package> _open_public_snapshot(const string &owner,
                                                      const string &map_id,
                                                      housing_snapshot_meta &meta)
{
    const string public_dir = _getenv_string("CRAWL_HOUSING_PUBLIC_DIR");
    if (public_dir.empty() || !_is_account_name(owner) || !_is_map_id(map_id))
        return nullptr;

    string snapshot_path;
    return _resolve_public_snapshot(public_dir, owner, map_id, snapshot_path,
                                    meta);
}

static string _anonymous_package_directory(package &source)
{
    // Prefer the package's own directory: canonical owner saves and the first
    // URL-visitor capsule both have a caller-selected filename. Relative
    // console save paths remain valid because this is the exact path already
    // opened by Crawl, not a separately supplied temp-directory setting.
    const string source_path = source.get_filename();
    if (!source_path.empty() && source_path != "[tmp]")
    {
        string source_dir = _path_without_trailing_separators(
            get_parent_directory(source_path));
        if (source_dir.empty())
            source_dir = ".";
        if (dir_exists(source_dir))
            return source_dir;
    }

    // Later visitor-to-visitor hops use an already anonymous source. Prefer
    // their server-created session root so disposable character data never
    // acquires even a short-lived directory entry beside the canonical save.
    const string session_dir = _path_without_trailing_separators(
        _getenv_string("CRAWL_HOUSING_SESSION_DIR"));
    if (is_absolute_path(session_dir) && dir_exists(session_dir))
        return session_dir;

    // Owner-origin visits have no session root. Keep their anonymous capsule
    // beside the authenticated canonical save rather than falling back to the
    // launcher's possibly read-only CWD.
    if (!_housing_canonical_save_path.empty())
    {
        string canonical_dir = _path_without_trailing_separators(
            get_parent_directory(_housing_canonical_save_path));
        if (canonical_dir.empty())
            canonical_dir = ".";
        if (dir_exists(canonical_dir))
            return canonical_dir;
    }

    const string canonical = _getenv_string("CRAWL_HOUSING_CANONICAL_SAVE");
    if (is_absolute_path(canonical))
    {
        const string canonical_dir = _path_without_trailing_separators(
            get_parent_directory(canonical));
        if (dir_exists(canonical_dir))
            return canonical_dir;
    }

    fail("Housing has no safe directory for its temporary save");
}

static std::unique_ptr<package> _clone_with_snapshot_level(
    package &source, package &snapshot)
{
    std::unique_ptr<package> clone(
        new package(_anonymous_package_directory(source)));
    housing_copy_visitor_state(source, *clone);
    clone->copy_chunk_from(snapshot, HOUSING_LEVEL_CHUNK,
                           _housing_save_level_chunk());
    clone->commit();
    return clone;
}

static void _checkpoint_owner_before_visit()
{
    ASSERT(housing_is_owner());
    ASSERT(you.save);
    ASSERT(you.on_current_level);

    // This checkpoint is unconditional: even a zero-turn portal traversal must
    // establish the exact owner rollback boundary before any map-bound state is
    // cleared or the save sink is swapped.
    save_level(level_id::current());
    housing_sync_current_map();
    save_game(false);
    you.save->commit();
    housing_publish_current_map();
}

static bool _open_detached_owner_save()
{
    if (_housing_owner_save)
        return true;

    const string filename = _housing_canonical_save_path.empty()
                                ? _getenv_string("CRAWL_HOUSING_CANONICAL_SAVE")
                                : _housing_canonical_save_path;
    const string session_dir = _getenv_string("CRAWL_HOUSING_SESSION_DIR");
    if (filename.empty() || !is_absolute_path(filename)
        || get_base_filename(filename) != you.your_name + ".cs"
        || (!session_dir.empty() && _path_is_within(filename, session_dir)))
    {
        return false;
    }

    std::unique_ptr<package> owner(new package(filename.c_str(), true));
    if (!owner->has_chunk("chr") || !owner->has_chunk("you")
        || !owner->has_chunk(_housing_save_level_chunk()))
    {
        owner->abort();
        return false;
    }
    _housing_owner_save = owner.release();
    _housing_canonical_save_path = filename;
    return true;
}

static bool _valid_owner_context_file(const string &path,
                                      const string &expected_basename,
                                      const string &session_dir,
                                      bool may_be_missing)
{
    if (path.empty() || !is_absolute_path(path)
        || get_base_filename(path) != expected_basename)
    {
        return false;
    }

    if (file_exists(path))
        return session_dir.empty() || !_path_is_within(path, session_dir);
    if (!may_be_missing)
        return false;

    const string parent = _path_without_trailing_separators(
        get_parent_directory(path));
    return dir_exists(parent)
        && (session_dir.empty() || !_path_is_within(parent, session_dir));
}

static bool _valid_owner_context_directory(const string &path,
                                           const string &session_dir)
{
    if (path.empty() || !is_absolute_path(path) || file_exists(path))
        return false;

    // fixup_options() creates a missing morgue directory. Validate its existing
    // parent in that case so a new account is not rejected merely because it
    // has never produced a morgue file, while still keeping visitor output out
    // of the disposable session tree.
    const string existing = dir_exists(path)
                                ? path
                                : _path_without_trailing_separators(
                                      get_parent_directory(path));
    return dir_exists(existing)
        && (session_dir.empty() || !_path_is_within(existing, session_dir));
}

static bool _apply_canonical_owner_context()
{
    // An owner-origin process never changed its rc/macro/morgue context; only
    // URL-launched visitor sessions need to switch back from server-created
    // disposable paths before their same-process owner restore.
    if (!_housing_started_as_visitor)
        return true;

    const string session_dir = _getenv_string("CRAWL_HOUSING_SESSION_DIR");
    const string save = _housing_canonical_save_path;
    const string rc = _getenv_string("CRAWL_HOUSING_CANONICAL_RC");
    const string macro = _getenv_string("CRAWL_HOUSING_CANONICAL_MACRO");
    const string morgue = _getenv_string("CRAWL_HOUSING_CANONICAL_MORGUE");
    if (session_dir.empty()
        || !_valid_owner_context_file(save, you.your_name + ".cs",
                                      session_dir, false)
        || !_valid_owner_context_file(rc, you.your_name + ".rc",
                                      session_dir, true)
        || !_valid_owner_context_file(macro, you.your_name + ".macro",
                                      session_dir, true)
        || !_valid_owner_context_directory(morgue, session_dir))
    {
        return false;
    }

    // In a DGAMELAUNCH build crawl_dir is the base save directory itself.
    // The canonical file is <crawl_dir>/housing/<name>.cs.
    const string housing_dir = _path_without_trailing_separators(
        get_parent_directory(save));
    string crawl_dir = _path_without_trailing_separators(
        get_parent_directory(housing_dir));
#ifndef DGAMELAUNCH
    // Non-DGL reset_paths() appends "saves/" to crawl_dir. A canonical save at
    // <crawl_dir>/saves/housing/<name>.cs therefore needs one more parent than
    // the DGL layout, where crawl_dir is already the base save directory.
    crawl_dir = _path_without_trailing_separators(
        get_parent_directory(crawl_dir));
#endif
    if (crawl_dir.empty() || !dir_exists(crawl_dir)
        || _path_is_within(crawl_dir, session_dir))
    {
        return false;
    }

    SysEnv.crawl_dir = crawl_dir;
    SysEnv.crawl_rc = rc;
    SysEnv.macro_dir = macro;
    SysEnv.morgue_dir = morgue;
    Options.reset_paths();
    Options.morgue_dir = morgue;
    Options.game.name = you.your_name;
    Options.game.type = GAME_TYPE_HOUSING;
    Options.game.filename = get_base_filename(save);
    return true;
}

static std::unique_ptr<package> _stage_owner_map(package &canonical,
                                                 const string &target_map,
                                                 string &previous_map,
                                                 bool &template_needs_refresh)
{
    template_needs_refresh = false;
    if (!_is_map_id(target_map)
        || !canonical.has_chunk(_housing_save_level_chunk()))
    {
        return nullptr;
    }

    vector<string> maps;
    string current_map;
    const bool legacy = !housing_read_map_index(canonical, maps, current_map);
    if (legacy)
    {
        if (target_map != "main")
            return nullptr;
        maps.push_back("main");
        current_map = "main";
    }
    if (std::find(maps.begin(), maps.end(), target_map) == maps.end())
        return nullptr;
    if (!legacy
        && !_package_chunks_equal(canonical, _housing_save_level_chunk(),
                                  housing_map_chunk_name(current_map)))
    {
        corrupted("The canonical Housing level does not match its map index");
    }

    std::unique_ptr<package> staged(
        new package(_anonymous_package_directory(canonical)));
    for (const string &chunk : canonical.list_chunks())
        staged->copy_chunk_from(canonical, chunk, chunk);
    if (legacy)
    {
        _copy_package_chunk(*staged, _housing_save_level_chunk(),
                            housing_map_chunk_name("main"));
    }
    if (!staged->has_chunk(HOUSING_TEMPLATE_CHUNK))
    {
        _copy_package_chunk(*staged, housing_map_chunk_name("main"),
                            HOUSING_TEMPLATE_CHUNK);
    }
    if (current_map != target_map)
    {
        _copy_package_chunk(*staged, housing_map_chunk_name(target_map),
                            _housing_save_level_chunk());
    }
    housing_write_map_index(*staged, maps, target_map);
    staged->commit();
    previous_map = current_map;
    template_needs_refresh = legacy;
    return staged;
}

static NORETURN void _restart_canonical_after_owner_transition_failure(
    package *staged, package *canonical, const string &notice,
    bool exact_rollback = false, int rollback_turn_origin = -1)
{
    if (!staged)
        staged = _housing_staged_owner_save;
    const string filename = canonical
                                ? canonical->get_filename()
                                : _housing_canonical_save_path;
    if (_housing_restore_save == staged || _housing_restore_save == canonical)
        _housing_restore_save = nullptr;
    if (you.save == staged || you.save == canonical)
        you.save = nullptr;
    if (_housing_owner_save == staged || _housing_owner_save == canonical)
        _housing_owner_save = nullptr;
    if (_housing_visitor_save == staged || _housing_visitor_save == canonical)
        _housing_visitor_save = nullptr;
    if (_housing_owner_promotion_save == staged
        || _housing_owner_promotion_save == canonical)
    {
        _housing_owner_promotion_save = nullptr;
    }
    _housing_staged_owner_save = nullptr;
    _housing_owner_map_changed = false;
    _housing_owner_template_needs_refresh = false;
    if (staged)
    {
        staged->abort();
        delete staged;
    }

    if (canonical && canonical != staged)
    {
        canonical->abort();
        delete canonical;
    }

    std::unique_ptr<package> reopened(new package(filename.c_str(), true));
    vector<string> maps;
    string current_map;
    if (!housing_read_map_index(*reopened, maps, current_map))
        current_map = "main";

    _housing_owner_promotion_save = nullptr;
    _housing_runtime_role = housing_role_type::owner;
    _housing_current_map_owner.clear();
    _housing_current_map_id = current_map;
    _housing_owner_save = reopened.release();
    _housing_restore_save = _housing_owner_save;
    _housing_owner_rollback_restore_pending = true;
    _housing_visitor_rollback_restore_pending = false;
    _housing_visitor_save = nullptr;
    _housing_skip_next_checkpoint = true;
    _housing_entry_requires_spawn = false;
    _housing_map_entry_finished = false;
    _housing_exact_map_rollback = exact_rollback;
    if (exact_rollback)
        _housing_turn_origin = rollback_turn_origin;
    _housing_pending_notice = notice;
    // _reset_game() clears crawl_state and the player, while _post_init() has
    // already re-read the rc file and reset Options.game. Pin the canonical
    // identity so the next startup is Housing before any role-dependent code
    // runs. The restore hook still owns the package and never trusts this path
    // to reopen or create a save.
    if (!you.your_name.empty())
        _housing_restore_name = you.your_name;
    Options.game.name = _housing_restore_name;
    Options.game.type = GAME_TYPE_HOUSING;
    Options.game.filename = get_base_filename(filename);
    macro_clear_mappings();
    game_ended(game_exit::housing_transition);
}

static void _promote_staged_owner_map(bool exact_rollback,
                                      int rollback_turn_origin)
{
    ASSERT(housing_is_owner());
    ASSERT(_housing_owner_promotion_save);
    ASSERT(you.save);

    package *staged = you.save;
    package *canonical = _housing_owner_promotion_save;
    vector<string> maps;
    string target_map;

    try
    {
        if (!housing_read_map_index(*staged, maps, target_map)
            || target_map != _housing_current_map_id)
        {
            fail("The staged Housing owner map does not match its index");
        }

        // Promote only after TAG_LEVEL has loaded and a valid spawn was chosen.
        // D, the named target, housing_index and TAG_YOU then enter the
        // canonical package in one commit made by save_game(false).
        you.save = canonical;
        save_level(level_id::current());
        if (_housing_owner_template_needs_refresh)
        {
            _copy_package_chunk(*canonical, _housing_save_level_chunk(),
                                HOUSING_TEMPLATE_CHUNK);
        }
        else if (!canonical->has_chunk(HOUSING_TEMPLATE_CHUNK))
        {
            canonical->copy_chunk_from(*staged, HOUSING_TEMPLATE_CHUNK,
                                       HOUSING_TEMPLATE_CHUNK);
        }
        _copy_package_chunk(*canonical, _housing_save_level_chunk(),
                            housing_map_chunk_name(target_map));
        housing_write_map_index(*canonical, maps, target_map);
        save_game(false);
        canonical->commit();
    }
    catch (const std::exception &error)
    {
        dprf("Housing owner map promotion failed: %s", error.what());
        fprintf(stderr, "Housing owner map promotion failed: %s\n",
                error.what());
        fflush(stderr);
        _restart_canonical_after_owner_transition_failure(
            staged, canonical,
            "That Housing map could not be saved safely; the last committed "
            "owner map was restored.", exact_rollback,
            rollback_turn_origin);
    }

    staged->abort();
    delete staged;
    _housing_staged_owner_save = nullptr;
    _housing_owner_promotion_save = nullptr;
    _housing_owner_map_changed = false;
    _housing_owner_template_needs_refresh = false;
    _housing_owner_save = canonical;
    you.save = canonical;
    _housing_restore_name.clear();
}

bool housing_transition_restore_pending()
{
    return housing_owner_restore_pending()
        || _housing_visitor_rollback_restore_pending;
}

bool housing_owner_restore_pending()
{
    // Both tokens outlive _reset_game() and remain true after
    // housing_open_save_for_restore() consumes _housing_restore_save. This
    // keeps staged and canonical rollback restores out of chargen and generic
    // corrupted-save deletion prompts. Do not consult crawl_state or `you`
    // here: _reset_game() deliberately clears both before startup asks this
    // question again.
    return _housing_owner_promotion_save
        || _housing_owner_rollback_restore_pending;
}

bool housing_owner_restore_is_staged()
{
    return _housing_owner_promotion_save && _housing_staged_owner_save;
}

void housing_complete_staged_owner_restore()
{
    if (_housing_visitor_rollback_restore_pending)
    {
        // The exact previous visitor package has passed restore_game(), all
        // post-load startup work and its no-cleanup/no-respawn entry hook.
        _housing_visitor_rollback_restore_pending = false;
        _housing_restore_name.clear();
        update_whereis();
        return;
    }
    if (_housing_owner_rollback_restore_pending
        && !housing_owner_restore_is_staged())
    {
        // The canonical package has passed restore_game(), _post_init(), level
        // validation, rc/Lua initialisation and the first view setup. It is
        // now safe for later transitions to distinguish this completed
        // rollback from a still-guarded restart.
        _housing_owner_rollback_restore_pending = false;
        _housing_restore_name.clear();
        update_whereis();
        return;
    }
    if (!housing_owner_restore_is_staged())
        return;

    // The early load hook already validated the target and placed its spawn.
    // Promotion waits until startup_step has completed all remaining Lua, rc,
    // tile, travel and view initialisation.
    if (_housing_entry_requires_spawn)
        fail("The staged Housing owner map was not finalised during load");
    _promote_staged_owner_map();
    update_whereis();
}

void housing_rollback_staged_owner_restore()
{
    if (!housing_transition_restore_pending())
        return;

    _housing_entry_requires_spawn = false;
    if ((_housing_owner_rollback_restore_pending
         || _housing_visitor_rollback_restore_pending)
        && !_housing_owner_promotion_save)
    {
        // The one allowed exact rollback has itself failed to restore.
        // Retrying the same bytes would create an unbounded housing_transition
        // loop. Drop every alias before deleting the package once, then stop
        // this process without ever offering chargen or save deletion.
        std::set<package*> failed_saves;
        failed_saves.insert(you.save);
        failed_saves.insert(_housing_owner_save);
        failed_saves.insert(_housing_restore_save);
        failed_saves.insert(_housing_staged_owner_save);
        failed_saves.insert(_housing_owner_promotion_save);
        failed_saves.insert(_housing_visitor_save);
        failed_saves.erase(nullptr);
        you.save = nullptr;
        _housing_owner_save = nullptr;
        _housing_restore_save = nullptr;
        _housing_staged_owner_save = nullptr;
        _housing_owner_promotion_save = nullptr;
        _housing_visitor_save = nullptr;
        _housing_owner_rollback_restore_pending = false;
        _housing_visitor_rollback_restore_pending = false;
        _housing_owner_map_changed = false;
        _housing_owner_template_needs_refresh = false;
        _housing_restore_name.clear();
        for (package *failed : failed_saves)
        {
            failed->abort();
            delete failed;
        }
        game_ended(game_exit::crash,
                   "The previous Housing state could not be "
                   "restored safely.");
    }

    package *canonical = _housing_owner_promotion_save;
    _restart_canonical_after_owner_transition_failure(
        _housing_staged_owner_save, canonical,
        "That Housing owner map could not be loaded safely; the last "
        "committed owner state was restored.");
}

static NORETURN void _return_to_owner(std::unique_ptr<package> staged,
                                      const string &target_map,
                                      const string &previous_map,
                                      bool template_needs_refresh)
{
    ASSERT(housing_is_visitor());
    ASSERT(_housing_owner_save);
    ASSERT(staged);
    ASSERT(you.save);

    package *visitor = you.save;
    _housing_restore_name = you.your_name;
    const string canonical_filename = _housing_owner_save->get_filename();
    you.save = nullptr;
    _housing_visitor_save = nullptr;
    visitor->abort();
    delete visitor;

    _housing_runtime_role = housing_role_type::owner;
    _housing_current_map_owner.clear();
    _housing_current_map_id = target_map;
    _housing_owner_promotion_save = _housing_owner_save;
    _housing_staged_owner_save = staged.release();
    _housing_restore_save = _housing_staged_owner_save;
    _housing_owner_map_changed = target_map != previous_map;
    _housing_owner_template_needs_refresh = template_needs_refresh;
    _housing_skip_next_checkpoint = false;
    _housing_entry_requires_spawn = _housing_owner_map_changed;
    _housing_map_entry_finished = false;
    _housing_exact_map_rollback = false;
    // A non-DGL WebTiles restart consults Options.game before the supplied
    // staged package reaches restore_game(). Preserve the canonical identity
    // across _reset_game(), just as the transactional rollback paths do, so
    // Return Home can never fall through to the main menu or chargen.
    Options.game.name = _housing_restore_name;
    Options.game.type = GAME_TYPE_HOUSING;
    Options.game.filename = get_base_filename(canonical_filename);
    macro_clear_mappings();
    game_ended(game_exit::housing_transition);
}

static bool _return_to_owner_map(const string &target_map)
{
    if (!housing_is_visitor())
    {
        mpr("Only a Housing visitor can return home this way.");
        return false;
    }

    try
    {
        if (!_open_detached_owner_save())
        {
            mprf(MSGCH_ERROR, "%s", _owner_return_failure);
            return false;
        }

        string previous_map;
        bool template_needs_refresh;
        std::unique_ptr<package> staged =
            _stage_owner_map(*_housing_owner_save, target_map, previous_map,
                             template_needs_refresh);
        if (!staged || !_apply_canonical_owner_context())
        {
            _housing_owner_save->abort();
            delete _housing_owner_save;
            _housing_owner_save = nullptr;
            mprf(MSGCH_ERROR, "%s", _owner_return_failure);
            return false;
        }
        _return_to_owner(std::move(staged), target_map, previous_map,
                         template_needs_refresh);
    }
    catch (const game_ended_condition&)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        dprf("Housing owner restore open failed: %s", error.what());
        if (_housing_owner_save)
            _housing_owner_save->abort();
        delete _housing_owner_save;
        _housing_owner_save = nullptr;
        mprf(MSGCH_ERROR, "%s", _owner_return_failure);
        return false;
    }

    return false;
}

bool housing_return_home()
{
    return _return_to_owner_map("main");
}

static bool _enter_owner_map(const string &target_map)
{
    if (!housing_is_owner() || !you.save || !_is_map_id(target_map))
        return false;
    if (!_ensure_owner_map_storage())
        return false;

    vector<string> maps;
    string current_map;
    if (!housing_read_map_index(*you.save, maps, current_map)
        || std::find(maps.begin(), maps.end(), target_map) == maps.end())
    {
        mpr("That Housing map does not exist.");
        return false;
    }
    if (current_map == target_map)
    {
        mprf("You are already on Housing map '%s'.", target_map.c_str());
        return false;
    }

    package *canonical = you.save;
    _housing_restore_name = you.your_name;
    package *staged_save = nullptr;
    const int rollback_turn_origin = _housing_turn_origin;
    try
    {
        _checkpoint_owner_before_visit();

        string previous_map;
        bool template_needs_refresh;
        std::unique_ptr<package> staged =
            _stage_owner_map(*canonical, target_map, previous_map,
                             template_needs_refresh);
        if (!staged || previous_map != current_map)
            fail("The Housing map changed while staging its transition");

        staged_save = staged.release();
        you.save = staged_save;
        _housing_owner_save = canonical;
        _housing_owner_promotion_save = canonical;
        _housing_staged_owner_save = staged_save;
        _housing_owner_map_changed = true;
        _housing_owner_template_needs_refresh = template_needs_refresh;
        _housing_current_map_id = target_map;
        _housing_entry_requires_spawn = true;
        _housing_map_entry_finished = false;
        _housing_exact_map_rollback = false;

        housing_prepare_map_transition_restore(true);
        const level_id old_level = level_id::current();
        load_level(DNGN_UNSEEN, LOAD_HOUSING_REPLACE, old_level);

        // LOAD_HOUSING_REPLACE has now completed every marker, travel and tile
        // hook. Promote the staged map only after that full boundary succeeds.
        _promote_staged_owner_map(true, rollback_turn_origin);
    }
    catch (const game_ended_condition&)
    {
        if (housing_owner_restore_is_staged())
            housing_rollback_staged_owner_restore();
        throw;
    }
    catch (const std::exception &error)
    {
        dprf("Housing owner map transition failed: %s", error.what());
        fprintf(stderr, "Housing owner map transition failed: %s\n",
                error.what());
        fflush(stderr);
        _housing_entry_requires_spawn = false;
        _restart_canonical_after_owner_transition_failure(
            staged_save, canonical,
            "That Housing map could not be loaded safely; the previous owner "
            "map was restored.", true, rollback_turn_origin);
    }

    crawl_state.need_save = true;
    you.turn_is_over = false;
    const bool published = housing_publish_current_map();
    update_whereis();
    mprf("You enter %s:%s.", you.your_name.c_str(), target_map.c_str());
    if (!published)
    {
        mprf(MSGCH_ERROR,
             "You entered the map, but its public Housing snapshot was not "
             "published. Saving or completing another Housing action will "
             "retry it.");
    }
    return true;
}

static bool _create_owner_map(const string &map_id, int theme)
{
    if (!_is_map_id(map_id) || !_valid_housing_theme(theme))
    {
        mpr("The Housing map id or branch theme is invalid.");
        return false;
    }
    if (!housing_authorize_action("create a map", 0)
        || !_ensure_owner_map_storage())
    {
        return false;
    }

    vector<string> maps;
    string current_map;
    if (!housing_read_map_index(*you.save, maps, current_map))
        fail("The canonical Housing map index disappeared");
    if (std::find(maps.begin(), maps.end(), map_id) != maps.end())
    {
        mpr("A Housing map with that id already exists.");
        return false;
    }
    if (maps.size() >= HOUSING_MAX_MAPS)
    {
        mprf("You can keep at most %d Housing maps.", HOUSING_MAX_MAPS);
        return false;
    }
    if (!you.save->has_chunk(HOUSING_TEMPLATE_CHUNK))
        fail("The canonical Housing map template is missing");

    _copy_package_chunk(*you.save, HOUSING_TEMPLATE_CHUNK,
                        housing_map_chunk_name(map_id));
    _write_housing_map_theme(*you.save, map_id, theme);
    maps.push_back(map_id);
    housing_write_map_index(*you.save, maps, current_map);
    you.save->commit();
    mprf("Created Housing map '%s' with the %s theme.", map_id.c_str(),
         HOUSING_THEMES[theme].name);
    return true;
}

static bool _unlink_public_map_file(const string &path,
                                    const string &public_dir)
{
    // Resolve the containing directory rather than the entry itself: realpath
    // on a missing or dangling entry cannot distinguish ENOENT from failures
    // such as EACCES/ENOTDIR. Once the parent is proven inside the public tree,
    // unlinking the exact basename is safe even when that entry is a symlink.
    // The public tree is server-owned and not user-writable; this is static
    // corruption hardening, not a boundary against a hostile same-uid process
    // racing directory replacement between these path-based checks.
    const string parent = _path_without_trailing_separators(
        get_parent_directory(path));
    if (parent.empty())
        return false;
#ifndef TARGET_OS_WINDOWS
    struct stat parent_info;
    errno = 0;
    if (lstat(parent.c_str(), &parent_info) != 0)
    {
        // A map that has never been published may have no account/name
        // directories yet. That is a genuine idempotent absence; every other
        // lookup error is retained as a fail-closed deletion failure.
        return errno == ENOENT;
    }
    if (!S_ISDIR(parent_info.st_mode))
        return false;
#else
    return false;
#endif
    if (!_path_is_within(parent, public_dir))
        return false;
    errno = 0;
    if (unlink_u(path.c_str()) == 0)
        return true;
    return errno == ENOENT;
}

bool housing_unpublish_map_files(const string &public_dir,
                                 const string &account_id,
                                 const string &owner_name,
                                 const string &map_id)
{
    if (!_is_account_name(owner_name) || !_is_map_id(map_id))
        return false;
    if (public_dir.empty())
        return account_id.empty();
    if (!_is_decimal_id(account_id))
    {
        return false;
    }

    const string account_path =
        catpath(catpath(public_dir, account_id), map_id + ".hmap");
    const string by_name_path =
        catpath(catpath(catpath(public_dir, "by-name"),
                        _lowercase_ascii(owner_name)), map_id + ".hmap");
    // Remove both externally visitable aliases before canonical deletion. If
    // either exact unlink fails, retain the canonical map so no stale public
    // snapshot can outlive a successfully deleted owner map.
    const bool account_removed =
        _unlink_public_map_file(account_path, public_dir);
    const bool name_removed =
        _unlink_public_map_file(by_name_path, public_dir);
    return account_removed && name_removed;
}

static bool _unpublish_owner_map(const string &map_id)
{
    const string public_dir = _getenv_string("CRAWL_HOUSING_PUBLIC_DIR");
    if (public_dir.empty())
    {
        if (_getenv_string("CRAWL_HOUSING_ACCOUNT_ID").empty())
            return true;
        mprf(MSGCH_ERROR,
             "The Housing public map directory is not configured; the map "
             "was not deleted.");
        return false;
    }

    return housing_unpublish_map_files(
        public_dir, _getenv_string("CRAWL_HOUSING_ACCOUNT_ID"),
        you.your_name, map_id);
}

static bool _delete_owner_map(const string &map_id)
{
    if (map_id == "main")
    {
        mpr("The main Housing map cannot be deleted.");
        return false;
    }
    if (!_is_map_id(map_id)
        || !housing_authorize_action("delete a map", 0)
        || !_ensure_owner_map_storage())
    {
        return false;
    }

    vector<string> maps;
    string current_map;
    if (!housing_read_map_index(*you.save, maps, current_map))
        fail("The canonical Housing map index disappeared");
    auto found = std::find(maps.begin(), maps.end(), map_id);
    if (found == maps.end())
    {
        mpr("That Housing map no longer exists.");
        return false;
    }

    if (current_map == map_id && !_enter_owner_map("main"))
        return false;

    // _enter_owner_map may have swapped packages and invalidated the old index
    // vector's relationship to the canonical directory; reload it.
    maps.clear();
    current_map.clear();
    if (!housing_read_map_index(*you.save, maps, current_map))
        fail("The canonical Housing map index disappeared");
    found = std::find(maps.begin(), maps.end(), map_id);
    if (found == maps.end() || current_map == map_id)
        fail("The Housing map could not be made inactive for deletion");

    if (!_unpublish_owner_map(map_id))
    {
        mprf(MSGCH_ERROR,
             "The public Housing snapshot could not be removed; the map was "
             "kept. It is safe to retry deletion.");
        return false;
    }

    you.save->delete_chunk(housing_map_chunk_name(map_id));
    const string theme_chunk = _housing_map_theme_chunk_name(map_id);
    if (you.save->has_chunk(theme_chunk))
        you.save->delete_chunk(theme_chunk);
    maps.erase(found);
    housing_write_map_index(*you.save, maps, current_map);
    you.save->commit();
    mprf("Deleted Housing map '%s'.", map_id.c_str());
    return true;
}

static string _select_housing_map(const vector<string> &maps,
                                  const string &current_map)
{
    Menu menu(MF_SINGLESELECT | MF_ARROWS_SELECT | MF_INIT_HOVER
              | MF_ALLOW_FORMATTING);
    menu.set_title(new MenuEntry("Housing maps", MEL_TITLE));
    menu.set_more("Select a map, or press <w>+</w> to create one.");

    vector<string> values;
    values.reserve(maps.size() + 1);
    values.push_back("\1create");
    auto *create = new MenuEntry("Create a new map", MEL_ITEM, 1, '+');
    create->data = &values.back();
    menu.add_entry(create);

    menu_letter hotkey('a');
    for (const string &map_id : maps)
    {
        values.push_back(map_id);
        string label = map_id;
        if (map_id == current_map)
            label += "  <lightgreen>[current]</lightgreen>";
        if (map_id == "main")
            label += "  <darkgrey>[protected]</darkgrey>";
        const int map_hotkey = values.size() <= 53
                                   ? static_cast<char>(hotkey++) : 0;
        auto *entry = new MenuEntry(label, MEL_ITEM, 1, map_hotkey);
        entry->data = &values.back();
        menu.add_entry(entry);
    }

    const vector<MenuEntry*> selected = menu.show();
    return selected.empty() ? "" : *static_cast<string*>(selected[0]->data);
}

static char _select_housing_map_action(const string &map_id,
                                       bool is_current)
{
    Menu menu(MF_SINGLESELECT | MF_ARROWS_SELECT | MF_INIT_HOVER);
    menu.set_title(new MenuEntry("Housing map: " + map_id, MEL_TITLE));

    char actions[2] = { 'e', 'd' };
    auto *enter = new MenuEntry(is_current ? "Already on this map"
                                           : "Enter this map",
                                MEL_ITEM, 1, 'e');
    enter->data = &actions[0];
    enter->set_enabled(!is_current);
    menu.add_entry(enter);

    auto *remove = new MenuEntry(map_id == "main" ? "Delete (main is protected)"
                                                   : "Delete this map",
                                 MEL_ITEM, 1, 'd');
    remove->data = &actions[1];
    remove->set_enabled(map_id != "main");
    menu.add_entry(remove);

    const vector<MenuEntry*> selected = menu.show();
    return selected.empty() ? 0 : *static_cast<char*>(selected[0]->data);
}

static int _select_housing_map_theme()
{
    if (!housing_branch_theme_catalog_valid())
        fail("Housing branch theme catalog is invalid");

    Menu menu(MF_SINGLESELECT | MF_ARROWS_SELECT | MF_INIT_HOVER);
    menu.set_title(new MenuEntry("Choose a Housing branch theme", MEL_TITLE));

    vector<int> themes;
    themes.reserve(ARRAYSZ(HOUSING_THEMES));
    menu_letter hotkey('a');
    for (int i = 0;
         i < static_cast<int>(ARRAYSZ(HOUSING_THEME_MENU_ORDER)); ++i)
    {
        const int theme = HOUSING_THEME_MENU_ORDER[i];
        ASSERT(_valid_housing_theme(theme));
        themes.push_back(theme);
        auto *entry = new MenuEntry(HOUSING_THEMES[theme].name, MEL_ITEM, 1,
                                    static_cast<char>(hotkey++));
        entry->data = &themes.back();
        menu.add_entry(entry);
    }

    const vector<MenuEntry*> selected = menu.show();
    return selected.empty() ? -1 : *static_cast<int*>(selected[0]->data);
}

bool housing_manage_maps()
{
    if (!housing_is_owner())
    {
        mpr("Only the owner can manage Housing maps.");
        return false;
    }

    package *canonical = you.save;
    try
    {
        if (!_ensure_owner_map_storage())
            return false;

        vector<string> maps;
        string current_map;
        if (!housing_read_map_index(*you.save, maps, current_map))
            fail("The canonical Housing map index disappeared");

        const string selected = _select_housing_map(maps, current_map);
        if (selected.empty())
            return false;
        if (selected == "\1create")
        {
            char map_id[32] = "";
            if (msgwin_get_line("New map id (1-20 ASCII letters, digits, _): ",
                                map_id, sizeof(map_id))
                || !map_id[0])
            {
                canned_msg(MSG_OK);
                return false;
            }
            if (!housing_valid_map_id(map_id))
            {
                mpr("Map ids use 1-20 ASCII letters, digits, or underscores.");
                return false;
            }
            const int theme = _select_housing_map_theme();
            if (theme < 0)
                return false;
            if (!_create_owner_map(map_id, theme))
                return false;
            // Creation is an in-process owner-map transition. The current
            // character and socket stay intact; only the pristine themed
            // TAG_LEVEL is loaded and then published.
            return _enter_owner_map(map_id);
        }

        const char action = _select_housing_map_action(
            selected, selected == current_map);
        if (action == 'e')
            return _enter_owner_map(selected);
        if (action != 'd')
            return false;
        if (!yesno(make_stringf("Really delete Housing map '%s'?",
                                selected.c_str()).c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
        return _delete_owner_map(selected);
    }
    catch (const game_ended_condition&)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        dprf("Housing map management failed: %s", error.what());
        _restart_canonical_after_owner_transition_failure(
            nullptr, canonical,
            "Housing map management failed safely; the last committed owner "
            "state was restored.", true, _housing_turn_origin);
    }
}

static NORETURN void _restart_previous_map_after_visit_failure(
    package *next, package *previous, housing_role_type previous_role,
    const string &previous_owner, const string &previous_map,
    const string &player_name, int rollback_turn_origin,
    const string &error)
{
    fprintf(stderr, "Housing visitor map transition failed: %s\n",
            error.c_str());
    fflush(stderr);

    if (you.save == next)
        you.save = nullptr;
    if (_housing_visitor_save == next)
        _housing_visitor_save = nullptr;
    next->abort();
    delete next;

    _housing_restore_name = player_name;
    if (previous_role == housing_role_type::owner)
    {
        _restart_canonical_after_owner_transition_failure(
            nullptr, previous,
            "That Housing map could not be loaded safely; the previous map "
            "was restored.", true, rollback_turn_origin);
    }

    // A visitor-to-visitor failure restores the still-open disposable package
    // exactly once. It is not discoverable in the save directory, so pin both
    // its role and startup identity across _reset_game rather than allowing
    // DGL startup to fall through to save discovery or chargen.
    ASSERT(previous_role == housing_role_type::visitor);
    _housing_runtime_role = housing_role_type::visitor;
    _housing_current_map_owner = previous_owner;
    _housing_current_map_id = previous_map;
    _housing_restore_save = previous;
    _housing_owner_save = nullptr;
    _housing_visitor_save = previous;
    _housing_owner_rollback_restore_pending = false;
    _housing_visitor_rollback_restore_pending = true;
    _housing_skip_next_checkpoint = false;
    _housing_entry_requires_spawn = false;
    _housing_map_entry_finished = false;
    _housing_exact_map_rollback = true;
    _housing_turn_origin = rollback_turn_origin;
    _housing_pending_notice =
        "That Housing map could not be loaded safely; the previous map was "
        "restored.";
    Options.game.name = _housing_restore_name;
    Options.game.type = GAME_TYPE_HOUSING;
    Options.game.filename = get_save_filename(_housing_restore_name);
    macro_clear_mappings();
    game_ended(game_exit::housing_transition);
}

static void _replace_with_visitor_map(std::unique_ptr<package> replacement,
                                      const string &owner,
                                      const string &map_id)
{
    ASSERT(replacement);
    ASSERT(you.save);

    package *previous = you.save;
    package *next = replacement.release();
    const housing_role_type previous_role = housing_current_role();
    const bool previous_was_owner = previous_role == housing_role_type::owner;
    const string previous_map_owner = _housing_current_map_owner;
    const string previous_map_id = _housing_current_map_id;
    const string previous_player_name = you.your_name;
    const int rollback_turn_origin = _housing_turn_origin;
    if (previous_was_owner)
        _housing_canonical_save_path = previous->get_filename();

    you.save = next;
    _housing_visitor_save = next;
    _housing_runtime_role = housing_role_type::visitor;
    _housing_current_map_owner = owner;
    _housing_current_map_id = map_id;
    _housing_map_entry_finished = false;
    _housing_exact_map_rollback = false;

    // Everything below the checkpoint mutates only the disposable visitor
    // state. The ordinary level loader resets env/menv and all tile caches;
    // these hooks clear the remaining character/global references to the old
    // map before TAG_LEVEL is restored.
    try
    {
        housing_prepare_map_transition_restore(true);
        const level_id old_level = level_id::current();
        load_level(DNGN_UNSEEN, LOAD_HOUSING_REPLACE, old_level);
    }
    catch (const std::exception &error)
    {
        // The loader may already have reset global level state, so continuing
        // the input loop is unsafe. Roll the whole Crawl game state back from
        // the still-open previous package, while retaining the process/socket.
        _restart_previous_map_after_visit_failure(
            next, previous, previous_role, previous_map_owner,
            previous_map_id, previous_player_name, rollback_turn_origin,
            error.what());
    }
    catch (...)
    {
        _restart_previous_map_after_visit_failure(
            next, previous, previous_role, previous_map_owner,
            previous_map_id, previous_player_name, rollback_turn_origin,
            "unknown exception");
    }

    // The replacement is fully loaded now. Close the prior package; a durable
    // owner is reopened only when returning home, so visits hold no canonical
    // save lock.
    if (previous_was_owner)
    {
        _housing_owner_save = nullptr;
        delete previous;
    }
    else
    {
        previous->abort();
        delete previous;
    }

    crawl_state.need_save = true;
    you.turn_is_over = false;
    update_whereis();
    mprf("You enter %s:%s.", owner.c_str(), map_id.c_str());
}

bool housing_travel_to_target(const string &target)
{
    if (!crawl_state.game_is_housing() || !housing_valid_map_target(target))
    {
        mpr("Use an account:map identifier (ASCII letters, digits, and _).");
        return false;
    }

    string owner;
    string map_id;
    if (!_split_map_target(target, owner, map_id))
        return false;

    // A portal bearing our authenticated character name enters that canonical
    // owner map. From a visitor session this first restores the last committed
    // owner character; from another owner map it is an in-process level swap.
    if (_lowercase_ascii(owner) == _lowercase_ascii(you.your_name))
    {
        if (housing_is_owner())
            return _enter_owner_map(map_id);
        return _return_to_owner_map(map_id);
    }

    try
    {
        housing_snapshot_meta meta;
        std::unique_ptr<package> snapshot =
            _open_public_snapshot(owner, map_id, meta);
        if (!snapshot)
        {
            mprf(MSGCH_ERROR, "That Housing map is unavailable.");
            return false;
        }

        const string own_account_id =
            _getenv_string("CRAWL_HOUSING_ACCOUNT_ID");
        if (!_is_decimal_id(own_account_id)
            || meta.account_id == own_account_id)
        {
            mprf(MSGCH_ERROR, "That Housing map cannot be visited this way.");
            return false;
        }

        if (housing_is_owner())
            _checkpoint_owner_before_visit();
        else
        {
            // Make the previous disposable package a complete rollback point
            // before loading a second visitor map.
            save_level(level_id::current());
            save_game(false);
            you.save->commit();
        }
        std::unique_ptr<package> replacement =
            _clone_with_snapshot_level(*you.save, *snapshot);
        _replace_with_visitor_map(std::move(replacement), meta.owner_name,
                                  meta.map_id);
    }
    catch (const game_ended_condition&)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        dprf("Housing map transition failed: %s", error.what());
        mprf(MSGCH_ERROR, "That Housing map is unavailable.");
        return false;
    }
    return true;
}

bool housing_take_portal(const coord_def &pos)
{
    if (!crawl_state.game_is_housing()
        || !map_bounds(pos) || env.grid(pos) != DNGN_ENTER_PORTAL_VAULT)
    {
        return false;
    }

    string target;
    if (!_portal_target_at(pos, &target))
    {
        mprf(MSGCH_ERROR,
             "This Housing portal is malformed and cannot be used.");
        return true;
    }

    // Returning true means the terrain was handled, even if its destination
    // was unavailable; callers must not fall through to ordinary portal code.
    housing_travel_to_target(target);
    return true;
}

static bool _housing_monster_can_be_published(const monster &mons)
{
    return mons.alive()
        && housing_monster_type_allowed(mons.type)
        && testbits(mons.flags, MF_NO_REWARD)
        && _housing_created_monster(mons)
        && mons.props.exists(NEVER_CORPSE_KEY);
}

static bool _housing_shop_can_be_published(const coord_def &pos)
{
    const auto found = env.shop.find(pos);
    if (found == env.shop.end())
        return false;
    const shop_struct &shop = found->second;
    if (shop.pos != pos || shop.type < 0 || shop.type >= NUM_SHOPS
        || shop.stock.size() > 64)
    {
        return false;
    }
    return std::all_of(shop.stock.begin(), shop.stock.end(),
                       [](const item_def &item) { return item.is_valid(); });
}

static bool _current_map_can_be_published()
{
    const vector<coord_def> spawns = _stored_spawns();
    if (spawns.empty())
    {
        mprf(MSGCH_ERROR, "Housing publish rejected: no valid spawn.");
        return false;
    }

    for (rectangle_iterator pos(0); pos; ++pos)
    {
        if (!map_bounds(*pos))
            continue;
        const dungeon_feature_type feat = env.grid(*pos);
        // Encompass vaults legitimately leave unused cells outside their map
        // as unseen. They are serialized but never editable or valid spawns.
        if (feat == DNGN_UNSEEN)
            continue;
        if (feat == DNGN_RUNELIGHT)
        {
            if (!housing_is_spawn(*pos) || !_spawn_marker_at(*pos))
            {
                mprf(MSGCH_ERROR,
                     "Housing publish rejected: invalid spawn at (%d,%d).",
                     pos->x, pos->y);
                return false;
            }
        }
        else if (feat == DNGN_ENTER_SHOP)
        {
            if (!_housing_shop_can_be_published(*pos))
            {
                mprf(MSGCH_ERROR,
                     "Housing publish rejected: invalid shop at (%d,%d).",
                     pos->x, pos->y);
                return false;
            }
        }
        else if (feat == DNGN_ABANDONED_SHOP)
        {
            // An exhausted Housing merchant leaves an inert visible fixture.
        }
        else if (feat == DNGN_ENTER_PORTAL_VAULT)
        {
            if (!_portal_target_at(*pos, nullptr))
            {
                mprf(MSGCH_ERROR,
                     "Housing publish rejected: invalid portal at (%d,%d).",
                     pos->x, pos->y);
                return false;
            }
        }
        else if (feat == DNGN_TRANSPORTER_LANDING)
        {
            if (!_visitor_strip_marker_at(*pos))
            {
                mprf(MSGCH_ERROR,
                     "Housing publish rejected: invalid visitor inventory "
                     "tile at (%d,%d).", pos->x, pos->y);
                return false;
            }
        }
        else if (feat == DNGN_STONE_ARCH
                 && _feature_tile_override_at(*pos,
                                              HOUSING_LOCAL_PORTAL_TILE))
        {
            if (!_local_portal_name_at(*pos, nullptr))
            {
                mprf(MSGCH_ERROR,
                     "Housing publish rejected: unauthenticated named "
                     "passage tile at (%d,%d).", pos->x, pos->y);
                return false;
            }
        }
        else if (!housing_feature_allowed(feat))
        {
            mprf(MSGCH_ERROR,
                 "Housing publish rejected: feature %d at (%d,%d).",
                 static_cast<int>(feat), pos->x, pos->y);
            return false;
        }
    }
    int housing_monsters = 0;
    for (const monster &mons : menv_real)
        if (mons.alive())
        {
            if (++housing_monsters > HOUSING_MAX_MONSTERS
                || !_housing_monster_can_be_published(mons))
            {
                mprf(MSGCH_ERROR,
                     "Housing publish rejected: unsafe monster %d.",
                     static_cast<int>(mons.type));
                return false;
            }
        }

    if (env.shop.size() > HOUSING_MAX_SHOPS)
    {
        mprf(MSGCH_ERROR, "Housing publish rejected: too many shops.");
        return false;
    }
    for (const auto &entry : env.shop)
        if (env.grid(entry.first) != DNGN_ENTER_SHOP
            || !_housing_shop_can_be_published(entry.first))
        {
            mprf(MSGCH_ERROR,
                 "Housing publish rejected: inconsistent shop state.");
            return false;
        }

    int local_portals = 0;
    for (map_marker *marker : env.markers.get_all())
    {
        const bool local_portal =
            _local_portal_name_at(marker->pos, nullptr);
        if (local_portal)
            ++local_portals;
        if ((marker->get_type() != MAT_WIZ_PROPS
             || (!_portal_target_at(marker->pos, nullptr)
                 && !_spawn_marker_at(marker->pos)
                 && !_current_visitor_wall_marker_at(marker->pos)
                 && !local_portal
                 && !_visitor_strip_marker_at(marker->pos))))
        {
            mprf(MSGCH_ERROR,
                 "Housing publish rejected: marker %d at (%d,%d).",
                 static_cast<int>(marker->get_type()),
                 marker->pos.x, marker->pos.y);
            return false;
        }
    }
    if (local_portals > HOUSING_MAX_LOCAL_PORTALS)
    {
        mprf(MSGCH_ERROR,
             "Housing publish rejected: too many named passage endpoints.");
        return false;
    }

    // Public snapshots are data, not executable map definitions. Only the
    // strictly validated authenticated Housing fixtures above are accepted.
    if (!env.cloud.empty())
    {
        mprf(MSGCH_ERROR, "Housing publish rejected: cloud state.");
        return false;
    }
    return true;
}

void housing_sync_current_map()
{
    const string expected_map = _housing_current_map_id;
    if (!_ensure_owner_map_storage())
        fail("The canonical Housing map index is unavailable");

    vector<string> maps;
    string current_map;
    if (!housing_read_map_index(*you.save, maps, current_map))
        fail("The canonical Housing map index disappeared");
    if ((!expected_map.empty() && current_map != expected_map)
        || current_map != _housing_current_map_id
        || !you.save->has_chunk(_housing_save_level_chunk()))
    {
        fail("The active Housing map does not match its canonical index");
    }

    _copy_package_chunk(*you.save, _housing_save_level_chunk(),
                        housing_map_chunk_name(current_map));
    housing_write_map_index(*you.save, maps, current_map);
}

static void _write_public_snapshot(const string &final_path,
                                   const string &account_id,
                                   const string &map_id)
{
    const string temporary_path = final_path + ".tmp";
    package *snapshot = nullptr;
    try
    {
        snapshot = new package(temporary_path.c_str(), true, true);
        _write_snapshot_meta(*snapshot, account_id, map_id);
        snapshot->copy_chunk_from(*you.save, _housing_save_level_chunk(),
                                  HOUSING_LEVEL_CHUNK);
        snapshot->commit();
        delete snapshot;
        snapshot = nullptr;
        if (rename_u(temporary_path.c_str(), final_path.c_str()) != 0)
            sysfail("could not publish Housing map");
    }
    catch (...)
    {
        if (snapshot)
        {
            snapshot->abort();
            delete snapshot;
        }
        unlink_u(temporary_path.c_str());
        throw;
    }
}

bool housing_publish_current_map()
{
    if (!housing_is_owner())
        return true;
    if (!you.save || !you.on_current_level)
        return false;

    const string account_id = _getenv_string("CRAWL_HOUSING_ACCOUNT_ID");
    const string map_id = housing_current_map_id();
    string public_dir = _getenv_string("CRAWL_HOUSING_PUBLIC_DIR");
    if (public_dir.empty())
    {
        // A completely unbound console session deliberately has nowhere to
        // publish. Once WebTiles supplies an account id, omitting its public
        // directory is a deployment failure and must not be reported as a
        // successful map creation/entry.
        if (account_id.empty())
            return true;
        mprf(MSGCH_ERROR,
             "The Housing public map directory is not configured.");
        return false;
    }

    if (!_is_decimal_id(account_id) || !_is_map_id(map_id)
        || !_is_account_name(you.your_name))
    {
        mprf(MSGCH_ERROR,
             "The Housing account/map publication binding is invalid.");
        return false;
    }
    if (!_current_map_can_be_published())
    {
        mprf(MSGCH_ERROR, "This Housing map is not safe to publish.");
        return false;
    }
    const string save_level_chunk = _housing_save_level_chunk();
    if (!you.save->has_chunk(save_level_chunk))
    {
        mprf(MSGCH_ERROR, "The Housing level is not ready to publish.");
        return false;
    }

    if (!check_mkdir("Housing public map directory", &public_dir, true))
    {
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
        return false;
    }
    string account_dir = catpath(public_dir, account_id);
    if (!check_mkdir("Housing account map directory", &account_dir, true))
    {
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
        return false;
    }

    try
    {
        // Bind the display name before exposing a numeric payload. A crash in
        // between makes a new name temporarily unavailable; it can never make
        // that name select another account or a duplicate by-name generation.
        string by_name_dir = catpath(public_dir, "by-name");
        if (!check_mkdir("Housing name index directory", &by_name_dir, true))
            fail("could not create Housing name index");
        string owner_dir = catpath(by_name_dir,
                                   _lowercase_ascii(you.your_name));
        if (!check_mkdir("Housing owner name index directory", &owner_dir,
                         true))
        {
            fail("could not create Housing owner name index");
        }
        if (!_bind_public_owner(public_dir, you.your_name, account_id))
            fail("Housing owner name is bound to another account");

        // This is the sole payload generation written by current cores.
        // Direct travel resolves the stable binding back to this same atomic
        // account-id file; legacy by-name map files are discovery metadata
        // only and are never served as a competing payload generation.
        _write_public_snapshot(catpath(account_dir, map_id + ".hmap"),
                               account_id, map_id);
        return true;
    }
    catch (const std::exception &error)
    {
        dprf("Housing map publication failed: %s", error.what());
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
        return false;
    }
    catch (...)
    {
        dprf("Housing map publication failed with a non-standard exception");
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
        return false;
    }
}

void housing_checkpoint()
{
    if (_housing_skip_next_checkpoint)
    {
        _housing_skip_next_checkpoint = false;
        return;
    }
    if (!housing_is_owner() || !crawl_state.need_save
        || crawl_state.saving_game || !you.on_current_level || !you.save)
    {
        return;
    }

    save_level(level_id::current());
    housing_sync_current_map();
    save_game(false);
    // save_game(false) honours DIS_SAVE_CHECKPOINTS. Publication must not:
    // the canonical character+level generation always precedes its snapshot.
    you.save->commit();
    housing_publish_current_map();
}

bool housing_begin_respawn()
{
    if (housing_current_role() == housing_role_type::none)
        return false;

    housing_ensure_level();
    crawl_state.cancel_cmd_all();
    stop_delay(true);
    if (you.deaths < INT_MAX)
        ++you.deaths;
    you.pending_revival = true;

    canned_msg(MSG_YOU_DIE);
    mpr("You will return at this map's housing spawn point.");
    housing_checkpoint();
    return true;
}

bool housing_respawn()
{
    if (housing_current_role() == housing_role_type::none)
        return false;

    housing_ensure_level();
    const vector<coord_def> spawns = _stored_spawns();
    if (spawns.empty())
        return false;

    if (!_move_to_spawn(spawns, false))
        return false;
    housing_checkpoint();
    return true;
}
