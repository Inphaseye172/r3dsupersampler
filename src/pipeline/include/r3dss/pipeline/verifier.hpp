// Re-opens an output clip and checks it against what the job expected.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/concurrency.hpp"

namespace r3dss::pipeline {

struct Expectation {
    uint32_t width = 0, height = 0;
    int64_t frameCount = 0;
    Rational fps{24, 1};
    AudioInfo audio;
    std::optional<Timecode> startTimecode;
    int64_t audioSamples = 0; // expected sample count (taken from the source), 0 = do not check
    bool decodeAllFrames = true;     // decode every frame of the output
    bool decodeFirstAndLast = true;  // used only when decodeAllFrames is false
    // When set, the output's audio samples are compared byte for byte with this source clip's audio.
    std::filesystem::path audioReference;
    std::shared_ptr<CancellationToken> cancel;
    std::function<void(int64_t done, int64_t total)> progress; // frames decoded so far
};

struct VerificationResult {
    bool ok = true;
    bool cancelled = false;
    std::vector<std::string> problems;
    std::vector<std::string> checked; // human readable list of what passed
};

VerificationResult verifyOutput(const std::filesystem::path& output, const Expectation& e);

} // namespace r3dss::pipeline
