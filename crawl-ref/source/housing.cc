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
#include <set>
#include <string>
#include <vector>

#ifndef TARGET_OS_WINDOWS
#include <sys/stat.h>
#endif

#include "act-iter.h"
#include "actor.h"
#include "branch.h"
#include "cloud.h"
#include "coord.h"
#include "coordit.h"
#include "delay.h"
#include "dgn-overview.h"
#include "end.h"
#include "env.h"
#include "errors.h"
#include "files.h"
#include "god-companions.h"
#include "god-passive.h"
#include "initfile.h"
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
#include "teleport.h"
#include "terrain.h"
#include "traps.h"
#include "travel.h"
#include "version.h"
#ifdef USE_TILE_WEB
#include "tileweb.h"
#endif

using std::string;
using std::vector;

static const char * const HOUSING_SPAWNS_KEY = "housing_spawn_points";
static const char * const HOUSING_LAST_FEATURE_KEY = "housing_last_feature";
static const char * const HOUSING_META_CHUNK = "housing_meta";
static const char * const HOUSING_LEVEL_CHUNK = "level";
static const char * const HOUSING_INDEX_CHUNK = "housing_index";
static const char * const HOUSING_MAP_CHUNK_PREFIX = "housing_map_";
static const char * const HOUSING_TEMPLATE_CHUNK = "housing_template";
static const char * const HOUSING_PORTAL_TARGET_KEY = "housing_target";
static const char * const HOUSING_SPAWN_MARKER_KEY = "housing_spawn";
static const char * const HOUSING_MONSTER_KEY = "housing_created_monster";
static const int HOUSING_SNAPSHOT_LEGACY_SCHEMA = 1;
// Schema 2 permits the strictly validated spawn fixtures, Housing monsters,
// and shops introduced by this build. Old cores reject it instead of loading
// actors without the corresponding MID and payload validation.
static const int HOUSING_SNAPSHOT_SCHEMA = 2;
static const int HOUSING_MAX_MONSTERS = 64;
static const int HOUSING_MAX_SHOPS = 32;
static const int HOUSING_INDEX_SCHEMA = 1;
static const int HOUSING_MAX_MAPS = 64;
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
static bool _housing_skip_next_checkpoint = false;
static string _housing_current_map_id;
static string _housing_current_map_owner;
static string _housing_pending_notice;
static bool _housing_entry_requires_spawn = false;

static const char * const _visitor_start_failure =
    "The Housing visit could not be started safely.";
static const char * const _owner_return_failure =
    "Your canonical Housing home is unavailable.";

static string _lowercase_ascii(string value);
static string _getenv_string(const char *name);
static bool _ensure_owner_map_storage();
static void _promote_staged_owner_map();

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
    return chunk == _housing_save_level_chunk()
        || chunk == HOUSING_INDEX_CHUNK
        || chunk == HOUSING_TEMPLATE_CHUNK
        || chunk.compare(0, map_prefix.size(), map_prefix) == 0;
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
           || schema == HOUSING_SNAPSHOT_SCHEMA;
}

