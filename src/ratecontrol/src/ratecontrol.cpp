#include "r3dss/ratecontrol/ratecontrol.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace r3dss::ratecontrol {

ProfileParams profileParams(QualityProfile q) {
    switch (q) {
    case QualityProfile::Q1_Maximum: return {2.4, 4.0, false};
    case QualityProfile::Q3_High: return {1.6, 3.0, false};
    case QualityProfile::Q5_Normal: return {1.0, 2.0, false};
    case QualityProfile::Q7_Proxy: return {0.5, 1.0, false};
    }
    return {1.0, 2.0, false};
}

ComplexityStats estimateComplexity(const BayerFrame& f, const EstimatorParams& p, ThreadPool* pool) {
    ComplexityStats st;
    if (f.width < 6 || f.height < 6) return st;
    const uint32_t pw = f.width / 2, ph = f.height / 2;
    const uint32_t bs = std::max(4u, p.blockSize);
    const uint32_t bx = (pw + bs - 1) / bs, by = (ph + bs - 1) / bs;
    const double range = std::max(1.0, static_cast<double>(f.white) - static_cast<double>(f.black[0]));
    const uint32_t stride = std::max(1u, p.rowStride);

    // Per plane, per block: sum and count of |L|.
    std::vector<double> blockSum(static_cast<size_t>(bx) * by, 0.0);
    std::vector<uint32_t> blockCnt(static_cast<size_t>(bx) * by, 0);
    std::mutex mergeMutex;

    auto planeWork = [&](int plane) {
        const uint32_t ox = planeOffsetX(plane), oy = planeOffsetY(plane);
        std::vector<double> ls(blockSum.size(), 0.0);
        std::vector<uint32_t> lc(blockCnt.size(), 0);
        for (uint32_t j = 1; j + 1 < ph; j += stride) {
            const uint16_t* up = f.data.data() + (static_cast<size_t>(2 * (j - 1) + oy) * f.width) + ox;
            const uint16_t* row = f.data.data() + (static_cast<size_t>(2 * j + oy) * f.width) + ox;
            const uint16_t* dn = f.data.data() + (static_cast<size_t>(2 * (j + 1) + oy) * f.width) + ox;
            const uint32_t bj = j / bs;
            for (uint32_t i = 1; i + 1 < pw; ++i) {
                const int c = row[static_cast<size_t>(i) * 2];
                const int l = row[static_cast<size_t>(i - 1) * 2], r = row[static_cast<size_t>(i + 1) * 2];
                const int u = up[static_cast<size_t>(i) * 2], d = dn[static_cast<size_t>(i) * 2];
                const int lap = 4 * c - l - r - u - d;
                const size_t b = static_cast<size_t>(bj) * bx + i / bs;
                ls[b] += std::abs(lap);
                lc[b] += 1;
            }
        }
        std::lock_guard<std::mutex> lk(mergeMutex);
        for (size_t b = 0; b < ls.size(); ++b) {
            blockSum[b] += ls[b];
            blockCnt[b] += lc[b];
        }
    };
    if (pool && pool->size() > 1) pool->parallelFor(4, [&](size_t p4) { planeWork(static_cast<int>(p4)); });
    else for (int plane = 0; plane < 4; ++plane) planeWork(plane);

    std::vector<double> means;
    means.reserve(blockSum.size());
    double total = 0, count = 0;
    for (size_t b = 0; b < blockSum.size(); ++b) {
        if (!blockCnt[b]) continue;
        means.push_back(blockSum[b] / blockCnt[b] / (4.0 * range));
        total += blockSum[b];
        count += blockCnt[b];
    }
    if (means.empty()) return st;
    st.laplacianMean = total / count / (4.0 * range);
    std::sort(means.begin(), means.end());
    st.noiseFloor = means[means.size() / 10];
    st.complexity = std::clamp((st.laplacianMean - st.noiseFloor) / std::max(1e-9, p.curveScale), 0.0, 1.0);
    return st;
}

VbrController::VbrController(QualityProfile q, double emaAlpha) : prof_(profileParams(q)), alpha_(emaAlpha) {}

codec::FrameBudget VbrController::next(const BayerFrame& frame, const ComplexityStats& stats) {
    const double raw = prof_.floorBpp + (prof_.capBpp - prof_.floorBpp) * stats.complexity;
    ema_ = ema_ < 0 ? raw : alpha_ * raw + (1.0 - alpha_) * ema_;
    lastBpp_ = std::clamp(ema_, prof_.floorBpp, prof_.capBpp);
    codec::FrameBudget b;
    b.lossless = prof_.lossless;
    b.bits = static_cast<uint64_t>(std::llround(lastBpp_ * static_cast<double>(frame.sampleCount())));
    return b;
}

} // namespace r3dss::ratecontrol
