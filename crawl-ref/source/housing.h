/**
 * @file
 * @brief Runtime support for the persistent Housing game mode.
 */

#pragma once

#include <string>
#include <vector>

#include "coord-def.h"
#include "dungeon-feature-type.h"

class package;

using std::string;
using std::vector;

enum class housing_role_type
{
    none,
    owner,
    visitor,
};

housing_role_type housing_current_role();
const char *housing_role_name();
bool housing_is_owner();
bool housing_is_visitor();
const string &housing_current_map_id();

// Housing map identifiers are case-sensitive ASCII names. The canonical save
// keeps an index plus one namespaced TAG_LEVEL chunk for every identifier; the
// ordinary Dungeon level chunk is only the loader-facing alias for the active
// map.
bool housing_valid_map_id(const string &map_id);
string housing_map_chunk_name(const string &map_id);
// Return false for a legacy single-map save with no index. A present but
// malformed index throws instead of guessing, so canonical data fails closed.
bool housing_read_map_index(package &save, vector<string> &maps,
                            string &current_map);
void housing_write_map_index(package &save, const vector<string> &maps,
                             const string &current_map);
// Copy only character/global state into a disposable visitor capsule. The
// loader-facing level and every private owner multi-map chunk are omitted;
// the caller installs the authenticated public level afterwards.
void housing_copy_visitor_state(package &source, package &destination);

// Read the externally managed balance. This deliberately does not cache.
int housing_points();
bool housing_authorize_points(int cost);
bool housing_authorize_action(const char *action, int cost);

// Initialise persistent per-map spawn metadata if it is missing. The turn
// origin is transient and must be reset on every map/session entry.
void housing_ensure_level();
// Finalise entry after the target level has loaded. Visitors are placed on a
// persisted spawn and every Housing session gets a fresh displayed turn 0.
void housing_finish_map_entry();
void housing_reset_map_turns();
int housing_map_turns();

bool housing_is_spawn(const coord_def &pos);
bool housing_can_edit(const coord_def &pos);
bool housing_feature_allowed(dungeon_feature_type feat);
dungeon_feature_type housing_last_feature();
void housing_set_last_feature(dungeon_feature_type feat);
bool housing_valid_map_target(const string &target);
bool housing_create_portal(const coord_def &pos, const string &target);
// Create one persistable, no-reward Housing monster from a plain canonical
// monster name. Map-definition specs and unsafe derived actors are rejected.
bool housing_create_monster();
bool housing_can_create_shop();
bool housing_portal_is_valid(const coord_def &pos);
// Travel to an account:map target without requiring a portal square. Self
// targets enter canonical owner maps; foreign targets preserve visitor state.
bool housing_travel_to_target(const string &target);
// Returns true if the square was a Housing portal (including malformed ones).
// A valid portal is resolved and loaded by the Crawl process. Foreign maps
// keep the current visitor character; returning home restores the exact owner
// checkpoint without replacing the process or WebSocket.
bool housing_take_portal(const coord_def &pos);
// Discard a visitor session and restore the canonical owner checkpoint without
// replacing the Crawl process. Returns false, with a user-facing error, if the
// canonical save/context cannot be opened; success does not return.
bool housing_return_home();

// Owner-only map management. The menu supports creating, entering and
// deleting maps; successful entry replaces only the level in this process.
bool housing_manage_maps();

// Remove both externally visitable aliases for one map. Missing files are
// accepted so a partially completed deletion can be retried safely.
bool housing_unpublish_map_files(const string &public_dir,
                                 const string &account_id,
                                 const string &owner_name,
                                 const string &map_id);

// Visitor -> owner returns restore through an anonymous target-map package.
// The pending predicate is true even before restore_game installs that package
// as you.save; startup.cc guards restore plus all post-load initialisation.
bool housing_owner_restore_pending();
bool housing_owner_restore_is_staged();
void housing_complete_staged_owner_restore();
void housing_rollback_staged_owner_restore();

// Commit both the character and current map for an owner session.
void housing_checkpoint();
// Mirror the loader-facing level into the active named map before the package
// commit. Callers that save outside housing_checkpoint() use this to keep the
// level, character and map index in one crash-consistent generation.
void housing_sync_current_map();

// Save isolation/publication boundary. Visitor saves are server-created
// disposable copies; before restore, their D:1 chunk is replaced with the
// read-only public snapshot selected by the authenticated WebTiles session.
package *housing_open_save_for_restore(const string &filename);
// Repair the exact invalid location written by pre-fix Housing Delvers. This
// is called after TAG_YOU restores branch depths and before validating them.
void housing_normalize_legacy_delver_depth();
void housing_scrub_visitor_transition_state();
// Raise the visitor character's MID allocator above every live actor in the
// just-restored public level. Called immediately after TAG_LEVEL restore and
// before marker activation or other actor creation.
void housing_prepare_loaded_level();
void housing_publish_current_map();

// Housing deaths are recoverable. The first function schedules the revival;
// the second moves the revived player to one uniformly selected spawn point.
bool housing_begin_respawn();
bool housing_respawn();
