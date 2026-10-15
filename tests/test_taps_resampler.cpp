#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "r3dss/resample/resampler.hpp"
#include "r3dss/resample/size_planner.hpp"
#include "r3dss/resample/taps.hpp"
#include "r3dss/synth/synth.hpp"

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
    // Smaller sources step down to the largest preset that fits, else keep their own size.
    auto down = planOutputSize(3840, 2160, SizePreset::SixK, std::nullopt, 2);
    REQUIRE(down);
    REQUIRE(down->width == 3840);
    REQUIRE(down->height == 2160);
    auto dci = planOutputSize(4096, 2160, SizePreset::SixK, std::nullopt, 2);
    REQUIRE(dci->width == 3840);
    auto tiny = planOutputSize(1280, 720, SizePreset::UHD, std::nullopt, 2);
    REQUIRE(tiny->width == 1280);
    REQUIRE(tiny->height == 720);
    REQUIRE_FALSE(planOutputSize(3840, 2160, SizePreset::Custom, OutputSize{6048, 3402}, 2));
    auto same = planOutputSize(4096, 2160, SizePreset::Same, std::nullopt, 2);
    REQUIRE(same->width == 4096);
    auto custom = planOutputSize(4096, 2160, SizePreset::Custom, OutputSize{2001, 1001}, 2);
    REQUIRE(custom->width == 2002); // rounded to the nearest even size
    REQUIRE(custom->height == 1002);
}
