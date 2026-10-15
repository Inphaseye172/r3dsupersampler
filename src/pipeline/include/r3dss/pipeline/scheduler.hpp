// Runs a BatchJob: parallel clip conversions, archive copies, progress and cancellation.
#pragma once
#include <functional>
#include <memory>
#include "r3dss/pipeline/archive_copier.hpp"
#include "r3dss/pipeline/clip_converter.hpp"
#include "r3dss/pipeline/job.hpp"

namespace r3dss::pipeline {

struct BatchCallbacks {
    std::function<void(size_t clipIndex)> onClipStart;
    std::function<void(size_t clipIndex, const ClipProgress&)> onClipProgress;
    // Called once for every clip, also for clips that never started because the batch was cancelled.
    std::function<void(size_t clipIndex, const ClipResult&)> onClipDone;
    std::function<void(size_t copyIndex, const Expected<CopyResult>&)> onCopyDone;
};

struct BatchResult {
    std::vector<ClipResult> clips;
    std::vector<CopyResult> copies;
    std::vector<std::string> copyErrors;
    size_t done = 0, failed = 0, skipped = 0, cancelled = 0;
    size_t skippedExisting = 0; // part of `skipped`: a complete output already existed
    bool aborted = false;       // the batch was cancelled
    double seconds = 0.0;
    std::filesystem::path manifest;
};

// Process exit code for a finished batch (shared by r3dss-cli and r3dss-gui --run-job):
// 0 = every clip done or skipped because its complete output already exists (and every copy verified);
// 1 = a clip failed or was cancelled, a copy failed, or the batch was cancelled;
// 3 = no failure, but a clip was skipped for another reason (e.g. a size larger than the source).
int batchExitCode(const BatchResult& r);

class Scheduler {
public:
    Scheduler();
    // Blocking. Use from a worker thread in the GUI. Before any clip starts, every clip's destination
    // is worked out: a clip whose output would collide with an earlier clip's (or overwrite a source)
    // fails with a message saying so, and copies whose destinations collide fail the same way.
    BatchResult run(const BatchJob& job, const BatchCallbacks& cb);
    void cancel();
    void pause();
    void resume();
    bool paused() const { return pause_->paused(); }
    std::shared_ptr<CancellationToken> token() const { return cancel_; }

private:
    std::shared_ptr<CancellationToken> cancel_;
    std::shared_ptr<PauseGate> pause_;
};

} // namespace r3dss::pipeline