static void _write_snapshot_meta(package &snapshot, const string &account_id,
                                 const string &map_id)
{
    writer output(&snapshot, HOUSING_META_CHUNK);
    marshallInt(output, HOUSING_SNAPSHOT_SCHEMA);
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

const string &housing_current_map_id()
{
    _initialize_housing_runtime();
    return _housing_current_map_id;
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
        if (housing_is_owner())
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

void housing_scrub_visitor_transition_state()
{
    // A visitor returning through a staged owner-map package has already
    // switched roles before TAG_YOU restore, but its map-bound references are
    // still from the canonical map that preceded the requested target.
    if (housing_is_visitor()
        || (_housing_owner_promotion_save && _housing_owner_map_changed))
        _scrub_housing_map_transition_state();
}

void housing_prepare_loaded_level()
{
    if (!crawl_state.game_is_housing() || !housing_is_visitor())
        return;

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

static bool _spawn_marker_at(const coord_def &pos)
{
    if (!map_bounds(pos) || env.grid(pos) != DNGN_RUNELIGHT)
        return false;
    const vector<map_marker*> markers = env.markers.get_markers_at(pos);
    return markers.size() == 1
        && markers.front()->get_type() == MAT_WIZ_PROPS
        && markers.front()->property(HOUSING_SPAWN_MARKER_KEY) == "yes"
        && markers.front()->property("veto_destroy") == "veto";
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

void housing_ensure_level()
{
    if (housing_current_role() == housing_role_type::none
        || !you.on_current_level || !in_bounds(you.pos()))
    {
        return;
    }

    vector<coord_def> spawns = _stored_spawns();
    if (spawns.empty())
    {
        if (!housing_is_owner())
            return;
        spawns.push_back(you.pos());
    }

    spawns.erase(std::remove_if(spawns.begin(), spawns.end(),
                               [](const coord_def &pos) {
                                   return !_ensure_spawn_fixture(pos);
                               }),
                 spawns.end());
    if (spawns.empty())
    {
        if (housing_is_visitor())
            return;
        if (_ensure_spawn_fixture(you.pos()))
            spawns.push_back(you.pos());
    }

    // Visitor maps are disposable, so recording a legacy fixture migration in
    // their temporary level cannot affect the public/canonical snapshot.
    _store_spawns(spawns);

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

void housing_finish_map_entry()
{
    if (housing_current_role() == housing_role_type::none)
        return;

    housing_ensure_level();
    if (housing_is_owner() && !_ensure_owner_map_storage())
        fail("The canonical Housing map index is unavailable");
    if (housing_is_owner()
        && !_package_chunks_equal(*you.save, _housing_save_level_chunk(),
                                  housing_map_chunk_name(
                                      _housing_current_map_id)))
    {
        corrupted("The active Housing level does not match its map index");
    }
    const bool place_at_spawn = housing_is_visitor()
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

    housing_reset_map_turns();
    if (!_housing_pending_notice.empty())
    {
        mprf(MSGCH_ERROR, "%s", _housing_pending_notice.c_str());
        _housing_pending_notice.clear();
    }
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
    case DNGN_ROCK_WALL:
    case DNGN_STONE_WALL:
    case DNGN_METAL_WALL:
    case DNGN_CRYSTAL_WALL:
    case DNGN_CLEAR_ROCK_WALL:
    case DNGN_CLEAR_STONE_WALL:
    case DNGN_CLOSED_DOOR:
    case DNGN_OPEN_DOOR:
#if TAG_MAJOR_VERSION > 34
    case DNGN_CLOSED_CLEAR_DOOR:
    case DNGN_OPEN_CLEAR_DOOR:
#endif
    case DNGN_TREE:
    case DNGN_GRANITE_STATUE:
    case DNGN_SHALLOW_WATER:
    case DNGN_DEEP_WATER:
    case DNGN_LAVA:
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
    if (markers.size() != 1 || markers.front()->get_type() != MAT_WIZ_PROPS)
        return false;
    const string value = markers.front()->property(HOUSING_PORTAL_TARGET_KEY);
    if (!housing_valid_map_target(value)
        || markers.front()->property("veto_destroy") != "veto")
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

bool housing_create_portal(const coord_def &pos, const string &target)
{
    if (!housing_valid_map_target(target))
    {
        mprf(MSGCH_PROMPT,
             "Use an account:map identifier (ASCII letters, digits, and _).");
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

static bool _housing_monster_type_allowed(monster_type type)
{
    if (type <= MONS_PROGRAM_BUG || type >= NUM_MONSTERS
        || mons_is_unique(type) || mons_is_pghost(type)
        || mons_class_is_test(type) || mons_class_is_peripheral(type)
        || mons_is_projectile(type) || mons_is_seeker(type)
        || mons_is_tentacle_head(type)
        || mons_is_tentacle_or_tentacle_segment(type)
        || mons_class_flag(type, M_CANT_SPAWN | M_UNFINISHED | M_UNSTABLE
                                 | M_NO_GEN_DERIVED | M_ANCESTOR | M_AVATAR))
    {
        return false;
    }
    return true;
}

bool housing_create_monster()
{
    const int live_monsters = std::count_if(
        menv_real.begin(), menv_real.end(),
        [](const monster &mons) { return mons.alive(); });
    if (live_monsters >= HOUSING_MAX_MONSTERS)
    {
        mpr("This Housing map already has the maximum number of monsters.");
        return false;
    }

    char name[128];
    mprf(MSGCH_PROMPT, "Monster name: ");
    if (cancellable_get_line_autohist(name, sizeof name) || !*name)
    {
        canned_msg(MSG_OK);
        return false;
    }

    const monster_type type = get_monster_by_name(name);
    if (!_housing_monster_type_allowed(type))
    {
        mpr("That monster cannot be created in Housing.");
        return false;
    }

    coord_def place = find_newmons_square(type, you.pos(), 2,
                                          you.current_vision);
    if (!in_bounds(place) || housing_is_spawn(place))
    {
        mpr("There is no unprotected space for that monster nearby.");
        return false;
    }

    mgen_data mg(type, BEH_HOSTILE, place, MHITYOU, MG_FORBID_BANDS);
    mg.extra_flags |= MF_NO_REWARD;
    monster *created = create_monster(mg);
    if (!created)
    {
        mpr("The monster could not be created.");
        return false;
    }
    created->props[HOUSING_MONSTER_KEY] = true;
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

static std::unique_ptr<package> _open_public_snapshot(const string &owner,
                                                      const string &map_id,
                                                      housing_snapshot_meta &meta)
{
    const string public_dir = _getenv_string("CRAWL_HOUSING_PUBLIC_DIR");
    if (public_dir.empty() || !_is_account_name(owner) || !_is_map_id(map_id))
        return nullptr;

    const string snapshot_path =
        catpath(catpath(catpath(public_dir, "by-name"),
                        _lowercase_ascii(owner)), map_id + ".hmap");
    if (!_path_is_within(snapshot_path, public_dir))
        return nullptr;

    std::unique_ptr<package> snapshot(new package(snapshot_path.c_str(), false));
    meta = _read_snapshot_meta(*snapshot);
    if (!_supported_snapshot_schema(meta.schema)
        || !_is_decimal_id(meta.account_id)
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

static std::unique_ptr<package> _clone_with_snapshot_level(
    package &source, package &snapshot)
{
    std::unique_ptr<package> clone(new package());
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

    std::unique_ptr<package> staged(new package());
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
    package *staged, package *canonical, const string &notice)
{
    if (!staged)
        staged = _housing_staged_owner_save;
    if (_housing_restore_save == staged)
        _housing_restore_save = nullptr;
    if (you.save == staged || you.save == canonical)
        you.save = nullptr;
    _housing_staged_owner_save = nullptr;
    _housing_owner_map_changed = false;
    _housing_owner_template_needs_refresh = false;
    if (staged)
    {
        staged->abort();
        delete staged;
    }

    const string filename = canonical
                                ? canonical->get_filename()
                                : _housing_canonical_save_path;
    if (canonical)
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
    _housing_visitor_save = nullptr;
    _housing_skip_next_checkpoint = true;
    _housing_entry_requires_spawn = false;
    _housing_pending_notice = notice;
    macro_clear_mappings();
    game_ended(game_exit::housing_transition);
}

static void _promote_staged_owner_map()
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
        _restart_canonical_after_owner_transition_failure(
            staged, canonical,
            "That Housing map could not be saved safely; the last committed "
            "owner map was restored.");
    }

    staged->abort();
    delete staged;
    _housing_staged_owner_save = nullptr;
    _housing_owner_promotion_save = nullptr;
    _housing_owner_map_changed = false;
    _housing_owner_template_needs_refresh = false;
    _housing_owner_save = canonical;
    you.save = canonical;
}

bool housing_owner_restore_pending()
{
    // The canonical promotion package is the transaction token. In
    // particular this stays true if generic restore code clears you.save
    // while reporting a malformed anonymous package.
    return housing_is_owner() && _housing_owner_promotion_save;
}

bool housing_owner_restore_is_staged()
{
    return housing_owner_restore_pending() && _housing_staged_owner_save;
}

void housing_complete_staged_owner_restore()
{
    if (!housing_owner_restore_is_staged())
        return;

    // The early load hook already validated the target and placed its spawn.
    // Promotion waits until startup_step has completed all remaining Lua, rc,
    // tile, travel and view initialisation.
    if (_housing_entry_requires_spawn)
        fail("The staged Housing owner map was not finalised during load");
    _promote_staged_owner_map();
}

void housing_rollback_staged_owner_restore()
{
    if (!housing_owner_restore_pending())
        return;

    _housing_entry_requires_spawn = false;
    _restart_canonical_after_owner_transition_failure(
        _housing_staged_owner_save, _housing_owner_promotion_save,
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
    package *staged_save = nullptr;
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

        stop_delay(true, true);
        heal_flayed_effect(&you, true, true);
        clear_level_bound_player_state(true);
        _scrub_housing_map_transition_state();
        drop_pending_monster_resets();
        crawl_state.potential_pursuers.clear();
        const level_id old_level = level_id::current();
        you.position.reset();
        load_level(DNGN_UNSEEN, LOAD_HOUSING_REPLACE, old_level);

        // LOAD_HOUSING_REPLACE has now completed every marker, travel and tile
        // hook. Promote the staged map only after that full boundary succeeds.
        _promote_staged_owner_map();
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
        _housing_entry_requires_spawn = false;
        _restart_canonical_after_owner_transition_failure(
            staged_save, canonical,
            "That Housing map could not be loaded safely; the previous owner "
            "map was restored.");
    }

    crawl_state.need_save = true;
    you.turn_is_over = false;
    housing_publish_current_map();
    mprf("You enter %s:%s.", you.your_name.c_str(), target_map.c_str());
    return true;
}

static bool _create_owner_map(const string &map_id)
{
    if (!_is_map_id(map_id))
    {
        mpr("Map ids use 1-20 ASCII letters, digits, or underscores.");
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
    maps.push_back(map_id);
    housing_write_map_index(*you.save, maps, current_map);
    you.save->commit();
    mprf("Created Housing map '%s'. Enter it once before other players can "
         "visit it.", map_id.c_str());
    return true;
}

static bool _unlink_public_map_file(const string &path,
                                    const string &public_dir)
{
    // Resolve the containing directory rather than the entry itself: realpath
    // on a missing or dangling entry cannot distinguish ENOENT from failures
    // such as EACCES/ENOTDIR. Once the parent is proven inside the public tree,
    // unlinking the exact basename is safe even when that entry is a symlink.
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
    if (public_dir.empty() || !_is_decimal_id(account_id)
        || !_is_account_name(owner_name) || !_is_map_id(map_id))
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
        return true;

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
            return _create_owner_map(map_id);
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
            "state was restored.");
    }
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
    if (previous_was_owner)
        _housing_canonical_save_path = previous->get_filename();

    you.save = next;
    _housing_visitor_save = next;
    _housing_runtime_role = housing_role_type::visitor;
    _housing_current_map_owner = owner;
    _housing_current_map_id = map_id;

    // Everything below the checkpoint mutates only the disposable visitor
    // state. The ordinary level loader resets env/menv and all tile caches;
    // these hooks clear the remaining character/global references to the old
    // map before TAG_LEVEL is restored.
    try
    {
        stop_delay(true, true);
        heal_flayed_effect(&you, true, true);
        clear_level_bound_player_state(true);
        housing_scrub_visitor_transition_state();
        // KILL_RESET effects created by the cleanup refer to the outgoing menv;
        // discard them before TAG_LEVEL replaces that array. Pursuer pointers
        // have the same lifetime boundary.
        drop_pending_monster_resets();
        crawl_state.potential_pursuers.clear();
        const level_id old_level = level_id::current();
        you.position.reset();
        load_level(DNGN_UNSEEN, LOAD_HOUSING_REPLACE, old_level);
    }
    catch (...)
    {
        // The loader may already have reset global level state, so continuing
        // the input loop is unsafe. Roll the whole Crawl game state back from
        // the still-open previous package, while retaining the process/socket.
        you.save = nullptr;
        next->abort();
        delete next;
        _housing_runtime_role = previous_role;
        _housing_current_map_owner = previous_map_owner;
        _housing_current_map_id = previous_map_id;
        _housing_restore_save = previous;
        _housing_owner_save = previous_was_owner ? previous : nullptr;
        _housing_visitor_save = previous_was_owner ? nullptr : previous;
        _housing_skip_next_checkpoint = previous_was_owner;
        _housing_pending_notice =
            "That Housing map could not be loaded safely; the previous map "
            "was restored.";
        macro_clear_mappings();
        game_ended(game_exit::housing_transition);
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
        && _housing_monster_type_allowed(mons.type)
        && testbits(mons.flags, MF_NO_REWARD)
        && mons.props.exists(HOUSING_MONSTER_KEY)
        && mons.props[HOUSING_MONSTER_KEY].get_type() == SV_BOOL
        && mons.props[HOUSING_MONSTER_KEY].get_bool();
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

    for (map_marker *marker : env.markers.get_all())
        if ((marker->get_type() != MAT_WIZ_PROPS
             || (!_portal_target_at(marker->pos, nullptr)
                 && !_spawn_marker_at(marker->pos))))
        {
            mprf(MSGCH_ERROR,
                 "Housing publish rejected: marker %d at (%d,%d).",
                 static_cast<int>(marker->get_type()),
                 marker->pos.x, marker->pos.y);
            return false;
        }

    // Public snapshots are data, not executable map definitions. Only the
    // strictly validated Housing portal and spawn markers above are accepted.
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

void housing_publish_current_map()
{
    if (!housing_is_owner() || !you.save || !you.on_current_level)
        return;

    const string account_id = _getenv_string("CRAWL_HOUSING_ACCOUNT_ID");
    const string map_id = housing_current_map_id();
    string public_dir = _getenv_string("CRAWL_HOUSING_PUBLIC_DIR");
    if (public_dir.empty())
        return; // Console development without a WebTiles account binding.

    if (!_is_decimal_id(account_id) || !_is_map_id(map_id)
        || !_is_account_name(you.your_name))
    {
        mprf(MSGCH_ERROR,
             "The Housing account/map publication binding is invalid.");
        return;
    }
    if (!_current_map_can_be_published())
    {
        mprf(MSGCH_ERROR, "This Housing map is not safe to publish.");
        return;
    }
    const string save_level_chunk = _housing_save_level_chunk();
    if (!you.save->has_chunk(save_level_chunk))
    {
        mprf(MSGCH_ERROR, "The Housing level is not ready to publish.");
        return;
    }

    if (!check_mkdir("Housing public map directory", &public_dir, true))
    {
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
        return;
    }
    string account_dir = catpath(public_dir, account_id);
    if (!check_mkdir("Housing account map directory", &account_dir, true))
    {
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
        return;
    }

    try
    {
        _write_public_snapshot(catpath(account_dir, map_id + ".hmap"),
                               account_id, map_id);

        // The authenticated server still stores the authoritative snapshot by
        // immutable account id. This validated name index lets the already
        // running Crawl process resolve a portal without a WebTiles handoff.
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
        _write_public_snapshot(catpath(owner_dir, map_id + ".hmap"),
                               account_id, map_id);
    }
    catch (...)
    {
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
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
