#include "r3dss/resample/size_planner.hpp"
#include <algorithm>
#include <cmath>

namespace r3dss::resample {

uint32_t presetWidth(SizePreset p) {
    switch (p) {
    case SizePreset::SixK: return 6048;
    case SizePreset::UHD: return 3840;
    case SizePreset::TwoK: return 2048;
    case SizePreset::FHD: return 1920;
    default: return 0;
    }
}

namespace {
uint32_t alignTo(double v, uint32_t a) {
    a = std::max(2u, a);
    if (a & 1u) a *= 2; // keep output even for a valid mosaic
    const double units = v / a;
    return static_cast<uint32_t>(std::max(1.0, std::round(units))) * a;
}
} // namespace

Expected<OutputSize> planOutputSize(uint32_t srcW, uint32_t srcH, SizePreset preset, std::optional<OutputSize> custom,
                                    uint32_t alignment) {
    if (srcW < 2 || srcH < 2) return makeError(ErrorCode::InvalidArgument, "invalid source size");
    OutputSize out;
    if (preset == SizePreset::Same) {
        out.width = srcW;
        out.height = srcH;
    } else if (preset == SizePreset::Custom) {
        if (!custom) return makeError(ErrorCode::InvalidArgument, "custom size preset without a size");
        out.width = alignTo(custom->width, alignment);
        out.height = alignTo(custom->height, alignment);
    } else {
        auto fromPreset = [&](SizePreset p) {
            const uint32_t w = presetWidth(p);
            return OutputSize{alignTo(w, alignment), alignTo(static_cast<double>(w) * srcH / srcW, alignment)};
        };
        // A source smaller than the preset is converted at the largest preset that fits, else at its own
        // size, so mixed-resolution batches convert every clip instead of skipping the small ones.
        static constexpr SizePreset kDescending[] = {SizePreset::SixK, SizePreset::UHD, SizePreset::TwoK, SizePreset::FHD};
        out = OutputSize{srcW, srcH};
        bool reached = false;
        for (SizePreset p : kDescending) {
            reached = reached || p == preset;
            if (!reached) continue;
            const OutputSize cand = fromPreset(p);
            if (cand.width <= srcW && cand.height <= srcH) {
                out = cand;
                break;
            }
        }
    }
    if (out.width > srcW || out.height > srcH)
        return makeError(ErrorCode::Unsupported, "output " + std::to_string(out.width) + "x" + std::to_string(out.height) +
                                                     " would be larger than source " + std::to_string(srcW) + "x" +
                                                     std::to_string(srcH));
    return out;
}

} // namespace r3dss::resample
