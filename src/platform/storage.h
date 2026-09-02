// Where files live, per platform.
//
// This is the piece that decides whether the app is usable at all on a phone.
// A desktop user can type a path; a phone user cannot, and has no idea what the
// filesystem looks like anyway. So the app has to know sensible places to start
// looking and a writable place of its own.
//
// One trap this exists to contain: on iOS the app's container directory is
// reassigned on every install, so an absolute path saved during setup is dead
// by the next build. Paths that need to survive must be stored relative to a
// directory looked up at runtime, never absolute.
#pragma once

#include <string>
#include <vector>

namespace retro3do {

struct StorageLocation {
    std::string label;   // shown to the user
    std::string path;
};

class Storage {
public:
    // Where the app may write: saves, states, NVRAM, settings. Always exists
    // and is always writable, on every platform.
    static std::string writable_directory();

    // Sensible places to start browsing for a BIOS or a disc. Ordered most
    // likely first. On mobile this is what replaces "type a path".
    static std::vector<StorageLocation> browse_roots();

    // Join two path fragments with the platform's separator, tolerating a
    // trailing separator on the first.
    static std::string join(const std::string& base, const std::string& leaf);

    // The parent of a directory, or an empty string if there is none.
    static std::string parent_of(const std::string& path);

    // The last component of a path, which is what a user recognises a file by.
    static std::string base_name(const std::string& path);

    static bool is_directory(const std::string& path);
    static bool exists(const std::string& path);
};

}  // namespace retro3do
