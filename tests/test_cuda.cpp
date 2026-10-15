#include <catch2/catch_test_macros.hpp>
#include "r3dss/resample/resampler.hpp"
#include "r3dss/synth/synth.hpp"
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
