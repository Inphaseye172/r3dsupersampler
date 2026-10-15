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

// Width presets: 6K = 6048, UHD = 3840, 2K = 2048 (DCI), FHD = 1920. Height keeps the source aspect ratio, is
// rounded to the nearest multiple of `alignment` (>= 2, even), never upscales: a preset larger than the source
// steps down to the largest preset that fits, or to the source size when none does. Only a custom size larger
// than the source is an error.
Expected<OutputSize> planOutputSize(uint32_t srcW, uint32_t srcH, SizePreset preset, std::optional<OutputSize> custom,
                                    uint32_t alignment = 2);

uint32_t presetWidth(SizePreset p);

} // namespace r3dss::resample
