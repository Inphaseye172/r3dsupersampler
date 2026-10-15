#include "r3dss/resample/taps.hpp"
#include <algorithm>
#include <cmath>

namespace r3dss::resample {

double lanczos3(double x) {
    constexpr double a = 3.0;
    x = std::fabs(x);
    if (x < 1e-9) return 1.0;
    if (x >= a) return 0.0;
    constexpr double pi = 3.14159265358979323846;
    const double px = pi * x;
    return a * std::sin(px) * std::sin(px / a) / (px * px);
}

TapTable buildTaps(uint32_t srcLen, uint32_t dstLen, uint32_t offset, ResampleKernel kernel) {
    TapTable t;
    t.srcPlaneLen = srcLen / 2;
    t.dstPlaneLen = dstLen / 2;
    t.scale = static_cast<double>(srcLen) / static_cast<double>(dstLen);
    const double s = t.scale;
    const double sEff = std::max(1.0, s); // anti-aliasing: kernel widens when downscaling
    const int srcMax = static_cast<int>(t.srcPlaneLen) - 1;

    struct Row {
        int start;
        std::vector<double> w;
    };
    std::vector<Row> rows(t.dstPlaneLen);
    uint32_t maxTaps = 0;
    for (uint32_t i = 0; i < t.dstPlaneLen; ++i) {
        const double u = (2.0 * i + offset + 0.5) * s;   // mosaic coordinate of the output site centre
        const double tt = (u - offset - 0.5) / 2.0;      // continuous source plane coordinate
        int n0, n1;
        Row r;
        if (kernel == ResampleKernel::Lanczos3) {
            const double radius = 3.0 * sEff;
            n0 = static_cast<int>(std::ceil(tt - radius));
            n1 = static_cast<int>(std::floor(tt + radius));
            for (int n = n0; n <= n1; ++n) r.w.push_back(lanczos3((n - tt) / sEff));
        } else { // Area: box of width sEff centred on tt, exact coverage of each source sample
            const double lo = tt - sEff / 2.0, hi = tt + sEff / 2.0;
            n0 = static_cast<int>(std::floor(lo + 0.5));
            n1 = static_cast<int>(std::ceil(hi - 0.5));
            if (n1 < n0) n1 = n0;
            for (int n = n0; n <= n1; ++n) {
                const double a = std::max(lo, n - 0.5), b = std::min(hi, n + 0.5);
                r.w.push_back(std::max(0.0, b - a));
            }
        }
        // Clamp-to-edge: fold out-of-range taps onto the edge samples.
        std::vector<double> folded;
        int fStart = std::clamp(n0, 0, srcMax);
        int fEnd = std::clamp(n1, 0, srcMax);
        folded.assign(static_cast<size_t>(fEnd - fStart + 1), 0.0);
        for (int n = n0; n <= n1; ++n) {
            const int c = std::clamp(n, 0, srcMax);
            folded[static_cast<size_t>(c - fStart)] += r.w[static_cast<size_t>(n - n0)];
        }
        double sum = 0;
        for (double w : folded) sum += w;
        if (std::fabs(sum) < 1e-12) {
            folded.assign(1, 1.0);
            fStart = std::clamp(static_cast<int>(std::lround(tt)), 0, srcMax);
            sum = 1.0;
        }
        for (double& w : folded) w /= sum;
        r.start = fStart;
        r.w = std::move(folded);
        maxTaps = std::max<uint32_t>(maxTaps, static_cast<uint32_t>(r.w.size()));
        rows[i] = std::move(r);
    }
    t.maxTaps = std::max<uint32_t>(1, maxTaps);
    t.start.resize(t.dstPlaneLen);
    t.count.resize(t.dstPlaneLen);
    t.weights.assign(static_cast<size_t>(t.dstPlaneLen) * t.maxTaps, 0.0f);
    for (uint32_t i = 0; i < t.dstPlaneLen; ++i) {
        t.start[i] = rows[i].start;
        t.count[i] = static_cast<uint16_t>(rows[i].w.size());
        for (size_t k = 0; k < rows[i].w.size(); ++k)
            t.weights[static_cast<size_t>(i) * t.maxTaps + k] = static_cast<float>(rows[i].w[k]);
    }
    return t;
}

AxisTaps buildAxisTaps(uint32_t srcLen, uint32_t dstLen, uint32_t offset, ResampleKernel kernel) {
    AxisTaps a;
    a.main = buildTaps(srcLen, dstLen, offset, kernel);
    a.area = kernel == ResampleKernel::Area ? a.main : buildTaps(srcLen, dstLen, offset, ResampleKernel::Area);
    return a;
}

} // namespace r3dss::resample
