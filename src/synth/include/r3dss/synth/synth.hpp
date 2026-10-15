// Synthetic Bayer mosaics for tests, demos and benchmarks (docs/TEST_PLAN.md §fixtures).
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include "r3dss/codec/codec.hpp"

namespace r3dss::synth {

enum class Pattern { Constant, PlaneConstants, RampX, RampY, ZonePlate, Impulse, Noise, Scene };
const char* patternName(Pattern p);
std::optional<Pattern> patternFromName(std::string_view s);

struct SynthParams {
    uint32_t width = 256, height = 144;
    uint16_t bits = 12;
    CfaPattern cfa = CfaPattern::RGGB;
    Pattern pattern = Pattern::Scene;
    uint32_t seed = 1;
    int64_t frameIndex = 0;
    uint32_t black = 64;  // written into frame.black for all planes
    double constant = 0.5; // for Constant, as a fraction of white
};

BayerFrame makeFrame(const SynthParams& p);

// Generates a complete CinemaDNG clip (frames + optional float32 WAV audio) for end-to-end tests.
struct SynthClipParams {
    SynthParams frame;
    int64_t frames = 12;
    Rational fps{25, 1};
    uint32_t audioChannels = 2;       // 0 = no audio
    SampleFormat audioFormat = SampleFormat::Float32;
    uint32_t audioRate = 48000;
    std::optional<Timecode> startTimecode;
    std::string clipName = "synthetic";
    bool losslessJpeg = true;
};
Expected<codec::ClipInfo> writeSyntheticClip(const std::filesystem::path& dir, const SynthClipParams& p);

// Builds the same clip in the in-memory codec store under "mem://<name>".
Expected<codec::ClipInfo> makeMemoryClip(const std::string& name, const SynthClipParams& p);

// Fills an interleaved audio buffer with a deterministic test tone.
std::vector<uint8_t> makeAudio(const AudioInfo& info, int64_t samples, uint32_t seed);

} // namespace r3dss::synth
