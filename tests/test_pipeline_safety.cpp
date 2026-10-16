// Output safety, naming, cancellation, archive and verification behaviour of the pipeline
// (collisions, partial outputs, overwrite, source protection, exit codes, sidecars, read-back checks).
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include "r3dss/codec/memory_codec.hpp"
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/core/paths.hpp"
#include "r3dss/core/sha256.hpp"
#include "r3dss/pipeline/archive_copier.hpp"
#include "r3dss/pipeline/folder_scanner.hpp"
#include "r3dss/pipeline/output_claim.hpp"
#include "r3dss/pipeline/scheduler.hpp"
#include "r3dss/synth/synth.hpp"

using namespace r3dss;
using namespace r3dss::pipeline;
namespace fs = std::filesystem;

namespace {
fs::path freshDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("r3dss-safety-" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
struct Init {
    Init() { codec::registerBuiltinCodecs(); }
} init;

synth::SynthClipParams smallClip(int64_t frames = 4, uint32_t audio = 2, uint32_t seed = 1, const std::string& name = "synthetic") {
    synth::SynthClipParams p;
    p.clipName = name;
    p.frame.width = 128;
    p.frame.height = 72;
    p.frame.seed = seed;
    p.frames = frames;
    p.audioChannels = audio;
    return p;
}

JobOptions customOptions(uint32_t w, uint32_t h) {
    JobOptions o;
    o.size = SizePreset::Custom;
    o.customSize = resample::OutputSize{w, h};
    return o;
}

size_t entryCount(const fs::path& dir) {
    std::error_code ec;
    size_t n = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) ++n;
    return n;
}

std::string treeHash(const fs::path& dir) {
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::string all;
    for (const auto& f : files) all += toUtf8(f.lexically_relative(dir)) + ":" + *Sha256::hashFile(f) + "\n";
    return Sha256::hashBytes(all.data(), all.size());
}

void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("Output names: clip folder stem, normalised paths, WxH for custom sizes, lossless profile (I3)") {
    REQUIRE(normaliseSourcePath(fs::path("a") / "C001" / "") == fs::path("a") / "C001");
    REQUIRE(normaliseSourcePath(fs::path("a") / "C001" / ".") == fs::path("a") / "C001");
    REQUIRE(normaliseSourcePath("mem://x") == fs::path("mem://x"));
    REQUIRE(clipStem(fs::path("cards") / "C001" / "") == "C001");
    REQUIRE(clipStem("mem://src") == "src");

    const fs::path dir = freshDir("stem");
    REQUIRE(synth::writeSyntheticClip(dir / "clipN", smallClip(2, 0, 1, "clipN")));
    REQUIRE(clipStem(dir / "clipN" / "clipN_000001.dng") == "clipN"); // a frame names the whole clip
    REQUIRE(clipStem(dir / "clipN" / "") == "clipN");
    REQUIRE(clipStem(dir / "A001.R3D") == "A001");

    JobOptions o = customOptions(1281, 721);
    REQUIRE(expandName("{stem}_{size}_{profile}", "C001", o, codec::Format::CinemaDNG) == "C001_1282x722_lossless");
    o.size = SizePreset::TwoK;
    REQUIRE(expandName("{stem}_{size}_{profile}", "C001", o, codec::Format::CinemaDNG) == "C001_2K_lossless");
    o.size = SizePreset::Same;
    REQUIRE(expandName("{stem}_{size}", "C001", o, codec::Format::CinemaDNG) == "C001_same");

    // A .dng frame and a trailing separator both produce the folder-named output
    auto pf = planClipOutput(ClipJob(dir / "clipN" / "clipN_000001.dng", dir / "out"), customOptions(64, 36));
    REQUIRE(pf);
    REQUIRE(pf->dest == dir / "out" / "clipN_64x36_lossless");
    auto ps = planClipOutput(ClipJob(dir / "clipN" / "", dir / "out"), customOptions(64, 36));
    REQUIRE(ps);
    REQUIRE(ps->dest == pf->dest);
}

TEST_CASE("Strict size parsing for --size and job files (R8)") {
    JobOptions o;
    REQUIRE(parseSizeOption("960x540", o).empty());
    REQUIRE(o.size == SizePreset::Custom);
    REQUIRE(o.customSize->width == 960);
    REQUIRE(o.customSize->height == 540);
    REQUIRE(parseSizeOption("UHD", o).empty());
    REQUIRE(o.size == SizePreset::UHD);
    REQUIRE_FALSE(o.customSize);
    for (const char* bad : {"0x0", "3840x-2", "1280x720junk", "x720", "1280x", "1280X720", "custom", " 960x540", "14x16",
                            "65535x100", "9999999x10", "+960x540", "960x540x2"}) {
        JobOptions b;
        INFO(bad);
        REQUIRE_FALSE(parseSizeOption(bad, b).empty());
        REQUIRE(b.size == SizePreset::UHD); // unchanged on error
    }
    REQUIRE(parseSizeOption("15x16", o).empty()); // 15 rounds up to 16
    REQUIRE(parseSizeOption("65534x16", o).empty());
    REQUIRE(checkCustomSize(16, 16).empty());
    REQUIRE_FALSE(checkCustomSize(0, 100).empty());
    REQUIRE_FALSE(checkCustomSize(-2, 100).empty());

    REQUIRE_FALSE(batchJobFromJson(R"({"options":{"size":"custom","customSize":{"width":0,"height":0}}})"));
    REQUIRE_FALSE(batchJobFromJson(R"({"options":{"size":"custom","customSize":{"width":3840,"height":-2}}})"));
    REQUIRE_FALSE(batchJobFromJson(R"({"options":{"size":"custom"}})"));
    auto ok = batchJobFromJson(R"({"options":{"size":"custom","customSize":{"width":960,"height":540}},
                                   "clips":[{"source":"/x/C001/","outputDir":"/o","sidecars":["meta.xml","proxy/p.mov"]}]})");
    REQUIRE(ok);
    REQUIRE(ok->clips[0].source == fs::path("/x/C001")); // trailing separator removed on load
    REQUIRE(ok->clips[0].sidecars.size() == 2);
    auto back = batchJobFromJson(batchJobToJson(*ok));
    REQUIRE(back);
    REQUIRE(back->clips[0].sidecars[1] == fs::path("proxy/p.mov"));

