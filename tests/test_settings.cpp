// Settings persistence.
//
// Small surface, but it is the difference between an app that remembers your
// library and one that asks for it again every launch. The parsing cases
// matter because a settings file survives across builds and can be edited by
// hand on a device with no debugger attached.
#include <cstdio>

#include "core/settings.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

const char* scratch(const char* name) {
    static std::string path;
    path = std::string("/tmp/retro3do-settings-") + name;
    return path.c_str();
}

void write_text(const char* path, const char* text) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fputs(text, f);
    std::fclose(f);
}

}  // namespace

TEST(a_missing_file_is_a_first_run_not_a_failure) {
    Settings settings;
    CHECK(settings.load("/definitely/not/here/settings.cfg"));
    CHECK_EQ(settings.size(), size_t{0});
}

TEST(values_survive_a_save_and_load) {
    const char* path = scratch("roundtrip.cfg");
    std::remove(path);

    Settings out;
    out.load(path);
    out.set("bios.path", "/games/bios.rom");
    out.set_int("machine.region", 1);
    out.set_bool("input.touch_controls", true);
    CHECK(out.save());

    Settings in;
    CHECK(in.load(path));
    CHECK(in.get("bios.path") == "/games/bios.rom");
    CHECK_EQ(in.get_int("machine.region"), 1);
    CHECK(in.get_bool("input.touch_controls"));
}

TEST(a_value_may_contain_an_equals_sign) {
    // Not hypothetical: a SAF content URI very often contains one, so splitting
    // on the last '=' rather than the first would truncate every Android path.
    const char* path = scratch("equals.cfg");
    write_text(path,
               "disc.path=content://com.android.externalstorage/tree/primary%3AGames?x=1\n");

    Settings settings;
    CHECK(settings.load(path));
    CHECK(settings.get("disc.path") ==
          "content://com.android.externalstorage/tree/primary%3AGames?x=1");
}

TEST(comments_and_blank_lines_are_ignored) {
    const char* path = scratch("comments.cfg");
    write_text(path, "# a comment\n\n  \nkey=value\n# another\n");

    Settings settings;
    CHECK(settings.load(path));
    CHECK_EQ(settings.size(), size_t{1});
    CHECK(settings.get("key") == "value");
}

TEST(surrounding_whitespace_is_trimmed) {
    const char* path = scratch("spaces.cfg");
    write_text(path, "  key   =   value  \n");

    Settings settings;
    CHECK(settings.load(path));
    CHECK(settings.get("key") == "value");
}

TEST(a_line_with_no_equals_is_skipped_not_guessed_at) {
    const char* path = scratch("junk.cfg");
    write_text(path, "this is not a setting\nkey=value\n");

    Settings settings;
    CHECK(settings.load(path));
    CHECK_EQ(settings.size(), size_t{1});
    CHECK(settings.get("key") == "value");
}

TEST(a_non_numeric_value_falls_back_rather_than_becoming_zero) {
    // Treating "banana" as 0 would silently look like a valid setting. A
    // corrupted line should leave the default in place instead.
    const char* path = scratch("badint.cfg");
    write_text(path, "machine.region=banana\nother=12x\n");

    Settings settings;
    CHECK(settings.load(path));
    CHECK_EQ(settings.get_int("machine.region", 7), 7);
    CHECK_EQ(settings.get_int("other", 7), 7);
}

TEST(booleans_accept_the_spellings_people_actually_write) {
    const char* path = scratch("bools.cfg");
    write_text(path, "a=1\nb=true\nc=yes\nd=0\ne=false\nf=no\ng=maybe\n");

    Settings settings;
    CHECK(settings.load(path));
    CHECK(settings.get_bool("a"));
    CHECK(settings.get_bool("b"));
    CHECK(settings.get_bool("c"));
    CHECK(!settings.get_bool("d", true));
    CHECK(!settings.get_bool("e", true));
    CHECK(!settings.get_bool("f", true));
    // Unrecognised keeps the caller's default rather than inventing an answer.
    CHECK(settings.get_bool("g", true));
}

TEST(a_missing_key_returns_the_fallback) {
    Settings settings;
    CHECK(settings.get("nope", "fallback") == "fallback");
    CHECK_EQ(settings.get_int("nope", 42), 42);
    CHECK(settings.get_bool("nope", true));
    CHECK(!settings.has("nope"));
}

TEST(removing_a_key_forgets_it_on_disk_too) {
    // This is how a revoked storage grant is forgotten, so it has to persist.
    const char* path = scratch("remove.cfg");
    std::remove(path);

    Settings out;
    out.load(path);
    out.set("disc.path", "/gone.iso");
    out.save();

    out.remove("disc.path");
    CHECK(out.save());

    Settings in;
    in.load(path);
    CHECK(!in.has("disc.path"));
}

TEST(saving_without_a_path_fails_rather_than_writing_somewhere_random) {
    Settings settings;
    settings.set("key", "value");
    CHECK(!settings.save());
}
