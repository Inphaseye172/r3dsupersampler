#include "r3dss/pipeline/scheduler.hpp"
#include "r3dss/core/paths.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "r3dss/core/logger.hpp"

namespace r3dss::pipeline {

Scheduler::Scheduler() : cancel_(std::make_shared<CancellationToken>()), pause_(std::make_shared<PauseGate>()) {}

void Scheduler::cancel() {
    cancel_->cancel();
    pause_->wakeForCancel();
}
void Scheduler::pause() { pause_->pause(); }
void Scheduler::resume() { pause_->resume(); }

BatchResult Scheduler::run(const BatchJob& job, const BatchCallbacks& cb) {
    const auto t0 = std::chrono::steady_clock::now();
    BatchResult result;
    result.clips.resize(job.clips.size());
    HardwareInfo hw = probeHardware();
    const ResamplerChoice rc = chooseResampler(job.options.useGpu);
    hw.cudaAvailable = rc.cuda;
    // Conservative frame-size estimate (8.3K 16-bit) until a clip is probed.
    const ThreadPlan plan = planThreads(hw, 8256ull * 4644ull * 2ull, job.options.maxParallelClips, job.options.maxThreads);
    const unsigned parallel = std::max(1u, std::min<unsigned>(plan.parallelClips, static_cast<unsigned>(std::max<size_t>(1, job.clips.size()))));
    ThreadPlan perClip = plan;
    perClip.resampleWorkers = std::max(1u, plan.resampleWorkers / parallel);
    R3DSS_LOG_INFO("scheduler", "batch '" + job.name + "': " + std::to_string(job.clips.size()) + " clips, " +
                                     std::to_string(job.copies.size()) + " copies, " + std::to_string(parallel) +
                                     " parallel clips, " + std::to_string(perClip.resampleWorkers) +
                                     " resample workers each, resampler=" + rc.name);

    std::atomic<size_t> nextClip{0};
    std::mutex resMutex;
    auto clipWorker = [&] {
        for (;;) {
            const size_t i = nextClip.fetch_add(1);
            if (i >= job.clips.size() || cancel_->isCancelled()) return;
            if (cb.onClipStart) cb.onClipStart(i);
            ClipRunContext ctx;
            ctx.cancel = std::make_shared<CancellationToken>();
            // Scoped link: removed when this clip ends, so batch-level callbacks never accumulate.
            CancelRegistration link = cancel_->onCancel([tok = ctx.cancel] { tok->cancel(); });
            ctx.pause = pause_;
            ctx.plan = perClip;
            ctx.progress = [&, i](const ClipProgress& p) { if (cb.onClipProgress) cb.onClipProgress(i, p); };
            ClipResult r = convertClip(job.clips[i], job.options, ctx);
            R3DSS_LOG_INFO("scheduler", toUtf8(job.clips[i].source.filename()) + ": " + clipStatusName(r.status) + " (" + r.message + ")");
            {
                std::lock_guard<std::mutex> lk(resMutex);
                result.clips[i] = r;
            }
            if (cb.onClipDone) cb.onClipDone(i, r);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < parallel; ++t) threads.emplace_back(clipWorker);

    // Archive copies on one extra thread.
    std::thread copyThread([&] {
        for (size_t i = 0; i < job.copies.size(); ++i) {
            if (cancel_->isCancelled()) return;
            pause_->wait(cancel_.get());
            auto r = copyAndVerify(job.copies[i].source, job.copies[i].dest, cancel_);
            {
                std::lock_guard<std::mutex> lk(resMutex);
                if (r) result.copies.push_back(*r);
                else result.copyErrors.push_back(toUtf8(job.copies[i].source) + ": " + r.error().toString());
            }
            if (cb.onCopyDone) cb.onCopyDone(i, r);
        }
    });
    for (auto& t : threads) t.join();
    copyThread.join();

    for (const auto& c : result.clips) {
        switch (c.status) {
        case ClipStatus::Done: ++result.done; break;
        case ClipStatus::Failed: ++result.failed; break;
        case ClipStatus::Skipped: ++result.skipped; break;
        case ClipStatus::Cancelled: ++result.cancelled; break;
        }
    }
    if (job.archiveMode && !job.outputRoot.empty() && !cancel_->isCancelled()) {
        std::vector<std::filesystem::path> produced;
        for (const auto& c : result.clips) produced.insert(produced.end(), c.files.begin(), c.files.end());
        result.manifest = job.outputRoot / "r3dss-manifest.json";
        if (auto m = writeManifest(result.manifest, result.copies, produced); !m)
            R3DSS_LOG_WARN("scheduler", "manifest not written: " + m.error().toString());
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

} // namespace r3dss::pipeline
