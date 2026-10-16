#include <mutex>
#include "r3dss/core/concurrency.hpp"

namespace r3dss {
namespace {
std::mutex g_m;
std::shared_ptr<ThreadPool> g_pool;
size_t g_threads = 0;
} // namespace

// Changing the size replaces the pool; work already running keeps the old pool alive (shared_ptr)
// until it finishes, so a batch can resize it before it starts. Same size: no-op.
void configureSharedThreadPool(size_t threads) {
    std::lock_guard<std::mutex> lk(g_m);
    if (threads == g_threads) return;
    g_threads = threads;
    g_pool.reset();
}

std::shared_ptr<ThreadPool> sharedThreadPool() {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_pool) {
        size_t n = g_threads ? g_threads : std::max(1u, std::thread::hardware_concurrency());
        g_pool = std::make_shared<ThreadPool>(n);
    }
    return g_pool;
}
} // namespace r3dss
