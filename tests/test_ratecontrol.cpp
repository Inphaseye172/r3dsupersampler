#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "r3dss/ratecontrol/ratecontrol.hpp"
#include "r3dss/synth/synth.hpp"

using namespace r3dss;
using namespace r3dss::ratecontrol;
using Catch::Approx;

namespace {
BayerFrame make(synth::Pattern p) {
    synth::SynthParams sp;
    sp.width = 256; sp.height = 144; sp.pattern = p;
    return synth::makeFrame(sp);
}
} // namespace

TEST_CASE("Complexity is zero for flat frames and grows with detail") {
    const auto flat = estimateComplexity(make(synth::Pattern::Constant));
    const auto zone = estimateComplexity(make(synth::Pattern::ZonePlate));
    const auto noise = estimateComplexity(make(synth::Pattern::Noise));
    REQUIRE(flat.complexity == Approx(0.0));
    REQUIRE(zone.complexity > flat.complexity);
    REQUIRE(noise.laplacianMean > flat.laplacianMean);
    REQUIRE(zone.complexity <= 1.0);
}

TEST_CASE("VBR budget stays inside the profile and tracks complexity") {
    for (auto q : {QualityProfile::Q1_Maximum, QualityProfile::Q3_High, QualityProfile::Q5_Normal, QualityProfile::Q7_Proxy}) {
        const ProfileParams pp = profileParams(q);
        VbrController vbr(q);
        BayerFrame f = make(synth::Pattern::Constant);
        auto b0 = vbr.next(f, estimateComplexity(f));
        REQUIRE(vbr.lastBpp() == Approx(pp.floorBpp));
        REQUIRE(b0.bits == static_cast<uint64_t>(std::llround(pp.floorBpp * f.sampleCount())));
        ComplexityStats max;
        max.complexity = 1.0;
        double prev = vbr.lastBpp();
        for (int i = 0; i < 30; ++i) {
            vbr.next(f, max);
            REQUIRE(vbr.lastBpp() >= prev - 1e-9); // EMA rises monotonically towards the cap
            REQUIRE(vbr.lastBpp() <= pp.capBpp + 1e-9);
            prev = vbr.lastBpp();
        }
        REQUIRE(vbr.lastBpp() == Approx(pp.capBpp).epsilon(0.01));
    }
    REQUIRE(profileParams(QualityProfile::Q7_Proxy).floorBpp >= 0.5);
}
