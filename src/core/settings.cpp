#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace retro3do {
namespace {

std::string trimmed(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) {
        ++first;
    }
    size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' ||
                            text[last - 1] == '\r' || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

}  // namespace

bool Settings::load(const std::string& path) {
    path_ = path;
    values_.clear();

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        // No file yet is the normal first-run case, not a failure.
        return true;
    }

    char line[2048];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const std::string text = trimmed(line);
        if (text.empty() || text[0] == '#') {
            continue;
        }
        const size_t equals = text.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        // Only the first '=' separates: a value may legitimately contain one,
        // and a SAF content URI very often does.
        const std::string key = trimmed(text.substr(0, equals));
        const std::string value = trimmed(text.substr(equals + 1));
        if (!key.empty()) {
            values_[key] = value;
        }
    }
    std::fclose(file);
    return true;
}

bool Settings::save() const {
    if (path_.empty()) {
        return false;
    }

    std::FILE* file = std::fopen(path_.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    std::fprintf(file, "# Retro-3DO settings\n");
    for (const auto& entry : values_) {
        // Values are written raw and read back trimmed. A value containing a
        // newline would corrupt the file, so it is skipped rather than written:
        // losing one setting beats losing the whole file.
        if (entry.second.find('\n') != std::string::npos) {
            continue;
        }
        std::fprintf(file, "%s=%s\n", entry.first.c_str(), entry.second.c_str());
    }

    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool Settings::save_as(const std::string& path) {
    path_ = path;
    return save();
}

std::string Settings::get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Settings::get_int(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    // A value that is not entirely a number is treated as absent rather than as
    // zero, so a corrupted line does not quietly become a valid-looking setting.
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(value);
}

bool Settings::get_bool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    const std::string& value = it->second;
    if (value == "1" || value == "true" || value == "yes") return true;
    if (value == "0" || value == "false" || value == "no") return false;
    return fallback;
}

void Settings::set(const std::string& key, const std::string& value) {
    values_[key] = value;
}

void Settings::set_int(const std::string& key, int value) {
    values_[key] = std::to_string(value);
}

void Settings::set_bool(const std::string& key, bool value) {
    values_[key] = value ? "1" : "0";
}

void Settings::remove(const std::string& key) {
    values_.erase(key);
}

bool Settings::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

}  // namespace retro3do
