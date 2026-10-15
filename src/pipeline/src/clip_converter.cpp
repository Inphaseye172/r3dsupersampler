#include "r3dss/pipeline/clip_converter.hpp"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include "r3dss/core/logger.hpp"
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
        tok_->onCancel([this] { std::lock_guard<std::mutex> lk(m_); cv_.notify_all(); });
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
};

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

ClipResult convertClip(const ClipJob& job, const JobOptions& options, const ClipRunContext& ctxIn) {
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
        finishTime();
        return res;
    };

    report({0, 0, 0, "probing"});
    auto& reg = codec::CodecRegistry::instance();
    auto inFactory = reg.probe(job.source);
    if (!inFactory) return failWith(ClipStatus::Failed, "not a recognised raw clip: " + job.source.string());
    if (!inFactory->caps().read)
        return failWith(ClipStatus::Failed, inFactory->displayName() + " cannot be decoded: " + inFactory->status().text);
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

    auto sizeR = resample::planOutputSize(info.width, info.height, options.size, options.customSize,
                                          outFactory->caps().dimAlignment);
    if (!sizeR) return failWith(ClipStatus::Skipped, sizeR.error().message);
    const resample::OutputSize outSize = *sizeR;
    res.outWidth = outSize.width;
    res.outHeight = outSize.height;

    const std::string stem = fs::is_directory(job.source) ? job.source.filename().string() : job.source.stem().string();
    const std::string name = job.outputName.empty() ? expandName(options.namingTemplate, stem, options, outFmt) : job.outputName;
    fs::path dest = job.outputDir / name;
    if (outFmt != codec::Format::CinemaDNG && outFmt != codec::Format::Memory) dest += codec::formatExtension(outFmt);
    if (outFmt == codec::Format::Memory) dest = fs::path("mem://" + name);
    res.output = dest;
    std::error_code ec;
    if (outFmt != codec::Format::Memory && fs::exists(dest, ec)) {
        if (!options.overwrite) return failWith(ClipStatus::Skipped, "output already exists: " + dest.string());
        fs::remove_all(dest, ec);
    }
    if (outFmt != codec::Format::Memory) fs::create_directories(job.outputDir, ec);

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
                it.stats = ratecontrol::estimateComplexity(*it.frame);
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
        const codec::FrameBudget budget = vbr.next(*it->frame, it->stats);
        budgetSum += vbr.lastBpp();
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
    res.files = summary->files;
    res.outputBytes = summary->totalBytes;
    res.averageBpp = res.frames ? static_cast<double>(res.outputBytes) * 8.0 /
                                      (static_cast<double>(res.frames) * outSize.width * outSize.height)
                                : 0.0;
    res.averageBudgetBpp = res.frames ? budgetSum / static_cast<double>(res.frames) : 0.0;

    if (options.verify) {
        report({N, N, 0.0, "verifying"});
        Expectation e;
        e.width = outSize.width;
        e.height = outSize.height;
        e.frameCount = N;
        e.fps = info.fps;
        e.audio = info.audio;
        e.audioSamples = audioSamples;
        e.startTimecode = info.startTimecode;
        res.verification = verifyOutput(dest, e);
        if (!res.verification.ok) {
            std::string msg = "verification failed:";
            for (const auto& p : res.verification.problems) msg += " " + p + ";";
            return failWith(ClipStatus::Failed, msg);
        }
    }
    res.status = ClipStatus::Done;
    res.message = "ok";
    finishTime();
    return res;
}

} // namespace r3dss::pipeline
