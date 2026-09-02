#include "android_storage.h"

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <jni.h>

namespace retro3do {
namespace {

constexpr const char* kActivityClass = "com/crownparkcomputing/retro3do/Retro3DOActivity";

// SDL owns the JVM attachment for whichever thread asks, so this is safe to
// call from the emulator thread as well as the main one.
JNIEnv* env() {
    return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

jclass activity_class(JNIEnv* e) {
    return e->FindClass(kActivityClass);
}

// Java exceptions must be cleared or the next JNI call fails for reasons that
// have nothing to do with it — one of the more confusing ways to lose an hour.
void clear_pending_exception(JNIEnv* e) {
    if (e->ExceptionCheck()) {
        e->ExceptionDescribe();
        e->ExceptionClear();
    }
}

std::string call_static_string(const char* name, const char* signature,
                               const std::string* argument) {
    JNIEnv* e = env();
    if (e == nullptr) return {};

    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return {};
    }

    jmethodID method = e->GetStaticMethodID(cls, name, signature);
    if (method == nullptr) {
        clear_pending_exception(e);
        e->DeleteLocalRef(cls);
        return {};
    }

    jstring result = nullptr;
    if (argument != nullptr) {
        jstring arg = e->NewStringUTF(argument->c_str());
        result = static_cast<jstring>(e->CallStaticObjectMethod(cls, method, arg));
        e->DeleteLocalRef(arg);
    } else {
        result = static_cast<jstring>(e->CallStaticObjectMethod(cls, method));
    }
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);

    if (result == nullptr) return {};

    const char* chars = e->GetStringUTFChars(result, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) e->ReleaseStringUTFChars(result, chars);
    e->DeleteLocalRef(result);
    return out;
}

// The Java side returns newline-separated records so that only strings cross
// the boundary — far less to get wrong than a structured return, and trivial to
// read in a log when something is off.
std::vector<DocumentEntry> parse_listing(const std::string& text, bool tagged) {
    std::vector<DocumentEntry> entries;
    size_t start = 0;
    while (start < text.size()) {
        size_t newline = text.find('\n', start);
        if (newline == std::string::npos) newline = text.size();
        const std::string line = text.substr(start, newline - start);
        start = newline + 1;
        if (line.empty()) continue;

        DocumentEntry entry;
        if (tagged) {
            // "D|name|uri" or "F|name|uri"
            const size_t first = line.find('|');
            const size_t second = line.find('|', first + 1);
            if (first == std::string::npos || second == std::string::npos) continue;
            entry.is_directory = line[0] == 'D';
            entry.name = line.substr(first + 1, second - first - 1);
            entry.uri = line.substr(second + 1);
        } else {
            // "name|uri" — the granted roots, which are always folders.
            const size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            entry.is_directory = true;
            entry.name = line.substr(0, bar);
            entry.uri = line.substr(bar + 1);
        }
        entries.push_back(entry);
    }
    return entries;
}

std::vector<std::string> split_fields(const std::string& text, char separator) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(separator, start);
        fields.push_back(text.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

int decimal(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (...) {
        return 0;
    }
}

void call_static_void_two_strings(const char* name, const std::string& first,
                                  const std::string& second) {
    JNIEnv* e = env();
    if (e == nullptr) return;
    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return;
    }
    jmethodID method = e->GetStaticMethodID(
        cls, name, "(Ljava/lang/String;Ljava/lang/String;)V");
    if (method != nullptr) {
        jstring a = e->NewStringUTF(first.c_str());
        jstring b = e->NewStringUTF(second.c_str());
        e->CallStaticVoidMethod(cls, method, a, b);
        e->DeleteLocalRef(a);
        e->DeleteLocalRef(b);
    }
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);
}

void call_static_void(const char* name) {
    JNIEnv* e = env();
    if (e == nullptr) return;
    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return;
    }
    jmethodID method = e->GetStaticMethodID(cls, name, "()V");
    if (method != nullptr) e->CallStaticVoidMethod(cls, method);
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);
}

}  // namespace

bool AndroidStorage::available() { return true; }

void AndroidStorage::pick_folder() {
    JNIEnv* e = env();
    if (e == nullptr) return;
    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return;
    }
    jmethodID method = e->GetStaticMethodID(cls, "nativePickFolder", "()V");
    if (method != nullptr) {
        e->CallStaticVoidMethod(cls, method);
    }
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);
}

std::vector<DocumentEntry> AndroidStorage::granted_roots() {
    return parse_listing(
        call_static_string("nativeGrantedRoots", "()Ljava/lang/String;", nullptr),
        /*tagged=*/false);
}

