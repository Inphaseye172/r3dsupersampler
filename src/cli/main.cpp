// r3dss-cli: headless front end to the engine. Same job model as the GUI.
#include <CLI/CLI.hpp>
#include <chrono>
#include <iomanip>
#include <map>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/paths.hpp"
#include "r3dss/codec_cdng/tiff.hpp"
#include "r3dss/codec_cdng/wav.hpp"
#include "r3dss/codec_red/red_container.hpp"
#include "r3dss/core/hardware.hpp"
#include "r3dss/core/logger.hpp"
#include "r3dss/pipeline/folder_scanner.hpp"
#include "r3dss/pipeline/scheduler.hpp"
#include "r3dss/synth/synth.hpp"
#if defined(R3DSS_HAVE_CUDA)
#include "r3dss/resample_cuda/cuda_resampler.hpp"
#endif

using namespace r3dss;
namespace fs = std::filesystem;

namespace {

const char* moduleStateName(codec::ModuleState s) {
    switch (s) {
    case codec::ModuleState::Available: return "available";
    case codec::ModuleState::MissingSdk: return "missing-sdk";
    case codec::ModuleState::MissingLicense: return "missing-license";
    case codec::ModuleState::Stub: return "stub";
    }
    return "?";
}

// Machine-readable module list: what scripts/acceptance.py checks against release/capabilities.json.
int cmdCodecsJson() {
    nlohmann::json j;
    j["version"] = R3DSS_VERSION_STRING;
#if defined(R3DSS_RELEASE_BUILD)
    j["release"] = true;
#else
    j["release"] = false;
#endif
    j["modules"] = nlohmann::json::array();
    for (const auto& f : codec::CodecRegistry::instance().all()) {
        const auto st = f->status();
        const auto c = f->caps();
        j["modules"].push_back({{"format", codec::formatName(f->format())}, {"name", f->displayName()},
                                {"read", c.read}, {"write", c.write}, {"rateControl", c.rateControl},
                                {"lossless", c.lossless}, {"state", moduleStateName(st.state)}, {"status", st.text}});
    }
#if defined(R3DSS_HAVE_CUDA)
    j["cudaDevices"] = resample::cudaDeviceCount();
#else
    j["cudaDevices"] = nullptr;
#endif
    std::cout << j.dump(2) << "\n";
    return 0;
}

int cmdCodecs() {
    for (const auto& f : codec::CodecRegistry::instance().all()) {
        const auto st = f->status();
        const auto c = f->caps();
        std::cout << f->displayName() << "\n  format: " << codec::formatName(f->format())
                  << "\n  read: " << (c.read ? "yes" : "no") << "  write: " << (c.write ? "yes" : "no")
                  << "  cuda: " << (c.cuda ? "yes" : "no") << "  rate control: " << (c.rateControl ? "yes" : "no")
                  << "\n  status: " << st.text << "\n";
    }
    const HardwareInfo hw = probeHardware();
    std::cout << "hardware: " << hw.logicalCores << " logical cores, " << (hw.totalRamBytes >> 20) << " MiB RAM";
#if defined(R3DSS_HAVE_CUDA)
    std::cout << ", CUDA devices: " << resample::cudaDeviceCount();
    if (resample::cudaDeviceCount() > 0) std::cout << " (" << resample::cudaDeviceName(0) << ")";
#else
    std::cout << ", CUDA: not built";
#endif
    std::cout << "\n";
    return 0;
}

int cmdProbe(const fs::path& path, bool chunks) {
    auto f = codec::CodecRegistry::instance().probe(path);
    if (!f) {
        std::cerr << "not a recognised raw clip: " << path << "\n";
        return 2;
    }
    auto r = f->makeReader();
    auto info = r->open(path);
    if (!info) {
        std::cerr << "open failed: " << info.error().toString() << "\n";
        return 3;
    }
    nlohmann::json j;
    j["format"] = codec::formatName(info->format);
    j["source"] = info->sourceLabel.empty() ? codec::sourceKindName(info->sourceKind) : info->sourceLabel;
    j["module"] = f->status().text;
    j["width"] = info->width;
    j["height"] = info->height;
    j["bits"] = info->bits;
    j["cfa"] = cfaName(info->cfa);
    j["black"] = info->black;
    j["white"] = info->white;
    j["fps"] = info->fps.toDouble();
    j["frames"] = info->frameCount;
    j["duration_s"] = info->durationSeconds();
    j["bytes"] = info->sourceBytes;
    if (info->frameCount && info->durationSeconds() > 0)
        j["bitrate_mbps"] = static_cast<double>(info->sourceBytes) * 8.0 / 1e6 / info->durationSeconds();
    j["audio"] = {{"channels", info->audio.channels}, {"rate", info->audio.sampleRate},
                  {"format", sampleFormatName(info->audio.format)}, {"samples", info->audio.sampleCount}};
    if (info->startTimecode) j["timecode"] = info->startTimecode->toString();
    j["camera"] = info->meta.cameraMake + " " + info->meta.cameraModel;
    if (info->meta.iso) j["iso"] = *info->meta.iso;
    if (info->meta.whiteBalance) j["asShotNeutral"] = info->meta.whiteBalance->asShotNeutral;
    if (info->meta.lens) j["lens"] = info->meta.lens->model;
    for (const auto& [k, v] : info->meta.vendor)
        std::visit([&](const auto& x) { j["vendor"][k] = x; }, v);
    if (chunks && info->format == codec::Format::RedFamily) {
        auto list = codec::red::listChunks(path);
        if (!list) {
            std::cerr << "chunk walk failed: " << list.error().toString() << "\n";
            return 4;
        }
        j["chunks"] = nlohmann::json::array();
        for (const auto& c : *list)
            j["chunks"].push_back({{"offset", c.offset}, {"size", c.size}, {"tag", c.tag}, {"version", c.version}});
        for (const auto& [tag, st] : codec::red::chunkHistogram(*list)) {
            nlohmann::json v = nlohmann::json::array();
            for (uint8_t ver : st.versions) v.push_back(ver);
            j["chunkHistogram"][tag] = {{"count", st.count}, {"bytes", st.totalBytes}, {"minSize", st.minSize},
                                        {"maxSize", st.maxSize}, {"versions", v}};
        }
        auto strings = codec::red::readHeaderStrings(path);
        if (strings) j["headerStrings"] = *strings;
    }
    std::cout << j.dump(2) << "\n";
    return 0;
}

// ---- decode: exercise a decoder that returns developed RGB (the RED R3D SDK) --------------------
Expected<void> writeRgbTiff(const fs::path& path, const RgbFrame& f) {
    codec::tiff::Writer w;
    std::vector<uint8_t> bytes(f.data.size() * 2);
    for (size_t i = 0; i < f.data.size(); ++i) {
        bytes[2 * i] = static_cast<uint8_t>(f.data[i]);
        bytes[2 * i + 1] = static_cast<uint8_t>(f.data[i] >> 8);
    }
    const uint64_t off = w.appendData(bytes);
    codec::tiff::Writer::IfdBuilder ifd;
    using namespace codec::tiff;
    ifd.setU32(tag::ImageWidth, {f.width});
    ifd.setU32(tag::ImageLength, {f.height});
    ifd.setU16(tag::BitsPerSample, {16, 16, 16});
    ifd.setU16(tag::Compression, {1});
    ifd.setU16(tag::Photometric, {2});
    ifd.setU32(tag::StripOffsets, {static_cast<uint32_t>(off)});
    ifd.setU16(tag::SamplesPerPixel, {3});
    ifd.setU32(tag::RowsPerStrip, {f.height});
    ifd.setU32(tag::StripByteCounts, {static_cast<uint32_t>(bytes.size())});
    ifd.setU16(tag::PlanarConfiguration, {1});
    ifd.setAscii(tag::Software, "r3dss-cli decode (" + f.colorSpace + " / " + f.transfer + ")");
    w.setFirstIfd(w.writeIfd(ifd));
    return w.save(path);
}

int cmdDecode(const fs::path& path, int64_t frame, bool all, uint32_t downscale, bool look, const std::string& out,
              const std::string& audioOut) {
    auto f = codec::CodecRegistry::instance().probe(path);
    if (!f) {
        std::cerr << "not a recognised clip: " << toUtf8(path) << "\n";
        return 2;
    }
    if (!f->caps().readsRgb) {
        std::cerr << f->displayName() << " cannot decode to RGB in this build: " << f->status().text << "\n";
        return 3;
    }
    auto reader = f->makeReader();
    auto info = reader->open(path);
    if (!info) {
        std::cerr << "open failed: " << info.error().toString() << "\n";
        return 3;
    }
    codec::ICodecReader::RgbOptions opts;
    opts.downscale = downscale;
    opts.primaryDevelopment = !look;
    nlohmann::json j;
    j["clip"] = toUtf8(path);
    j["source"] = info->sourceLabel;
    j["size"] = {info->width, info->height};
    j["frames"] = info->frameCount;
    j["fps"] = info->fps.toDouble();
    if (info->startTimecode) j["timecode"] = info->startTimecode->toString();
    j["downscale"] = downscale;
    j["pipeline"] = look ? "clip default look" : "primary development (REDWideGamutRGB / Log3G10)";
    const int64_t first = all ? 0 : frame;
    const int64_t last = all ? info->frameCount - 1 : frame;
    if (first < 0 || last >= info->frameCount) {
        std::cerr << "frame out of range (clip has " << info->frameCount << " frames)\n";
        return 1;
    }
    RgbFrame img;
    double totalMs = 0;
    int64_t decoded = 0;
    for (int64_t i = first; i <= last; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = reader->readRgbFrame(i, img, opts);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r) {
            j["error"] = r.error().toString();
            j["failedFrame"] = i;
            std::cout << j.dump(2) << "\n";
            return 4;
        }
        totalMs += ms;
        ++decoded;
        if (i == first) {
            uint16_t mn[3] = {65535, 65535, 65535}, mx[3] = {0, 0, 0};
            double sum[3] = {0, 0, 0};
            for (size_t k = 0; k < img.data.size(); ++k) {
                const int c = static_cast<int>(k % 3);
                mn[c] = std::min(mn[c], img.data[k]);
                mx[c] = std::max(mx[c], img.data[k]);
                sum[c] += img.data[k];
            }
            const double px = static_cast<double>(img.width) * img.height;
            j["firstFrame"] = {{"index", i}, {"width", img.width}, {"height", img.height},
                               {"timecode", img.meta.timecode ? img.meta.timecode->toString() : ""},
                               {"min", {mn[0], mn[1], mn[2]}}, {"max", {mx[0], mx[1], mx[2]}},
                               {"mean", {sum[0] / px, sum[1] / px, sum[2] / px}}, {"ms", ms}};
            if (!out.empty()) {
                if (auto w = writeRgbTiff(fromUtf8(out), img); !w) {
                    std::cerr << "cannot write " << out << ": " << w.error().toString() << "\n";
                    return 5;
                }
                j["written"] = out;
            }
        }
    }
    j["decoded"] = decoded;
    j["msPerFrame"] = decoded ? totalMs / static_cast<double>(decoded) : 0.0;
    j["decodeFps"] = totalMs > 0 ? 1000.0 * static_cast<double>(decoded) / totalMs : 0.0;
    if (!audioOut.empty() && info->audio.present()) {
        codec::wav::StreamWriter wav;
        if (auto b = wav.begin(fromUtf8(audioOut), info->audio); !b) {
            std::cerr << b.error().toString() << "\n";
            return 5;
        }
        int64_t pos = 0;
        for (;;) {
            auto ab = reader->readAudio(pos, 48000);
            if (!ab) {
                std::cerr << "audio: " << ab.error().toString() << "\n";
                return 4;
            }
            if (ab->sampleCount == 0) break;
            if (auto w = wav.write(*ab); !w) {
                std::cerr << w.error().toString() << "\n";
                return 5;
            }
            pos += ab->sampleCount;
        }
        if (auto fin = wav.finish(); !fin) {
            std::cerr << fin.error().toString() << "\n";
            return 5;
        }
        j["audio"] = {{"channels", info->audio.channels}, {"rate", info->audio.sampleRate},
                      {"format", sampleFormatName(info->audio.format)}, {"samples", pos},
                      {"expectedSamples", info->audio.sampleCount}, {"written", audioOut}};
    }
    std::cout << j.dump(2) << "\n";
    return 0;
}

