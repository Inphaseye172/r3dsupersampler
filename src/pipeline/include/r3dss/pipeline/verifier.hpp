// Re-opens an output clip and checks it against what the job expected.
#pragma once
#include <string>
#include <vector>
#include "r3dss/codec/codec.hpp"

namespace r3dss::pipeline {

struct Expectation {
    uint32_t width = 0, height = 0;
    int64_t frameCount = 0;
    Rational fps{24, 1};
    AudioInfo audio;
    std::optional<Timecode> startTimecode;
    int64_t audioSamples = 0;
    bool decodeFirstAndLast = true;
};

struct VerificationResult {
    bool ok = true;
    std::vector<std::string> problems;
    std::vector<std::string> checked; // human readable list of what passed
};

VerificationResult verifyOutput(const std::filesystem::path& output, const Expectation& e);

} // namespace r3dss::pipeline
