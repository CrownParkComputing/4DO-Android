// Persistent, platform-neutral game library used by the launcher.
#pragma once

#include <string>
#include <vector>

namespace retro3do {

struct LibraryDisc {
    std::string name;
    std::string target;
    int number = 1;
};

struct LibraryGame {
    std::string name;
    std::string target;
    std::vector<LibraryDisc> discs;
};

// The model deliberately knows nothing about SDL or Android SAF. A target can
// be a normal path or an opaque document URI; sorting and filtering only ever
// inspect the friendly display name.
class GameLibrary {
public:
    bool add(std::string name, std::string target);
    bool remove(const std::string& target);
    void replace(const std::vector<LibraryGame>& games);

    const std::vector<LibraryGame>& games() const { return games_; }

    // group is 0 for all, '#' for 0-9, or an uppercase A-Z letter.
    std::vector<const LibraryGame*> filtered(const std::string& search,
                                             char group) const;

    static std::string display_name(const std::string& filename);

private:
    void sort();
    std::vector<LibraryGame> games_;
};

}  // namespace retro3do
