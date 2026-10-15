#include "r3dss/core/logger.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

namespace r3dss {

const char* logLevelName(LogLevel l) {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off: return "OFF";
    }
    return "?";
}

struct Logger::Impl {
    std::mutex m;
    LogLevel level = LogLevel::Info;
    bool toStderr = true;
    std::ofstream file;
    std::filesystem::path filePath;
    size_t maxBytes = 0;
    int keep = 3;
    size_t written = 0;
    std::function<void(const LogRecord&)> callback;

    void rotate() {
        file.close();
        std::error_code ec;
        for (int i = keep - 1; i >= 1; --i) {
            auto from = filePath;
            from += "." + std::to_string(i);
            auto to = filePath;
            to += "." + std::to_string(i + 1);
            if (std::filesystem::exists(from, ec)) std::filesystem::rename(from, to, ec);
        }
        auto first = filePath;
        first += ".1";
        std::filesystem::rename(filePath, first, ec);
        file.open(filePath, std::ios::out | std::ios::trunc);
        written = 0;
    }
};

Logger::Logger() : impl_(new Impl) {}
Logger::~Logger() { delete impl_; }

Logger& Logger::instance() {
    static Logger l;
    return l;
}

void Logger::setLevel(LogLevel l) { std::lock_guard<std::mutex> lk(impl_->m); impl_->level = l; }
LogLevel Logger::level() const { std::lock_guard<std::mutex> lk(impl_->m); return impl_->level; }
void Logger::enableStderr(bool on) { std::lock_guard<std::mutex> lk(impl_->m); impl_->toStderr = on; }

bool Logger::openFile(const std::filesystem::path& path, size_t maxBytes, int keep) {
    std::lock_guard<std::mutex> lk(impl_->m);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    impl_->file.close();
    impl_->file.open(path, std::ios::out | std::ios::app);
    if (!impl_->file) return false;
    impl_->filePath = path;
    impl_->maxBytes = maxBytes;
    impl_->keep = keep;
    impl_->written = std::filesystem::exists(path, ec) ? static_cast<size_t>(std::filesystem::file_size(path, ec)) : 0;
    return true;
}

void Logger::closeFile() { std::lock_guard<std::mutex> lk(impl_->m); impl_->file.close(); }

void Logger::setCallback(std::function<void(const LogRecord&)> cb) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->callback = std::move(cb);
}

void Logger::log(LogLevel l, std::string_view category, std::string_view message) {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (l < impl_->level) return;
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[40];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    LogRecord rec{l, ts, std::string(category), std::string(message)};
    const std::string line = rec.timestamp + " [" + logLevelName(l) + "] " + rec.category + ": " + rec.message + "\n";
    if (impl_->toStderr) {
        std::cerr << line;
        std::cerr.flush();
    }
    if (impl_->file.is_open()) {
        impl_->file << line;
        impl_->file.flush();
        impl_->written += line.size();
        if (impl_->maxBytes && impl_->written > impl_->maxBytes) impl_->rotate();
    }
    if (impl_->callback) impl_->callback(rec);
}

std::filesystem::path Logger::defaultLogDir() {
#ifdef _WIN32
    if (const char* la = std::getenv("LOCALAPPDATA")) return std::filesystem::path(la) / "r3dss" / "logs";
    return std::filesystem::temp_directory_path() / "r3dss" / "logs";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME")) return std::filesystem::path(x) / "r3dss" / "logs";
    if (const char* h = std::getenv("HOME")) return std::filesystem::path(h) / ".local" / "share" / "r3dss" / "logs";
    return std::filesystem::temp_directory_path() / "r3dss" / "logs";
#endif
}

} // namespace r3dss
