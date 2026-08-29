// Path string handling.
//
// Pure string logic, deliberately separate from anything that touches a real
// filesystem, so it can be tested without a platform layer. Navigation depends
// on it: a wrong parent_of means the browser cannot go up, which on a phone
// means the user is stuck.
#pragma once

#include <string>

namespace retro3do {
namespace path {

// Join with the platform separator, tolerating a trailing one on `base`.
std::string join(const std::string& base, const std::string& leaf);

// The containing directory, or empty when there is none. The parent of a
// top-level entry is the root, not an empty string, or navigation dead-ends one
// level too early.
std::string parent(const std::string& path);

// The final component.
std::string filename(const std::string& path);

// The extension including the dot, lowercased. Empty when there is none.
std::string extension(const std::string& path);

}  // namespace path
}  // namespace retro3do
