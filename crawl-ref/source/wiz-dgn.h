/**
 * @file
 * @brief Dungeon related wizard functions.
**/

#pragma once

#include <string>
#include <vector>

// Define this constant to put the builder into a debug mode where on a veto
// triggered by a wizmode travel command or level reload, instead of retrying
// it immediately exits the builder so that you can see what happened.
//#define DEBUG_VETO_RESUME

#include "player.h"

#include "command-type.h"

#define WIZ_LAST_FEATURE_TYPE_PROP "last_created_feature"

enum class housing_terrain_brush_result
{
    rejected,
    unchanged,
    changed,
};

constexpr int HOUSING_TERRAIN_BRUSH_MIN_SIZE = 1;
constexpr int HOUSING_TERRAIN_BRUSH_MAX_SIZE = 8;

int wizard_housing_brush_size_after_command(int size,
                                             command_type command);
std::vector<coord_def> wizard_housing_brush_cells(const coord_def &centre,
                                                   int size);
housing_terrain_brush_result wizard_apply_housing_terrain_brush(
    const coord_def &centre, dungeon_feature_type feat, int size);

std::vector<std::string> wizard_feature_matches(const std::string &name,
                                                bool housing_only=false);
bool wizard_housing_feature_selectable(dungeon_feature_type feat);
dungeon_feature_type wizard_select_feature(bool mimic, bool allow_fprop=false);
dungeon_feature_type wizard_select_housing_feature();
bool wizard_create_feature(const coord_def& pos = you.pos(),
                                    dungeon_feature_type feat=DNGN_UNSEEN,
                                    bool mimic=false);
bool wizard_create_feature(dist &target, dungeon_feature_type feat, bool mimic,
                           bool housing_edit=false,
                           bool housing_clear=false);
void wizard_list_branches();
void wizard_map_level();
void wizard_place_stairs(bool down);
void wizard_level_travel(bool down);
void wizard_interlevel_travel();
void wizard_list_levels();
void wizard_recreate_level();
void wizard_clear_used_vaults();
bool debug_make_shop(const coord_def& pos = you.pos());
void debug_place_map(bool primary);
void wizard_primary_vault();
void debug_test_explore();
void wizard_abyss_speed();

bool is_wizard_travel_target(const level_id l);
