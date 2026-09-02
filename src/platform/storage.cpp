#include "storage.h"

#include <SDL3/SDL.h>

#include "core/path.h"
#include "platform/platform.h"

namespace retro3do {
namespace {

void add_if_present(std::vector<StorageLocation>& out, const char* label,
                    const char* path) {
    if (path == nullptr || *path == '\0') {
        return;
    }
    if (!Storage::is_directory(path)) {
        return;
    }
    // Do not offer the same directory twice under two names.
    for (const StorageLocation& existing : out) {
        if (existing.path == path) return;
    }
    out.push_back(StorageLocation{label, path});
}

void add_user_folder(std::vector<StorageLocation>& out, const char* label,
                     SDL_Folder folder) {
    const char* path = SDL_GetUserFolder(folder);
    add_if_present(out, label, path);
}

}  // namespace

std::string Storage::writable_directory() {
    // SDL puts this in the right place on every platform: the app's private
    // data directory on Android, Application Support on iOS and macOS,
    // AppData on Windows, XDG data on Linux. It is created if missing.
    char* path = SDL_GetPrefPath("CrownParkComputing", "Retro-3DO");
    if (path == nullptr) {
        return {};
    }
    std::string result(path);
    SDL_free(path);
    return result;
}

std::vector<StorageLocation> Storage::browse_roots() {
    std::vector<StorageLocation> roots;

#if RETRO3DO_PLATFORM_IOS
    // On iOS the FIRST root has to be Documents, not the app's writable
    // directory. SDL_GetPrefPath returns Library/Application Support, and
    // UIFileSharingEnabled exposes Documents ONLY - so Application Support is
    // exactly the one folder a user cannot put a file into. Offer it second,
    // for anyone looking for the settings and NVRAM the app wrote there.
    //
    // Get this the wrong way round and the app looks broken in a way nobody can
    // diagnose: the BIOS is visibly there in the Files app and the emulator's
    // own browser cannot see it.
    add_user_folder(roots, "Documents (Files app)", SDL_FOLDER_DOCUMENTS);
    add_if_present(roots, "App storage", writable_directory().c_str());
#else
    const std::string writable = writable_directory();
    add_if_present(roots, "App storage", writable.c_str());
#endif

#if defined(__ANDROID__)
    // Shared storage, where a file manager or a download will have left things.
    add_if_present(roots, "Shared storage", "/storage/emulated/0");
    add_if_present(roots, "Downloads", "/storage/emulated/0/Download");
    add_if_present(roots, "Documents", "/storage/emulated/0/Documents");
    // A memory card, if there is one. The numeric name varies by device, so
    // the parent is offered and the user picks.
    add_if_present(roots, "SD card", "/storage");
#else
    add_user_folder(roots, "Downloads", SDL_FOLDER_DOWNLOADS);
    add_user_folder(roots, "Documents", SDL_FOLDER_DOCUMENTS);
    add_user_folder(roots, "Home", SDL_FOLDER_HOME);
#endif

    // Somewhere to go if none of the above resolved, so the browser is never
    // empty and unusable.
    if (roots.empty()) {
#if defined(_WIN32)
        add_if_present(roots, "C:", "C:\\");
#else
        add_if_present(roots, "Root", "/");
#endif
    }

    return roots;
}

std::string Storage::join(const std::string& base, const std::string& leaf) {
    return path::join(base, leaf);
}

std::string Storage::base_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Storage::parent_of(const std::string& path) {
    return path::parent(path);
}

bool Storage::is_directory(const std::string& path) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info)) {
        return false;
    }
    return info.type == SDL_PATHTYPE_DIRECTORY;
}

bool Storage::exists(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info);
}

}  // namespace retro3do
