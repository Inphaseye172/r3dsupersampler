#include "r3dss/pipeline/output_claim.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include "r3dss/core/logger.hpp"
#include "r3dss/core/paths.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace r3dss::pipeline {

namespace fs = std::filesystem;

namespace {

std::atomic<unsigned> g_uniq{0};

std::string uniqueSuffix() {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const long pid = static_cast<long>(getpid());
#endif
    return std::to_string(pid) + "-" + std::to_string(g_uniq.fetch_add(1));
}

std::string currentHost() {
#ifdef _WIN32
    const char* h = std::getenv("COMPUTERNAME");
    return h ? std::string(h) : std::string();
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) != 0) return {};
    return buf;
#endif
}

long long currentPid() {
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

std::string readSmallFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s;
    s.resize(4096);
    in.read(s.data(), static_cast<std::streamsize>(s.size()));
    s.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
    return s;
}

// Identifies one process instance across PID reuse. nullopt: no such process. "": exists, no token.
std::optional<std::string> processStartToken(long long pid) {
#if defined(__linux__)
    const std::string stat = readSmallFile("/proc/" + std::to_string(pid) + "/stat");
    if (stat.empty()) return std::nullopt;
    const size_t rp = stat.rfind(')');
    if (rp == std::string::npos) return std::string();
    std::istringstream is(stat.substr(rp + 1));
    std::string field;
    // Fields after ')' start at field 3 (state); starttime is field 22.
    for (int i = 3; i <= 22 && (is >> field); ++i) {}
    std::string boot = readSmallFile("/proc/sys/kernel/random/boot_id");
    while (!boot.empty() && (boot.back() == '\n' || boot.back() == '\r')) boot.pop_back();
    return boot + ":" + field;
#elif defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER ? std::nullopt : std::optional<std::string>(std::string());
    DWORD code = 0;
    if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
        CloseHandle(h);
        return std::nullopt;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    std::string tok;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user))
        tok = std::to_string((static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    CloseHandle(h);
    return tok;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) return std::string();
    return std::nullopt;
#endif
}

std::string ownerText() {
    const auto tok = processStartToken(currentPid());
    return "r3dss-partial-owner 1\nhost " + currentHost() + "\npid " + std::to_string(currentPid()) + "\nstart " +
           (tok ? *tok : std::string()) + "\n";
}

struct OwnerInfo {
    std::string host, start;
    long long pid = 0;
};

std::optional<OwnerInfo> parseOwner(const std::string& text) {
    std::istringstream is(text);
    std::string line;
    if (!std::getline(is, line) || line.rfind("r3dss-partial-owner", 0) != 0) return std::nullopt;
    OwnerInfo o;
    bool havePid = false;
    while (std::getline(is, line)) {
        if (line.rfind("host ", 0) == 0) o.host = line.substr(5);
        else if (line.rfind("start ", 0) == 0) o.start = line.substr(6);
        else if (line.rfind("pid ", 0) == 0) {
            char* end = nullptr;
            o.pid = std::strtoll(line.c_str() + 4, &end, 10);
            havePid = end && *end == '\0' && o.pid > 0;
        }
    }
    if (!havePid) return std::nullopt;
    return o;
}

// False only when the owner provably no longer runs (same machine, process gone or PID reused).
bool ownerAlive(const OwnerInfo& o) {
    if (o.host.empty() || o.host != currentHost()) return true; // another machine: cannot tell
    const auto tok = processStartToken(o.pid);
    if (!tok) return false;
    if (!o.start.empty() && !tok->empty() && *tok != o.start) return false;
    return true;
}

bool createFileExclusive(const fs::path& p) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"wbx");
#else
    FILE* f = std::fopen(p.c_str(), "wbx");
#endif
    if (!f) return false;
    std::fclose(f);
    return true;
}

#if defined(_WIN32) || defined(__APPLE__)
std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
#endif

} // namespace

fs::path partialPathFor(const fs::path& dest) {
    fs::path p = dest;
    p += ".partial";
    return p;
}