    // convertClip refuses an invalid custom size even when a caller bypasses the parsers
    REQUIRE(synth::makeMemoryClip("r8src", smallClip(1, 0)));
    JobOptions zero = customOptions(0, 0);
    zero.outputFormat = codec::Format::Memory;
    ClipResult r = convertClip(ClipJob("mem://r8src", "", "r8dst"), zero, {});
    REQUIRE(r.status == ClipStatus::Failed);
    REQUIRE(r.message.find("invalid output size") != std::string::npos);
}

TEST_CASE("Clips whose outputs collide fail up front instead of mixing frames (PIPE-1)") {
    const fs::path dir = freshDir("collide");
    REQUIRE(synth::writeSyntheticClip(dir / "cardA" / "C001", smallClip(5, 2, 1)));
    REQUIRE(synth::writeSyntheticClip(dir / "cardB" / "C001", smallClip(7, 2, 7)));
    for (unsigned parallel : {2u, 1u}) {
        const fs::path out = dir / ("out" + std::to_string(parallel));
        BatchJob job;
        job.options = customOptions(64, 36);
        job.options.maxParallelClips = parallel;
        job.clips.push_back(ClipJob(dir / "cardA" / "C001", out));
        job.clips.push_back(ClipJob(dir / "cardB" / "C001" / "", out)); // trailing separator: same clip name
        Scheduler s;
        BatchResult res = s.run(job, {});
        INFO(res.clips[1].message);
        REQUIRE(res.done == 1);
        REQUIRE(res.failed == 1);
        REQUIRE(res.skipped == 0);
        REQUIRE(res.clips[1].status == ClipStatus::Failed);
        REQUIRE(res.clips[1].message.find("is also the output of clip 1") != std::string::npos);
        REQUIRE(batchExitCode(res) == 1);
        const fs::path dest = out / "C001_64x36_lossless";
        REQUIRE(codec::listDngFrames(dest).size() == 5); // only card A's frames
        REQUIRE(entryCount(out) == 1);                   // no partial left behind
    }
}

TEST_CASE("Failed and cancelled clips leave no output; a re-run converts (PIPE-3, PIPE-4, I4)") {
    const fs::path dir = freshDir("partial");
    REQUIRE(synth::writeSyntheticClip(dir / "F1", smallClip(8, 2, 1, "F1")));
    const fs::path frame = dir / "F1" / "F1_000004.dng";
    const fs::path keep = dir / "good4.dng";
    fs::copy_file(frame, keep);
    fs::resize_file(frame, 2000);
    const ClipJob job(dir / "F1", dir / "out");
    const JobOptions o = customOptions(64, 36);
    ClipResult r = convertClip(job, o, {});
    REQUIRE(r.status == ClipStatus::Failed);
    REQUIRE(r.files.empty());
    REQUIRE(entryCount(dir / "out") == 0); // neither the output nor its .partial folder
    fs::copy_file(keep, frame, fs::copy_options::overwrite_existing);
    r = convertClip(job, o, {});
    INFO(r.message);
    REQUIRE(r.status == ClipStatus::Done);
    REQUIRE(r.output == dir / "out" / "F1_64x36_lossless");
    REQUIRE(r.files.size() == 9);
    for (const auto& f : r.files) REQUIRE(f.parent_path() == r.output); // paths of the published folder
    REQUIRE_FALSE(fs::exists(dir / "out" / "F1_64x36_lossless" / OutputClaim::ownerFileName()));
    r = convertClip(job, o, {});
    REQUIRE(r.status == ClipStatus::Skipped);
    REQUIRE(r.skipReason == SkipReason::OutputExists);

    // Cancel half way through a clip written to disk
    REQUIRE(synth::writeSyntheticClip(dir / "K1", [] { auto p = smallClip(40, 2); p.frame.width = 512; p.frame.height = 288; return p; }()));
    ClipRunContext ctx;
    ctx.cancel = std::make_shared<CancellationToken>();
    ctx.progress = [&](const ClipProgress& p) { if (p.framesDone >= 3) ctx.cancel->cancel(); };
    r = convertClip(ClipJob(dir / "K1", dir / "outK"), customOptions(256, 144), ctx);
    REQUIRE(r.status == ClipStatus::Cancelled);
    REQUIRE(r.files.empty());
    REQUIRE(entryCount(dir / "outK") == 0);
}

