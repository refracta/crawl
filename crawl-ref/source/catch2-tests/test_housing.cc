#include "catch_amalgamated.hpp"

#include "AppHdr.h"

#include "ability-type.h"
#include "branch.h"
#include "dungeon.h"
#include "files.h"
#include "housing.h"
#include "jobs.h"
#include "package.h"
#include "player.h"
#include "state.h"
#include "tags.h"
#include "unwind.h"

#ifndef TARGET_OS_WINDOWS
#include <sys/stat.h>

#include "syscalls.h"
#endif

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
    REQUIRE(housing_valid_map_id("_"));
    REQUIRE(housing_valid_map_id("this_is_test_MAP07"));
    REQUIRE(housing_valid_map_id("12345678901234567890"));
    REQUIRE_FALSE(housing_valid_map_id(""));
    REQUIRE_FALSE(housing_valid_map_id("map-name"));
    REQUIRE_FALSE(housing_valid_map_id("123456789012345678901"));

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

TEST_CASE("Housing terrain permits decorative hazards and altars safely",
          "[single-file]")
{
    REQUIRE(housing_feature_allowed(DNGN_SHALLOW_WATER));
    REQUIRE(housing_feature_allowed(DNGN_DEEP_WATER));
    REQUIRE(housing_feature_allowed(DNGN_LAVA));
    REQUIRE(housing_feature_allowed(DNGN_ALTAR_ZIN));

    // A runelight is reserved for an authenticated Housing spawn fixture;
    // generic terrain editing must not be able to forge one.
    REQUIRE_FALSE(housing_feature_allowed(DNGN_RUNELIGHT));
    REQUIRE_FALSE(housing_feature_allowed(DNGN_UNKNOWN_ALTAR));
}

TEST_CASE("Housing ability ids remain append-only", "[single-file]")
{
    REQUIRE(static_cast<int>(ABIL_HOUSING_RETURN_HOME) == 9005);
    REQUIRE(static_cast<int>(ABIL_HOUSING_MANAGE_MAPS) == 9006);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CREATE_MONSTER) == 9007);
    REQUIRE(static_cast<int>(ABIL_HOUSING_CALL_MERCHANT) == 9008);
    REQUIRE(static_cast<int>(ABIL_HOUSING_TRAVEL_TO_MAP) == 9009);
}

static void _write_test_chunk(package &save, const string &chunk,
                              const string &payload)
{
    chunk_writer output(&save, chunk);
    output.write(payload.data(), payload.size());
}

TEST_CASE("Housing map index round-trips named map chunks", "[single-file]")
{
    package save;
    _write_test_chunk(save, housing_map_chunk_name("main"), "main level");
    _write_test_chunk(save, housing_map_chunk_name("Side_7"), "side level");

    const vector<string> expected = { "main", "Side_7" };
    housing_write_map_index(save, expected, "Side_7");
    save.commit();

    vector<string> maps;
    string current;
    REQUIRE(housing_read_map_index(save, maps, current));
    REQUIRE(maps == expected);
    REQUIRE(current == "Side_7");
    REQUIRE(housing_map_chunk_name("Side_7") == "housing_map_Side_7");
    REQUIRE(housing_map_chunk_name("bad-id").empty());
}

TEST_CASE("Housing map index rejects duplicate and missing maps",
          "[single-file]")
{
    package save;
    _write_test_chunk(save, housing_map_chunk_name("main"), "main level");
    {
        writer output(&save, "housing_index");
        marshallInt(output, 1);
        marshallString(output, "main");
        marshallInt(output, 2);
        marshallString(output, "main");
        marshallString(output, "main");
    }

    vector<string> maps;
    string current;
    REQUIRE_THROWS(housing_read_map_index(save, maps, current));

    package legacy;
    REQUIRE_FALSE(housing_read_map_index(legacy, maps, current));
    legacy.abort();
}

TEST_CASE("Housing visitor capsules omit canonical private map chunks",
          "[single-file]")
{
    package source;
    _write_test_chunk(source, "you", "portable character");
    _write_test_chunk(source, "chr", "portable summary");
    _write_test_chunk(source, "D", "active private level");
    _write_test_chunk(source, "housing_index", "private index");
    _write_test_chunk(source, "housing_template", "private template");
    _write_test_chunk(source, housing_map_chunk_name("main"), "private main");
    _write_test_chunk(source, housing_map_chunk_name("Side_7"),
                      "private side");
    source.commit();

    package capsule;
    housing_copy_visitor_state(source, capsule);
    capsule.commit();

    REQUIRE(capsule.list_chunks() == vector<string>{ "chr", "you" });
    REQUIRE_FALSE(capsule.has_chunk("D"));
    REQUIRE_FALSE(capsule.has_chunk("housing_index"));
    REQUIRE_FALSE(capsule.has_chunk("housing_template"));
    REQUIRE_FALSE(capsule.has_chunk(housing_map_chunk_name("main")));
    REQUIRE_FALSE(capsule.has_chunk(housing_map_chunk_name("Side_7")));
}

#ifndef TARGET_OS_WINDOWS
struct housing_public_test_tree
{
    string root;
    string account_dir;
    string by_name_dir;
    string owner_dir;
    string account_file;
    string name_file;
    string outside_dir;
    string outside_file;

