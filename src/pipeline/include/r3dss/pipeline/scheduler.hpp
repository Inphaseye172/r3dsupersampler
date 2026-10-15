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
    std::function<void(size_t clipIndex, const ClipResult&)> onClipDone;
    std::function<void(size_t copyIndex, const Expected<CopyResult>&)> onCopyDone;
};

struct BatchResult {
    std::vector<ClipResult> clips;
    std::vector<CopyResult> copies;
    std::vector<std::string> copyErrors;
    size_t done = 0, failed = 0, skipped = 0, cancelled = 0;
    double seconds = 0.0;
    std::filesystem::path manifest;
};

class Scheduler {
public:
    Scheduler();
    // Blocking. Use from a worker thread in the GUI.
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
