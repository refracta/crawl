#include "catch_amalgamated.hpp"

#include "AppHdr.h"

#include "housing.h"
#include "package.h"

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
