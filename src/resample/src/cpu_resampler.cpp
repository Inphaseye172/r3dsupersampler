#include <algorithm>
#include <cmath>
#include "r3dss/resample/resampler.hpp"
#include "r3dss/resample/taps.hpp"

namespace r3dss::resample {

Expected<void> validateParams(const BayerFrame& in, const ResampleParams& p) {
    if (in.width < 2 || in.height < 2 || (in.width & 1u) || (in.height & 1u))
        return makeError(ErrorCode::InvalidArgument, "source dimensions must be even and >= 2");
    if (p.outWidth < 2 || p.outHeight < 2 || (p.outWidth & 1u) || (p.outHeight & 1u))
        return makeError(ErrorCode::InvalidArgument, "output dimensions must be even and >= 2");
    if (p.outWidth > in.width || p.outHeight > in.height)
        return makeError(ErrorCode::Unsupported, "upscaling is not supported (output larger than source)");
    if (in.data.size() != in.sampleCount()) return makeError(ErrorCode::InvalidArgument, "frame data size mismatch");
    return {};
}

namespace {

// Cached tap tables for one (src, dst, kernel) geometry.
struct PlaneTaps {
    AxisTaps x, y;
};
struct Geometry {
    uint32_t sw = 0, sh = 0, dw = 0, dh = 0;
    ResampleKernel kernel = ResampleKernel::Lanczos3;
    PlaneTaps planes[4];
    bool matches(const BayerFrame& in, const ResampleParams& p) const {
        return sw == in.width && sh == in.height && dw == p.outWidth && dh == p.outHeight && kernel == p.kernel;
    }
};

class CpuResampler final : public IResampler {
public:
    explicit CpuResampler(std::shared_ptr<ThreadPool> pool) : pool_(std::move(pool)) {}
    std::string name() const override { return "cpu"; }

    Expected<void> resample(const BayerFrame& in, BayerFrame& out, const ResampleParams& p) override {
        R3DSS_TRY(validateParams(in, p));
        ensureGeometry(in, p);
        out.width = p.outWidth;
        out.height = p.outHeight;
        out.bits = in.bits;
        out.cfa = in.cfa;
        out.black = in.black;
        out.white = in.white;
        out.meta = in.meta;
        out.index = in.index;
        out.data.resize(out.sampleCount());
        for (int plane = 0; plane < 4; ++plane) resamplePlane(in, out, p, plane);
        return {};
    }

private:
    void ensureGeometry(const BayerFrame& in, const ResampleParams& p) {
        if (geo_.matches(in, p)) return;
        geo_.sw = in.width;
        geo_.sh = in.height;
        geo_.dw = p.outWidth;
        geo_.dh = p.outHeight;
        geo_.kernel = p.kernel;
        for (int plane = 0; plane < 4; ++plane) {
            geo_.planes[plane].x = buildAxisTaps(in.width, p.outWidth, planeOffsetX(plane), p.kernel);
            geo_.planes[plane].y = buildAxisTaps(in.height, p.outHeight, planeOffsetY(plane), p.kernel);
        }
    }

    void parallel(size_t n, const std::function<void(size_t)>& fn) {
        if (pool_ && pool_->size() > 1 && n > 1) {
            // Band the work so each task covers several rows.
            const size_t bands = std::min<size_t>(n, pool_->size() * 4);
            pool_->parallelFor(bands, [&](size_t b) {
                const size_t lo = n * b / bands, hi = n * (b + 1) / bands;
                for (size_t i = lo; i < hi; ++i) fn(i);
            });
        } else {
            for (size_t i = 0; i < n; ++i) fn(i);
        }
    }

