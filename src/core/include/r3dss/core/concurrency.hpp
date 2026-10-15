// Thread pool, bounded queue with backpressure, and a cooperative cancellation token.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <functional>
#include <map>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace r3dss {

class CancelRegistration;

// Cooperative cancellation. cancel() is idempotent and runs every registered callback exactly once,
// under the token's lock (callbacks must not call back into the same token). Callbacks are
// registered through onCancel(), which returns an RAII CancelRegistration: destroying it removes the
// callback, so objects that register `this` can never be called after they are destroyed.
class CancellationToken {
public:
    using CallbackId = uint64_t;

    void cancel() {
        std::lock_guard<std::mutex> lk(m_);
        if (cancelled_.exchange(true, std::memory_order_acq_rel)) return;
        for (auto& [id, cb] : callbacks_) cb();
        callbacks_.clear();
    }
    bool isCancelled() const { return cancelled_.load(std::memory_order_acquire); }

    // Runs `cb` once on cancel (immediately, synchronously, if already cancelled). The returned
    // registration must not outlive this token.
    [[nodiscard]] CancelRegistration onCancel(std::function<void()> cb);

    size_t callbackCount() const { std::lock_guard<std::mutex> lk(m_); return callbacks_.size(); }

private:
    friend class CancelRegistration;
    void remove(CallbackId id) {
        std::lock_guard<std::mutex> lk(m_);
        callbacks_.erase(id);
    }
    std::atomic<bool> cancelled_{false};
    mutable std::mutex m_;
    CallbackId nextId_ = 0;
    std::map<CallbackId, std::function<void()>> callbacks_;
};

class CancelRegistration {
public:
    CancelRegistration() = default;
    CancelRegistration(CancellationToken* token, CancellationToken::CallbackId id) : token_(token), id_(id) {}
    CancelRegistration(const CancelRegistration&) = delete;
    CancelRegistration& operator=(const CancelRegistration&) = delete;
    CancelRegistration(CancelRegistration&& o) noexcept : token_(o.token_), id_(o.id_) { o.token_ = nullptr; }
    CancelRegistration& operator=(CancelRegistration&& o) noexcept {
        if (this != &o) {
            reset();
            token_ = o.token_;
            id_ = o.id_;
            o.token_ = nullptr;
        }
        return *this;
    }
    ~CancelRegistration() { reset(); }
    void reset() {
        if (token_) token_->remove(id_);
        token_ = nullptr;
    }

private:
    CancellationToken* token_ = nullptr;
    CancellationToken::CallbackId id_ = 0;
};

inline CancelRegistration CancellationToken::onCancel(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(m_);
    if (cancelled_.load(std::memory_order_acquire)) {
        cb();
        return {};
    }
    const CallbackId id = ++nextId_;
    callbacks_.emplace(id, std::move(cb));
    return CancelRegistration(this, id);
}

// Bounded multi-producer / multi-consumer queue. push() blocks when full (backpressure);
// pop() blocks when empty. Both return false on cancellation or after close() drains.
template <class T> class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity, std::shared_ptr<CancellationToken> token = nullptr)
        : cap_(capacity ? capacity : 1), token_(std::move(token)) {
        if (token_) reg_ = token_->onCancel([this] { notifyAll(); });
    }
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(T item) {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [&] { return q_.size() < cap_ || closed_ || cancelled(); });
        if (closed_ || cancelled()) return false;
        q_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [&] { return !q_.empty() || closed_ || cancelled(); });
        if (cancelled()) return std::nullopt;
        if (q_.empty()) return std::nullopt; // closed and drained
        T item = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return item;
    }

    // No more items will be pushed; consumers drain then receive nullopt.
    void close() {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        notifyAllLocked();
    }
    bool closed() const { std::lock_guard<std::mutex> lk(m_); return closed_; }
    size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    size_t capacity() const { return cap_; }

private:
    bool cancelled() const { return token_ && token_->isCancelled(); }
    void notifyAll() { std::lock_guard<std::mutex> lk(m_); notifyAllLocked(); }
    void notifyAllLocked() { notEmpty_.notify_all(); notFull_.notify_all(); }

    mutable std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<T> q_;
    size_t cap_;
    bool closed_ = false;
    std::shared_ptr<CancellationToken> token_;
    CancelRegistration reg_; // declared after token_: unregistered before the token reference is dropped
};

class ThreadPool {
public:
    explicit ThreadPool(size_t threads) {
        if (threads == 0) threads = 1;
        for (size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { run(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    size_t size() const { return workers_.size(); }

    template <class F> auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(m_);
            tasks_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    // Runs fn(i) for i in [0, n) across the pool and waits for completion.
    void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
        if (n == 0) return;
        if (n == 1 || workers_.size() == 1) {
            for (size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        std::vector<std::future<void>> futs;
        futs.reserve(n);
        for (size_t i = 0; i < n; ++i) futs.push_back(submit([&fn, i] { fn(i); }));
        for (auto& f : futs) f.get();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace r3dss

namespace r3dss {
// Process-wide pool sized to the hardware (lazily created). Used for intra-frame parallelism
// (tile encoding, complexity estimation) by codecs that have no pool of their own.
std::shared_ptr<ThreadPool> sharedThreadPool();
void configureSharedThreadPool(size_t threads); // call before first use
} // namespace r3dss

namespace r3dss {
// Cooperative pause: workers call wait() between units of work.
class PauseGate {
public:
    void pause() { std::lock_guard<std::mutex> lk(m_); paused_ = true; }
    void resume() { { std::lock_guard<std::mutex> lk(m_); paused_ = false; } cv_.notify_all(); }
    bool paused() const { std::lock_guard<std::mutex> lk(m_); return paused_; }
    // Blocks while paused; returns false if `token` is (or becomes) cancelled. Cancelling the token
    // wakes the waiter even if nobody calls resume().
    bool wait(CancellationToken* token = nullptr) {
        CancelRegistration reg;
        if (token) reg = token->onCancel([this] {
            std::lock_guard<std::mutex> lk(m_);
            cv_.notify_all();
        });
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !paused_ || (token && token->isCancelled()); });
        return !(token && token->isCancelled());
    }
    void wakeForCancel() { std::lock_guard<std::mutex> lk(m_); cv_.notify_all(); }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool paused_ = false;
};
} // namespace r3dss
