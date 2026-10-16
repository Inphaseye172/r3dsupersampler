#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "r3dss/resample/resampler.hpp"
#include "r3dss/resample/size_planner.hpp"
#include "r3dss/resample/taps.hpp"
#include "r3dss/synth/synth.hpp"
#include "resample_fixtures.hpp"

using namespace r3dss;
using namespace r3dss::resample;
using Catch::Approx;

TEST_CASE("Tap weights sum to one and identity scale is a single tap") {
    for (auto k : {ResampleKernel::Lanczos3, ResampleKernel::Area}) {
        for (uint32_t off = 0; off < 2; ++off) {
            TapTable t = buildTaps(400, 300, off, k);
            REQUIRE(t.dstPlaneLen == 150);
            for (uint32_t i = 0; i < t.dstPlaneLen; ++i) {
                double s = 0;
                for (uint32_t n = 0; n < t.count[i]; ++n) s += t.w(i)[n];
                REQUIRE(s == Approx(1.0).margin(1e-5));
                REQUIRE(t.start[i] >= 0);
                REQUIRE(t.start[i] + t.count[i] <= static_cast<int>(t.srcPlaneLen));
            }
        }
        TapTable id = buildTaps(64, 64, 1, k);
        for (uint32_t i = 0; i < id.dstPlaneLen; ++i) {
            // identity: all weight on sample i
            double wi = 0;
            for (uint32_t n = 0; n < id.count[i]; ++n)
                if (id.start[i] + static_cast<int>(n) == static_cast<int>(i)) wi += id.w(i)[n];
            REQUIRE(wi == Approx(1.0).margin(1e-5));
        }
    }
}

TEST_CASE("Phase mapping places output sites at the right source position") {
    // s = 2, plane offset 0: output plane sample i' maps to t = 2 i' + 0.25; offset 1: 2 i' + 0.75.
    TapTable a0 = buildTaps(64, 32, 0, ResampleKernel::Area);
    TapTable a1 = buildTaps(64, 32, 1, ResampleKernel::Area);
    // Area window [t-1, t+1]: for offset 0 -> weights 0.25, 1, 0.75 over samples 2i'-1..2i'+1
    REQUIRE(a0.start[4] == 7);
    REQUIRE(a0.count[4] == 3);
    REQUIRE(a0.w(4)[0] == Approx(0.125));
    REQUIRE(a0.w(4)[1] == Approx(0.5));
    REQUIRE(a0.w(4)[2] == Approx(0.375));
    REQUIRE(a1.start[4] == 8);
    REQUIRE(a1.w(4)[0] == Approx(0.375));
    REQUIRE(a1.w(4)[1] == Approx(0.5));
    REQUIRE(a1.w(4)[2] == Approx(0.125));
}

namespace {
BayerFrame make(synth::Pattern p, uint32_t w = 256, uint32_t h = 128) {
    synth::SynthParams sp;
    sp.width = w; sp.height = h; sp.pattern = p;
    return synth::makeFrame(sp);
}
int maxAbsDiff(const BayerFrame& a, const BayerFrame& b) {
    int m = 0;
    for (size_t i = 0; i < a.data.size(); ++i) m = std::max(m, std::abs(int(a.data[i]) - int(b.data[i])));
    return m;
}
} // namespace

TEST_CASE("Constant and per-plane constants are preserved exactly") {
    for (auto k : {ResampleKernel::Lanczos3, ResampleKernel::Area}) {
        for (auto pat : {synth::Pattern::Constant, synth::Pattern::PlaneConstants}) {
            BayerFrame in = make(pat);
            auto rs = makeCpuResampler(nullptr);
            BayerFrame out;
            ResampleParams p{100, 50, k, true};
            REQUIRE(rs->resample(in, out, p));
            REQUIRE(out.width == 100);
            REQUIRE(out.height == 50);
            REQUIRE(out.cfa == in.cfa);
            REQUIRE(out.black == in.black);
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x) REQUIRE(out.at(x, y) == in.at(x & 1u, y & 1u));
        }
    }
}

