#include "r3dss/pipeline/clip_converter.hpp"
#include "r3dss/core/paths.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include "r3dss/core/logger.hpp"
#include "r3dss/pipeline/output_claim.hpp"
#include "r3dss/ratecontrol/ratecontrol.hpp"
#include "r3dss/resample/resampler.hpp"
#if defined(R3DSS_HAVE_CUDA)
#include "r3dss/resample_cuda/cuda_resampler.hpp"
#endif

namespace r3dss::pipeline {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

const char* clipStatusName(ClipStatus s) {
    switch (s) {
    case ClipStatus::Done: return "done";
    case ClipStatus::Failed: return "failed";
    case ClipStatus::Skipped: return "skipped";
    case ClipStatus::Cancelled: return "cancelled";
    }
    return "?";
}

ResamplerChoice chooseResampler(bool useGpu) {
#if defined(R3DSS_HAVE_CUDA)
    if (useGpu && resample::cudaDeviceCount() > 0) return {"cuda", true};
#else
    (void)useGpu;
#endif
    return {"cpu", false};
}

namespace {

std::unique_ptr<resample::IResampler> makeResampler(const ResamplerChoice& c) {
#if defined(R3DSS_HAVE_CUDA)
    if (c.cuda) {
        auto r = resample::makeCudaResampler();
        if (r) return r;
    }
#else
    (void)c;
#endif
    return resample::makeCpuResampler(nullptr);
}

struct Item {
    std::shared_ptr<BayerFrame> frame;
    ratecontrol::ComplexityStats stats;
};

// Reorders resampled frames back into index order with bounded capacity.
class OrderedBuffer {
public:
    OrderedBuffer(size_t cap, std::shared_ptr<CancellationToken> tok) : cap_(cap), tok_(std::move(tok)) {
        reg_ = tok_->onCancel([this] { std::lock_guard<std::mutex> lk(m_); cv_.notify_all(); });
    }
    bool push(int64_t idx, Item it) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return items_.size() < cap_ || idx == next_ || tok_->isCancelled(); });
        if (tok_->isCancelled()) return false;
        items_[idx] = std::move(it);
        cv_.notify_all();
        return true;
    }
    std::optional<Item> pop(int64_t idx) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return items_.count(idx) || tok_->isCancelled(); });
        if (tok_->isCancelled() && !items_.count(idx)) return std::nullopt;
        Item it = std::move(items_[idx]);
        items_.erase(idx);
        next_ = idx + 1;
        cv_.notify_all();
        return it;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<int64_t, Item> items_;
    size_t cap_;
    int64_t next_ = 0;
    std::shared_ptr<CancellationToken> tok_;
    CancelRegistration reg_; // unregistered before tok_ is released
};

// Undoes a started output unless disarmed: aborts the writer, then removes the claimed partial output.
struct OutputGuard {
    codec::ICodecWriter* writer = nullptr;
    OutputClaim* claim = nullptr;
    bool armed = true;
    ~OutputGuard() {
        if (!armed) return;
        if (writer) writer->abort();
        if (claim) claim->discard();
    }
};

std::string lowerExt(const fs::path& p) {
    std::string e = toUtf8(p.extension());
    for (char& c : e)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

bool isMemoryPath(const fs::path& p) { return toUtf8Generic(p).rfind("mem://", 0) == 0; }

// Maps a path inside `from` to the same relative path inside `to`.
fs::path rebase(const fs::path& p, const fs::path& from, const fs::path& to) {
    const fs::path rel = p.lexically_relative(from);
    return rel.empty() || *rel.begin() == ".." ? p : to / rel;
}

struct ErrorSlot {
    std::mutex m;
    std::optional<Error> err;
    void set(const Error& e, CancellationToken& tok) {
        std::lock_guard<std::mutex> lk(m);
        if (!err) err = e;
        tok.cancel();
    }
    std::optional<Error> get() { std::lock_guard<std::mutex> lk(m); return err; }
};

} // namespace

fs::path clipSourceRoot(const fs::path& sourceIn) {
    const fs::path source = normaliseSourcePath(sourceIn);
    if (isMemoryPath(source)) return {};
    std::error_code ec;
    if (!fs::is_directory(source, ec) && lowerExt(source) == ".dng") return source.parent_path();
    return source;
}

