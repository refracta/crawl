/**
 * @file
 * @brief Runtime support for the persistent Housing game mode.
 */

#pragma once

#include <string>
#include <vector>

#include "coord-def.h"
#include "dungeon-feature-type.h"
#include "monster-type.h"

class package;
class monster;

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
// Explore mode's optional-death semantics do not belong in persistent public
// Housing. Strip it when restored from old saves or startup options; ordinary
// wizard mode remains available for administration and testing.
void housing_enforce_explore_mode();
const string &housing_current_map_id();
// Lobby/whereis identity for the currently displayed map. The ordinary
// score-compatible place remains D:1; this supplementary field distinguishes
// owner maps and visitor destinations.
string housing_place();

// Public snapshot compatibility is deliberately explicit: schema 4 is the
// current capability for translucent owner-only barriers, while barrier-free
// maps remain schema 3 for rolling-process compatibility. Readers retain all
// three strictly validated legacy formats used by published Housing maps.
int housing_snapshot_schema_version();
bool housing_snapshot_schema_supported(int schema);

// Housing map identifiers are case-sensitive ASCII names. The canonical save
// keeps an index plus one namespaced TAG_LEVEL chunk for every identifier; the
// ordinary Dungeon level chunk is only the loader-facing alias for the active
// map.
bool housing_valid_map_id(const string &map_id);
string housing_map_chunk_name(const string &map_id);
// Branch themes have append-only stable ids because the id is stored in each
// canonical map chunk. The menu order is independent and follows Crawl's
// logical branch order; retired save-compatibility branches are omitted.
int housing_branch_theme_count();
const char *housing_branch_theme_name(int stable_id);
int housing_branch_theme_menu_id(int position);
bool housing_branch_theme_catalog_valid();
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
// Package half of the one-time 37db stale-template migration predicate. The
// loaded-level half additionally requires no spawn metadata and adoption of
// exactly one bare runelight before any chunk is rewritten.
bool housing_legacy_template_is_exact_clone(package &save,
                                            const string &map_id);

// Read the externally managed balance. This deliberately does not cache.
int housing_points();
bool housing_authorize_points(int cost);
bool housing_authorize_action(const char *action, int cost);

// Initialise persistent per-map spawn metadata if it is missing. The turn
// origin is transient and must be reset on every map/session entry.
void housing_ensure_level(bool place_new_owner = false);
// Finalise entry after the target level has loaded. Visitors are placed on a
// persisted spawn and every Housing session gets a fresh displayed turn 0.
void housing_finish_map_entry(bool new_game_entry = false);
void housing_reset_map_turns();
int housing_map_turns();

bool housing_is_spawn(const coord_def &pos);
// Add a visible authenticated runelight spawn on a safe floor, or remove the
// selected existing spawn. Every mutation re-reads the externally managed
// Housing point balance; a map can never lose its final spawn.
bool housing_toggle_spawn_point(const coord_def &pos);
// Clear one Housing cell through the normal terrain editor. Authenticated
// spawn points, owner-only barriers and shops receive their own transactional
// cleanup; ordinary editable terrain is changed directly to floor. The final
// spawn point is always protected.
bool housing_clear_terrain(const coord_def &pos);
bool housing_can_edit(const coord_def &pos);
bool housing_feature_allowed(dungeon_feature_type feat);
dungeon_feature_type housing_last_feature();
void housing_set_last_feature(dungeon_feature_type feat);
bool housing_valid_map_target(const string &target);
bool housing_create_portal(const coord_def &pos, const string &target);
// Prompt for a plain monster name, then choose an explicit target cell and
// create one persistable, no-reward Housing monster. Map-definition specs and
// unsafe derived actors are rejected, and target invariants are rechecked at
// mutation time.
bool housing_create_monster();
bool housing_monster_type_allowed(monster_type type);
// Editor-created monsters are inert while their owner is arranging the map.
// The same actors become fully active when the published level is loaded by a
// visitor, after owner-only barriers have opened.
bool housing_monster_is_owner_inert(const monster &mons);
// Toggle a solid, translucent owner-only wall which is persisted/published
// but converted to floor for visitors before marker activation and redraw.
bool housing_toggle_visitor_wall(const coord_def &pos);
// Loader boundary helpers. A visitor wall is accepted only when its current
// translucent permarock (or legacy metal) terrain and exact veto marker agree;
// opening removes that marker and changes the cell to floor before activation.
bool housing_visitor_wall_is_valid(const coord_def &pos);
void housing_open_visitor_wall(const coord_def &pos);
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
// Public map payloads are authoritative only under immutable account ids.
// A stable lowercase-name binding selects that directory; an existing name
// can never be rebound to a different account. The resolver validates
// snapshot metadata and uses a legacy by-name payload only to discover an
// account id; it never serves that payload without a matching numeric copy.
bool housing_bind_public_owner(const string &public_dir,
                               const string &owner_name,
                               const string &account_id);
bool housing_resolve_public_snapshot_path(const string &public_dir,
                                          const string &owner_name,
                                          const string &map_id,
                                          string &snapshot_path);

// Housing map transitions may restart from an anonymous staged owner package
// or the exact previous owner/visitor package after a failed replacement.
// These predicates stay true after _reset_game clears crawl_state and before
// restore_game installs the supplied package as you.save, so startup.cc can
// guard restore plus all post-load initialisation without entering chargen.
bool housing_transition_restore_pending();
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
// Clear delays, channels, actor references and synthetic D:1 caches after
// TAG_YOU restore but before a genuinely different Housing TAG_LEVEL loads.
// Returning to the same canonical owner map deliberately remains exact.
void housing_prepare_map_transition_restore(bool known_map_change = false);
// Raise the visitor character's MID allocator above every live actor in the
// just-restored public level. Called immediately after TAG_LEVEL restore and
// before marker activation or other actor creation.
void housing_prepare_loaded_level();
// Atomically publish the active owner TAG_LEVEL. A console session with
// neither account binding nor public directory is a successful no-op. A
// bound account without a public directory, or any validation/I/O failure,
// returns false after a user-facing diagnostic.
bool housing_publish_current_map();

// Housing deaths are recoverable. The first function schedules the revival;
// the second moves the revived player to one uniformly selected spawn point.
bool housing_begin_respawn();
bool housing_respawn();