TEST_CASE("Overwrite replaces an output only after the new one is complete (PIPE-4)") {
    const fs::path dir = freshDir("overwrite");
    REQUIRE(synth::writeSyntheticClip(dir / "F1", smallClip(6, 2, 1, "F1")));
    const ClipJob job(dir / "F1", dir / "out");
    JobOptions o = customOptions(64, 36);
    REQUIRE(convertClip(job, o, {}).status == ClipStatus::Done);
    const fs::path dest = dir / "out" / "F1_64x36_lossless";
    const std::string before = treeHash(dest);
    const fs::path frame = dir / "F1" / "F1_000003.dng";
    fs::copy_file(frame, dir / "keep.dng");
    fs::resize_file(frame, 1000);
    o.overwrite = true;
    ClipResult r = convertClip(job, o, {});
    REQUIRE(r.status == ClipStatus::Failed);
    REQUIRE(treeHash(dest) == before); // the previous output survives a failed re-run
    REQUIRE(entryCount(dir / "out") == 1);
    fs::copy_file(dir / "F1" / "F1_000002.dng", frame, fs::copy_options::overwrite_existing);
    r = convertClip(job, o, {});
    REQUIRE(r.status == ClipStatus::Done);
    REQUIRE(treeHash(dest) != before);
    REQUIRE(entryCount(dir / "out") == 1); // nothing "replaced-..." or ".partial" left
}

TEST_CASE("Destinations that equal, contain or lie inside a source are refused (B1)") {
    const fs::path dir = freshDir("overlap");
    REQUIRE(synth::writeSyntheticClip(dir / "src" / "F1", smallClip(3, 0)));
    const std::string srcHash = treeHash(dir / "src");
    JobOptions o = customOptions(64, 36);
    o.overwrite = true;
    // Output folder inside the clip
    ClipResult r = convertClip(ClipJob(dir / "src" / "F1", dir / "src" / "F1"), o, {});
    REQUIRE(r.status == ClipStatus::Failed);
    // Output equal to the clip (job file naming it like the source)
    r = convertClip(ClipJob(dir / "src" / "F1", dir / "src", "F1"), o, {});
    REQUIRE(r.status == ClipStatus::Failed);
    REQUIRE(r.message.find("overlap the source") != std::string::npos);
    // Output containing the clip, caught before anything starts
    BatchJob job;
    job.options = o;
    job.clips.push_back(ClipJob(dir / "src" / "F1", dir, "src"));
    Scheduler s;
    BatchResult res = s.run(job, {});
    REQUIRE(res.failed == 1);
    REQUIRE(res.clips[0].message.find("would contain") != std::string::npos);
    REQUIRE(treeHash(dir / "src") == srcHash);
    REQUIRE(pathsOverlap(dir / "a", dir / "a" / "b"));
    REQUIRE(pathsOverlap(dir / "a" / "b", dir / "a"));
    REQUIRE_FALSE(pathsOverlap(dir / "a", dir / "ab"));
}

