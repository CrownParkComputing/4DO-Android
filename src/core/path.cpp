#include "path.h"

#include <cctype>

namespace retro3do {
namespace path {
namespace {

#if defined(_WIN32)
constexpr char kSeparator = '\\';
#else
constexpr char kSeparator = '/';
#endif

bool is_separator(char c) { return c == '/' || c == '\\'; }

}  // namespace

std::string join(const std::string& base, const std::string& leaf) {
    if (base.empty()) return leaf;
    if (leaf.empty()) return base;

    std::string result = base;
    if (!is_separator(result.back())) {
        result += kSeparator;
    }
    result += leaf;
    return result;
}

std::string parent(const std::string& path) {
    if (path.empty()) return {};

    // Ignore trailing separators, so "/a/b/" gives "/a" rather than "/a/b".
    size_t end = path.size();
    while (end > 1 && is_separator(path[end - 1])) {
        --end;
    }
    if (end <= 1) {
        return {};  // already at the root
    }

    const size_t slash = path.find_last_of("/\\", end - 1);
    if (slash == std::string::npos) return {};
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string filename(const std::string& path) {
    size_t end = path.size();
    while (end > 0 && is_separator(path[end - 1])) {
        --end;
    }
    if (end == 0) return {};

    const size_t slash = path.find_last_of("/\\", end - 1);
    const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    return path.substr(start, end - start);
}

std::string extension(const std::string& path) {
    const std::string name = filename(path);
    const size_t dot = name.find_last_of('.');
    // A leading dot is a hidden file, not an extension.
    if (dot == std::string::npos || dot == 0) return {};

    std::string ext = name.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

}  // namespace path
}  // namespace retro3do
