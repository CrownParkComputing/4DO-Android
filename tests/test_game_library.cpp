#include "ui/game_library.h"
#include "test_harness.h"

using namespace retro3do;

TEST(game_library_cleans_disc_names_and_sorts_without_case_bias) {
    GameLibrary library;
    library.add("road_rash.CHD", "uri:road");
    library.add("Need for Speed.iso", "uri:nfs");
    library.add("3D Atlas.bin", "uri:atlas");

    CHECK_EQ(library.games().size(), size_t{3});
    CHECK(library.games()[0].name == "3D Atlas");
    CHECK(library.games()[1].name == "Need for Speed");
    CHECK(library.games()[2].name == "road rash");
}

TEST(game_library_filters_by_zero_to_nine_letter_and_search) {
    GameLibrary library;
    library.add("Road Rash.chd", "road");
    library.add("Road & Track Presents - The Need for Speed.chd", "nfs");
    library.add("3D Atlas.iso", "atlas");

    CHECK_EQ(library.filtered("", '#').size(), size_t{1});
    CHECK_EQ(library.filtered("", 'R').size(), size_t{2});
    CHECK_EQ(library.filtered("need", 0).size(), size_t{1});
    CHECK_EQ(library.filtered("need", 'N').size(), size_t{0});
}

TEST(game_library_deduplicates_opaque_targets_and_can_remove_them) {
    GameLibrary library;
    CHECK(library.add("Old.iso", "content://tree/file?id=1"));
    CHECK(library.add("New.chd", "content://tree/file?id=1"));
    CHECK_EQ(library.games().size(), size_t{1});
    CHECK(library.games()[0].name == "New");
    CHECK(library.remove("content://tree/file?id=1"));
    CHECK(library.games().empty());
    CHECK(!library.remove("content://tree/file?id=1"));
}

TEST(game_library_deduplicates_the_same_game_from_different_saf_grants) {
    GameLibrary library;
    library.add("Road Rash (USA).chd", "content://parent-tree/road-rash");
    library.add("Road Rash (USA).chd", "content://games-tree/road-rash");
    CHECK_EQ(library.games().size(), size_t{1});
    CHECK(library.games()[0].target == "content://games-tree/road-rash");
}

TEST(game_library_groups_and_orders_multi_disc_sets) {
    GameLibrary library;
    library.add("Wing Commander III (Disc 4).chd", "uri:disc4");
    library.add("Wing Commander III (Disc 1).chd", "uri:disc1");
    library.add("Wing Commander III [Disk 2 of 4].chd", "uri:disc2");
    library.add("Wing Commander III - CD3.chd", "uri:disc3");

    CHECK_EQ(library.games().size(), size_t{1});
    CHECK(library.games()[0].name == "Wing Commander III");
    CHECK_EQ(library.games()[0].discs.size(), size_t{4});
    CHECK(library.games()[0].target == "uri:disc1");
    CHECK(library.games()[0].discs[3].target == "uri:disc4");
}
