#include "r3dss/pipeline/scheduler.hpp"
#include "r3dss/core/paths.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include "r3dss/core/logger.hpp"
#include "r3dss/pipeline/output_claim.hpp"

namespace r3dss::pipeline {

namespace fs = std::filesystem;

namespace {

bool isMemoryPath(const fs::path& p) { return toUtf8Generic(p).rfind("mem://", 0) == 0; }

std::string clipLabel(const BatchJob& job, size_t i) {
    return "clip " + std::to_string(i + 1) + " (" + toUtf8(job.clips[i].source) + ")";
}

// Destinations that would collide or overwrite a source, found before anything is written.
struct Collisions {
    std::vector<std::optional<std::string>> clips;
    std::vector<std::optional<std::string>> copies;
};

Collisions findCollisions(const BatchJob& job) {
    Collisions c;
    c.clips.resize(job.clips.size());
    c.copies.resize(job.copies.size());

    // Every source of the batch (clip folders/files and archive-copy sources).
    std::map<std::string, std::string> sources; // key -> label
    for (size_t i = 0; i < job.clips.size(); ++i) {
        const fs::path root = clipSourceRoot(job.clips[i].source);
        if (!root.empty()) sources.emplace(pathKey(root), clipLabel(job, i));
    }
    for (const auto& cp : job.copies) sources.emplace(pathKey(cp.source), "the archived file " + toUtf8(cp.source));

    std::map<std::string, size_t> clipDests; // key of dest and of dest.partial -> clip index
    std::vector<fs::path> plannedDest(job.clips.size());
    for (size_t i = 0; i < job.clips.size(); ++i) {
        auto planned = planClipOutput(job.clips[i], job.options);
        if (!planned) continue; // convertClip reports it
        const fs::path& dest = planned->dest;
        const bool mem = isMemoryPath(dest) || planned->format == codec::Format::Memory;
        const std::string key = mem ? toUtf8Generic(dest) : pathKey(dest);
        if (auto it = clipDests.find(key); it != clipDests.end()) {
            c.clips[i] = "output " + toUtf8(dest) + " is also the output of " + clipLabel(job, it->second) +
                         "; give the clips different output folders or names";
            continue;
        }
        if (!mem) {
            for (const auto& k : pathKeyAndParents(key)) {
                if (auto s = sources.find(k); s != sources.end()) {
                    c.clips[i] = "output " + toUtf8(dest) + " would be written inside " + s->second;
                    break;
                }
            }
            if (c.clips[i]) continue;
        }
        clipDests.emplace(key, i);
        if (!mem) clipDests.emplace(pathKey(partialPathFor(dest)), i);
        plannedDest[i] = dest;
    }
    // An output folder that contains a source would be destroyed by --overwrite.
    for (const auto& [srcKey, label] : sources) {
        for (const auto& k : pathKeyAndParents(srcKey)) {
            auto d = clipDests.find(k);
            if (d == clipDests.end()) continue;
            if (!c.clips[d->second]) c.clips[d->second] = "output " + toUtf8(plannedDest[d->second]) + " would contain " + label;
            break;
        }
    }

    std::map<std::string, size_t> copyDests;
    for (size_t i = 0; i < job.copies.size(); ++i) {
        const auto& cp = job.copies[i];
        const std::string key = pathKey(cp.dest);
        if (auto it = copyDests.find(key); it != copyDests.end()) {
            c.copies[i] = "destination " + toUtf8(cp.dest) + " is also the destination of " +
                          toUtf8(job.copies[it->second].source) + " (not copied)";
            continue;
        }
        copyDests.emplace(key, i);
        for (const auto& k : pathKeyAndParents(key)) {
            if (auto d = clipDests.find(k); d != clipDests.end()) {
                c.copies[i] = "destination " + toUtf8(cp.dest) + " collides with the output of " + clipLabel(job, d->second);
                break;
            }
            if (auto s = sources.find(k); s != sources.end()) {
                c.copies[i] = "destination " + toUtf8(cp.dest) + " would overwrite " + s->second;
                break;
            }
        }
    }
    return c;
}

// Files of an output that already existed (skipped clip), for the manifest of a re-run.
std::vector<fs::path> listOutputFiles(const fs::path& out) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_regular_file(out, ec)) {
        files.push_back(out);
        return files;
    }
    if (!fs::is_directory(out, ec)) return files;
    for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

int batchExitCode(const BatchResult& r) {
    if (r.aborted || r.failed || r.cancelled || !r.copyErrors.empty()) return 1;
    if (r.skipped > r.skippedExisting) return 3;
    return 0;
}

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
    // --threads / the GUI's thread limit also bounds the shared encode pool (LJ92 tiles); 0 = all cores.
    configureSharedThreadPool(job.options.maxThreads ? std::max(1u, plan.encodeWorkers) : 0);
    const unsigned parallel = std::max(1u, std::min<unsigned>(plan.parallelClips, static_cast<unsigned>(std::max<size_t>(1, job.clips.size()))));
    ThreadPlan perClip = plan;
    perClip.resampleWorkers = std::max(1u, plan.resampleWorkers / parallel);
    R3DSS_LOG_INFO("scheduler", "batch '" + job.name + "': " + std::to_string(job.clips.size()) + " clips, " +
                                     std::to_string(job.copies.size()) + " copies, " + std::to_string(parallel) +
                                     " parallel clips, " + std::to_string(perClip.resampleWorkers) +
                                     " resample workers each, resampler=" + rc.name);