TEST_CASE("Output claims: live owners block, leftovers of ended processes are reclaimed (I4)") {
    const fs::path dir = freshDir("claim");
    const fs::path dest = dir / "C001_64x36_lossless";
    const fs::path partial = partialPathFor(dest);
    std::string ownText;
    {
        auto c = OutputClaim::claim(dest, true);
        REQUIRE(c);
        REQUIRE(fs::is_directory(partial));
        ownText = readText(partial / OutputClaim::ownerFileName());
        REQUIRE(ownText.find("pid ") != std::string::npos);
        // A second claim while the first is held: destination in use
        auto c2 = OutputClaim::claim(dest, true);
        REQUIRE_FALSE(c2);
        REQUIRE(c2.error().message.find("destination in use") != std::string::npos);
    }
    REQUIRE_FALSE(fs::exists(partial)); // released claim removes its partial folder

    auto plant = [&](const std::string& owner) {
        fs::create_directories(partial);
        if (!owner.empty()) writeText(partial / OutputClaim::ownerFileName(), owner);
        writeText(partial / "C001_64x36_lossless_000000.dng", "stale frame");
    };
    // Owned by this (running) process
    plant(ownText);
    REQUIRE_FALSE(OutputClaim::claim(dest, true));
    REQUIRE(fs::exists(partial / "C001_64x36_lossless_000000.dng"));
    fs::remove_all(partial);
    // No owner marker: cannot tell, so it is left alone
    plant("");
    REQUIRE_FALSE(OutputClaim::claim(dest, true));
    fs::remove_all(partial);
    // Another machine
    std::string other = ownText;
    other.replace(other.find("host "), other.find('\n', other.find("host ")) - other.find("host "), "host some-other-machine");
    plant(other);
    REQUIRE_FALSE(OutputClaim::claim(dest, true));
    fs::remove_all(partial);
    // A process that no longer runs: reclaimed, stale content removed
    std::string dead = ownText;
    const size_t pidPos = dead.find("pid ");
    dead.replace(pidPos, dead.find('\n', pidPos) - pidPos, "pid 2147480000");
    plant(dead);
    {
        auto c = OutputClaim::claim(dest, true);
        INFO((c ? std::string() : c.error().message));
        REQUIRE(c);
        REQUIRE_FALSE(fs::exists(partial / "C001_64x36_lossless_000000.dng"));
        writeText(partial / "f.dng", "new");
        REQUIRE(c->commit(false));
    }
    REQUIRE(fs::exists(dest / "f.dng"));
    REQUIRE_FALSE(fs::exists(dest / OutputClaim::ownerFileName()));
    REQUIRE(entryCount(dir) == 1);

    // commit(false) never replaces an output that appeared meanwhile; commit(true) does, keeping it on failure
    {
        auto c = OutputClaim::claim(dest, true);
        REQUIRE(c);
        writeText(partial / "f.dng", "newer");
        REQUIRE_FALSE(c->commit(false));
        REQUIRE(readText(dest / "f.dng") == "new");
        REQUIRE_FALSE(c->commit(true, {dest / "f.dng"})); // would replace a protected source
        REQUIRE(readText(dest / "f.dng") == "new");
        REQUIRE(c->commit(true));
    }
    REQUIRE(readText(dest / "f.dng") == "newer");
    REQUIRE(entryCount(dir) == 1);
    REQUIRE(renameNoReplace(dir / "missing", dir / "x").hasValue() == false);
}

TEST_CASE("Skip reasons and batch exit codes (R5, I2)") {
    const fs::path dir = freshDir("exitcodes");
    REQUIRE(synth::writeSyntheticClip(dir / "HD", smallClip(1, 0)));
    BatchJob job;
    job.options.size = SizePreset::UHD; // larger than the 128x72 source
    job.clips.push_back(ClipJob(dir / "HD", dir / "out"));
    BatchResult res = Scheduler().run(job, {});
    REQUIRE(res.skipped == 1);
    REQUIRE(res.skippedExisting == 0);
    REQUIRE(res.clips[0].skipReason == SkipReason::Other);
    REQUIRE(batchExitCode(res) == 3);

    job.options = customOptions(64, 36);
    res = Scheduler().run(job, {});
    REQUIRE(batchExitCode(res) == 0);
    res = Scheduler().run(job, {}); // output already exists
    REQUIRE(res.skippedExisting == 1);
    REQUIRE(res.clips[0].skipReason == SkipReason::OutputExists);
    REQUIRE(batchExitCode(res) == 0);

    BatchResult r;
    r.done = 2;
    REQUIRE(batchExitCode(r) == 0);
    r.skipped = 1;
    REQUIRE(batchExitCode(r) == 3); // something was not written
    r.failed = 1;
    REQUIRE(batchExitCode(r) == 1);
    BatchResult c;
    c.aborted = true;
    REQUIRE(batchExitCode(c) == 1);
    BatchResult e;
    e.copyErrors.push_back("x");
    REQUIRE(batchExitCode(e) == 1);
}

TEST_CASE("Custom sizes: aspect warning (R6) and no rate-control budget for CinemaDNG (R4)") {
    synth::SynthClipParams p = smallClip(1, 0);
    p.frame.width = 512;
    p.frame.height = 288;
    REQUIRE(synth::makeMemoryClip("aspectsrc", p));
    JobOptions o = customOptions(256, 200);
    o.outputFormat = codec::Format::Memory;
    ClipResult r = convertClip(ClipJob("mem://aspectsrc", "", "aspectdst"), o, {});
    REQUIRE(r.status == ClipStatus::Done);
    REQUIRE(r.outWidth == 256);
    REQUIRE(r.outHeight == 200); // the exact size asked for is kept
    REQUIRE(r.warnings.size() == 1);
    REQUIRE(r.warnings[0].find("anamorphic") != std::string::npos);
    o.customSize = resample::OutputSize{256, 144};
    r = convertClip(ClipJob("mem://aspectsrc", "", "aspectdst2"), o, {});
    REQUIRE(r.warnings.empty());
    o.customSize = resample::OutputSize{258, 144}; // 0.8 % off
    REQUIRE(convertClip(ClipJob("mem://aspectsrc", "", "aspectdst3"), o, {}).warnings.size() == 1);
    o.customSize = resample::OutputSize{400, 226}; // 0.4 % off: rounding, no warning
    REQUIRE(convertClip(ClipJob("mem://aspectsrc", "", "aspectdst4"), o, {}).warnings.empty());

    const fs::path dir = freshDir("r4");
    REQUIRE(synth::writeSyntheticClip(dir / "C1", smallClip(2, 0)));
    for (QualityProfile q : {QualityProfile::Q1_Maximum, QualityProfile::Q7_Proxy}) {
        JobOptions d = customOptions(64, 36);
        d.profile = q;
        d.overwrite = true;
        ClipResult rr = convertClip(ClipJob(dir / "C1", dir / "out"), d, {});
        REQUIRE(rr.status == ClipStatus::Done);
        REQUIRE_FALSE(rr.rateControl);
        REQUIRE(rr.averageBudgetBpp == 0.0);
        REQUIRE(rr.output.filename() == "C1_64x36_lossless");
    }
}


