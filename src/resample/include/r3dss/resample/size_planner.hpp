// Maps a size preset to an exact even output size for a given source (docs/RESAMPLING.md §sizes).
#pragma once
#include <cstdint>
#include <optional>
#include "r3dss/core/expected.hpp"
#include "r3dss/core/types.hpp"

namespace r3dss::resample {

struct OutputSize {
    uint32_t width = 0, height = 0;
    bool operator==(const OutputSize&) const = default;
};

// Width presets: 6K = 6048, UHD = 3840, FHD = 1920. Height keeps the source aspect ratio, is
// rounded to the nearest multiple of `alignment` (>= 2, even), never upscales.
Expected<OutputSize> planOutputSize(uint32_t srcW, uint32_t srcH, SizePreset preset, std::optional<OutputSize> custom,
                                    uint32_t alignment = 2);

uint32_t presetWidth(SizePreset p);

} // namespace r3dss::resample
