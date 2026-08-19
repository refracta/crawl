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
#include <string>
#include <vector>

#include "act-iter.h"
#include "actor.h"
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
#include "mapmark.h"
#include "message.h"
#include "monster.h"
#include "mon-place.h"
#include "mon-transit.h"
#include "options.h"
#include "package.h"
#include "player.h"
#include "random.h"
#include "state.h"
#include "stash.h"
#include "store.h"
#include "stringutil.h"
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
static const char * const HOUSING_PORTAL_TARGET_KEY = "housing_target";
static const int HOUSING_SNAPSHOT_SCHEMA = 1;
static int _housing_turn_origin = -1;
static string _housing_pending_transition;

static const char * const _visitor_start_failure =
    "The Housing visit could not be started safely.";

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

    const char *role_env = getenv("CRAWL_HOUSING_ROLE");
    if (!role_env || !*role_env)
        return housing_role_type::owner;

    const string role = _lowercase_ascii(role_env);
    if (role == "owner")
        return housing_role_type::owner;
    if (role == "visitor")
        return housing_role_type::visitor;

    // Unknown values fail closed: the session remains playable, but cannot
    // alter canonical owner state.
    return housing_role_type::visitor;
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

package *housing_open_save_for_restore(const string &filename)
{
    if (!housing_is_visitor())
        return new package(filename.c_str(), true);

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
        if (meta.schema != HOUSING_SNAPSHOT_SCHEMA
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
        // replace only the map. On a visitor->visitor handoff WebTiles reuses
        // this same disposable save, so HP/inventory/etc. continue; on return
        // to owner the whole session directory is discarded.
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

    return save;
}

void housing_scrub_visitor_transition_state()
{
    if (!housing_is_visitor())
        return;

    // These references belong to the previous public map, not to the portable
    // visitor character capsule. This hook runs after TAG_YOU and auxiliary
    // chunks are restored, but before the replacement level is loaded.
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

    companion_list.clear();
    the_lost_ones.clear();
    const level_id housing_level(BRANCH_DUNGEON, 1);
    StashTrack.remove_level(housing_level);
    travel_cache.erase_level_info(housing_level);
    travel_cache.flush_invalid_waypoints();
    overview_clear();
    you.entering_level = false;
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
    return map_bounds(pos) && !cell_is_solid(pos);
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
    if (housing_is_owner())
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
    if (housing_is_visitor())
    {
        const vector<coord_def> spawns = _stored_spawns();
        if (spawns.empty())
            game_ended(game_exit::abort, _visitor_start_failure);

        // Map-local actor references cannot cross a process handoff. Persistent
        // character state (HP, inventory, durations, etc.) remains in the
        // disposable visitor save and therefore survives visitor->visitor.
        stop_delay(true, true);

        if (!_move_to_spawn(spawns, true))
            game_ended(game_exit::abort, _visitor_start_failure);
    }
    housing_reset_map_turns();
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
        && housing_feature_allowed(env.grid(pos));
}

bool housing_feature_allowed(dungeon_feature_type feat)
{
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

#ifdef USE_TILE_WEB
    // Defer the server-only control message until _save_game_exit has closed
    // the package successfully. The server then additionally requires a
    // normal saved exit and child status 0 before starting the successor.
    _housing_pending_transition = target;
    save_game(true, "Moving to another Housing map...");
#else
    mpr("Housing portals currently require WebTiles.");
#endif
    return true;
}

bool housing_transition_pending()
{
    return housing_valid_map_target(_housing_pending_transition);
}

void housing_send_pending_transition()
{
#ifdef USE_TILE_WEB
    if (!housing_transition_pending())
        return;
    tiles.write_message("*");
    tiles.write_message("{\"msg\":\"housing_transition\",\"target\":\"");
    tiles.write_message_escaped(_housing_pending_transition);
    tiles.write_message("\"}");
    tiles.finish_message();
    _housing_pending_transition.clear();
#endif
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
        if (feat == DNGN_ENTER_PORTAL_VAULT)
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
    for (const monster &mons : menv_real)
        if (mons.alive())
        {
            mprf(MSGCH_ERROR,
                 "Housing publish rejected: live monster %d.",
                 static_cast<int>(mons.type));
            return false;
        }

    for (map_marker *marker : env.markers.get_all())
        if (marker->get_type() != MAT_WIZ_PROPS
            || !_portal_target_at(marker->pos, nullptr))
        {
            mprf(MSGCH_ERROR,
                 "Housing publish rejected: marker %d at (%d,%d).",
                 static_cast<int>(marker->get_type()),
                 marker->pos.x, marker->pos.y);
            return false;
        }

    // Public snapshots are data, not executable map definitions. Only the
    // inert, strictly validated Housing portal marker above is accepted.
    if (!env.shop.empty() || !env.cloud.empty())
    {
        mprf(MSGCH_ERROR, "Housing publish rejected: shop/cloud state.");
        return false;
    }
    return true;
}

void housing_publish_current_map()
{
    if (!housing_is_owner() || !you.save || !you.on_current_level)
        return;

    const string account_id = _getenv_string("CRAWL_HOUSING_ACCOUNT_ID");
    const string map_id = _getenv_string("CRAWL_HOUSING_MAP_ID").empty()
                            ? "main"
                            : _getenv_string("CRAWL_HOUSING_MAP_ID");
    string public_dir = _getenv_string("CRAWL_HOUSING_PUBLIC_DIR");
    if (public_dir.empty())
        return; // Console development without a WebTiles account binding.

    if (!_is_decimal_id(account_id) || !_is_map_id(map_id))
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

    const string final_path = catpath(account_dir, map_id + ".hmap");
    const string temporary_path = final_path + ".tmp";
    package *snapshot = nullptr;
    try
    {
        snapshot = new package(temporary_path.c_str(), true, true);
        _write_snapshot_meta(*snapshot, account_id, map_id);
        snapshot->copy_chunk_from(*you.save, save_level_chunk,
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
        mprf(MSGCH_ERROR, "The Housing map could not be published.");
    }
}

void housing_checkpoint()
{
    if (!housing_is_owner() || !crawl_state.need_save
        || crawl_state.saving_game || !you.on_current_level || !you.save)
    {
        return;
    }

    save_level(level_id::current());
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