namespace {
Expectation expectationFor(const fs::path& source, uint32_t w, uint32_t h) {
    auto f = codec::CodecRegistry::instance().probe(source);
    REQUIRE(f);
    auto rd = f->makeReader();
    auto info = rd->open(source);
    REQUIRE(info);
    Expectation e;
    e.width = w;
    e.height = h;
    e.frameCount = info->frameCount;
    e.fps = info->fps;
    e.audio = info->audio;
    e.audioSamples = info->audio.sampleCount;
    e.audioReference = source;
    e.startTimecode = info->startTimecode;
    return e;
}

void scribble(const fs::path& p, uint64_t offset, size_t n) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(offset));
    for (size_t i = 0; i < n; ++i) f.put(static_cast<char>((i * 37) & 0xFF));
}
} // namespace

TEST_CASE("Verification reads every file back and checks every frame and the audio samples (PIPE-5)") {
    const fs::path dir = freshDir("verify");
    REQUIRE(synth::writeSyntheticClip(dir / "src", smallClip(8, 2)));
    JobOptions o;
    o.size = SizePreset::Same;
    o.verify = false;
    ClipResult r = convertClip(ClipJob(dir / "src", dir / "out"), o, {});
    REQUIRE(r.status == ClipStatus::Done);
    const fs::path out = r.output;
    fs::copy(out, dir / "backup", fs::copy_options::recursive);
    Expectation e = expectationFor(dir / "src", 128, 72);
    for (const auto& f : r.files) e.fileDigests.emplace_back(f, *fileDigest(f));
    VerificationResult v = verifyOutput(out, e);
    INFO((v.problems.empty() ? std::string() : v.problems[0]));
    REQUIRE(v.ok);

    // Garbage in the middle of frame 4: caught by the read-back comparison
    const fs::path f4 = out / "src_same_lossless_000004.dng";
    scribble(f4, fs::file_size(f4) / 3, 512);
    v = verifyOutput(out, e);
    REQUIRE_FALSE(v.ok);
    REQUIRE(v.problems[0].find("src_same_lossless_000004.dng") != std::string::npos);

    // A truncated middle frame is caught by decoding every frame (not only the first and last)
    Expectation noDigest = e;
    noDigest.fileDigests.clear();
    fs::resize_file(f4, 1000);
    v = verifyOutput(out, noDigest);
    REQUIRE_FALSE(v.ok);
    bool decodeProblem = false;
    for (const auto& p : v.problems) decodeProblem |= p.find("frame 4 does not decode") != std::string::npos;
    REQUIRE(decodeProblem);
    fs::copy_file(dir / "backup" / "src_same_lossless_000004.dng", f4, fs::copy_options::overwrite_existing);
    REQUIRE(verifyOutput(out, noDigest).ok);

    // Corrupted audio samples are caught by comparing with the source
    const fs::path wav = out / "src_same_lossless.wav";
    scribble(wav, fs::file_size(wav) / 2, 256);
    v = verifyOutput(out, noDigest);
    REQUIRE_FALSE(v.ok);
    bool audioProblem = false;
    for (const auto& p : v.problems) audioProblem |= p.find("audio samples differ") != std::string::npos;
    REQUIRE(audioProblem);
    // Missing audio samples against the source's length
    noDigest.audioReference.clear();
    noDigest.audioSamples += 48000;
    REQUIRE_FALSE(verifyOutput(out, noDigest).ok);
}

