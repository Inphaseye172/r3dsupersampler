#include "interrupt.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace r3dss::cli {

namespace {
std::atomic<bool> g_interrupted{false};
std::function<void()> g_onInterrupt;

void firstInterrupt() {
    std::fputs("\ninterrupted: cancelling the batch and removing unfinished outputs "
               "(interrupt again to quit at once)\n",
               stderr);
    std::fflush(stderr);
    g_interrupted = true;
    if (g_onInterrupt) g_onInterrupt();
}

[[noreturn]] void secondInterrupt() {
    std::fputs("\ninterrupted again: quitting without cleanup\n", stderr);
    std::fflush(stderr);
    std::_Exit(1);
}

#ifdef _WIN32
std::atomic<int> g_count{0};
std::atomic<bool> g_finished{false};

BOOL WINAPI consoleCtrlHandler(DWORD type) {
    // Runs on a thread the system creates for the event, so calling into the scheduler is safe.
    const int n = ++g_count;
    if (n == 1) firstInterrupt();
    else if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) secondInterrupt();
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Windows ends the process when this returns (after about 5 s at most): give the batch time to
        // remove its unfinished outputs.
        for (int i = 0; i < 45 && !g_finished; ++i) Sleep(100);
    }
    return TRUE;
}
#else
int g_pipe[2] = {-1, -1};
const int kSignals[] = {SIGINT, SIGTERM, SIGHUP};
struct sigaction g_old[3];
std::thread g_watcher;

extern "C" void onSignal(int sig) {
    // Only async-signal-safe work here: wake the watcher thread.
    const unsigned char c = static_cast<unsigned char>(sig > 0 && sig < 256 ? sig : 255);
    const ssize_t r = ::write(g_pipe[1], &c, 1);
    (void)r;
}
#endif
} // namespace

InterruptGuard::InterruptGuard(std::function<void()> onInterrupt) {
    g_onInterrupt = std::move(onInterrupt);
    g_interrupted = false;
#ifdef _WIN32
    g_count = 0;
    g_finished = false;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    if (::pipe(g_pipe) != 0) {
        g_pipe[0] = g_pipe[1] = -1;
        return; // keep the default behaviour
    }
    for (int fd : g_pipe) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    g_watcher = std::thread([] {
        int count = 0;
        for (;;) {
            unsigned char c = 0;
            const ssize_t n = ::read(g_pipe[0], &c, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || c == 0) return; // guard destroyed
            if (++count == 1) firstInterrupt();
            else secondInterrupt();
        }
    });
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (size_t i = 0; i < 3; ++i) ::sigaction(kSignals[i], &sa, &g_old[i]);
#endif
}

InterruptGuard::~InterruptGuard() {
#ifdef _WIN32
    g_finished = true;
    SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
#else
    if (g_pipe[0] >= 0) {
        for (size_t i = 0; i < 3; ++i) ::sigaction(kSignals[i], &g_old[i], nullptr);
        const unsigned char stop = 0;
        const ssize_t r = ::write(g_pipe[1], &stop, 1);
        (void)r;
        if (g_watcher.joinable()) g_watcher.join();
        ::close(g_pipe[0]);
        ::close(g_pipe[1]);
        g_pipe[0] = g_pipe[1] = -1;
    }
#endif
    g_onInterrupt = nullptr;
}

bool InterruptGuard::interrupted() const { return g_interrupted; }

} // namespace r3dss::cli
