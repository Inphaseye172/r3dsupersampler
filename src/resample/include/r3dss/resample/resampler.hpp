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
    bool clipAware = true;  // fall back to Area next to clipped highlights
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

// Validates geometry shared by all implementations.
Expected<void> validateParams(const BayerFrame& in, const ResampleParams& p);

} // namespace r3dss::resample
