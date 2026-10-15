// Tap tables for phase-correct, per-plane Bayer resampling (docs/RESAMPLING.md).
//
// For one axis: the source mosaic has `srcLen` sites, the output `dstLen` sites, the plane
// being resampled has offset `offset` (0 or 1) on that axis. Output plane sample i' maps to
// source plane coordinate t = ((2 i' + offset + 0.5) * s - offset - 0.5) / 2 with s = srcLen/dstLen.
#pragma once
#include <cstdint>
#include <vector>
#include "r3dss/core/types.hpp"

namespace r3dss::resample {

struct TapTable {
    uint32_t dstPlaneLen = 0;
    uint32_t srcPlaneLen = 0;
    uint32_t maxTaps = 0;
    std::vector<int32_t> start;  // per output sample: first source plane index (already clamped >= 0)
    std::vector<uint16_t> count; // per output sample: number of taps (<= maxTaps)
    std::vector<float> weights;  // dstPlaneLen * maxTaps, row-major, unused entries are 0
    double scale = 1.0;

    const float* w(uint32_t i) const { return weights.data() + static_cast<size_t>(i) * maxTaps; }
};

double lanczos3(double x);

// Builds the table for one axis and one plane offset. `kernel` selects Lanczos-3 or Area.
TapTable buildTaps(uint32_t srcLen, uint32_t dstLen, uint32_t offset, ResampleKernel kernel);

// Convenience: both kernels for a plane offset so the clip-aware fallback can switch.
struct AxisTaps {
    TapTable main; // requested kernel
    TapTable area; // Area kernel, used for the clip-aware fallback
};
AxisTaps buildAxisTaps(uint32_t srcLen, uint32_t dstLen, uint32_t offset, ResampleKernel kernel);

} // namespace r3dss::resample
