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

}  // namespace retro3do

#else  // not Android

namespace retro3do {

bool AndroidStorage::available() { return false; }
void AndroidStorage::pick_folder() {}
std::vector<DocumentEntry> AndroidStorage::granted_roots() { return {}; }
void AndroidStorage::forget_root(const std::string&) {}
std::vector<DocumentEntry> AndroidStorage::list(const std::string&) { return {}; }
int AndroidStorage::open_document(const std::string&) { return -1; }

}  // namespace retro3do

#endif