std::string pathKey(const fs::path& p) {
    if (p.empty()) return {};
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    fs::path c = fs::weakly_canonical(a, ec);
    if (ec) c = a;
    std::string s = toUtf8Generic(c.lexically_normal());
    // Drop a trailing separator unless the path is a root ("/" or "C:/").
    while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();
#if defined(_WIN32) || defined(__APPLE__)
    s = lowerAscii(s);
#endif
    return s;
}

bool pathKeysOverlap(const std::string& ka, const std::string& kb) {
    if (ka.empty() || kb.empty()) return false;
    if (ka == kb) return true;
    auto inside = [](const std::string& outer, const std::string& inner) {
        if (inner.size() <= outer.size() || inner.compare(0, outer.size(), outer) != 0) return false;
        return outer.back() == '/' || inner[outer.size()] == '/';
    };
    return inside(ka, kb) || inside(kb, ka);
}

bool pathsOverlap(const fs::path& a, const fs::path& b) { return pathKeysOverlap(pathKey(a), pathKey(b)); }

std::vector<std::string> pathKeyAndParents(const std::string& key) {
    std::vector<std::string> out;
    std::string k = key;
    while (!k.empty()) {
        out.push_back(k);
        const size_t slash = k.rfind('/');
        if (slash == std::string::npos) break;
        std::string parent = k.substr(0, slash);
        // Keep the root's separator: "/a" -> "/", "c:/a" -> "c:/".
        if (parent.empty() || (parent.size() == 2 && parent[1] == ':')) parent += '/';
        if (parent == k) break;
        k = parent;
    }
    return out;
}

void flushAndDropCache(const fs::path& p, bool flush) {
#ifdef _WIN32
    if (!flush) return;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(h);
    CloseHandle(h);
#else
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (flush) (void)::fsync(fd);
#if defined(POSIX_FADV_DONTNEED)
    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    ::close(fd);
#endif
}

Expected<void> renameNoReplace(const fs::path& from, const fs::path& to) {
    const std::string what = toUtf8(from) + " -> " + toUtf8(to);
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), 0)) return {};
    const DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)
        return makeError(ErrorCode::Io, "not renamed, destination exists: " + what);
    return makeError(ErrorCode::Io, "rename failed (Windows error " + std::to_string(e) + "): " + what);
#else
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return {};
    if (errno == EEXIST || errno == ENOTEMPTY) return makeError(ErrorCode::Io, "not renamed, destination exists: " + what);
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP && errno != EOPNOTSUPP)
        return makeError(ErrorCode::Io, std::string("rename failed (") + std::strerror(errno) + "): " + what);
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return {};
    if (errno == EEXIST) return makeError(ErrorCode::Io, "not renamed, destination exists: " + what);
    if (errno != ENOTSUP && errno != EINVAL)
        return makeError(ErrorCode::Io, std::string("rename failed (") + std::strerror(errno) + "): " + what);
#endif
    // File system without an atomic no-replace rename: check, then rename (never replaces what is there).
    std::error_code ec;
    if (fs::exists(fs::symlink_status(to, ec))) return makeError(ErrorCode::Io, "not renamed, destination exists: " + what);
    fs::rename(from, to, ec);
    if (ec) return makeError(ErrorCode::Io, "rename failed (" + ec.message() + "): " + what);
    return {};
#endif
}

const char* OutputClaim::ownerFileName() { return ".r3dss-owner"; }

OutputClaim::OutputClaim(OutputClaim&& o) noexcept
    : dest_(std::move(o.dest_)), partial_(std::move(o.partial_)), directory_(o.directory_), active_(o.active_) {
    o.active_ = false;
}

OutputClaim& OutputClaim::operator=(OutputClaim&& o) noexcept {
    if (this != &o) {
        discard();
        dest_ = std::move(o.dest_);
        partial_ = std::move(o.partial_);
        directory_ = o.directory_;
        active_ = o.active_;
        o.active_ = false;
    }
    return *this;
}

OutputClaim::~OutputClaim() { discard(); }

