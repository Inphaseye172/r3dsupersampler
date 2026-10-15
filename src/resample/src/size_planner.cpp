#include "r3dss/resample/size_planner.hpp"
#include <algorithm>
#include <cmath>

namespace r3dss::resample {

uint32_t presetWidth(SizePreset p) {
    switch (p) {
    case SizePreset::SixK: return 6048;
    case SizePreset::UHD: return 3840;
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
        const uint32_t w = presetWidth(preset);
        out.width = alignTo(w, alignment);
        out.height = alignTo(static_cast<double>(w) * srcH / srcW, alignment);
    }
    if (out.width > srcW || out.height > srcH)
        return makeError(ErrorCode::Unsupported, "output " + std::to_string(out.width) + "x" + std::to_string(out.height) +
                                                     " would be larger than source " + std::to_string(srcW) + "x" +
                                                     std::to_string(srcH));
    return out;
}

} // namespace r3dss::resample
