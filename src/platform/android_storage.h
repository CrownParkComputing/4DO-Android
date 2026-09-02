// Android's Storage Access Framework, from C++.
//
// The user grants access to individual folders and the app gets exactly those,
// persisted. This is deliberately not all-files access: that needs a Play
// sensitive-permission declaration and review, and the app does not need it —
// it needs the folder the user points at.
//
// The consequence that shapes everything else: SAF hands out content URIs, not
// paths. None of these can be fopen'd. A file is read by asking the system for
// a descriptor, which is why Disc has an open-by-descriptor path.
//
// On every other platform these are stubs returning "no SAF here", so callers
// need no per-platform branches.
#pragma once

#include <string>
#include <vector>

namespace retro3do {

struct DocumentEntry {
    std::string name;
    std::string uri;
    bool is_directory = false;
};

struct GpuDriverImport {
    bool ready = false;
    bool success = false;
    std::string name;
    std::string directory;
    std::string library;
    std::string message;
};

// One asynchronous RetroMedia operation. Account credentials never cross this
// bridge on the way back: Java exchanges them for a revocable server session,
// keeps that token in private app storage, and only reports ordinary account
// and progress information to the native UI.
struct RetroMediaResult {
    bool ready = false;
    bool success = false;
    std::string operation;
    std::string email;
    int credits = 0;
    int free_remaining = 0;
    int matched = 0;
    int downloaded = 0;
    bool is_admin = false;
    std::string message;
};

struct RetroMediaGame {
    std::string slug;
    std::string name;
    int rom_files = 0;
    long long total_bytes = 0;
};

struct RetroMediaArtwork {
    std::string key;
    std::string path;
    int width = 0;
    int height = 0;
};

class AndroidStorage {
public:
    // False everywhere except Android, so the file browser can pick between
    // filesystem paths and document URIs without knowing which platform it is on.
    static bool available();

    // Show the system folder picker. Returns immediately: the grant arrives
    // later, and granted_roots() will include it once the user has chosen.
    static void pick_folder();

    // Folders the user has granted, most recently added last.
    static std::vector<DocumentEntry> granted_roots();

    // The folder most recently returned by the system picker, then clears it.
    // Empty when the picker is still open or was cancelled.
    static DocumentEntry consume_picked_folder();

    // Give up a folder. The system revokes the persisted grant too, so the app
    // holds nothing it is not using.
    static void forget_root(const std::string& uri);

    // List one folder, by tree URI or document URI.
    static std::vector<DocumentEntry> list(const std::string& uri);

    // Open a document for reading. Returns a descriptor the caller owns and must
    // close, or -1. Ownership matters: the Java side detaches it precisely so
    // garbage collection cannot close it underneath C++.
    static int open_document(const std::string& uri);

    // Custom Android GPU drivers are selected through a one-file SAF picker,
    // then validated and unpacked by Java into the app's private storage.
    static void pick_gpu_driver_package();
    static GpuDriverImport consume_gpu_driver_import();
    static std::string native_library_directory();

    // RetroMedia is currently an Android service integration. Calls start work
    // on a Java background thread; consume_retro_media_result() is polled from
    // the normal frame loop so network access can never stall emulation/audio.
    static void begin_retro_media_status();
    static void begin_retro_media_login(const std::string& email,
                                        const std::string& password);
    static void begin_retro_media_logout();
    static void begin_retro_media_sync(const std::vector<std::string>& games,
                                       const std::string& media_type);
    static void begin_retro_media_catalogue(const std::string& search);
    static void begin_retro_media_download(const std::string& slug,
                                           const std::string& games_folder);
    static RetroMediaResult consume_retro_media_result();
    static std::vector<RetroMediaGame> retro_media_catalogue();
    static std::vector<RetroMediaArtwork> retro_media_artwork(
        const std::string& media_type);
    static std::string retro_media_saved_email();
};

}  // namespace retro3do
