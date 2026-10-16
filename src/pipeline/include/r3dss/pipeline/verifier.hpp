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
    // Files the writer produced, with fileDigest() taken right after writing. Each one is flushed,
    // read back from storage and must still match (catches damage the decoder would not notice).
    std::vector<std::pair<std::filesystem::path, uint64_t>> fileDigests;
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

// Fast 64-bit content digest of a file (integrity check, not cryptographic).
Expected<uint64_t> fileDigest(const std::filesystem::path& p);

} // namespace r3dss::pipeline
