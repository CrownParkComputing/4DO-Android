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

    // Give up a folder. The system revokes the persisted grant too, so the app
    // holds nothing it is not using.
    static void forget_root(const std::string& uri);

    // List one folder, by tree URI or document URI.
    static std::vector<DocumentEntry> list(const std::string& uri);

    // Open a document for reading. Returns a descriptor the caller owns and must
    // close, or -1. Ownership matters: the Java side detaches it precisely so
    // garbage collection cannot close it underneath C++.
    static int open_document(const std::string& uri);
};

}  // namespace retro3do
