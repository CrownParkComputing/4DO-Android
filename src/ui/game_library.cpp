#include "game_library.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace retro3do {
namespace {

std::string lowercased(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

char first_group(const std::string& text) {
    for (unsigned char c : text) {
        if (std::isdigit(c)) return '#';
        if (std::isalpha(c)) return static_cast<char>(std::toupper(c));
    }
    return 0;
}

struct DiscIdentity {
    std::string title;
    int number = 1;
    bool marked = false;
};

DiscIdentity disc_identity(const std::string& display_name) {
    // Redump and collection names use all of these in practice: (Disc 1),
    // [Disk 2 of 4], "- CD3" and "Disc_1". The marker is only accepted at
    // the end so a title which happens to contain "CD" is not grouped.
    static const std::regex marker(
        R"(^(.+?)[\s._-]*(?:\(|\[)?(?:disc|disk|cd)[\s._-]*0*([1-9][0-9]*)(?:[\s._-]*(?:of|/)[\s._-]*[0-9]+)?(?:\)|\])?[\s._-]*$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(display_name, match, marker)) {
        return {display_name, 1, false};
    }
    std::string title = match[1].str();
    while (!title.empty() && (std::isspace(static_cast<unsigned char>(title.back())) ||
                              title.back() == '-' || title.back() == '_' ||
                              title.back() == '.')) {
        title.pop_back();
    }
    int number = 1;
    try { number = std::stoi(match[2].str()); } catch (...) {}
    return {title.empty() ? display_name : title, number, true};
}

void sort_discs(LibraryGame& game) {
    std::stable_sort(game.discs.begin(), game.discs.end(),
                     [](const LibraryDisc& a, const LibraryDisc& b) {
        if (a.number != b.number) return a.number < b.number;
        return lowercased(a.name) < lowercased(b.name);
    });
    if (!game.discs.empty()) game.target = game.discs.front().target;
}

}  // namespace

std::string GameLibrary::display_name(const std::string& filename) {
    std::string name = filename;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);

    const std::string lower = lowercased(name);
    static constexpr const char* kExtensions[] = {
        ".chd", ".iso", ".bin", ".cue", ".img"
    };
    for (const char* extension : kExtensions) {
        const size_t length = std::char_traits<char>::length(extension);
        if (lower.size() > length &&
            lower.compare(lower.size() - length, length, extension) == 0) {
            name.resize(name.size() - length);
            break;
        }
    }
    for (char& c : name) {
        if (c == '_') c = ' ';
    }
    return name.empty() ? std::string("Untitled game") : name;
}

bool GameLibrary::add(std::string name, std::string target) {
    if (target.empty()) return false;
    const std::string disc_name = display_name(name.empty() ? target : name);
    const DiscIdentity identity = disc_identity(disc_name);
    name = identity.title;

    const std::string normalized_name = lowercased(name);
    for (LibraryGame& game : games_) {
        const auto same_target = std::find_if(
            game.discs.begin(), game.discs.end(), [&](const LibraryDisc& disc) {
                return disc.target == target;
            });
        if (same_target != game.discs.end() ||
            lowercased(game.name) == normalized_name) {
            bool changed = game.name != name;
            game.name = name;
            auto existing = std::find_if(
                game.discs.begin(), game.discs.end(), [&](const LibraryDisc& disc) {
                    return disc.target == target ||
                           (identity.marked && disc.number == identity.number);
                });
            if (!identity.marked && existing == game.discs.end() &&
                game.discs.size() == 1) {
                existing = game.discs.begin();
            }
            if (existing == game.discs.end()) {
                game.discs.push_back({disc_name, target, identity.number});
                changed = true;
            } else {
                changed = changed || existing->name != disc_name ||
                          existing->target != target ||
                          existing->number != identity.number;
                *existing = {disc_name, target, identity.number};
            }
            sort_discs(game);
            sort();
            return changed;
        }
    }
    LibraryGame game;
    game.name = std::move(name);
    game.target = target;
    game.discs.push_back({disc_name, std::move(target), identity.number});
    sort_discs(game);
    games_.push_back(std::move(game));
    sort();
    return true;
}

bool GameLibrary::remove(const std::string& target) {
    bool removed = false;
    for (LibraryGame& game : games_) {
        const auto it = std::remove_if(
            game.discs.begin(), game.discs.end(), [&](const LibraryDisc& disc) {
                return disc.target == target;
            });
        if (it != game.discs.end()) {
            game.discs.erase(it, game.discs.end());
            sort_discs(game);
            removed = true;
        }
    }
    games_.erase(std::remove_if(games_.begin(), games_.end(),
                                [](const LibraryGame& game) {
                                    return game.discs.empty();
                                }),
                 games_.end());
    return removed;
}

void GameLibrary::replace(const std::vector<LibraryGame>& games) {
    games_.clear();
    for (const LibraryGame& game : games) {
        if (game.discs.empty()) {
            add(game.name, game.target);
        } else {
            for (const LibraryDisc& disc : game.discs) {
                add(disc.name.empty() ? game.name : disc.name, disc.target);
            }
        }
    }
}

std::vector<const LibraryGame*> GameLibrary::filtered(
    const std::string& search, char group) const {
    const std::string needle = lowercased(search);
    std::vector<const LibraryGame*> result;
    for (const LibraryGame& game : games_) {
        if (group != 0 && first_group(game.name) != group) continue;
        if (!needle.empty() && lowercased(game.name).find(needle) == std::string::npos) {
            continue;
        }
        result.push_back(&game);
    }
    return result;
}

void GameLibrary::sort() {
    std::stable_sort(games_.begin(), games_.end(),
                     [](const LibraryGame& a, const LibraryGame& b) {
        const std::string left = lowercased(a.name);
        const std::string right = lowercased(b.name);
        if (left != right) return left < right;
        return a.target < b.target;
    });
}

}  // namespace retro3do
