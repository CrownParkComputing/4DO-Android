// Path string handling.
//
// Navigation in the file browser rests entirely on parent(): if it is wrong,
// a phone user cannot go up a level and is simply stuck, with no keyboard to
// type their way out of it. So the edge cases get their own assertions.
#include "core/path.h"
#include "test_harness.h"

using namespace retro3do;

TEST(join_inserts_exactly_one_separator) {
    CHECK(path::join("/a", "b") == "/a/b");
    CHECK(path::join("/a/", "b") == "/a/b");
    CHECK(path::join("/a//", "b") == "/a//b");
}

TEST(join_tolerates_an_empty_side) {
    CHECK(path::join("", "b") == "b");
    CHECK(path::join("/a", "") == "/a");
}

TEST(parent_drops_the_last_component) {
    CHECK(path::parent("/a/b/c") == "/a/b");
    CHECK(path::parent("/a/b") == "/a");
}

TEST(parent_ignores_a_trailing_separator) {
    // "/a/b/" must give "/a", not "/a/b" — otherwise going up appears to do
    // nothing the first time it is pressed.
    CHECK(path::parent("/a/b/") == "/a");
    CHECK(path::parent("/a/b///") == "/a");
}

TEST(the_parent_of_a_top_level_entry_is_the_root) {
    // Not an empty string: returning empty here dead-ends navigation one level
    // above where it should.
    CHECK(path::parent("/foo") == "/");
    CHECK(path::parent("/foo/") == "/");
}

TEST(the_root_has_no_parent) {
    CHECK(path::parent("/").empty());
    CHECK(path::parent("").empty());
}

TEST(filename_is_the_last_component) {
    CHECK(path::filename("/a/b/game.iso") == "game.iso");
    CHECK(path::filename("game.iso") == "game.iso");
    CHECK(path::filename("/a/b/") == "b");
    CHECK(path::filename("/").empty());
}

TEST(extension_is_lowercased_and_includes_the_dot) {
    CHECK(path::extension("/a/GAME.ISO") == ".iso");
    CHECK(path::extension("game.CuE") == ".cue");
    CHECK(path::extension("/a/b/disc.bin") == ".bin");
}

TEST(a_file_with_no_extension_has_none) {
    CHECK(path::extension("/a/README").empty());
    CHECK(path::extension("README").empty());
}

TEST(a_dotfile_is_hidden_not_an_extension) {
    // ".bashrc" is a hidden file whose extension is nothing, not ".bashrc".
    CHECK(path::extension("/home/jon/.bashrc").empty());
}

TEST(only_the_last_dot_counts) {
    CHECK(path::extension("/a/my.game.disc.iso") == ".iso");
}

TEST(a_dot_in_a_directory_name_is_not_the_files_extension) {
    // "/a.b/game" has no extension: the dot belongs to the directory.
    CHECK(path::extension("/a.b/game").empty());
}
