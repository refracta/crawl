/**
 * @file
 * @brief Runtime support for the persistent Housing game mode.
 */

#include "AppHdr.h"

#include "housing.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "actor.h"
#include "cloud.h"
#include "coord.h"
#include "delay.h"
#include "env.h"
#include "files.h"
#include "mapmark.h"
#include "message.h"
#include "monster.h"
#include "mon-place.h"
#include "options.h"
#include "player.h"
#include "random.h"
#include "state.h"
#include "store.h"
#include "stringutil.h"
#include "syscalls.h"
#include "teleport.h"
#include "terrain.h"
#include "traps.h"

using std::string;
using std::vector;

static const char * const HOUSING_SPAWNS_KEY = "housing_spawn_points";
static const char * const HOUSING_LAST_FEATURE_KEY = "housing_last_feature";
static int _housing_turn_origin = -1;

static string _lowercase_ascii(string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
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

void housing_checkpoint()
{
    if (!housing_is_owner() || !crawl_state.need_save
        || crawl_state.saving_game || !you.on_current_level || !you.save)
    {
        return;
    }

    save_level(level_id::current());
    save_game(false);
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

    // Choose before resolving temporary occupancy so every persisted spawn has
    // exactly the same selection probability.
    const coord_def destination = spawns[random2(spawns.size())];
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
    housing_checkpoint();
    return true;
}