TEST_CASE("Archive copies never replace a different file and hash the source independently (B3, B4)") {
    const fs::path dir = freshDir("archive");
    writeText(dir / "src" / "a.txt", "hello");
    auto r = copyAndVerify(dir / "src" / "a.txt", dir / "dst" / "a.txt");
    REQUIRE(r);
    REQUIRE(r->verified);
    REQUIRE_FALSE(r->alreadyPresent);
    REQUIRE(r->sourceSha256 == Sha256::hashBytes("hello", 5));
    REQUIRE(r->destSha256 == r->sourceSha256);
    REQUIRE_FALSE(fs::exists(dir / "dst" / "a.txt.part"));
    auto again = copyAndVerify(dir / "src" / "a.txt", dir / "dst" / "a.txt");
    REQUIRE(again);
    REQUIRE(again->alreadyPresent);
    writeText(dir / "src" / "b.txt", "mine");
    writeText(dir / "dst" / "b.txt", "someone else's");
    auto conflict = copyAndVerify(dir / "src" / "b.txt", dir / "dst" / "b.txt");
    REQUIRE_FALSE(conflict);
    REQUIRE(conflict.error().code == ErrorCode::Verification);
    REQUIRE(readText(dir / "dst" / "b.txt") == "someone else's");
    REQUIRE_FALSE(copyAndVerify(dir / "src" / "a.txt", dir / "src" / "a.txt"));

    REQUIRE(writeManifest(dir / "m.json", {*r, *again}, {dir / "dst" / "a.txt", dir / "dst" / "gone.dng"}, false));
    auto j = nlohmann::json::parse(readText(dir / "m.json"));
    REQUIRE(j["complete"] == false);
    REQUIRE(j["copies"].size() == 2);
    REQUIRE(j["copies"][0]["sourceSha256"] == r->sourceSha256);
    REQUIRE(j["copies"][0]["destSha256"] == r->destSha256);
    REQUIRE(j["copies"][1]["alreadyArchived"] == true);
    REQUIRE(j["produced"].size() == 1); // gone.dng does not exist
    fs::remove(dir / "dst" / "a.txt");
    REQUIRE(writeManifest(dir / "m.json", {*r}, {}));
    REQUIRE(nlohmann::json::parse(readText(dir / "m.json"))["copies"].empty());
}

TEST_CASE("Folder scan: sidecars, nested clips, output root excluded, loops (PIPE-6, B2)") {
    const fs::path dir = freshDir("scan2");
    const fs::path root = dir / "root";
    REQUIRE(synth::writeSyntheticClip(root / "day1" / "C001", smallClip(2, 2)));
    writeText(root / "day1" / "C001" / "C001_meta.xml", "xml");
    writeText(root / "day1" / "C001" / "proxy" / "C001.mov", "mov");
    writeText(root / "day1" / "C001" / ".DS_Store", "junk");
    REQUIRE(synth::writeSyntheticClip(root / "day1" / "C001" / "sub" / "C009", smallClip(2, 0)));
    writeText(root / "day1" / "notes.txt", "notes");
    REQUIRE(synth::writeSyntheticClip(root / "converted" / "OLD_64x36_lossless", smallClip(1, 0)));
#ifndef _WIN32
    fs::create_directory_symlink("..", root / "day1" / "loop");
#endif
    ScanOptions so;
    so.archiveMode = true;
    auto s = scanFolder(root, root / "converted", so);
    REQUIRE(s);
    REQUIRE(s->clips.size() == 2); // C001 and the nested C009; nothing from the output root
    const ClipJob* c001 = nullptr;
    const ClipJob* c009 = nullptr;
    for (const auto& c : s->clips) {
        if (c.source.filename() == "C001") c001 = &c;
        if (c.source.filename() == "C009") c009 = &c;
    }
    REQUIRE(c001);
    REQUIRE(c009);
    REQUIRE(c001->sidecars.size() == 2);
    REQUIRE(std::find(c001->sidecars.begin(), c001->sidecars.end(), fs::path("C001_meta.xml")) != c001->sidecars.end());
    REQUIRE(std::find(c001->sidecars.begin(), c001->sidecars.end(), fs::path("proxy") / "C001.mov") != c001->sidecars.end());
    REQUIRE(c009->outputDir == root / "converted" / "day1" / "C001" / "sub");
    REQUIRE(s->copies.size() == 1);
    REQUIRE(s->copies[0].source == root / "day1" / "notes.txt");
    // Not recursive: the nested clip is not looked for
    so.recursive = false;
    auto flat = scanFolder(root, dir / "elsewhere", so);
    REQUIRE(flat);
    REQUIRE(flat->clips.empty()); // day1 is a plain folder, not descended into
    so.recursive = true;
    // Output root equal to the scanned root is refused (also written with a trailing separator)
    REQUIRE_FALSE(scanFolder(root, root, so));
    REQUIRE_FALSE(scanFolder(root / "", root, so));
    // A root that holds frames itself: clips in its subfolders are found too
    REQUIRE(synth::writeSyntheticClip(dir / "r2", smallClip(1, 0)));
    REQUIRE(synth::writeSyntheticClip(dir / "r2" / "sub" / "C9", smallClip(1, 0)));
    auto r2 = scanFolder(dir / "r2", dir / "o2", ScanOptions{});
    REQUIRE(r2);
    REQUIRE(r2->clips.size() == 2);
}

