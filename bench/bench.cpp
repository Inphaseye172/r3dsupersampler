// r3dss-bench: per-stage and end-to-end throughput on synthetic clips (docs/TEST_PLAN.md §bench).
#include <CLI/CLI.hpp>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include "r3dss/codec/codec.hpp"
#include "r3dss/codec_cdng/ljpeg.hpp"
#include "r3dss/core/hardware.hpp"
#include "r3dss/pipeline/clip_converter.hpp"
#include "r3dss/ratecontrol/ratecontrol.hpp"
#include "r3dss/resample/resampler.hpp"
#include "r3dss/synth/synth.hpp"
#if defined(R3DSS_HAVE_CUDA)
#include "r3dss/resample_cuda/cuda_resampler.hpp"
#endif

using namespace r3dss;
using Clock = std::chrono::steady_clock;

namespace {
double seconds(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }
struct Row { std::string stage; double fps; double extra; std::string note; };
} // namespace

int main(int argc, char** argv) {
    codec::registerBuiltinCodecs();
    CLI::App app{"r3dss benchmark"};
    uint32_t w = 8256, h = 4644, frames = 10;
    std::string size = "UHD", jsonOut, tmpDir;
    bool noGpu = false, skipDisk = false;
    app.add_option("--width", w);
    app.add_option("--height", h);
    app.add_option("--frames", frames);
    app.add_option("--size", size, "6K|UHD|FHD");
    app.add_option("--json", jsonOut, "write results JSON here");
    app.add_option("--tmp", tmpDir, "directory for the disk end-to-end run (default: system temp)");
    app.add_flag("--no-gpu", noGpu);
    app.add_flag("--skip-disk", skipDisk, "skip the CinemaDNG on-disk run");
    CLI11_PARSE(app, argc, argv);

    const HardwareInfo hw = probeHardware();
    auto preset = sizePresetFromName(size).value_or(SizePreset::UHD);
    auto outSize = resample::planOutputSize(w, h, preset, std::nullopt, 2);
    if (!outSize) { std::cerr << outSize.error().toString() << "\n"; return 1; }
    std::vector<Row> rows;
    nlohmann::json j;
    j["hardware"] = {{"cores", hw.logicalCores}, {"ram_mib", hw.totalRamBytes >> 20}};
    j["source"] = {{"width", w}, {"height", h}, {"frames", frames}};
    j["output"] = {{"width", outSize->width}, {"height", outSize->height}};
    std::cout << "source " << w << "x" << h << " -> " << outSize->width << "x" << outSize->height << ", " << frames
              << " frames, " << hw.logicalCores << " cores\n";

    synth::SynthParams sp;
    sp.width = w; sp.height = h; sp.pattern = synth::Pattern::Scene;
    auto t = Clock::now();
    BayerFrame src = synth::makeFrame(sp);
    rows.push_back({"synthetic frame generation", 1.0 / seconds(t), 0, ""});

    resample::ResampleParams rp;
    rp.outWidth = outSize->width; rp.outHeight = outSize->height;
    BayerFrame out;
    {
        auto rs = resample::makeCpuResampler(nullptr);
        rs->resample(src, out, rp); // warm up (tap tables)
        t = Clock::now();
        for (uint32_t i = 0; i < frames; ++i) rs->resample(src, out, rp);
        rows.push_back({"resample CPU, 1 thread", frames / seconds(t), 0, "lanczos3 + clip-aware"});
    }
    {
        auto pool = std::make_shared<ThreadPool>(hw.logicalCores);
        auto rs = resample::makeCpuResampler(pool);
        rs->resample(src, out, rp);
        t = Clock::now();
        for (uint32_t i = 0; i < frames; ++i) rs->resample(src, out, rp);
        rows.push_back({"resample CPU, " + std::to_string(hw.logicalCores) + " threads (one frame)", frames / seconds(t), 0, ""});
    }
#if defined(R3DSS_HAVE_CUDA)
    if (!noGpu && resample::cudaDeviceCount() > 0) {
        auto rs = resample::makeCudaResampler();
        rs->resample(src, out, rp);
        t = Clock::now();
        for (uint32_t i = 0; i < frames; ++i) rs->resample(src, out, rp);
        rows.push_back({"resample CUDA (incl. H2D/D2H)", frames / seconds(t), 0, resample::cudaDeviceName(0)});
    }
#else
    (void)noGpu;
#endif
    {
        t = Clock::now();
        for (uint32_t i = 0; i < frames; ++i) ratecontrol::estimateComplexity(out);
        rows.push_back({"complexity estimate (output size)", frames / seconds(t), 0, ""});
    }
    {
        codec::ljpeg::EncodeParams ep; ep.components = 2;
        t = Clock::now();
        size_t bytes = 0;
        for (uint32_t i = 0; i < std::min(frames, 3u); ++i) {
            auto e = codec::ljpeg::encode(out.data.data(), out.width, out.height, ep);
            bytes = e ? e->size() : 0;
        }
        const double fps = std::min(frames, 3u) / seconds(t);
        rows.push_back({"lossless JPEG encode, 1 thread (output frame)", fps, bytes * 8.0 / out.sampleCount(), "bpp"});
    }
    {
        synth::SynthClipParams cp;
        cp.frame = sp; cp.frames = frames; cp.audioChannels = 2;
        const std::string name = "bench";
        auto mk = synth::makeMemoryClip(name, cp);
        if (!mk) { std::cerr << mk.error().toString() << "\n"; return 1; }
        pipeline::ClipJob job{"mem://" + name, "", "bench_out", ""};
        pipeline::JobOptions o;
        o.size = preset; o.outputFormat = codec::Format::Memory; o.verify = false; o.useGpu = !noGpu;
        pipeline::ClipRunContext ctx;
        HardwareInfo hw2 = hw;
        hw2.cudaAvailable = pipeline::chooseResampler(o.useGpu).cuda;
        ctx.plan = planThreads(hw2, src.byteSize());
        auto r = pipeline::convertClip(job, o, ctx);
        rows.push_back({"full pipeline, memory codec (" + r.resampler + ")", r.frames / r.seconds, 0, r.message});
    }
    if (!skipDisk) {
        namespace fs = std::filesystem;
        fs::path base = tmpDir.empty() ? fs::temp_directory_path() / "r3dss-bench" : fs::path(tmpDir);
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base, ec);
        synth::SynthClipParams cp;
        cp.frame = sp; cp.frames = frames; cp.audioChannels = 2;
        t = Clock::now();
        auto wr = synth::writeSyntheticClip(base / "src", cp);
        if (!wr) { std::cerr << wr.error().toString() << "\n"; return 1; }
        rows.push_back({"write synthetic CinemaDNG source (" + std::to_string(wr->sourceBytes >> 20) + " MiB)", frames / seconds(t), 0, ""});
        pipeline::ClipJob job{base / "src", base / "out", "", ""};
        pipeline::JobOptions o;
        o.size = preset; o.verify = true; o.useGpu = !noGpu;
        pipeline::ClipRunContext ctx;
        HardwareInfo hw2 = hw;
        hw2.cudaAvailable = pipeline::chooseResampler(o.useGpu).cuda;
        ctx.plan = planThreads(hw2, src.byteSize());
        auto r = pipeline::convertClip(job, o, ctx);
        rows.push_back({"full pipeline, CinemaDNG disk -> disk incl. verify (" + r.resampler + ")", r.frames / r.seconds,
                        static_cast<double>(r.outputBytes >> 20), r.message + ", MiB written"});
        fs::remove_all(base, ec);
    }

    std::cout << std::left << std::setw(64) << "stage" << std::right << std::setw(10) << "fps" << "  note\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(64) << r.stage << std::right << std::setw(10) << std::fixed << std::setprecision(2)
                  << r.fps << "  " << r.note;
        if (r.extra > 0) std::cout << " " << std::setprecision(2) << r.extra;
        std::cout << "\n";
        j["rows"].push_back({{"stage", r.stage}, {"fps", r.fps}, {"extra", r.extra}, {"note", r.note}});
    }
    if (!jsonOut.empty()) {
        std::ofstream jsonFile(jsonOut);
        jsonFile << j.dump(2);
    }
    return 0;
}