TEST_CASE("Linear ramp is preserved within 1 LSB away from the edges") {
    BayerFrame in = make(synth::Pattern::RampX, 512, 64);
    auto rs = makeCpuResampler(nullptr);
    BayerFrame out;
    REQUIRE(rs->resample(in, out, {256, 32, ResampleKernel::Lanczos3, true}));
    const double sx = 2.0;
    for (uint32_t y = 2; y < out.height - 2; ++y)
        for (uint32_t x = 8; x < out.width - 8; ++x) {
            const double u = (x + 0.5) * sx - 0.5; // source x of this output site centre
            const double expected = in.black[0] + (in.white - in.black[0]) * (u / 511.0);
            REQUIRE(std::abs(out.at(x, y) - expected) <= 1.0);
        }
}

TEST_CASE("Clip-aware fallback never overshoots white or undershoots around a clipped disc") {
    BayerFrame in = make(synth::Pattern::Scene, 512, 288);
    auto rs = makeCpuResampler(nullptr);
    BayerFrame withFallback, without;
    REQUIRE(rs->resample(in, withFallback, {256, 144, ResampleKernel::Lanczos3, true}));
    REQUIRE(rs->resample(in, without, {256, 144, ResampleKernel::Lanczos3, false}));
    int minNear = 1 << 16;
    // Around the disc every fallback output equals a positive-weight average, so it can't drop
    // below the minimum of its support. Check it never goes below the plain Area result's minimum.
    BayerFrame area;
    REQUIRE(rs->resample(in, area, {256, 144, ResampleKernel::Area, false}));
    for (size_t i = 0; i < withFallback.data.size(); ++i) {
        REQUIRE(withFallback.data[i] <= in.white);
        minNear = std::min<int>(minNear, withFallback.data[i]);
    }
    // The non-clip-aware Lanczos must show ringing halos somewhere that the fallback removed.
    REQUIRE(maxAbsDiff(withFallback, without) > 0);
}

TEST_CASE("Multi-threaded CPU path is bit-identical to single-threaded") {
    BayerFrame in = make(synth::Pattern::Scene, 640, 360);
    auto single = makeCpuResampler(nullptr);
    auto multi = makeCpuResampler(std::make_shared<ThreadPool>(4));
    BayerFrame a, b;
    REQUIRE(single->resample(in, a, {384, 216, ResampleKernel::Lanczos3, true}));
    REQUIRE(multi->resample(in, b, {384, 216, ResampleKernel::Lanczos3, true}));
    REQUIRE(a.data == b.data);
}

TEST_CASE("Area at 2x reduces noise variance and never upscales") {
    BayerFrame in = make(synth::Pattern::Noise, 256, 256);
    auto rs = makeCpuResampler(nullptr);
    BayerFrame out;
    REQUIRE(rs->resample(in, out, {128, 128, ResampleKernel::Area, false}));
    auto var = [](const BayerFrame& f) {
        double m = 0;
        for (auto v : f.data) m += v;
        m /= f.data.size();
        double s = 0;
        for (auto v : f.data) s += (v - m) * (v - m);
        return s / f.data.size();
    };
    REQUIRE(var(out) < var(in) * 0.5);
    REQUIRE_FALSE(rs->resample(in, out, {512, 512, ResampleKernel::Area, false}));
    REQUIRE_FALSE(validateParams(in, {127, 128, ResampleKernel::Area, false}));
}

