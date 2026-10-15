// RIFF/WAVE reader and streaming writer: PCM 16/24/32 and IEEE float 32, any channel count.
#pragma once
#include <cstdio>
#include <filesystem>
#include "r3dss/core/expected.hpp"
#include "r3dss/core/types.hpp"

namespace r3dss::codec::wav {

struct WavInfo {
    AudioInfo audio;
    uint64_t dataOffset = 0;
    uint64_t dataBytes = 0;
};

Expected<WavInfo> probe(const std::filesystem::path& p);
// Reads `count` sample frames starting at `firstSample` (clamped at end of data).
Expected<AudioBlock> read(const std::filesystem::path& p, const WavInfo& info, int64_t firstSample, int64_t count);

class StreamWriter {
public:
    ~StreamWriter();
    Expected<void> begin(const std::filesystem::path& p, const AudioInfo& info);
    Expected<void> write(const AudioBlock& block);
    Expected<void> finish(); // patches sizes
    void abort();
    int64_t samplesWritten() const { return samples_; }

private:
    std::FILE* f_ = nullptr;
    std::filesystem::path path_;
    AudioInfo info_;
    int64_t samples_ = 0;
    long dataSizePos_ = 0, riffSizePos_ = 0;
};

} // namespace r3dss::codec::wav
