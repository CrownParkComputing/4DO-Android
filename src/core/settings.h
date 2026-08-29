// Settings that survive a restart.
//
// Small and deliberately dumb: a flat key/value file. There is nothing here
// worth a format with a schema, and a text file can be read and fixed by hand
// when something goes wrong on a device you cannot debug.
//
// One rule shapes the API: **paths are stored, but never trusted on load.**
// On Android a SAF grant can be revoked from system settings, and on iOS the
// app's container is reassigned on every install, so a path that worked
// yesterday may be meaningless today. Callers ask for a remembered path and
// then check it still resolves; the settings layer promises only to give back
// what it was told.
#pragma once

#include <map>
#include <string>

namespace retro3do {

class Settings {
public:
    // Load from `path`. A missing file is not an error — it is a first run.
    bool load(const std::string& path);

    // Write to the path last loaded from, or to `path`. Returns false if the
    // file could not be written, which on a full device is worth surfacing
    // rather than losing the user's settings silently.
    bool save() const;
    bool save_as(const std::string& path);

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    int get_int(const std::string& key, int fallback = 0) const;
    bool get_bool(const std::string& key, bool fallback = false) const;

    void set(const std::string& key, const std::string& value);
    void set_int(const std::string& key, int value);
    void set_bool(const std::string& key, bool value);

    void remove(const std::string& key);
    bool has(const std::string& key) const;

    size_t size() const { return values_.size(); }
    const std::string& path() const { return path_; }

private:
    std::map<std::string, std::string> values_;
    std::string path_;
};

// The keys the app actually uses, in one place so a typo is a compile error
// rather than a setting that silently never loads.
namespace settings_key {
constexpr const char* kBiosPath = "bios.path";
constexpr const char* kBiosName = "bios.name";
constexpr const char* kDiscPath = "disc.path";
constexpr const char* kDiscName = "disc.name";
constexpr const char* kRegion = "machine.region";
constexpr const char* kTouchControls = "input.touch_controls";
}  // namespace settings_key

}  // namespace retro3do
