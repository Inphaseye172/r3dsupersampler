#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include "r3dss/codec/memory_codec.hpp"
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/core/paths.hpp"
#include "r3dss/resample/resampler.hpp"
#include "r3dss/pipeline/archive_copier.hpp"
#include "r3dss/pipeline/folder_scanner.hpp"
#include "r3dss/pipeline/scheduler.hpp"
#include "r3dss/synth/synth.hpp"

using namespace r3dss;
using namespace r3dss::pipeline;
namespace fs = std::filesystem;

namespace {
fs::path tmpDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("r3dss-test-" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
struct Init {
    Init() { codec::registerBuiltinCodecs(); }
} init;
} // namespace

TEST_CASE("Job JSON round trip") {
    BatchJob b;
    b.name = "n";
    b.outputRoot = "/out";
    b.archiveMode = true;
    b.options.size = SizePreset::FHD;
    b.options.customSize = resample::OutputSize{100, 50};
    b.options.outputFormat = codec::Format::RedFamily;
    b.options.profile = QualityProfile::Q3_High;
    b.options.kernel = ResampleKernel::Area;
    b.options.overwrite = true;
    b.clips.push_back({"/in/a.R3D", "/out", "a_out", "sub"});
    b.copies.push_back({"/in/x.txt", "/out/x.txt"});
    auto back = batchJobFromJson(batchJobToJson(b));
    REQUIRE(back);
    REQUIRE(back->name == "n");
    REQUIRE(back->archiveMode);
    REQUIRE(back->options.size == SizePreset::FHD);
    REQUIRE(back->options.customSize->width == 100);
    REQUIRE(*back->options.outputFormat == codec::Format::RedFamily);
    REQUIRE(back->options.profile == QualityProfile::Q3_High);
    REQUIRE(back->options.kernel == ResampleKernel::Area);
    REQUIRE(back->options.overwrite);
    REQUIRE(back->clips.size() == 1);
    REQUIRE(back->clips[0].outputName == "a_out");
    REQUIRE(back->copies[0].dest == fs::path("/out/x.txt"));
    REQUIRE_FALSE(batchJobFromJson("{not json"));
    REQUIRE(expandName("{stem}_{size}_{profile}", "clip", b.options, codec::Format::RedFamily) == "clip_FHD_Q3");
}

TEST_CASE("Memory clip through the full pipeline keeps geometry, audio and timecode") {
    synth::SynthClipParams p;
    p.frame.width = 512;
    p.frame.height = 288;
    p.frames = 8;
    p.audioChannels = 3;
    p.startTimecode = Timecode::parse("02:00:00:00", p.fps);
    REQUIRE(synth::makeMemoryClip("src", p));
    ClipJob job{"mem://src", "", "dst", ""};
    JobOptions o;
    o.size = SizePreset::Custom;
    o.customSize = resample::OutputSize{256, 144};
    o.outputFormat = codec::Format::Memory;
    o.verify = true;
    ClipRunContext ctx;
    ctx.plan.resampleWorkers = 3;
    int progressCalls = 0;
    ctx.progress = [&](const ClipProgress&) { ++progressCalls; };
    ClipResult r = convertClip(job, o, ctx);
    INFO(r.message);
    REQUIRE(r.status == ClipStatus::Done);
    REQUIRE(r.frames == 8);
    REQUIRE(r.outWidth == 256);
    REQUIRE(r.verification.ok);
    REQUIRE(progressCalls > 0);
    auto out = codec::MemoryStore::instance().get("dst");
    REQUIRE(out);
    REQUIRE(out->frames.size() == 8);
    REQUIRE(out->info.audio.channels == 3);
    REQUIRE(out->info.audio.sampleCount == std::llround(8.0 / 25.0 * 48000));
    REQUIRE(out->info.startTimecode->toString() == "02:00:00:00");
    for (size_t i = 0; i < 8; ++i) REQUIRE(out->frames[i].index == static_cast<int64_t>(i)); // ordered
    // Compare against a direct single-threaded resample of frame 3
    auto src = codec::MemoryStore::instance().get("src");
    auto rs = resample::makeCpuResampler(nullptr);
    BayerFrame ref;
    REQUIRE(rs->resample(src->frames[3], ref, {256, 144, ResampleKernel::Lanczos3, true}));
    REQUIRE(out->frames[3].data == ref.data);
}

TEST_CASE("CinemaDNG clip end to end on disk with verification and output naming") {
    const fs::path dir = tmpDir("e2e");
    synth::SynthClipParams p;
    p.frame.width = 384;
    p.frame.height = 216;
    p.frames = 6;
    p.audioChannels = 2;
    p.startTimecode = Timecode::parse("00:10:00:00", p.fps);
    p.clipName = "A001";
    REQUIRE(synth::writeSyntheticClip(dir / "A001", p));
    ClipJob job{dir / "A001", dir / "out", "", ""};
    JobOptions o;
    o.size = SizePreset::Custom;
    o.customSize = resample::OutputSize{192, 108};
    o.verify = true;
    ClipRunContext ctx;
    ClipResult r = convertClip(job, o, ctx);
    INFO(r.message);
    REQUIRE(r.status == ClipStatus::Done);
    REQUIRE(r.output == dir / "out" / "A001_custom_Q5");
    REQUIRE(fs::exists(dir / "out" / "A001_custom_Q5" / "A001_custom_Q5.wav"));
    REQUIRE(codec::listDngFrames(dir / "out" / "A001_custom_Q5").size() == 6);
    REQUIRE(r.verification.ok);
    REQUIRE(r.averageBudgetBpp >= 1.0);
    // Second run is skipped (exists), overwrite re-runs
    ClipResult r2 = convertClip(job, o, ctx);
    REQUIRE(r2.status == ClipStatus::Skipped);
    o.overwrite = true;
    REQUIRE(convertClip(job, o, ctx).status == ClipStatus::Done);
    // Upscale request is skipped with a message
    o.size = SizePreset::UHD;
    ClipResult r3 = convertClip(job, o, ctx);
    REQUIRE(r3.status == ClipStatus::Skipped);
    // REDCODE output is refused with the documented reason
    o.size = SizePreset::Same;
    o.outputFormat = codec::Format::RedFamily;
    ClipResult r4 = convertClip(job, o, ctx);
    REQUIRE(r4.status == ClipStatus::Failed);
    REQUIRE(r4.message.find("REDCODE cannot be written") != std::string::npos);
}

TEST_CASE("Folder scan, archive copy with hash verification, scheduler and manifest") {
    const fs::path dir = tmpDir("scan");
    synth::SynthClipParams p;
    p.frame.width = 128;
    p.frame.height = 72;
    p.frames = 3;
    p.audioChannels = 0;
    REQUIRE(synth::writeSyntheticClip(dir / "src" / "day1" / "B001", p));
    REQUIRE(synth::writeSyntheticClip(dir / "src" / "day2" / "sub" / "B002", p));
    { std::ofstream(dir / "src" / "day1" / "notes.txt") << "hello"; }
    { std::ofstream(dir / "src" / "readme.md") << "root file"; }
    { std::ofstream(dir / "src" / "fake.R3D") << "not really raw"; }
    ScanOptions so;
    so.archiveMode = true;
    auto scan = scanFolder(dir / "src", dir / "dst", so);
    REQUIRE(scan);
    REQUIRE(scan->clips.size() == 2);
    REQUIRE(scan->clips[0].outputDir == dir / "dst" / "day1");
    REQUIRE(scan->clips[1].outputDir == dir / "dst" / "day2" / "sub");
    REQUIRE(scan->copies.size() == 3); // notes.txt, readme.md, fake.R3D (not a RED container -> copied)
    BatchJob job;
    job.name = "t";
    job.outputRoot = dir / "dst";
    job.archiveMode = true;
    job.options.size = SizePreset::Custom;
    job.options.customSize = resample::OutputSize{64, 36};
    job.clips = scan->clips;
    job.copies = scan->copies;
    Scheduler s;
    BatchCallbacks cb;
    size_t doneCalls = 0;
    cb.onClipDone = [&](size_t, const ClipResult&) { ++doneCalls; };
    BatchResult res = s.run(job, cb);
    REQUIRE(res.done == 2);
    REQUIRE(res.failed == 0);
    REQUIRE(doneCalls == 2);
    REQUIRE(res.copies.size() == 3);
    for (const auto& c : res.copies) REQUIRE(c.verified);
    REQUIRE(fs::exists(dir / "dst" / "day1" / "notes.txt"));
    REQUIRE(fs::exists(dir / "dst" / "day2" / "sub" / "B002_custom_Q5"));
    REQUIRE(fs::exists(res.manifest));
    // Scanning a clip directory directly yields exactly that clip
    auto single = scanFolder(dir / "src" / "day1" / "B001", dir / "x", so);
    REQUIRE(single->clips.size() == 1);
}

TEST_CASE("Cancellation stops a running clip") {
    synth::SynthClipParams p;
    p.frame.width = 1024;
    p.frame.height = 576;
    p.frames = 40;
    p.audioChannels = 0;
    REQUIRE(synth::makeMemoryClip("cancelsrc", p));
    ClipJob job{"mem://cancelsrc", "", "canceldst", ""};
    JobOptions o;
    o.size = SizePreset::Custom;
    o.customSize = resample::OutputSize{512, 288};
    o.outputFormat = codec::Format::Memory;
    ClipRunContext ctx;
    ctx.cancel = std::make_shared<CancellationToken>();
    ctx.progress = [&](const ClipProgress& pr) { if (pr.framesDone >= 2) ctx.cancel->cancel(); };
    ClipResult r = convertClip(job, o, ctx);
    REQUIRE(r.status == ClipStatus::Cancelled);
}

TEST_CASE("Non-ASCII folder and clip names convert end to end and survive job JSON") {
    const fs::path dir = tmpDir("utf8");
    const fs::path clipDir = dir / fromUtf8("Drehtag \xC3\x84") / fromUtf8("\xE6\x92\xAE\xE5\xBD\xB1_A001");
    synth::SynthClipParams p;
    p.frame.width = 128;
    p.frame.height = 72;
    p.frames = 2;
    p.audioChannels = 1;
    p.clipName = toUtf8(clipDir.filename());
    REQUIRE(synth::writeSyntheticClip(clipDir, p));
    BatchJob b;
    b.name = "utf8";
    b.outputRoot = dir / fromUtf8("Ausgabe \xC3\x9C");
    b.options.size = SizePreset::Custom;
    b.options.customSize = resample::OutputSize{64, 36};
    b.clips.push_back({clipDir, b.outputRoot, "", ""});
    REQUIRE(saveBatchJob(b, dir / fromUtf8("job \xC3\xA9.json")));
    auto loaded = loadBatchJob(dir / fromUtf8("job \xC3\xA9.json"));
    REQUIRE(loaded);
    REQUIRE(loaded->clips.at(0).source == clipDir);
    REQUIRE(loaded->outputRoot == b.outputRoot);
    Scheduler s;
    BatchResult res = s.run(*loaded, {});
    REQUIRE(res.done == 1);
    const fs::path out = b.outputRoot / fromUtf8("\xE6\x92\xAE\xE5\xBD\xB1_A001_custom_Q5");
    REQUIRE(codec::listDngFrames(out).size() == 2);
    REQUIRE(fs::exists(out / fromUtf8("\xE6\x92\xAE\xE5\xBD\xB1_A001_custom_Q5.wav")));
}