DocumentEntry AndroidStorage::consume_picked_folder() {
    const std::string text = call_static_string(
        "nativeConsumePickedFolder", "()Ljava/lang/String;", nullptr);
    const size_t bar = text.find('|');
    if (bar == std::string::npos) return {};
    return DocumentEntry{text.substr(0, bar), text.substr(bar + 1), true};
}

void AndroidStorage::forget_root(const std::string& uri) {
    JNIEnv* e = env();
    if (e == nullptr) return;
    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return;
    }
    jmethodID method =
        e->GetStaticMethodID(cls, "nativeForgetRoot", "(Ljava/lang/String;)V");
    if (method != nullptr) {
        jstring arg = e->NewStringUTF(uri.c_str());
        e->CallStaticVoidMethod(cls, method, arg);
        e->DeleteLocalRef(arg);
    }
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);
}

std::vector<DocumentEntry> AndroidStorage::list(const std::string& uri) {
    return parse_listing(
        call_static_string("nativeListFolder",
                           "(Ljava/lang/String;)Ljava/lang/String;", &uri),
        /*tagged=*/true);
}

int AndroidStorage::open_document(const std::string& uri) {
    JNIEnv* e = env();
    if (e == nullptr) return -1;
    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return -1;
    }
    jmethodID method =
        e->GetStaticMethodID(cls, "nativeOpenDocument", "(Ljava/lang/String;)I");
    if (method == nullptr) {
        clear_pending_exception(e);
        e->DeleteLocalRef(cls);
        return -1;
    }
    jstring arg = e->NewStringUTF(uri.c_str());
    const jint fd = e->CallStaticIntMethod(cls, method, arg);
    e->DeleteLocalRef(arg);
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);
    return static_cast<int>(fd);
}

void AndroidStorage::pick_gpu_driver_package() {
    JNIEnv* e = env();
    if (e == nullptr) return;
    jclass cls = activity_class(e);
    if (cls == nullptr) {
        clear_pending_exception(e);
        return;
    }
    jmethodID method = e->GetStaticMethodID(cls, "nativePickGpuDriver", "()V");
    if (method != nullptr) e->CallStaticVoidMethod(cls, method);
    clear_pending_exception(e);
    e->DeleteLocalRef(cls);
}