Expected<PlannedOutput> planClipOutput(const ClipJob& jobIn, const JobOptions& options) {
    const fs::path source = normaliseSourcePath(jobIn.source);
    auto inFactory = codec::CodecRegistry::instance().probe(source);
    if (!inFactory) return makeError(ErrorCode::InvalidArgument, "not a recognised raw clip: " + toUtf8(source));
    PlannedOutput p;
    p.format = options.outputFormat.value_or(inFactory->format());
    p.name = jobIn.outputName.empty() ? expandName(options.namingTemplate, clipStem(source), options, p.format) : jobIn.outputName;
    if (p.name.empty()) return makeError(ErrorCode::InvalidArgument, "empty output name for " + toUtf8(source));
    if (p.format == codec::Format::Memory) {
        p.dest = fromUtf8("mem://" + p.name);
    } else {
        p.dest = jobIn.outputDir / fromUtf8(p.name);
        if (p.format != codec::Format::CinemaDNG) p.dest += codec::formatExtension(p.format);
    }
    return p;
}

ClipResult convertClip(const ClipJob& jobIn, const JobOptions& options, const ClipRunContext& ctxIn) {
    ClipJob job = jobIn;
    job.source = normaliseSourcePath(jobIn.source); // "C001/" names the clip C001, not ""
    ClipResult res;
    const auto t0 = Clock::now();
    ClipRunContext ctx = ctxIn;
    if (!ctx.cancel) ctx.cancel = std::make_shared<CancellationToken>();
    if (!ctx.pause) ctx.pause = std::make_shared<PauseGate>();
    auto report = [&](const ClipProgress& p) { if (ctx.progress) ctx.progress(p); };
    auto finishTime = [&] { res.seconds = std::chrono::duration<double>(Clock::now() - t0).count(); };
    auto failWith = [&](ClipStatus st, const std::string& msg) {
        res.status = st;
        res.message = msg;
        res.files.clear(); // nothing of an unfinished output is left on disk
        res.sidecarCopies.clear();
        finishTime();
        return res;
    };

    report({0, 0, 0, "probing"});
    auto& reg = codec::CodecRegistry::instance();
    auto inFactory = reg.probe(job.source);
    if (!inFactory) return failWith(ClipStatus::Failed, "not a recognised raw clip: " + toUtf8(job.source));
    if (!inFactory->caps().read)
        return failWith(ClipStatus::Failed, inFactory->displayName() + " cannot be decoded: " + inFactory->status().text);
    if (!inFactory->caps().readsMosaic)
        return failWith(ClipStatus::Failed,
                        inFactory->displayName() +
                            ": RAW-to-RAW conversion is not available. Its decoder returns only a developed RGB image, "
                            "not the sensor mosaic (docs/FEASIBILITY.md).");
    auto reader = inFactory->makeReader();
    auto infoR = reader->open(job.source);
    if (!infoR) return failWith(ClipStatus::Failed, "open failed: " + infoR.error().toString());
    const codec::ClipInfo info = *infoR;
    res.inputBytes = info.sourceBytes;
    res.srcWidth = info.width;
    res.srcHeight = info.height;

    const codec::Format outFmt = options.outputFormat.value_or(info.format);
    auto outFactory = reg.byFormat(outFmt);
    if (!outFactory) return failWith(ClipStatus::Failed, std::string("no codec for output format ") + codec::formatName(outFmt));
    if (!outFactory->caps().write || !outFactory->status().usable())
        return failWith(ClipStatus::Failed,
                        outFmt == codec::Format::RedFamily
                            ? "REDCODE cannot be written (no public encoder exists); choose CinemaDNG, ProRes or DNxHR output"
                            : outFactory->displayName() + " cannot be written: " + outFactory->status().text);

    if (options.size == SizePreset::Custom) {
        if (!options.customSize) return failWith(ClipStatus::Failed, "invalid output size: custom size without width and height");
        if (const std::string err = checkCustomSize(options.customSize->width, options.customSize->height); !err.empty())
            return failWith(ClipStatus::Failed, "invalid output size " + std::to_string(options.customSize->width) + "x" +
                                                    std::to_string(options.customSize->height) + ": " + err);
    }
    auto sizeR = resample::planOutputSize(info.width, info.height, options.size, options.customSize,
                                          outFactory->caps().dimAlignment);
    if (!sizeR) {
        // Never upscale: a size larger than the source skips the clip (exit code 3 when nothing is written).
        if (sizeR.error().code != ErrorCode::Unsupported)
            return failWith(ClipStatus::Failed, "invalid output size: " + sizeR.error().message);
        res.skipReason = SkipReason::Other;
        return failWith(ClipStatus::Skipped, sizeR.error().message);
    }
    const resample::OutputSize outSize = *sizeR;
    res.outWidth = outSize.width;
    res.outHeight = outSize.height;
    res.rateControl = outFactory->caps().rateControl;
    if (options.size == SizePreset::Custom && info.width && info.height && outSize.height) {
        const double srcAspect = static_cast<double>(info.width) / info.height;
        const double outAspect = static_cast<double>(outSize.width) / outSize.height;
        const double change = outAspect / srcAspect - 1.0;
        if (std::fabs(change) > 0.005) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "output %ux%u has aspect %.3f, the source %ux%u has %.3f (%+.1f%%): the image is %s "
                          "(anamorphic output, pixels are no longer square)",
                          outSize.width, outSize.height, outAspect, info.width, info.height, srcAspect, change * 100.0,
                          change < 0 ? "squeezed horizontally" : "squeezed vertically");
            res.warnings.push_back(buf);
            R3DSS_LOG_WARN("convert", toUtf8(job.source) + ": " + buf);
        }
    }

    auto planned = planClipOutput(job, options);
    if (!planned) return failWith(ClipStatus::Failed, planned.error().message);
    const std::string name = planned->name;
    const fs::path finalDest = planned->dest; // where the finished output is published
    fs::path dest = finalDest;                // where the output is written (the claimed "<finalDest>.partial")
    res.output = finalDest;
    const bool onDisk = outFmt != codec::Format::Memory && !isMemoryPath(finalDest);
    const fs::path srcRoot = clipSourceRoot(job.source);
    OutputClaim claim;
    std::error_code ec;
    if (onDisk) {
        if (!srcRoot.empty() && (pathsOverlap(finalDest, srcRoot) || pathsOverlap(partialPathFor(finalDest), srcRoot)))
            return failWith(ClipStatus::Failed, "output " + toUtf8(finalDest) + " would overlap the source " + toUtf8(srcRoot) +
                                                    ": choose an output folder outside the source clip");
        if (fs::exists(fs::symlink_status(finalDest, ec)) && !options.overwrite) {
            res.skipReason = SkipReason::OutputExists;
            return failWith(ClipStatus::Skipped, "output already exists: " + toUtf8(finalDest));
        }
        if (const fs::path parent = finalDest.parent_path(); !parent.empty()) {
            fs::create_directories(parent, ec);
            if (!fs::is_directory(parent, ec))
                return failWith(ClipStatus::Failed, "cannot create output folder " + toUtf8(parent));
        }
        auto c = OutputClaim::claim(finalDest, outFmt == codec::Format::CinemaDNG);
        if (!c) return failWith(ClipStatus::Failed, c.error().message);
        claim = std::move(*c);
        dest = claim.partial();
    }

    codec::WriterConfig wc;
    wc.dest = dest;
    wc.clipName = name;
    wc.width = outSize.width;
    wc.height = outSize.height;
    wc.bits = info.bits;
    wc.cfa = info.cfa;
    wc.black = info.black;
    wc.white = info.white;
    wc.fps = info.fps;
    wc.profile = options.profile;
    wc.audio = info.audio;
    wc.startTimecode = info.startTimecode;
    wc.meta = info.meta;
    wc.lossless = options.lossless;
    auto writer = outFactory->makeWriter();
    OutputGuard guard{writer.get(), &claim, true}; // any return before publication removes the partial output
    if (auto b = writer->begin(wc); !b) return failWith(ClipStatus::Failed, "cannot start output: " + b.error().toString());

    const ResamplerChoice choice = chooseResampler(options.useGpu);
    res.resampler = choice.name;
    resample::ResampleParams rp;
    rp.outWidth = outSize.width;
    rp.outHeight = outSize.height;
    rp.kernel = options.kernel;
    rp.clipAware = options.clipAware;
    const bool identity = outSize.width == info.width && outSize.height == info.height;

    const int64_t N = info.frameCount;
    const unsigned workers = std::max(1u, choice.cuda ? std::min(2u, ctx.plan.resampleWorkers) : ctx.plan.resampleWorkers);
    BoundedQueue<std::shared_ptr<BayerFrame>> q1(ctx.plan.queueDepth, ctx.cancel);
    OrderedBuffer ordered(ctx.plan.queueDepth * 2, ctx.cancel);
    ErrorSlot errors;
    std::atomic<int64_t> framesDone{0};
    ratecontrol::VbrController vbr(options.profile);
    double budgetSum = 0.0;
    const bool rateControl = res.rateControl; // codecs without rate control (CinemaDNG) need no budget

    // Reader thread
    std::thread readerThread([&] {
        for (int64_t i = 0; i < N; ++i) {
            if (!ctx.pause->wait(ctx.cancel.get())) return;
            if (ctx.cancel->isCancelled()) return;
            auto f = std::make_shared<BayerFrame>();
            if (auto r = reader->readFrame(i, *f); !r) {
                errors.set(Error(r.error().code, "frame " + std::to_string(i) + ": " + r.error().message), *ctx.cancel);
                return;
            }
            if (!q1.push(std::move(f))) return;
        }
        q1.close();
    });

    // Resample workers
    std::vector<std::thread> workerThreads;
    for (unsigned w = 0; w < workers; ++w) {
        workerThreads.emplace_back([&] {
            auto rs = makeResampler(choice);
            for (;;) {
                auto in = q1.pop();
                if (!in) return;
                Item it;
                it.frame = std::make_shared<BayerFrame>();
                if (identity) {
                    *it.frame = **in;
                } else if (auto r = rs->resample(**in, *it.frame, rp); !r) {
                    errors.set(r.error(), *ctx.cancel);
                    return;
                }
                if (rateControl) it.stats = ratecontrol::estimateComplexity(*it.frame);
                const int64_t idx = it.frame->index;
                if (!ordered.push(idx, std::move(it))) return;
            }
        });
    }

    // Writer (this thread)
    report({0, N, 0.0, "converting"});
    auto lastReport = Clock::now();
    bool writeOk = true;
    for (int64_t i = 0; i < N; ++i) {
        auto it = ordered.pop(i);
        if (!it) { writeOk = false; break; }
        codec::FrameBudget budget;
        if (rateControl) {
            budget = vbr.next(*it->frame, it->stats);
            budgetSum += vbr.lastBpp();
        }
        auto st = writer->writeFrame(*it->frame, budget);
        if (!st) {
            errors.set(Error(st.error().code, "write frame " + std::to_string(i) + ": " + st.error().message), *ctx.cancel);
            writeOk = false;
            break;
        }
        res.outputBytes += st->bytes;
        const int64_t done = ++framesDone;
        const auto now = Clock::now();
        if (std::chrono::duration<double>(now - lastReport).count() >= 0.1 || done == N) {
            lastReport = now;
            const double el = std::chrono::duration<double>(now - t0).count();
            report({done, N, el > 0 ? done / el : 0.0, "converting"});
        }
    }
    if (!writeOk) ctx.cancel->cancel();
    readerThread.join();
    for (auto& t : workerThreads) t.join();
    if (auto e = errors.get()) {
        writer->abort();
        return failWith(e->code == ErrorCode::Cancelled ? ClipStatus::Cancelled : ClipStatus::Failed, e->toString());
    }
    if (ctx.cancel->isCancelled()) {
        writer->abort();
        return failWith(ClipStatus::Cancelled, "cancelled");
    }

    // Audio passthrough in one-second blocks
    int64_t audioSamples = 0;
    if (info.audio.present()) {
        report({N, N, 0.0, "audio"});
        const int64_t block = std::max<int64_t>(1, info.audio.sampleRate);
        int64_t pos = 0;
        for (;;) {
            if (ctx.cancel->isCancelled()) { writer->abort(); return failWith(ClipStatus::Cancelled, "cancelled"); }
            auto ab = reader->readAudio(pos, block);
            if (!ab) { writer->abort(); return failWith(ClipStatus::Failed, "audio read: " + ab.error().toString()); }
            if (ab->sampleCount == 0) break;
            if (auto w = writer->writeAudio(*ab); !w) { writer->abort(); return failWith(ClipStatus::Failed, "audio write: " + w.error().toString()); }
            audioSamples += ab->sampleCount;
            pos += ab->sampleCount;
            if (ab->sampleCount < block) break;
        }
    }
    auto summary = writer->finish();
    if (!summary) { writer->abort(); return failWith(ClipStatus::Failed, "finish: " + summary.error().toString()); }
    reader->close();
    res.frames = summary->framesWritten;
    res.outputBytes = summary->totalBytes;
    res.averageBpp = res.frames ? static_cast<double>(res.outputBytes) * 8.0 /
                                      (static_cast<double>(res.frames) * outSize.width * outSize.height)
                                : 0.0;
    res.averageBudgetBpp = rateControl && res.frames ? budgetSum / static_cast<double>(res.frames) : 0.0;

    if (options.verify) {
        report({0, N, 0.0, "verifying"});
        Expectation e;
        e.width = outSize.width;
        e.height = outSize.height;
        e.frameCount = N;
        e.fps = info.fps;
        e.audio = info.audio;
        // The source decides how much audio there must be, not what happened to be written.
        e.audioSamples = info.audio.sampleCount ? info.audio.sampleCount : audioSamples;
        if (info.audio.present()) e.audioReference = job.source;
        e.startTimecode = info.startTimecode;
        e.cancel = ctx.cancel;
        // Snapshot of what was just written; the verifier reads each file back from storage and compares.
        for (const auto& f : summary->files) {
            if (!onDisk || isMemoryPath(f)) continue;
            auto d = fileDigest(f);
            if (!d) return failWith(ClipStatus::Failed, "verification failed: " + d.error().message);
            e.fileDigests.emplace_back(f, *d);
        }
        auto lastVerifyReport = Clock::now();
        e.progress = [&](int64_t done, int64_t total) {
            const auto now = Clock::now();
            if (done == total || std::chrono::duration<double>(now - lastVerifyReport).count() >= 0.1) {
                lastVerifyReport = now;
                report({done, total, 0.0, "verifying"});
            }
        };
        res.verification = verifyOutput(dest, e);
        if (res.verification.cancelled || ctx.cancel->isCancelled()) return failWith(ClipStatus::Cancelled, "cancelled");
        if (!res.verification.ok) {
            std::string msg = "verification failed:";
            for (const auto& p : res.verification.problems) msg += " " + p + ";";
            return failWith(ClipStatus::Failed, msg);
        }
    }

    // Archive mode: the clip folder's other files go into the output clip folder, verified.
    std::vector<std::string> sidecarErrors;
    if (onDisk && outFmt == codec::Format::CinemaDNG && !job.sidecars.empty() && !srcRoot.empty()) {
        report({N, N, 0.0, "copying"});
        for (const auto& rel : job.sidecars) {
            if (ctx.cancel->isCancelled()) return failWith(ClipStatus::Cancelled, "cancelled");
            if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
                sidecarErrors.push_back("invalid sidecar path " + toUtf8(rel));
                continue;
            }
            auto cr = copyAndVerify(srcRoot / rel, dest / rel, ctx.cancel);
            if (cr) res.sidecarCopies.push_back(*cr);
            else if (cr.error().code == ErrorCode::Cancelled) return failWith(ClipStatus::Cancelled, "cancelled");
            else sidecarErrors.push_back(toUtf8(srcRoot / rel) + ": " + cr.error().toString());
        }
    }

    if (ctx.cancel->isCancelled()) return failWith(ClipStatus::Cancelled, "cancelled");
    if (onDisk) {
        if (auto c = claim.commit(options.overwrite, {srcRoot}); !c)
            return failWith(ClipStatus::Failed, "cannot publish the output: " + c.error().message);
    }
    guard.armed = false;
    for (const auto& f : summary->files) res.files.push_back(onDisk ? rebase(f, dest, finalDest) : f);
    for (auto& c : res.sidecarCopies) {
        c.dest = rebase(c.dest, dest, finalDest);
        res.files.push_back(c.dest);
    }
    res.copyErrors = sidecarErrors;
    res.status = ClipStatus::Done;
    res.message = "ok";
    finishTime();
    return res;
}

} // namespace r3dss::pipeline