    void resamplePlane(const BayerFrame& in, BayerFrame& out, const ResampleParams& p, int plane) {
        const uint32_t ox = planeOffsetX(plane), oy = planeOffsetY(plane);
        const PlaneTaps& pt = geo_.planes[plane];
        const TapTable& tx = pt.x.main;
        const TapTable& ty = pt.y.main;
        const uint32_t spw = in.width / 2, sph = in.height / 2;   // source plane size
        const uint32_t dpw = out.width / 2, dph = out.height / 2; // destination plane size
        const float clipThr = static_cast<float>(in.white) - 1.0f;
        const bool clipAware = p.clipAware && p.kernel != ResampleKernel::Area;

        // Horizontal pass: tmp[sph][dpw] (+ clip flags)
        tmp_.resize(static_cast<size_t>(sph) * dpw);
        tmpClip_.assign(clipAware ? static_cast<size_t>(sph) * dpw : 0, 0);
        parallel(sph, [&](size_t j) {
            const uint16_t* row = in.data.data() + (static_cast<size_t>(2 * j + oy) * in.width) + ox;
            float* trow = tmp_.data() + j * dpw;
            uint8_t* crow = clipAware ? tmpClip_.data() + j * dpw : nullptr;
            for (uint32_t i = 0; i < dpw; ++i) {
                const float* w = tx.w(i);
                const int st = tx.start[i];
                const int cnt = tx.count[i];
                float acc = 0.0f;
                bool clip = false;
                for (int k = 0; k < cnt; ++k) {
                    const float v = static_cast<float>(row[static_cast<size_t>(st + k) * 2]);
                    acc += w[k] * v;
                    clip |= v >= clipThr;
                }
                trow[i] = acc;
                if (crow) crow[i] = clip ? 1 : 0;
            }
        });

        // Vertical pass: out plane
        const float maxCode = static_cast<float>(out.maxCode());
        parallel(dph, [&](size_t jj) {
            const uint32_t j = static_cast<uint32_t>(jj);
            const float* w = ty.w(j);
            const int st = ty.start[j];
            const int cnt = ty.count[j];
            uint16_t* orow = out.data.data() + (static_cast<size_t>(2 * j + oy) * out.width) + ox;
            for (uint32_t i = 0; i < dpw; ++i) {
                float acc = 0.0f;
                bool clip = false;
                for (int k = 0; k < cnt; ++k) {
                    const size_t idx = static_cast<size_t>(st + k) * dpw + i;
                    acc += w[k] * tmp_[idx];
                    if (clipAware) clip |= tmpClip_[idx] != 0;
                }
                if (clip) acc = areaSample(in, pt, i, j, ox, oy, spw, sph);
                const float r = std::round(acc);
                orow[static_cast<size_t>(i) * 2] = static_cast<uint16_t>(std::clamp(r, 0.0f, maxCode));
            }
        });
    }

    // Direct 2-D Area evaluation on the source plane (clip-aware fallback).
    static float areaSample(const BayerFrame& in, const PlaneTaps& pt, uint32_t i, uint32_t j, uint32_t ox,
                            uint32_t oy, uint32_t, uint32_t) {
        const TapTable& ax = pt.x.area;
        const TapTable& ay = pt.y.area;
        const float* wx = ax.w(i);
        const float* wy = ay.w(j);
        float acc = 0.0f;
        for (int ky = 0; ky < ay.count[j]; ++ky) {
            const size_t sy = static_cast<size_t>(ay.start[j] + ky) * 2 + oy;
            const uint16_t* row = in.data.data() + sy * in.width + ox;
            float racc = 0.0f;
            for (int kx = 0; kx < ax.count[i]; ++kx)
                racc += wx[kx] * static_cast<float>(row[static_cast<size_t>(ax.start[i] + kx) * 2]);
            acc += wy[ky] * racc;
        }
        return acc;
    }

    std::shared_ptr<ThreadPool> pool_;
    Geometry geo_;
    std::vector<float> tmp_;
    std::vector<uint8_t> tmpClip_;
};

} // namespace

std::unique_ptr<IResampler> makeCpuResampler(std::shared_ptr<ThreadPool> pool) {
    return std::make_unique<CpuResampler>(std::move(pool));
}

} // namespace r3dss::resample
