// Ctrl-C / SIGTERM handling for a running batch: the first interrupt cancels it cleanly (unfinished
// outputs are removed), a second one quits at once.
#pragma once
#include <functional>

namespace r3dss::cli {

// While alive, SIGINT, SIGTERM and SIGHUP (POSIX) or Ctrl-C, Ctrl-Break and closing the console
// (Windows, SetConsoleCtrlHandler) call `onInterrupt` once, from a helper thread (never from a signal
// handler). Only one guard may exist at a time.
class InterruptGuard {
public:
    explicit InterruptGuard(std::function<void()> onInterrupt);
    ~InterruptGuard();
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
    bool interrupted() const;
};

} // namespace r3dss::cli
