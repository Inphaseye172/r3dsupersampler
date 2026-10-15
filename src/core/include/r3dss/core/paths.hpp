// Process paths and UTF-8 <-> std::filesystem::path conversion.
//
// Every std::string in this code base that holds a path or a file name is UTF-8. Never use
// path::string() for that: on Windows it converts to the ANSI code page and throws for characters
// the code page cannot represent (e.g. a folder named "Drehtag Ä" under a non-Latin code page).
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace r3dss {
// Directory containing the running executable (empty if it cannot be determined).
std::filesystem::path executableDir();

// UTF-8 text of a path (native separators). Never throws.
std::string toUtf8(const std::filesystem::path& p);
// UTF-8 text with forward slashes (for JSON job files and manifests). Never throws.
std::string toUtf8Generic(const std::filesystem::path& p);
// Path from UTF-8 text.
std::filesystem::path fromUtf8(std::string_view utf8);
} // namespace r3dss
