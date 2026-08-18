/**
 * @file
 * @brief Runtime support for the persistent Housing game mode.
 */

#pragma once

#include "coord-def.h"
#include "dungeon-feature-type.h"

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
void housing_reset_map_turns();
int housing_map_turns();

bool housing_is_spawn(const coord_def &pos);
bool housing_can_edit(const coord_def &pos);
bool housing_feature_allowed(dungeon_feature_type feat);
dungeon_feature_type housing_last_feature();
void housing_set_last_feature(dungeon_feature_type feat);

// Commit both the character and current map for an owner session.
void housing_checkpoint();

// Housing deaths are recoverable. The first function schedules the revival;
// the second moves the revived player to one uniformly selected spawn point.
bool housing_begin_respawn();
bool housing_respawn();
