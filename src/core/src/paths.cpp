#include "r3dss/core/paths.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace r3dss {

std::filesystem::path executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return std::filesystem::path(buf).parent_path();
#endif
}

} // namespace r3dss
