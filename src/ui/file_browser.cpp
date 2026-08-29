#include "file_browser.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>

#include "imgui.h"
#include "platform/storage.h"

namespace retro3do {
namespace {

std::string lowercased(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool has_extension(const std::string& name,
                   const std::vector<std::string>& extensions) {
    if (extensions.empty()) {
        return true;
    }
    const std::string lower = lowercased(name);
    for (const std::string& ext : extensions) {
        if (lower.size() >= ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

// SDL hands each entry to this callback as it walks the directory.
struct EnumerationState {
    std::vector<std::pair<std::string, bool>> entries;
};

SDL_EnumerationResult SDLCALL on_entry(void* userdata, const char* directory,
                                       const char* name) {
    auto* state = static_cast<EnumerationState*>(userdata);

    // Skip dotfiles: on Android especially, shared storage is full of them and
    // none are ever a disc image.
    if (name == nullptr || name[0] == '.') {
        return SDL_ENUM_CONTINUE;
    }

    SDL_PathInfo info;
    const std::string full = Storage::join(directory, name);
    bool is_directory = false;
    if (SDL_GetPathInfo(full.c_str(), &info)) {
        is_directory = info.type == SDL_PATHTYPE_DIRECTORY;
    }
    state->entries.emplace_back(name, is_directory);
    return SDL_ENUM_CONTINUE;
}

}  // namespace

void FileBrowser::open(const std::string& title,
                       std::vector<std::string> extensions) {
    open_ = true;
    title_ = title;
    extensions_ = std::move(extensions);
    for (std::string& ext : extensions_) {
        ext = lowercased(ext);
    }

    const std::vector<StorageLocation> roots = Storage::browse_roots();
    navigate_to(roots.empty() ? std::string("/") : roots.front().path);
}

void FileBrowser::close() {
    open_ = false;
    entries_.clear();
    error_.clear();
}

void FileBrowser::navigate_to(const std::string& directory) {
    directory_ = directory;
    entries_.clear();
    error_.clear();

    EnumerationState state;
    if (!SDL_EnumerateDirectory(directory.c_str(), on_entry, &state)) {
        // A directory that cannot be read is normal on mobile — shared storage
        // needs a permission the app may not have been granted. Say so plainly
        // rather than showing an empty list that looks like an empty folder.
        error_ = std::string("Cannot read this folder: ") + SDL_GetError();
        return;
    }

    for (auto& entry : state.entries) {
        if (entry.second || has_extension(entry.first, extensions_)) {
            entries_.push_back(Entry{entry.first, entry.second});
        }
    }

    // Directories first, then files, each alphabetically — the order people
    // expect, and it puts the folders you are looking for at the top.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.is_directory != b.is_directory) return a.is_directory;
        return lowercased(a.name) < lowercased(b.name);
    });
}

bool FileBrowser::draw(std::string* chosen_path) {
    if (!open_) {
        return false;
    }

    bool chose = false;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f),
        ImGuiCond_Always);

    if (ImGui::Begin(title_.c_str(), &open_,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        // Shortcuts to the places worth starting from. On a phone this is the
        // whole navigation story.
        for (const StorageLocation& root : Storage::browse_roots()) {
            if (ImGui::Button(root.label.c_str())) {
                navigate_to(root.path);
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Separator();
        ImGui::TextWrapped("%s", directory_.c_str());
        ImGui::Separator();

        const std::string parent = Storage::parent_of(directory_);
        ImGui::BeginDisabled(parent.empty());
        if (ImGui::Button("Up")) {
            navigate_to(parent);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            close();
            ImGui::End();
            return false;
        }

        ImGui::Separator();

        if (!error_.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s",
                               error_.c_str());
        }

        // A tall touch-friendly list. Selectable rows rather than tree nodes:
        // a finger needs a big target, and there is nothing to expand.
        ImGui::BeginChild("entries", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (const Entry& entry : entries_) {
            const std::string label =
                (entry.is_directory ? "[ ] " : "    ") + entry.name;
            if (ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0, ImGui::GetTextLineHeight() * 1.8f))) {
                const std::string full = Storage::join(directory_, entry.name);
                if (entry.is_directory) {
                    navigate_to(full);
                } else {
                    *chosen_path = full;
                    chose = true;
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (chose) {
        close();
    }
    if (!open_) {
        entries_.clear();
    }
    return chose;
}

}  // namespace retro3do