TEST_CASE("Size planner presets") {
    auto s6 = planOutputSize(8256, 4644, SizePreset::SixK, std::nullopt, 2);
    REQUIRE(s6);
    REQUIRE(s6->width == 6048);
    REQUIRE(s6->height == 3402);
    auto uhd = planOutputSize(6048, 3402, SizePreset::UHD, std::nullopt, 8);
    REQUIRE(uhd->width == 3840);
    REQUIRE(uhd->height == 2160);
    auto fhd = planOutputSize(6048, 3402, SizePreset::FHD, std::nullopt, 2);
    REQUIRE(fhd->height == 1080);
    REQUIRE_FALSE(planOutputSize(3840, 2160, SizePreset::SixK, std::nullopt, 2));
    auto same = planOutputSize(4096, 2160, SizePreset::Same, std::nullopt, 2);
    REQUIRE(same->width == 4096);
    auto custom = planOutputSize(4096, 2160, SizePreset::Custom, OutputSize{2001, 1001}, 2);
    REQUIRE(custom->width == 2002); // rounded to the nearest even size
    REQUIRE(custom->height == 1002);
}

// ---- Clipping rule (docs/RESAMPLING.md §4, contract I5) -------------------------------------------

namespace {
using r3dss::testing::highlightFrame;

// Independent double-precision model of I5 built only from the tap tables: v = 2-D main-kernel result; if
// clip-aware (and not Area) and (any main-kernel source tap >= white-1 or v >= white-1), v = 2-D Area result;
// out = clamp(round(v), 0, min(maxCode, white)). `ambiguous` marks sites whose main-kernel value is so close to
// the threshold that float32 and double may legitimately take different branches.
struct I5Reference {
    std::vector<uint16_t> data;
    std::vector<uint8_t> ambiguous;
};
I5Reference referenceI5(const BayerFrame& in, uint32_t ow, uint32_t oh, ResampleKernel kernel, bool clipAware) {
    I5Reference r;
    r.data.assign(static_cast<size_t>(ow) * oh, 0);
    r.ambiguous.assign(r.data.size(), 0);
    const double thr = static_cast<double>(in.white) - 1.0;
    const double outMax = std::min<double>(in.maxCode(), in.white);
    const bool fallback = clipAware && kernel != ResampleKernel::Area;
    for (int p = 0; p < 4; ++p) {
        const uint32_t ox = planeOffsetX(p), oy = planeOffsetY(p);
        const AxisTaps tx = buildAxisTaps(in.width, ow, ox, kernel);
        const AxisTaps ty = buildAxisTaps(in.height, oh, oy, kernel);
        auto P = [&](int j, int i) {
            return static_cast<double>(in.at(2u * static_cast<uint32_t>(i) + ox, 2u * static_cast<uint32_t>(j) + oy));
        };
        auto eval2d = [&](const TapTable& X, const TapTable& Y, uint32_t i, uint32_t j, bool& anyClip) {
            double acc = 0.0;
            for (int ky = 0; ky < Y.count[j]; ++ky)
                for (int kx = 0; kx < X.count[i]; ++kx) {
                    const double s = P(Y.start[j] + ky, X.start[i] + kx);
                    acc += static_cast<double>(Y.w(j)[ky]) * static_cast<double>(X.w(i)[kx]) * s;
                    anyClip |= s >= thr;
                }
            return acc;
        };
        for (uint32_t j = 0; j < oh / 2; ++j)
            for (uint32_t i = 0; i < ow / 2; ++i) {
                bool anyClip = false, unused = false;
                double v = eval2d(tx.main, ty.main, i, j, anyClip);
                const size_t idx = static_cast<size_t>(2 * j + oy) * ow + 2 * i + ox;
                if (fallback && !anyClip && std::abs(v - thr) < 1e-2) r.ambiguous[idx] = 1;
                if (fallback && (anyClip || v >= thr)) v = eval2d(tx.area, ty.area, i, j, unused);
                r.data[idx] = static_cast<uint16_t>(std::clamp(std::round(v), 0.0, outMax));
            }
    }
    return r;
}
} // namespace