int runBatch(const pipeline::BatchJob& job, bool quiet) {
    pipeline::Scheduler sched;
    pipeline::BatchCallbacks cb;
    std::mutex outMutex;
    cb.onClipStart = [&](size_t i) {
        std::lock_guard<std::mutex> lk(outMutex);
        if (!quiet) std::cout << "[" << i + 1 << "/" << job.clips.size() << "] " << toUtf8(job.clips[i].source) << "\n";
    };
    cb.onClipProgress = [&](size_t i, const pipeline::ClipProgress& p) {
        if (quiet || p.totalFrames == 0) return;
        std::lock_guard<std::mutex> lk(outMutex);
        std::cout << "\r  clip " << i + 1 << ": " << p.framesDone << "/" << p.totalFrames << " frames, " << std::fixed
                  << std::setprecision(1) << p.fps << " fps, " << p.stage << "      " << std::flush;
    };
    cb.onClipDone = [&](size_t, const pipeline::ClipResult& r) {
        std::lock_guard<std::mutex> lk(outMutex);
        std::cout << "\n  -> " << pipeline::clipStatusName(r.status) << ": " << r.message;
        if (r.status == pipeline::ClipStatus::Done)
            std::cout << " | " << r.srcWidth << "x" << r.srcHeight << " -> " << r.outWidth << "x" << r.outHeight << ", "
                      << r.frames << " frames in " << std::setprecision(2) << r.seconds << " s ("
                      << (r.seconds > 0 ? r.frames / r.seconds : 0.0) << " fps, " << r.resampler << "), " << (r.outputBytes >> 20)
                      << " MiB, " << std::setprecision(2) << r.averageBpp << " bpp (budget " << r.averageBudgetBpp << ")";
        std::cout << "\n";
    };
    cb.onCopyDone = [&](size_t, const Expected<pipeline::CopyResult>& r) {
        std::lock_guard<std::mutex> lk(outMutex);
        if (r) std::cout << "  copied " << toUtf8(r->dest) << " (" << r->sha256.substr(0, 12) << ", verified)\n";
        else std::cout << "  copy failed: " << r.error().toString() << "\n";
    };
    const auto res = sched.run(job, cb);
    std::cout << "batch '" << job.name << "': " << res.done << " done, " << res.failed << " failed, " << res.skipped
              << " skipped, " << res.cancelled << " cancelled, " << res.copies.size() << " copied, " << std::setprecision(1)
              << res.seconds << " s\n";
    if (!res.manifest.empty()) std::cout << "manifest: " << toUtf8(res.manifest) << "\n";
    return (res.failed || !res.copyErrors.empty()) ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    codec::registerBuiltinCodecs();
    CLI::App app{"r3dss command line (preview)"};
    app.require_subcommand(1);
    std::string logLevel = "info";
    app.add_option("--log-level", logLevel, "trace|debug|info|warn|error");

    bool codecsJson = false;
    auto* codecs = app.add_subcommand("codecs", "List codec modules and their status");
    codecs->add_flag("--json", codecsJson, "machine-readable output");
    std::string probePath;
    bool probeChunks = false;
    auto* probe = app.add_subcommand("probe", "Print clip information as JSON");
    probe->add_option("path", probePath, "clip file or CinemaDNG directory")->required();
    probe->add_flag("--chunks", probeChunks, "also list the RED container chunks, tag histogram and header strings");

    // decode
    auto* decode = app.add_subcommand("decode", "Decode frames to developed RGB (RED SDK clips) and report timing");
    std::string decodePath, decodeOut, decodeAudio, decodeRes = "full";
    int64_t decodeFrame = 0;
    bool decodeAll = false, decodeLook = false;
    decode->add_option("path", decodePath, "clip file")->required();
    decode->add_option("--frame", decodeFrame, "frame to decode (default 0)");
    decode->add_flag("--all", decodeAll, "decode every frame (the first one is written with --out)");
    decode->add_option("--res", decodeRes, "full|half|quarter|eighth|sixteenth");
    decode->add_flag("--look", decodeLook, "apply the clip's look instead of primary development (Log3G10 / RWG)");
    decode->add_option("--out", decodeOut, "write the first decoded frame as a 16-bit RGB TIFF");
    decode->add_option("--audio", decodeAudio, "write the clip's audio as WAV");

    // convert
    auto* convert = app.add_subcommand("convert", "Convert clips or run a job file");
    std::vector<std::string> inputs;
    std::string outDir, jobFile, size = "UHD", profile = "Q5", format = "keep", kernel = "lanczos3";
    unsigned parallel = 0, threads = 0;
    bool noVerify = false, noGpu = false, overwrite = false, quiet = false, noClipAware = false, lossless = false;
    convert->add_option("inputs", inputs, "clip files or CinemaDNG directories");
    convert->add_option("-o,--output", outDir, "output directory");
    convert->add_option("--job", jobFile, "batch job JSON (overrides the other options)");
    convert->add_option("--size", size, "6K|UHD|2K|FHD|same|WxH");
    convert->add_option("--profile", profile, "Q1|Q3|Q5|Q7");
    convert->add_option("--format,--container", format, "keep|cdng (ProRes / DNxHR arrive in milestone 4)");
    convert->add_option("--kernel", kernel, "lanczos3|area");
    convert->add_option("--parallel", parallel, "max parallel clips (0 = auto)");
    convert->add_option("--threads", threads, "max threads (0 = all)");
    convert->add_flag("--no-verify", noVerify, "skip output verification");
    convert->add_flag("--no-gpu", noGpu, "force the CPU resampler");
    convert->add_flag("--no-clip-aware", noClipAware, "disable the clipped-highlight fallback");
    convert->add_flag("--lossless", lossless, "request lossless encoding where supported");
    convert->add_flag("--overwrite", overwrite, "replace existing outputs");
    convert->add_flag("-q,--quiet", quiet, "less progress output");

    // scan
    auto* scan = app.add_subcommand("scan", "Scan a folder and write (or run) a batch job");
    std::string scanRoot, scanOut, writeJob;
    bool recursive = true, archive = false, flat = false, run = false;
    scan->add_option("root", scanRoot, "folder to scan")->required();
    scan->add_option("-o,--output", scanOut, "output root")->required();
    scan->add_option("--write-job", writeJob, "write the job JSON here");
    scan->add_flag("!--no-recursive", recursive, "do not recurse");
    scan->add_flag("--archive", archive, "archive mode: copy non-raw files and verify");
    scan->add_flag("--flat", flat, "do not preserve folder structure");
    scan->add_flag("--run", run, "run the job immediately");
    scan->add_option("--size", size, "6K|UHD|2K|FHD|same|WxH");
    scan->add_option("--profile", profile, "Q1|Q3|Q5|Q7");
    scan->add_option("--format,--container", format, "keep|cdng");
    scan->add_flag("--no-gpu", noGpu, "force the CPU resampler");
    scan->add_flag("--overwrite", overwrite, "replace existing outputs");

    // synth
    auto* synthCmd = app.add_subcommand("synth", "Generate a synthetic CinemaDNG clip for testing");
    std::string synthOut, pattern = "scene";
    uint32_t sw = 1920, sh = 1080, frames = 24, channels = 2, seed = 1;
    std::string synthFps = "25";
    uint16_t bits = 12;
    bool uncompressed = false;
    synthCmd->add_option("output", synthOut, "clip directory to create")->required();
    synthCmd->add_option("--width", sw);
    synthCmd->add_option("--height", sh);
    synthCmd->add_option("--frames", frames);
    synthCmd->add_option("--bits", bits);
    synthCmd->add_option("--pattern", pattern, "constant|planes|rampx|rampy|zoneplate|impulse|noise|scene");
    synthCmd->add_option("--audio-channels", channels);
    synthCmd->add_option("--seed", seed);
    synthCmd->add_option("--fps", synthFps, "frame rate, e.g. 24, 25, 24000/1001");
    synthCmd->add_flag("--uncompressed", uncompressed, "write uncompressed DNG instead of lossless JPEG");

    // Windows hands main() ANSI-encoded argv; CLI11 re-reads the wide command line so every
    // argument below is UTF-8 on all platforms (paths are then built with fromUtf8).
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);

    auto& logger = Logger::instance();
    if (logLevel == "trace") logger.setLevel(LogLevel::Trace);
    else if (logLevel == "debug") logger.setLevel(LogLevel::Debug);
    else if (logLevel == "warn") logger.setLevel(LogLevel::Warn);
    else if (logLevel == "error") logger.setLevel(LogLevel::Error);
    else logger.setLevel(LogLevel::Info);
    logger.openFile(Logger::defaultLogDir() / "r3dss-cli.log");

    auto parseOptions = [&](pipeline::JobOptions& o) -> bool {
        if (auto sp = sizePresetFromName(size)) o.size = *sp;
        else {
            unsigned w = 0, h = 0;
            if (std::sscanf(size.c_str(), "%ux%u", &w, &h) == 2) { o.size = SizePreset::Custom; o.customSize = resample::OutputSize{w, h}; }
            else { std::cerr << "bad --size\n"; return false; }
        }
        auto p = qualityProfileFromName(profile);
        if (!p) { std::cerr << "bad --profile\n"; return false; }
        o.profile = *p;
        if (format != "keep") {
            auto f = codec::formatFromName(format);
            if (!f) { std::cerr << "bad --format\n"; return false; }
            o.outputFormat = f;
        }
        auto k = kernelFromName(kernel);
        if (!k) { std::cerr << "bad --kernel\n"; return false; }
        o.kernel = *k;
        o.verify = !noVerify;
        o.useGpu = !noGpu;
        o.clipAware = !noClipAware;
        o.lossless = lossless;
        o.overwrite = overwrite;
        o.maxParallelClips = parallel;
        o.maxThreads = threads;
        return true;
    };

    if (codecs->parsed()) return codecsJson ? cmdCodecsJson() : cmdCodecs();
    if (probe->parsed()) return cmdProbe(fromUtf8(probePath), probeChunks);
    if (decode->parsed()) {
        const std::map<std::string, uint32_t> res = {{"full", 1}, {"half", 2}, {"quarter", 4}, {"eighth", 8}, {"sixteenth", 16}};
        auto it = res.find(decodeRes);
        if (it == res.end()) {
            std::cerr << "bad --res\n";
            return 1;
        }
        return cmdDecode(fromUtf8(decodePath), decodeFrame, decodeAll, it->second, decodeLook, decodeOut, decodeAudio);
    }
    if (synthCmd->parsed()) {
        synth::SynthClipParams p;
        p.frame.width = sw;
        p.frame.height = sh;
        p.frame.bits = bits;
        p.frame.seed = seed;
        auto pat = synth::patternFromName(pattern);
        if (!pat) { std::cerr << "bad --pattern\n"; return 1; }
        p.frame.pattern = *pat;
        p.frames = frames;
        {
            long long num = 0, den = 1;
            if (std::sscanf(synthFps.c_str(), "%lld/%lld", &num, &den) < 1 || num <= 0 || den <= 0) {
                std::cerr << "bad --fps\n";
                return 1;
            }
            p.fps = Rational{num, den};
        }
        p.audioChannels = channels;
        p.startTimecode = Timecode::parse("01:00:00:00", p.fps);
        p.clipName = toUtf8(fromUtf8(synthOut).filename());
        p.losslessJpeg = !uncompressed;
        auto r = synth::writeSyntheticClip(fromUtf8(synthOut), p);
        if (!r) { std::cerr << r.error().toString() << "\n"; return 1; }
        std::cout << "wrote " << r->frameCount << " frames " << r->width << "x" << r->height << " to " << synthOut << "\n";
        return 0;
    }
    if (convert->parsed()) {
        pipeline::BatchJob job;
        if (!jobFile.empty()) {
            auto j = pipeline::loadBatchJob(fromUtf8(jobFile));
            if (!j) { std::cerr << j.error().toString() << "\n"; return 1; }
            job = *j;
        } else {
            if (inputs.empty() || outDir.empty()) { std::cerr << "need inputs and -o, or --job\n"; return 1; }
            job.name = "cli";
            job.outputRoot = fromUtf8(outDir);
            if (!parseOptions(job.options)) return 1;
            for (const auto& in : inputs) job.clips.push_back({fromUtf8(in), fromUtf8(outDir), "", ""});
        }
        return runBatch(job, quiet);
    }
    if (scan->parsed()) {
        pipeline::ScanOptions so;
        so.recursive = recursive;
        so.archiveMode = archive;
        so.preserveStructure = !flat;
        auto res = pipeline::scanFolder(fromUtf8(scanRoot), fromUtf8(scanOut), so);
        if (!res) { std::cerr << res.error().toString() << "\n"; return 1; }
        pipeline::BatchJob job;
        job.name = toUtf8(fromUtf8(scanRoot).filename());
        job.outputRoot = fromUtf8(scanOut);
        job.archiveMode = archive;
        if (!parseOptions(job.options)) return 1;
        job.clips = res->clips;
        job.copies = res->copies;
        std::cout << "found " << job.clips.size() << " clips and " << job.copies.size() << " files to copy\n";
        if (!writeJob.empty()) {
            if (auto s = pipeline::saveBatchJob(job, fromUtf8(writeJob)); !s) { std::cerr << s.error().toString() << "\n"; return 1; }
            std::cout << "wrote " << writeJob << "\n";
        } else if (!run) {
            std::cout << pipeline::batchJobToJson(job) << "\n";
        }
        if (run) return runBatch(job, false);
        return 0;
    }
    return 0;
}
