#include "catch_amalgamated.hpp"

#include "AppHdr.h"

#include "branch.h"
#include "dungeon.h"
#include "housing.h"
#include "jobs.h"
#include "package.h"
#include "player.h"
#include "state.h"
#include "unwind.h"

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
