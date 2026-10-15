// Content-driven VBR: per-frame complexity -> bit budget inside the profile's [floor, cap] bpp.
#pragma once
#include <memory>
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/concurrency.hpp"

namespace r3dss::ratecontrol {

struct ProfileParams {
    double floorBpp = 1.0;
    double capBpp = 2.0;
    bool lossless = false;
};
ProfileParams profileParams(QualityProfile q);

struct ComplexityStats {
    double laplacianMean = 0.0; // mean |3x3 Laplacian| over all planes, normalised to [0,1] range
    double noiseFloor = 0.0;    // 10th percentile of 16x16-block means (flat regions)
    double complexity = 0.0;    // (laplacianMean - noiseFloor) / curveScale, clamped to [0,1]
};

struct EstimatorParams {
    double curveScale = 0.05; // detail level (fraction of range) that saturates the budget
    uint32_t blockSize = 16;
    uint32_t rowStride = 2;   // evaluate every Nth plane row for speed
};

ComplexityStats estimateComplexity(const BayerFrame& f, const EstimatorParams& p = {}, ThreadPool* pool = nullptr);

class VbrController {
public:
    explicit VbrController(QualityProfile q, double emaAlpha = 0.3);
    // Returns the budget for `frame` given its complexity. Updates the EMA state.
    codec::FrameBudget next(const BayerFrame& frame, const ComplexityStats& stats);
    double lastBpp() const { return lastBpp_; }
    const ProfileParams& profile() const { return prof_; }
    void reset() { ema_ = -1.0; lastBpp_ = 0.0; }

private:
    ProfileParams prof_;
    double alpha_;
    double ema_ = -1.0;
    double lastBpp_ = 0.0;
};

} // namespace r3dss::ratecontrol
