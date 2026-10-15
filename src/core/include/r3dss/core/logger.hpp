// Small thread-safe logger with stderr, rotating-file and callback sinks.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace r3dss {

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };
const char* logLevelName(LogLevel l);

struct LogRecord {
    LogLevel level;
    std::string timestamp; // ISO-8601 local time
    std::string category;
    std::string message;
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel l);
    LogLevel level() const;
    void enableStderr(bool on);
    // Opens (creating directories) a log file; rotates when it exceeds maxBytes.
    bool openFile(const std::filesystem::path& path, size_t maxBytes = 8u * 1024u * 1024u, int keep = 3);
    void closeFile();
    // Callback sink (e.g. GUI log pane). Called on the logging thread.
    void setCallback(std::function<void(const LogRecord&)> cb);

    void log(LogLevel l, std::string_view category, std::string_view message);
    // Default per-platform log directory (Windows: %LOCALAPPDATA%/R3DSupersampler/logs).
    static std::filesystem::path defaultLogDir();

private:
    Logger();
    ~Logger();
    struct Impl;
    Impl* impl_;
};

#define R3DSS_LOG(level, cat, msg) ::r3dss::Logger::instance().log(level, cat, msg)
#define R3DSS_LOG_TRACE(cat, msg) R3DSS_LOG(::r3dss::LogLevel::Trace, cat, msg)
#define R3DSS_LOG_DEBUG(cat, msg) R3DSS_LOG(::r3dss::LogLevel::Debug, cat, msg)
#define R3DSS_LOG_INFO(cat, msg) R3DSS_LOG(::r3dss::LogLevel::Info, cat, msg)
#define R3DSS_LOG_WARN(cat, msg) R3DSS_LOG(::r3dss::LogLevel::Warn, cat, msg)
#define R3DSS_LOG_ERROR(cat, msg) R3DSS_LOG(::r3dss::LogLevel::Error, cat, msg)

} // namespace r3dss
