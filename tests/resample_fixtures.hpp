// Synthetic frames shared by the CPU and CUDA resampler tests.
#pragma once
#include <algorithm>
#include <cmath>
#include "r3dss/core/types.hpp"

namespace r3dss::testing {

// Near-white highlight scene (docs/RESAMPLING.md §4): 0.3 of the range as background, a 7-site wide bar at
// 0.97 * white that is NOT clipped, optionally a clipped disc (== white) and 64 hot sites at 2^bits - 1, above
// WhiteLevel when the container allows it. A small deterministic noise keeps neighbouring values distinct.
inline BayerFrame highlightFrame(uint32_t w, uint32_t h, uint16_t bits, uint32_t white, uint32_t black, bool disc,
                                 bool hotAboveWhite = false) {
    BayerFrame f = BayerFrame::create(w, h, bits, CfaPattern::RGGB);
    f.white = white;
    f.black = {black, black, black, black};
    uint32_t lcg = 12345;
    const double range = static_cast<double>(white) - black;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            lcg = lcg * 1664525u + 1013904223u;
            double v = black + 0.3 * range + static_cast<double>(lcg >> 28); // + 0..15
            if (disc && std::hypot(x - w / 2.0, y - h / 2.0) < std::min(w, h) / 6.0) v = white;
            if (x > w / 8 && x < w / 8 + 7 && y > h / 8 && y < h * 7 / 8) v = 0.97 * white;
            f.at(x, y) = static_cast<uint16_t>(std::lround(std::min<double>(v, white)));
        }
    if (hotAboveWhite)
        for (uint32_t k = 0; k < 64; ++k) f.at((k * 37u) % w, (k * 53u) % h) = static_cast<uint16_t>(f.maxCode());
    return f;
}

} // namespace r3dss::testing