TEST_CASE("Archive batch puts clip-folder sidecars into the output clip and its manifest (PIPE-6, B3)") {
    const fs::path dir = freshDir("archive2");
    const fs::path root = dir / "card";
    REQUIRE(synth::writeSyntheticClip(root / "C001", smallClip(2, 2)));
    writeText(root / "C001" / "C001_meta.xml", "xml");
    writeText(root / "C001" / "proxy" / "C001.mov", "mov");
    writeText(root / "notes.txt", "notes");
    ScanOptions so;
    so.archiveMode = true;
    auto s = scanFolder(root, dir / "out", so);
    REQUIRE(s);
    BatchJob job;
    job.outputRoot = dir / "out";
    job.archiveMode = true;
    job.options = customOptions(64, 36);
    job.clips = s->clips;
    job.copies = s->copies;
    BatchResult res = Scheduler().run(job, {});
    REQUIRE(res.done == 1);
    REQUIRE(batchExitCode(res) == 0);
    const fs::path clipOut = dir / "out" / "C001_64x36_lossless";
    REQUIRE(readText(clipOut / "C001_meta.xml") == "xml");
    REQUIRE(readText(clipOut / "proxy" / "C001.mov") == "mov");
    REQUIRE(res.copies.size() == 3);
    auto j = nlohmann::json::parse(readText(res.manifest));
    REQUIRE(j["complete"] == true);
    REQUIRE(j["copies"].size() == 3);
    // Re-run: everything already there; the manifest still lists every copy, exit 0
    res = Scheduler().run(job, {});
    REQUIRE(res.skippedExisting == 1);
    REQUIRE(batchExitCode(res) == 0);
    j = nlohmann::json::parse(readText(res.manifest));
    REQUIRE(j["copies"].size() == 3);
    for (const auto& c : j["copies"]) REQUIRE(c["alreadyArchived"] == true);
    // A different file already at a copy destination is not replaced and fails the batch
    writeText(dir / "out" / "notes.txt", "changed");
    res = Scheduler().run(job, {});
    REQUIRE(res.copyErrors.size() == 1);
    REQUIRE(batchExitCode(res) == 1);
    REQUIRE(readText(dir / "out" / "notes.txt") == "changed");
    // Two copies with one destination: the second fails up front
    BatchJob dup;
    dup.copies.push_back({root / "notes.txt", dir / "dup" / "n.txt"});
    dup.copies.push_back({root / "C001" / "C001_meta.xml", dir / "dup" / "n.txt"});
    res = Scheduler().run(dup, {});
    REQUIRE(res.copies.size() == 1);
    REQUIRE(res.copyErrors.size() == 1);
    REQUIRE(readText(dir / "dup" / "n.txt") == "notes");
}

TEST_CASE("A cancelled batch reports every clip, leaves no partial output and an accurate manifest (B6)") {
    const fs::path dir = freshDir("cancelbatch");
    for (int i = 0; i < 3; ++i) {
        synth::SynthClipParams p = smallClip(30, 2, static_cast<uint32_t>(i + 1));
        p.frame.width = 512;
        p.frame.height = 288;
        REQUIRE(synth::writeSyntheticClip(dir / "src" / ("C00" + std::to_string(i)), p));
    }
    writeText(dir / "src" / "notes.txt", "n");
    ScanOptions so;
    so.archiveMode = true;
    auto s = scanFolder(dir / "src", dir / "out", so);
    REQUIRE(s);
    BatchJob job;
    job.outputRoot = dir / "out";
    job.archiveMode = true;
    job.options = customOptions(256, 144);
    job.options.maxParallelClips = 1;
    job.clips = s->clips;
    Scheduler sched;
    BatchCallbacks cb;
    std::atomic<int> doneCalls{0};
    cb.onClipProgress = [&](size_t i, const ClipProgress& p) { if (i == 0 && p.framesDone >= 3) sched.cancel(); };
    cb.onClipDone = [&](size_t, const ClipResult& r) {
        ++doneCalls;
        REQUIRE(r.files.empty());
    };
    BatchResult res = sched.run(job, cb);
    REQUIRE(res.aborted);
    REQUIRE(doneCalls == 3);
    REQUIRE(res.cancelled == 3);
    REQUIRE(res.failed == 0);
    REQUIRE(res.clips[1].message.find("not started") != std::string::npos);
    REQUIRE(batchExitCode(res) == 1);
    REQUIRE(entryCount(dir / "out") == 1); // only the manifest: no clip output, no .partial
    auto j = nlohmann::json::parse(readText(res.manifest));
    REQUIRE(j["complete"] == false);
    REQUIRE(j["produced"].empty());
}

namespace {
// A writer whose output fails while the user has paused the batch (output drive unplugged, disk full).
std::function<void()> g_onFailingWrite;
class FailingWriter final : public codec::ICodecWriter {
public:
    Expected<void> begin(const codec::WriterConfig&) override { return {}; }
    Expected<codec::EncodedStats> writeFrame(const BayerFrame&, codec::FrameBudget) override {
        if (g_onFailingWrite) g_onFailingWrite(); // the user presses Pause
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the reader reaches the pause gate
        return makeError(ErrorCode::Io, "output drive went away");
    }
    Expected<void> writeAudio(const AudioBlock&) override { return {}; }
    Expected<codec::WriteSummary> finish() override { return codec::WriteSummary{}; }
    void abort() override {}
};
class FailingFactory final : public codec::ICodecFactory {
public:
    codec::Format format() const override { return codec::Format::Unknown; }
    std::string displayName() const override { return "failing test writer"; }
    codec::Capabilities caps() const override {
        codec::Capabilities c;
        c.write = true;
        c.rateControl = true;
        return c;
    }
    codec::ModuleStatus status() const override { return {codec::ModuleState::Available, "test"}; }
    bool probe(const fs::path&) const override { return false; }
    std::unique_ptr<codec::ICodecReader> makeReader() const override { return nullptr; }
    std::unique_ptr<codec::ICodecWriter> makeWriter() const override { return std::make_unique<FailingWriter>(); }
};
} // namespace