    const Collisions collisions = findCollisions(job);
    std::vector<char> finished(job.clips.size(), 0);
    std::atomic<size_t> nextClip{0};
    std::mutex resMutex;
    auto clipWorker = [&] {
        for (;;) {
            const size_t i = nextClip.fetch_add(1);
            if (i >= job.clips.size() || cancel_->isCancelled()) return;
            if (cb.onClipStart) cb.onClipStart(i);
            ClipResult r;
            if (collisions.clips[i]) {
                r.status = ClipStatus::Failed;
                r.message = *collisions.clips[i];
                if (auto p = planClipOutput(job.clips[i], job.options)) r.output = p->dest;
            } else {
                ClipRunContext ctx;
                ctx.cancel = std::make_shared<CancellationToken>();
                // Scoped link: removed when this clip ends, so batch-level callbacks never accumulate.
                CancelRegistration link = cancel_->onCancel([tok = ctx.cancel] { tok->cancel(); });
                ctx.pause = pause_;
                ctx.plan = perClip;
                ctx.progress = [&, i](const ClipProgress& p) { if (cb.onClipProgress) cb.onClipProgress(i, p); };
                r = convertClip(job.clips[i], job.options, ctx);
            }
            R3DSS_LOG_INFO("scheduler", toUtf8(job.clips[i].source) + ": " + clipStatusName(r.status) + " (" + r.message + ")");
            {
                std::lock_guard<std::mutex> lk(resMutex);
                result.clips[i] = r;
                finished[i] = 1;
            }
            if (cb.onClipDone) cb.onClipDone(i, r);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < parallel; ++t) threads.emplace_back(clipWorker);

    // Archive copies on one extra thread.
    std::thread copyThread([&] {
        for (size_t i = 0; i < job.copies.size(); ++i) {
            if (cancel_->isCancelled() || !pause_->wait(cancel_.get())) return;
            Expected<CopyResult> r = collisions.copies[i]
                                         ? Expected<CopyResult>(makeError(ErrorCode::InvalidArgument, *collisions.copies[i]))
                                         : copyAndVerify(job.copies[i].source, job.copies[i].dest, cancel_);
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

    result.aborted = cancel_->isCancelled();
    for (size_t i = 0; i < result.clips.size(); ++i) {
        if (finished[i]) continue;
        ClipResult& r = result.clips[i];
        r.status = ClipStatus::Cancelled;
        r.message = "not started: batch cancelled";
        if (cb.onClipDone) cb.onClipDone(i, r);
    }
    for (const auto& c : result.clips) {
        switch (c.status) {
        case ClipStatus::Done: ++result.done; break;
        case ClipStatus::Failed: ++result.failed; break;
        case ClipStatus::Skipped:
            ++result.skipped;
            if (c.skipReason == SkipReason::OutputExists) ++result.skippedExisting;
            break;
        case ClipStatus::Cancelled: ++result.cancelled; break;
        }
        if (c.status != ClipStatus::Done) continue;
        result.copies.insert(result.copies.end(), c.sidecarCopies.begin(), c.sidecarCopies.end());
        for (const auto& e : c.copyErrors) result.copyErrors.push_back(e);
    }
    if (job.archiveMode && !job.outputRoot.empty()) {
        // Lists only what exists: finished outputs (also those a previous run produced) and verified copies.
        std::vector<fs::path> produced;
        for (const auto& c : result.clips) {
            if (c.status == ClipStatus::Done) produced.insert(produced.end(), c.files.begin(), c.files.end());
            else if (c.status == ClipStatus::Skipped && c.skipReason == SkipReason::OutputExists && !c.output.empty()) {
                const auto files = listOutputFiles(c.output);
                produced.insert(produced.end(), files.begin(), files.end());
            }
        }
        std::error_code ec;
        fs::create_directories(job.outputRoot, ec);
        result.manifest = job.outputRoot / "r3dss-manifest.json";
        if (auto m = writeManifest(result.manifest, result.copies, produced, !result.aborted); !m) {
            R3DSS_LOG_WARN("scheduler", "manifest not written: " + m.error().toString());
            result.manifest.clear();
        }
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

} // namespace r3dss::pipeline
