// Converts one clip: reader -> Q1 -> resample workers -> ordered buffer -> rate control -> writer.
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include "r3dss/core/concurrency.hpp"
#include "r3dss/core/hardware.hpp"
#include "r3dss/pipeline/archive_copier.hpp"
#include "r3dss/pipeline/job.hpp"
#include "r3dss/pipeline/verifier.hpp"

namespace r3dss::pipeline {

enum class ClipStatus { Done, Failed, Skipped, Cancelled };
const char* clipStatusName(ClipStatus s);

// Why a clip was skipped. Only OutputExists counts as success for the batch exit code.
enum class SkipReason { None, OutputExists, Other };

struct ClipProgress {
    int64_t framesDone = 0;
    int64_t totalFrames = 0;
    double fps = 0.0; // frames per second so far
    std::string stage; // "probing", "converting", "audio", "verifying", "copying"
};

struct ClipResult {
    ClipStatus status = ClipStatus::Failed;
    SkipReason skipReason = SkipReason::None;
    std::string message;
    std::vector<std::string> warnings; // the clip converted, but something deserves attention
    std::filesystem::path output;
    std::vector<std::filesystem::path> files; // files of the finished output; empty unless Done
    int64_t frames = 0;
    uint64_t inputBytes = 0, outputBytes = 0;
    double seconds = 0.0;
    double averageBpp = 0.0;
    double averageBudgetBpp = 0.0; // what the VBR controller asked for (only when rateControl)
    bool rateControl = false;      // the output codec honours a bit budget (CinemaDNG: no, always lossless)
    uint32_t srcWidth = 0, srcHeight = 0, outWidth = 0, outHeight = 0;
    std::string resampler; // "cpu" / "cuda"
    VerificationResult verification;
    std::vector<CopyResult> sidecarCopies; // archive mode: non-raw files copied into the output clip folder
    std::vector<std::string> copyErrors;   // archive mode: sidecar files that could not be copied (batch exit 1)
};

using ProgressFn = std::function<void(const ClipProgress&)>;

struct ClipRunContext {
    std::shared_ptr<CancellationToken> cancel;
    std::shared_ptr<PauseGate> pause;
    ThreadPlan plan;
    ProgressFn progress;
};

// Converts one clip. The output is written into "<dest>.partial" (claimed exclusively) and renamed to
// "<dest>" only after the writer finished and verification passed; failure or cancellation removes it.
// An existing "<dest>" is skipped (SkipReason::OutputExists), or with `overwrite` replaced only once the
// new output is complete. A destination that equals, contains or lies inside the source is refused.
ClipResult convertClip(const ClipJob& job, const JobOptions& options, const ClipRunContext& ctx);

// Where convertClip will write a clip, worked out without decoding it (used to find collisions).
struct PlannedOutput {
    std::filesystem::path dest; // final output path ("mem://name" for the memory codec)
    std::string name;
    codec::Format format = codec::Format::Unknown;
};
Expected<PlannedOutput> planClipOutput(const ClipJob& job, const JobOptions& options);

// What a clip reads on disk: the CinemaDNG folder (also when a single .dng frame was given) or the
// clip file. Empty for "mem://" clips.
std::filesystem::path clipSourceRoot(const std::filesystem::path& source);

// Chooses the resampler: CUDA when built, requested and a device exists; CPU otherwise.
struct ResamplerChoice {
    std::string name;
    bool cuda = false;
};
ResamplerChoice chooseResampler(bool useGpu);

} // namespace r3dss::pipeline