Expected<OutputClaim> OutputClaim::claim(const fs::path& dest, bool directory) {
    OutputClaim c;
    c.dest_ = dest;
    c.partial_ = partialPathFor(dest);
    c.directory_ = directory;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::error_code ec;
        bool created = false;
        if (directory) {
            created = fs::create_directory(c.partial_, ec); // non-recursive: fails if it exists
            std::error_code ec2;
            if (!created && ec && !fs::exists(fs::symlink_status(c.partial_, ec2)))
                return makeError(ErrorCode::Io, "cannot create " + toUtf8(c.partial_) + ": " + ec.message());
        } else {
            created = createFileExclusive(c.partial_);
            if (!created && !fs::exists(fs::symlink_status(c.partial_, ec)))
                return makeError(ErrorCode::Io, "cannot create " + toUtf8(c.partial_));
        }
        if (created) {
            c.active_ = true;
            if (directory) {
                std::ofstream o(c.partial_ / ownerFileName(), std::ios::binary | std::ios::trunc);
                o << ownerText();
                o.close();
                if (!o) return makeError(ErrorCode::Io, "cannot write " + toUtf8(c.partial_ / ownerFileName()));
            }
            return c;
        }
        // Exists. Reclaim it only when it provably belongs to a process that has ended on this machine.
        if (attempt > 0 || !directory) break;
        const std::string text = readSmallFile(c.partial_ / ownerFileName());
        const auto owner = parseOwner(text);
        if (!owner || ownerAlive(*owner)) break;
        fs::path aside = c.partial_;
        aside += ".stale-" + uniqueSuffix();
        if (!renameNoReplace(c.partial_, aside)) break;
        if (readSmallFile(aside / ownerFileName()) != text) {
            // Someone reclaimed it between our read and our rename: give it back untouched.
            (void)renameNoReplace(aside, c.partial_);
            break;
        }
        fs::remove_all(aside, ec);
        R3DSS_LOG_WARN("output", "removed the unfinished output of an interrupted run (process " + std::to_string(owner->pid) +
                                     " no longer runs): " + toUtf8(c.partial_));
    }
    return makeError(ErrorCode::Io, "destination in use: " + toUtf8(c.partial_) +
                                        " exists. Another conversion is writing this output; if none is running, delete "
                                        "that folder and run again");
}

Expected<void> OutputClaim::commit(bool replaceExisting, const std::vector<fs::path>& protect) {
    if (!active_) return makeError(ErrorCode::Internal, "output claim is not active");
    std::error_code ec;
    if (directory_) {
        fs::remove(partial_ / ownerFileName(), ec);
        if (ec) return makeError(ErrorCode::Io, "cannot finish " + toUtf8(partial_) + ": " + ec.message());
    }
    const bool exists = fs::exists(fs::symlink_status(dest_, ec));
    if (!exists) {
        R3DSS_TRY(renameNoReplace(partial_, dest_));
        active_ = false;
        return {};
    }
    if (!replaceExisting) return makeError(ErrorCode::Io, "output appeared while converting, not replaced: " + toUtf8(dest_));
    for (const auto& p : protect)
        if (!p.empty() && pathsOverlap(dest_, p))
            return makeError(ErrorCode::InvalidArgument, "refusing to replace " + toUtf8(dest_) + ": it overlaps the source " + toUtf8(p));
    fs::path aside = dest_;
    aside += ".replaced-" + uniqueSuffix();
    if (auto r = renameNoReplace(dest_, aside); !r)
        return makeError(ErrorCode::Io, "cannot move the previous output aside: " + r.error().message);
    if (auto r = renameNoReplace(partial_, dest_); !r) {
        (void)renameNoReplace(aside, dest_); // put the previous output back
        return r;
    }
    active_ = false;
    fs::remove_all(aside, ec);
    if (ec) R3DSS_LOG_WARN("output", "could not delete the replaced output " + toUtf8(aside) + ": " + ec.message());
    return {};
}

void OutputClaim::discard() {
    if (!active_) return;
    active_ = false;
    std::error_code ec;
    if (directory_) fs::remove_all(partial_, ec);
    else fs::remove(partial_, ec);
    if (ec) R3DSS_LOG_WARN("output", "could not remove the unfinished output " + toUtf8(partial_) + ": " + ec.message());
}

} // namespace r3dss::pipeline