    ~housing_public_test_tree()
    {
        unlink_u(account_file.c_str());
        unlink_u(name_file.c_str());
        unlink_u(owner_dir.c_str());
        unlink_u(outside_file.c_str());
        rmdir(owner_dir.c_str());
        rmdir(by_name_dir.c_str());
        rmdir(account_dir.c_str());
        rmdir(root.c_str());
        rmdir(outside_dir.c_str());
    }
};

static bool _touch_test_file(const string &path)
{
    FILE *file = fopen_u(path.c_str(), "wb");
    if (!file)
        return false;
    const bool wrote = fputs("test", file) >= 0;
    const bool closed = fclose(file) == 0;
    return wrote && closed;
}

TEST_CASE("Housing public map deletion retries after one alias fails",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *root = mkdtemp(root_template);
    REQUIRE(root != nullptr);

    housing_public_test_tree tree;
    tree.root = root;
    tree.account_dir = catpath(tree.root, "17");
    tree.by_name_dir = catpath(tree.root, "by-name");
    tree.owner_dir = catpath(tree.by_name_dir, "owner");
    tree.account_file = catpath(tree.account_dir, "Side_7.hmap");
    tree.name_file = catpath(tree.owner_dir, "Side_7.hmap");
    tree.outside_dir = tree.root + ".outside";
    tree.outside_file = catpath(tree.outside_dir, "Side_7.hmap");

    REQUIRE(mkdir_u(tree.account_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.by_name_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.outside_dir.c_str(), 0700) == 0);
    REQUIRE(_touch_test_file(tree.account_file));
    REQUIRE(_touch_test_file(tree.outside_file));
    REQUIRE(symlink(tree.outside_dir.c_str(), tree.owner_dir.c_str()) == 0);

    // The ordinary account-id alias is removed, but a parent symlink resolving
    // outside the public tree is rejected before unlink. The operation as a
    // whole fails so callers retain the canonical map for a safe retry.
    REQUIRE_FALSE(housing_unpublish_map_files(tree.root, "17", "Owner",
                                               "Side_7"));
    REQUIRE_FALSE(file_exists(tree.account_file));
    REQUIRE(file_exists(tree.name_file));
    REQUIRE(file_exists(tree.outside_file));

    // Retrying is idempotent: ENOENT for the first alias is accepted and the
    // now-safe second alias is removed.
    REQUIRE(unlink_u(tree.owner_dir.c_str()) == 0);
    REQUIRE(mkdir_u(tree.owner_dir.c_str(), 0700) == 0);
    REQUIRE(_touch_test_file(tree.name_file));
    REQUIRE(housing_unpublish_map_files(tree.root, "17", "Owner", "Side_7"));
    REQUIRE_FALSE(file_exists(tree.name_file));
    REQUIRE(file_exists(tree.outside_file));
}

TEST_CASE("Housing public deletion unlinks a dangling alias symlink",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *root = mkdtemp(root_template);
    REQUIRE(root != nullptr);

    housing_public_test_tree tree;
    tree.root = root;
    tree.account_dir = catpath(tree.root, "17");
    tree.by_name_dir = catpath(tree.root, "by-name");
    tree.owner_dir = catpath(tree.by_name_dir, "owner");
    tree.account_file = catpath(tree.account_dir, "Side_7.hmap");
    tree.name_file = catpath(tree.owner_dir, "Side_7.hmap");
    tree.outside_file = tree.root + ".missing";

    REQUIRE(mkdir_u(tree.account_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.by_name_dir.c_str(), 0700) == 0);
    REQUIRE(mkdir_u(tree.owner_dir.c_str(), 0700) == 0);
    REQUIRE(symlink(tree.outside_file.c_str(), tree.name_file.c_str()) == 0);

    struct stat before;
    REQUIRE(lstat(tree.name_file.c_str(), &before) == 0);
    REQUIRE(S_ISLNK(before.st_mode));
    REQUIRE_FALSE(file_exists(tree.name_file));

    // The account alias is ENOENT and the by-name alias is dangling. Direct
    // unlink after parent containment validation handles both idempotently.
    REQUIRE(housing_unpublish_map_files(tree.root, "17", "Owner", "Side_7"));
    struct stat after;
    errno = 0;
    REQUIRE(lstat(tree.name_file.c_str(), &after) == -1);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("Housing permits deletion before a map is first published",
          "[single-file]")
{
    char root_template[] = "/tmp/crawl-housing-unpublish-XXXXXX";
    char *root = mkdtemp(root_template);
    REQUIRE(root != nullptr);

    housing_public_test_tree tree;
    tree.root = root;
    tree.account_dir = catpath(tree.root, "17");
    tree.by_name_dir = catpath(tree.root, "by-name");
    tree.owner_dir = catpath(tree.by_name_dir, "owner");
    tree.account_file = catpath(tree.account_dir, "Side_7.hmap");
    tree.name_file = catpath(tree.owner_dir, "Side_7.hmap");

    // A newly created map can be deleted before its first entry/publication,
    // when neither public alias nor its account/name parent exists.
    REQUIRE(housing_unpublish_map_files(tree.root, "17", "Owner", "Side_7"));
}
#endif

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
