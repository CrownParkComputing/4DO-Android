// A file browser for picking a BIOS or a disc image.
//
// It exists because typing a path is not something a phone user can do, and the
// launcher is otherwise unusable on exactly the platforms this project is for.
// It is deliberately plain: a list of places to start, a list of entries, and a
// way back up. No thumbnails, no search, no favourites — those can come later
// and none of them are what makes the app work.
#pragma once

#include <string>
#include <vector>

namespace retro3do {

class FileBrowser {
public:
    // Open at the most sensible starting place. `extensions` are matched
    // case-insensitively, with the leading dot, and an empty list shows every
    // file.
    void open(const std::string& title, std::vector<std::string> extensions);
    void close();
    bool is_open() const { return open_; }

    // Draw it. Returns true on the frame a file was chosen, with the full path
    // in `chosen_path`.
    bool draw(std::string* chosen_path);

private:
    void navigate_to(const std::string& directory);

    struct Entry {
        std::string name;
        bool is_directory = false;
    };

    bool open_ = false;
    std::string title_;
    std::string directory_;
    std::vector<std::string> extensions_;
    std::vector<Entry> entries_;
    std::string error_;
};

}  // namespace retro3do
