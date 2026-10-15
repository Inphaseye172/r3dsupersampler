// QString <-> std::filesystem::path without lossy narrow conversions.
// QString::toStdString() is UTF-8, but std::filesystem::path(std::string) on Windows interprets its
// argument in the ANSI code page, and path::string() throws for unrepresentable characters. Every
// GUI path conversion goes through these two functions.
#pragma once
#include <QString>
#include <filesystem>
#include "r3dss/core/paths.hpp"

namespace r3dss::gui {

inline std::filesystem::path toPath(const QString& s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return r3dss::fromUtf8(s.toStdString());
#endif
}

inline QString toQString(const std::filesystem::path& p) { return QString::fromStdString(r3dss::toUtf8(p)); }

} // namespace r3dss::gui