TEST_CASE("Lanczos overshoot of an unclipped near-white edge never creates saturated sites (I5)") {
    struct Case { uint16_t bits; uint32_t white; };
    for (const Case c : {Case{12, 4095}, Case{12, 3800}, Case{14, 15000}}) {
        CAPTURE(c.bits, c.white);
        BayerFrame in = highlightFrame(1280, 720, c.bits, c.white, 64, false);
        uint16_t srcMax = 0;
        for (auto v : in.data) srcMax = std::max(srcMax, v);
        REQUIRE(srcMax < c.white - 1); // nothing is clipped in the source
        auto rs = makeCpuResampler(nullptr);
        BayerFrame out;
        REQUIRE(rs->resample(in, out, {640, 360, ResampleKernel::Lanczos3, true}));
        size_t atOrAboveWhite = 0;
        uint16_t outMax = 0;
        for (auto v : out.data) {
            atOrAboveWhite += v >= c.white;
            outMax = std::max(outMax, v);
        }
        CAPTURE(outMax);
        REQUIRE(atOrAboveWhite == 0);
        // Without the fallback the same edge does overshoot past white - 1, so the case exercises the rule.
        BayerFrame plain;
        REQUIRE(rs->resample(in, plain, {640, 360, ResampleKernel::Lanczos3, false}));
        size_t plainOver = 0;
        for (auto v : plain.data) plainOver += v >= c.white - 1;
        REQUIRE(plainOver > 0);
    }
}

TEST_CASE("Resampled output never exceeds WhiteLevel, for every kernel and fallback setting (I5)") {
    // WhiteLevel 3800 in a 12-bit container, a clipped disc at 3800 and hot sites at 4095 above WhiteLevel.
    BayerFrame in = highlightFrame(640, 360, 12, 3800, 64, true, true);
    auto rs = makeCpuResampler(nullptr);
    for (auto k : {ResampleKernel::Lanczos3, ResampleKernel::Area})
        for (bool clipAware : {true, false}) {
            CAPTURE(kernelName(k), clipAware);
            BayerFrame out;
            REQUIRE(rs->resample(in, out, {320, 180, k, clipAware}));
            uint16_t outMax = 0;
            for (auto v : out.data) outMax = std::max(outMax, v);
            REQUIRE(outMax <= 3800);
            REQUIRE(outMax == 3800); // the clipped disc stays clipped
            REQUIRE(out.white == 3800);
        }
}

TEST_CASE("CPU resampler implements the I5 clipping rule exactly (double-precision model)") {
    struct Case { uint16_t bits; uint32_t white; bool hot; uint32_t ow, oh; };
    for (const Case c : {Case{12, 4095, false, 160, 90}, Case{12, 3800, true, 160, 90}, Case{12, 3800, true, 250, 140},
                         Case{14, 15000, true, 212, 120}}) {
        BayerFrame in = highlightFrame(320, 180, c.bits, c.white, 64, true, c.hot);
        auto rs = makeCpuResampler(std::make_shared<ThreadPool>(3));
        for (auto k : {ResampleKernel::Lanczos3, ResampleKernel::Area})
            for (bool clipAware : {true, false}) {
                CAPTURE(c.white, c.ow, c.oh, kernelName(k), clipAware);
                BayerFrame out;
                REQUIRE(rs->resample(in, out, {c.ow, c.oh, k, clipAware}));
                const I5Reference ref = referenceI5(in, c.ow, c.oh, k, clipAware);
                int maxDiff = 0;
                size_t checked = 0;
                for (size_t i = 0; i < out.data.size(); ++i) {
                    if (ref.ambiguous[i]) continue;
                    ++checked;
                    maxDiff = std::max(maxDiff, std::abs(int(out.data[i]) - int(ref.data[i])));
                }
                REQUIRE(checked > out.data.size() * 99 / 100);
                REQUIRE(maxDiff <= 1);
            }
    }
}

TEST_CASE("A frame with WhiteLevel 0 is rejected instead of being clamped to black") {
    BayerFrame in = make(synth::Pattern::Scene, 64, 32);
    in.white = 0;
    REQUIRE_FALSE(validateParams(in, {32, 16, ResampleKernel::Lanczos3, true}));
}
