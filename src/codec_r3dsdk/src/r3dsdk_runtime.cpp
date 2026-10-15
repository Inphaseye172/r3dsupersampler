// Built against the RED R3D SDK (R3DSS_HAVE_R3DSDK). Initialises the SDK once and locates the
// runtime libraries: compile-time R3DSS_R3DSDK_LIBDIR, then <exe dir>/redsdk, then $R3DSDK_LIBDIR.
#include <cstdlib>
#include <mutex>
#include "R3DSDK.h"
#include "r3dss/codec_r3dsdk/r3dsdk.hpp"
#include "r3dss/core/logger.hpp"
#include "r3dss/core/paths.hpp"

namespace r3dss::codec::r3dsdk {

namespace {
std::once_flag g_once;
bool g_ok = false;
std::string g_status;
std::string g_dir;

bool hasRuntime(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = toUtf8(e.path().filename());
        if (n.rfind("REDR3D", 0) == 0) return true;
    }
    return false;
}

void doInit() {
    std::vector<std::filesystem::path> candidates;
#ifdef R3DSS_R3DSDK_LIBDIR
    candidates.emplace_back(R3DSS_R3DSDK_LIBDIR);
#endif
    if (auto exe = executableDir(); !exe.empty()) candidates.push_back(exe / "redsdk");
    if (const char* env = std::getenv("R3DSDK_LIBDIR")) candidates.emplace_back(env);
    for (const auto& dir : candidates) {
        if (!hasRuntime(dir)) continue;
        std::string full;
        try {
            // InitializeSdk() takes a narrow string; on Windows that is the ANSI code page.
            full = std::filesystem::absolute(dir).string();
        } catch (const std::exception&) {
            g_status = "RED R3D SDK runtime folder " + toUtf8(dir) +
                       " cannot be expressed in the system code page; install to a path with plain characters";
            R3DSS_LOG_WARN("r3dsdk", g_status);
            continue;
        }
        const R3DSDK::InitializeStatus st = R3DSDK::InitializeSdk(full.c_str(), OPTION_RED_NONE);
        if (st == R3DSDK::ISInitializeOK) {
            g_ok = true;
            g_dir = full;
            g_status = std::string("RED R3D SDK ") + R3DSDK::GetSdkVersion() + " (runtime: " + full + ")";
            R3DSS_LOG_INFO("r3dsdk", g_status);
            return;
        }
        g_status = "RED R3D SDK InitializeSdk failed with status " + std::to_string(static_cast<int>(st)) + " for " + full;
        R3DSS_LOG_WARN("r3dsdk", g_status);
    }
    if (g_status.empty())
        g_status = "RED R3D SDK runtime libraries not found (expected next to the executable in redsdk/ or in $R3DSDK_LIBDIR)";
}
} // namespace

bool builtWithSdk() { return true; }
bool initialize() {
    std::call_once(g_once, doInit);
    return g_ok;
}
std::string statusText() {
    initialize();
    return g_status;
}
std::string runtimeDir() {
    initialize();
    return g_dir;
}
std::unique_ptr<ICodecReader> makeSdkReader() {
    // Milestone 2 provides the decoder; until then the SDK-built module behaves like the
    // container-only reader so the product never claims more than it does.
    return nullptr;
}

} // namespace r3dss::codec::r3dsdk
