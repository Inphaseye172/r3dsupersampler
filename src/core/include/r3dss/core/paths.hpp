// Process paths.
#pragma once
#include <filesystem>

namespace r3dss {
// Directory containing the running executable (empty if it cannot be determined).
std::filesystem::path executableDir();
} // namespace r3dss
