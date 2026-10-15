// Converts one clip: reader -> Q1 -> resample workers -> ordered buffer -> rate control -> writer.
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include "r3dss/core/concurrency.hpp"
#include "r3dss/core/hardware.hpp"
#include "r3dss/pipeline/job.hpp"
#include "r3dss/pipeline/verifier.hpp"

namespace r3dss::pipeline {

enum class ClipStatus { Done, Failed, Skipped, Cancelled };
const char* clipStatusName(ClipStatus s);

struct ClipProgress {
    int64_t framesDone = 0;
    int64_t totalFrames = 0;
    double fps = 0.0; // frames per second so far
    std::string stage; // "probing", "converting", "audio", "verifying"
};

struct ClipResult {
    ClipStatus status = ClipStatus::Failed;
    std::string message;
    std::filesystem::path output;
    std::vector<std::filesystem::path> files;
    int64_t frames = 0;
    uint64_t inputBytes = 0, outputBytes = 0;
    double seconds = 0.0;
    double averageBpp = 0.0;
    double averageBudgetBpp = 0.0; // what the VBR controller asked for
    uint32_t srcWidth = 0, srcHeight = 0, outWidth = 0, outHeight = 0;
    std::string resampler; // "cpu" / "cuda"
    VerificationResult verification;
};

using ProgressFn = std::function<void(const ClipProgress&)>;

struct ClipRunContext {
    std::shared_ptr<CancellationToken> cancel;
    std::shared_ptr<PauseGate> pause;
    ThreadPlan plan;
    ProgressFn progress;
};

ClipResult convertClip(const ClipJob& job, const JobOptions& options, const ClipRunContext& ctx);

// Chooses the resampler: CUDA when built, requested and a device exists; CPU otherwise.
struct ResamplerChoice {
    std::string name;
    bool cuda = false;
};
ResamplerChoice chooseResampler(bool useGpu);

} // namespace r3dss::pipeline
