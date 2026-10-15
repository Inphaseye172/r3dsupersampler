#include <catch2/catch_test_macros.hpp>
#include "r3dss/resample/resampler.hpp"
#include "r3dss/synth/synth.hpp"
#include "resample_fixtures.hpp"
#if defined(R3DSS_HAVE_CUDA)
#include "r3dss/resample_cuda/cuda_resampler.hpp"
#endif

using namespace r3dss;

TEST_CASE("CUDA resampler parity with CPU (<= 1 LSB)", "[cuda]") {
#if defined(R3DSS_HAVE_CUDA)
    if (resample::cudaDeviceCount() == 0) {
        SKIP("no CUDA device");
    }
    synth::SynthParams sp;
    sp.width = 1024; sp.height = 576; sp.pattern = synth::Pattern::Scene;
    BayerFrame in = synth::makeFrame(sp);
    auto cpu = resample::makeCpuResampler(nullptr);
    auto gpu = resample::makeCudaResampler();
    REQUIRE(gpu);
    for (auto k : {ResampleKernel::Lanczos3, ResampleKernel::Area}) {
        BayerFrame a, b;
        resample::ResampleParams p{640, 360, k, true};
        REQUIRE(cpu->resample(in, a, p));
        REQUIRE(gpu->resample(in, b, p));
        REQUIRE(a.data.size() == b.data.size());
        int maxDiff = 0;
        for (size_t i = 0; i < a.data.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(a.data[i]) - int(b.data[i])));
        REQUIRE(maxDiff <= 1);
    }
#else
    SKIP("built without CUDA");
#endif
}

TEST_CASE("CUDA resampler follows the same clipping rule as the CPU (I5)", "[cuda]") {
#if defined(R3DSS_HAVE_CUDA)
    if (resample::cudaDeviceCount() == 0) {
        SKIP("no CUDA device");
    }
    auto cpu = resample::makeCpuResampler(nullptr);
    auto gpu = resample::makeCudaResampler();
    REQUIRE(gpu);
    struct Case { uint16_t bits; uint32_t white; bool disc; bool hot; };
    // Unclipped near-white bar (white 4095 / 3800 in 12 bits, 15000 in 14 bits); clipped disc + hot sites.
    for (const Case c : {Case{12, 4095, false, false}, Case{12, 3800, false, false}, Case{14, 15000, false, false},
                         Case{12, 3800, true, true}}) {
        BayerFrame in = testing::highlightFrame(1280, 720, c.bits, c.white, 64, c.disc, c.hot);
        for (auto k : {ResampleKernel::Lanczos3, ResampleKernel::Area})
            for (bool clipAware : {true, false}) {
                CAPTURE(c.bits, c.white, c.disc, kernelName(k), clipAware);
                BayerFrame a, b;
                resample::ResampleParams p{640, 360, k, clipAware};
                REQUIRE(cpu->resample(in, a, p));
                REQUIRE(gpu->resample(in, b, p));
                REQUIRE(a.data.size() == b.data.size());
                int maxDiff = 0;
                uint16_t gpuMax = 0;
                for (size_t i = 0; i < a.data.size(); ++i) {
                    maxDiff = std::max(maxDiff, std::abs(int(a.data[i]) - int(b.data[i])));
                    gpuMax = std::max(gpuMax, b.data[i]);
                }
                REQUIRE(maxDiff <= 1);
                REQUIRE(gpuMax <= c.white);                                        // never above WhiteLevel
                if (!c.disc && clipAware && k == ResampleKernel::Lanczos3) REQUIRE(gpuMax < c.white); // no new clip
            }
    }
#else
    SKIP("built without CUDA");
#endif
}
