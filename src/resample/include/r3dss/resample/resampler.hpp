// Resampler interface shared by the CPU and CUDA implementations.
#pragma once
#include <memory>
#include <string>
#include "r3dss/core/concurrency.hpp"
#include "r3dss/core/expected.hpp"
#include "r3dss/core/types.hpp"

namespace r3dss::resample {

struct ResampleParams {
    uint32_t outWidth = 0;  // even
    uint32_t outHeight = 0; // even
    ResampleKernel kernel = ResampleKernel::Lanczos3;
    // Clipping rule (docs/RESAMPLING.md §4): when set (ignored for the Area kernel), an output site whose
    // main-kernel source taps include a value >= white - 1, or whose main-kernel result is >= white - 1, takes
    // the Area result instead. Every output is clamped to [0, outputCeiling()] whatever this flag says.
    bool clipAware = true;
};

class IResampler {
public:
    virtual ~IResampler() = default;
    virtual std::string name() const = 0;
    // `out` is (re)initialised to the output geometry; metadata and levels copied from `in`.
    virtual Expected<void> resample(const BayerFrame& in, BayerFrame& out, const ResampleParams& p) = 0;
};

// CPU implementation. `pool` may be null (single-threaded).
std::unique_ptr<IResampler> makeCpuResampler(std::shared_ptr<ThreadPool> pool);

// Validates geometry (and a non-zero white level) shared by all implementations.
Expected<void> validateParams(const BayerFrame& in, const ResampleParams& p);

// Highest code a resampled output site may take: min(2^bits - 1, white). Shared by CPU and CUDA.
uint32_t outputCeiling(const BayerFrame& in);

} // namespace r3dss::resample