TEST_CASE("A clip that fails while the batch is paused ends at once and wakes the batch (B5)") {
    codec::CodecRegistry::instance().registerFactory(std::make_shared<FailingFactory>());
    synth::SynthClipParams p = smallClip(30, 0);
    p.frame.width = 512;
    p.frame.height = 288;
    REQUIRE(synth::makeMemoryClip("pausesrc", p));
    const fs::path dir = freshDir("pausefail");
    JobOptions o = customOptions(256, 144);
    o.outputFormat = codec::Format::Unknown;
    o.verify = false;

    // convertClip level
    ClipRunContext ctx;
    ctx.pause = std::make_shared<PauseGate>();
    ctx.plan.resampleWorkers = 2;
    g_onFailingWrite = [gate = ctx.pause] { gate->pause(); };
    std::atomic<bool> returned{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 100 && !returned; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ctx.pause->resume(); // never leave the test hanging
    });
    auto t0 = std::chrono::steady_clock::now();
    ClipResult r = convertClip(ClipJob("mem://pausesrc", dir, "pausedst"), o, ctx);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    returned = true;
    watchdog.join();
    REQUIRE(r.status == ClipStatus::Failed);
    REQUIRE(r.message.find("output drive went away") != std::string::npos);
    REQUIRE(secs < 3.0);
    REQUIRE(entryCount(dir) == 0); // its partial output was removed

    // Scheduler level: the paused batch still finishes and reports the failure
    BatchJob job;
    job.options = o;
    job.clips.push_back(ClipJob("mem://pausesrc", dir, "pausedst2"));
    Scheduler sched;
    g_onFailingWrite = [&sched] { sched.pause(); };
    returned = false;
    std::thread watchdog2([&] {
        for (int i = 0; i < 100 && !returned; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sched.resume();
    });
    t0 = std::chrono::steady_clock::now();
    BatchResult res = sched.run(job, {});
    const double secs2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    returned = true;
    watchdog2.join();
    g_onFailingWrite = nullptr;
    REQUIRE(res.failed == 1);
    REQUIRE(secs2 < 3.0);
}

TEST_CASE("The thread limit also bounds the shared encode pool (PIPE-14)") {
    BatchJob job;
    job.options.maxThreads = 1;
    Scheduler().run(job, {});
    REQUIRE(sharedThreadPool()->size() == 1);
    job.options.maxThreads = 0;
    Scheduler().run(job, {});
    REQUIRE(sharedThreadPool()->size() == std::max(1u, std::thread::hardware_concurrency()));
}

TEST_CASE("synth: CFA pattern and start timecode options (I1)") {
    std::string note;
    const Rational ntsc{30000, 1001};
    auto tc = synth::parseStartTimecode("00:59:59;28", ntsc, note);
    REQUIRE(tc);
    REQUIRE(tc->dropFrame);
    REQUIRE(note.empty());
    REQUIRE(synth::parseStartTimecode("01:00:00:00", Rational{25, 1}, note));
    REQUIRE(synth::parseStartTimecode("23:59:59:24", Rational{25, 1}, note));
    REQUIRE(synth::parseStartTimecode("01:00:00;00", Rational{24000, 1001}, note));
    REQUIRE_FALSE(note.empty()); // drop-frame flag without dropping at 23.976
    for (const char* bad : {"01:01:00;00", "01:00:00:25", "24:00:00:00", "01:60:00:00", "1:00:00:00", "01:00:00.00",
                            "01:00:00:0x", "01-00-00-00", "01:00:00:000"}) {
        INFO(bad);
        REQUIRE_FALSE(synth::parseStartTimecode(bad, bad == std::string("01:00:00:25") ? Rational{25, 1} : ntsc, note));
        REQUIRE_FALSE(note.empty());
    }
    REQUIRE(synth::parseStartTimecode("01:10:00;00", ntsc, note)); // every tenth minute keeps frames 00 and 01

    const fs::path dir = freshDir("synthcfa");
    synth::SynthClipParams p = smallClip(3, 0);
    p.frame.cfa = CfaPattern::GRBG;
    p.fps = ntsc;
    p.startTimecode = tc;
    auto info = synth::writeSyntheticClip(dir / "G1", p);
    REQUIRE(info);
    REQUIRE(info->cfa == CfaPattern::GRBG);
    REQUIRE(info->startTimecode->toString() == "00:59:59;28");
    auto f = codec::CodecRegistry::instance().probe(dir / "G1");
    auto rd = f->makeReader();
    REQUIRE(rd->open(dir / "G1"));
    BayerFrame fr;
    REQUIRE(rd->readFrame(2, fr));
    REQUIRE(fr.meta.timecode->toString() == "01:00:00;00");
}