GpuDriverImport AndroidStorage::consume_gpu_driver_import() {
    const std::string text = call_static_string(
        "nativeConsumeGpuDriverImport", "()Ljava/lang/String;", nullptr);
    if (text.empty()) return {};

    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t bar = text.find('|', start);
        fields.push_back(text.substr(start, bar == std::string::npos
                                               ? std::string::npos
                                               : bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }

    GpuDriverImport result;
    result.ready = true;
    result.success = !fields.empty() && fields[0] == "OK";
    if (result.success && fields.size() >= 4) {
        result.name = fields[1];
        result.directory = fields[2];
        result.library = fields[3];
        result.message = "Driver imported - restart Retro-3DO to apply";
    } else {
        result.message = fields.size() >= 2 ? fields[1]
                                            : "Driver package import failed";
    }
    return result;
}

std::string AndroidStorage::native_library_directory() {
    return call_static_string("nativeLibraryDirectory", "()Ljava/lang/String;",
                              nullptr);
}

void AndroidStorage::begin_retro_media_status() {
    call_static_void("nativeRetroMediaStatus");
}

void AndroidStorage::begin_retro_media_login(const std::string& email,
                                             const std::string& password) {
    call_static_void_two_strings("nativeRetroMediaLogin", email, password);
}

void AndroidStorage::begin_retro_media_logout() {
    call_static_void("nativeRetroMediaLogout");
}

void AndroidStorage::begin_retro_media_sync(
    const std::vector<std::string>& games, const std::string& media_type) {
    std::string names;
    for (const std::string& game : games) {
        if (!names.empty()) names.push_back('\n');
        // Display names originate in filenames. Removing separators keeps the
        // bridge line-oriented even for a deliberately odd file name.
        for (char c : game) names.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    call_static_void_two_strings("nativeRetroMediaSync", names, media_type);
}

RetroMediaResult AndroidStorage::consume_retro_media_result() {
    const std::string text = call_static_string(
        "nativeConsumeRetroMediaResult", "()Ljava/lang/String;", nullptr);
    if (text.empty()) return {};

    const std::vector<std::string> fields = split_fields(text, '|');
    RetroMediaResult result;
    result.ready = true;
    result.success = !fields.empty() && fields[0] == "OK";
    if (fields.size() > 1) result.operation = fields[1];
    if (fields.size() > 2) result.email = fields[2];
    if (fields.size() > 3) result.credits = decimal(fields[3]);
    if (fields.size() > 4) result.free_remaining = decimal(fields[4]);
    if (fields.size() > 5) result.matched = decimal(fields[5]);
    if (fields.size() > 6) result.downloaded = decimal(fields[6]);
    if (fields.size() > 8) {
        result.is_admin = fields[7] == "1";
        result.message = fields[8];
    } else if (fields.size() > 7) {
        result.message = fields[7];  // pre-admin bridge compatibility
    }
    return result;
}

void AndroidStorage::begin_retro_media_catalogue(const std::string& search) {
    call_static_void_two_strings("nativeRetroMediaCatalogue", search, "");
}

void AndroidStorage::begin_retro_media_download(
    const std::string& slug, const std::string& games_folder) {
    call_static_void_two_strings("nativeRetroMediaDownload", slug, games_folder);
}

std::vector<RetroMediaGame> AndroidStorage::retro_media_catalogue() {
    const std::string text = call_static_string(
        "nativeRetroMediaCatalogueResult", "()Ljava/lang/String;", nullptr);
    std::vector<RetroMediaGame> games;
    size_t start = 0;
    while (start < text.size()) {
        size_t newline = text.find('\n', start);
        if (newline == std::string::npos) newline = text.size();
        const std::vector<std::string> fields =
            split_fields(text.substr(start, newline - start), '|');
        start = newline + 1;
        if (fields.size() != 4 || fields[0].empty()) continue;
        RetroMediaGame game;
        game.slug = fields[0];
        game.name = fields[1];
        game.rom_files = decimal(fields[2]);
        try { game.total_bytes = std::stoll(fields[3]); } catch (...) {}
        games.push_back(std::move(game));
    }
    return games;
}

std::vector<RetroMediaArtwork> AndroidStorage::retro_media_artwork(
    const std::string& media_type) {
    const std::string text = call_static_string(
        "nativeRetroMediaArtwork", "(Ljava/lang/String;)Ljava/lang/String;",
        &media_type);
    std::vector<RetroMediaArtwork> artwork;
    size_t start = 0;
    while (start < text.size()) {
        size_t newline = text.find('\n', start);
        if (newline == std::string::npos) newline = text.size();
        const std::vector<std::string> fields =
            split_fields(text.substr(start, newline - start), '|');
        start = newline + 1;
        if (fields.size() != 4) continue;
        RetroMediaArtwork item;
        item.key = fields[0];
        item.path = fields[1];
        item.width = decimal(fields[2]);
        item.height = decimal(fields[3]);
        if (!item.key.empty() && !item.path.empty() && item.width > 0 &&
            item.height > 0) {
            artwork.push_back(std::move(item));
        }
    }
    return artwork;
}

std::string AndroidStorage::retro_media_saved_email() {
    return call_static_string("nativeRetroMediaSavedEmail",
                              "()Ljava/lang/String;", nullptr);
}

}  // namespace retro3do

#else  // not Android

namespace retro3do {

bool AndroidStorage::available() { return false; }
void AndroidStorage::pick_folder() {}
std::vector<DocumentEntry> AndroidStorage::granted_roots() { return {}; }
DocumentEntry AndroidStorage::consume_picked_folder() { return {}; }
void AndroidStorage::forget_root(const std::string&) {}
std::vector<DocumentEntry> AndroidStorage::list(const std::string&) { return {}; }
int AndroidStorage::open_document(const std::string&) { return -1; }
void AndroidStorage::pick_gpu_driver_package() {}
GpuDriverImport AndroidStorage::consume_gpu_driver_import() { return {}; }
std::string AndroidStorage::native_library_directory() { return {}; }
void AndroidStorage::begin_retro_media_status() {}
void AndroidStorage::begin_retro_media_login(const std::string&,
                                             const std::string&) {}
void AndroidStorage::begin_retro_media_logout() {}
void AndroidStorage::begin_retro_media_sync(const std::vector<std::string>&,
                                            const std::string&) {}
void AndroidStorage::begin_retro_media_catalogue(const std::string&) {}
void AndroidStorage::begin_retro_media_download(const std::string&,
                                                const std::string&) {}
RetroMediaResult AndroidStorage::consume_retro_media_result() { return {}; }
std::vector<RetroMediaGame> AndroidStorage::retro_media_catalogue() { return {}; }
std::vector<RetroMediaArtwork> AndroidStorage::retro_media_artwork(
    const std::string&) { return {}; }
std::string AndroidStorage::retro_media_saved_email() { return {}; }

}  // namespace retro3do

#endif
