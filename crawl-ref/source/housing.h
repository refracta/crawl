/**
 * @file
 * @brief Runtime support for the persistent Housing game mode.
 */

#pragma once

#include <string>

#include "coord-def.h"
#include "dungeon-feature-type.h"

class package;

using std::string;

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
bool housing_portal_is_valid(const coord_def &pos);
// Returns true if the square was a Housing portal (including malformed ones).
// A successful WebTiles transition does not return.
bool housing_take_portal(const coord_def &pos);
bool housing_transition_pending();
void housing_send_pending_transition();

// Commit both the character and current map for an owner session.
void housing_checkpoint();

// Save isolation/publication boundary. Visitor saves are server-created
// disposable copies; before restore, their D:1 chunk is replaced with the
// read-only public snapshot selected by the authenticated WebTiles session.
package *housing_open_save_for_restore(const string &filename);
void housing_scrub_visitor_transition_state();
void housing_publish_current_map();

// Housing deaths are recoverable. The first function schedules the revival;
// the second moves the revived player to one uniformly selected spawn point.
bool housing_begin_respawn();
bool housing_respawn();
