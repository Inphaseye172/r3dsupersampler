#include "r3dss/pipeline/verifier.hpp"
#include "r3dss/core/paths.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include "r3dss/pipeline/output_claim.hpp"

namespace r3dss::pipeline {

namespace {

// Four-lane multiply/rotate hash in the style of XXH64 (not bit-compatible): several GB/s, so
// reading the output back costs I/O, not CPU.
class Digest64 {
public:
    void update(const uint8_t* p, size_t n) {
        total_ += n;
        if (bufLen_) {
            const size_t take = std::min(n, sizeof buf_ - bufLen_);
            std::memcpy(buf_ + bufLen_, p, take);
            bufLen_ += take;
            p += take;
            n -= take;
            if (bufLen_ < sizeof buf_) return;
            stripe(buf_);
            bufLen_ = 0;
        }
        for (; n >= 32; p += 32, n -= 32) stripe(p);
        std::memcpy(buf_, p, n);
        bufLen_ = n;
    }
    uint64_t finish() const {
        uint64_t h = rotl(acc_[0], 1) + rotl(acc_[1], 7) + rotl(acc_[2], 12) + rotl(acc_[3], 18);
        h ^= total_ * kP5;
        for (size_t i = 0; i < bufLen_; ++i) h = rotl(h ^ (buf_[i] * kP5), 11) * kP1;
        h ^= h >> 33;
        h *= kP2;
        h ^= h >> 29;
        h *= kP3;
        h ^= h >> 32;
        return h;
    }

private:
    static constexpr uint64_t kP1 = 11400714785074694791ULL, kP2 = 14029467366897019727ULL,
                              kP3 = 1609587929392839161ULL, kP5 = 2870177450012600261ULL;
    static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    void stripe(const uint8_t* p) {
        for (int i = 0; i < 4; ++i) {
            uint64_t w;
            std::memcpy(&w, p + 8 * i, 8);
            acc_[i] = rotl(acc_[i] + w * kP2, 31) * kP1;
        }
    }
    uint64_t acc_[4] = {kP1 + kP2, kP2, 0, 0 - kP1};
    uint8_t buf_[32] = {};
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

// Compares the decoded audio of `output` with that of `reference`, block by block.
void compareAudio(codec::ICodecReader& out, const std::filesystem::path& reference, const Expectation& e,
                  VerificationResult& r) {
    auto fail = [&](const std::string& s) { r.ok = false; r.problems.push_back(s); };
    auto factory = codec::CodecRegistry::instance().probe(reference);
    if (!factory) {
        fail("audio reference not recognised: " + toUtf8(reference));
        return;
    }
    auto src = factory->makeReader();
    auto si = src->open(reference);
    if (!si) {
        fail("audio reference does not open: " + si.error().toString());
        return;
    }
    const int64_t block = std::max<int64_t>(1, si->audio.sampleRate);
    int64_t pos = 0;
    for (;;) {
        if (e.cancel && e.cancel->isCancelled()) {
            r.cancelled = true;
            fail("verification cancelled");
            break;
        }
        auto a = src->readAudio(pos, block);
        auto b = out.readAudio(pos, block);
        if (!a || !b) {
            fail("audio does not read back at sample " + std::to_string(pos) + ": " +
                 (!a ? a.error().toString() : b.error().toString()));
            break;
        }
        if (a->sampleCount != b->sampleCount) {
            fail("audio length differs from the source at sample " + std::to_string(pos));
            break;
        }
        if (a->bytes != b->bytes) {
            fail("audio samples differ from the source in the block at sample " + std::to_string(pos));
            break;
        }
        if (a->sampleCount == 0) {
            r.checked.push_back("audio samples identical to the source (" + std::to_string(pos) + " samples)");
            break;
        }
        pos += a->sampleCount;
    }
    src->close();
}

} // namespace

Expected<uint64_t> fileDigest(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + toUtf8(p));
    Digest64 d;
    std::vector<char> buf(1u << 20);
    for (;;) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (in.bad()) return makeError(ErrorCode::Io, "read failed: " + toUtf8(p));
        if (n > 0) d.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n));
        if (n <= 0 || in.eof()) break;
    }
    return d.finish();
}

VerificationResult verifyOutput(const std::filesystem::path& output, const Expectation& e) {
    VerificationResult r;
    auto fail = [&](const std::string& s) { r.ok = false; r.problems.push_back(s); };
    auto pass = [&](const std::string& s) { r.checked.push_back(s); };
    if (!e.fileDigests.empty()) {
        // Read every written file back from storage (not from the cache) and compare its content.
        size_t changed = 0;
        std::string first;
        for (const auto& [path, digest] : e.fileDigests) {
            if (e.cancel && e.cancel->isCancelled()) break;
            flushAndDropCache(path, true);
            auto d = fileDigest(path);
            if (!d || *d != digest) {
                if (changed++ == 0) first = toUtf8(path.filename()) + (d ? " changed after it was written" : ": " + d.error().message);
            }
        }
        if (changed) fail("file " + first + (changed > 1 ? " (" + std::to_string(changed) + " files differ)" : "") +
                          ": the storage did not keep what was written");
        else pass("all " + std::to_string(e.fileDigests.size()) + " files read back from storage unchanged");
    }
    auto factory = codec::CodecRegistry::instance().probe(output);
    if (!factory) {
        fail("output not recognised by any codec: " + toUtf8(output));
        return r;
    }
    auto reader = factory->makeReader();
    auto info = reader->open(output);
    if (!info) {
        fail("output does not open: " + info.error().toString());
        return r;
    }
    pass("opens as " + std::string(codec::formatName(info->format)));
    if (info->width != e.width || info->height != e.height)
        fail("resolution " + std::to_string(info->width) + "x" + std::to_string(info->height) + " != expected " +
             std::to_string(e.width) + "x" + std::to_string(e.height));
    else pass("resolution " + std::to_string(info->width) + "x" + std::to_string(info->height));
    if (info->frameCount != e.frameCount)
        fail("frame count " + std::to_string(info->frameCount) + " != expected " + std::to_string(e.frameCount));
    else pass("frame count " + std::to_string(info->frameCount));
    if (std::fabs(info->fps.toDouble() - e.fps.toDouble()) > 1e-6)
        fail("frame rate " + std::to_string(info->fps.toDouble()) + " != expected " + std::to_string(e.fps.toDouble()));
    else pass("frame rate and duration " + std::to_string(info->durationSeconds()) + " s");
    if (e.audio.present()) {
        if (info->audio.channels != e.audio.channels)
            fail("audio channels " + std::to_string(info->audio.channels) + " != expected " + std::to_string(e.audio.channels));
        else pass("audio channels " + std::to_string(info->audio.channels));
        if (info->audio.sampleRate != e.audio.sampleRate) fail("audio sample rate differs");
        if (info->audio.format != e.audio.format) fail("audio sample format differs");
        if (e.audioSamples && info->audio.sampleCount != e.audioSamples)
            fail("audio sample count " + std::to_string(info->audio.sampleCount) + " != expected " + std::to_string(e.audioSamples));
    } else if (info->audio.present()) {
        fail("unexpected audio track in output");
    }
    if (e.startTimecode) {
        if (!info->startTimecode) fail("start timecode missing");
        else if (!(*info->startTimecode == *e.startTimecode))
            fail("start timecode " + info->startTimecode->toString() + " != expected " + e.startTimecode->toString());
        else pass("start timecode " + info->startTimecode->toString());
    }
    if (e.decodeAllFrames && info->frameCount > 0) {
        BayerFrame f;
        int64_t bad = 0;
        std::string firstProblem;
        for (int64_t i = 0; i < info->frameCount; ++i) {
            if (e.cancel && e.cancel->isCancelled()) {
                r.cancelled = true;
                fail("verification cancelled");
                break;
            }
            auto d = reader->readFrame(i, f);
            if (!d || f.width != info->width || f.height != info->height) {
                if (bad++ == 0)
                    firstProblem = "frame " + std::to_string(i) + " does not decode" + (d ? ": wrong size" : ": " + d.error().toString());
            }
            if (e.progress) e.progress(i + 1, info->frameCount);
        }
        if (bad) fail(firstProblem + (bad > 1 ? " (" + std::to_string(bad) + " frames fail)" : ""));
        else if (!r.cancelled) pass("all " + std::to_string(info->frameCount) + " frames decode");
    } else if (e.decodeFirstAndLast && info->frameCount > 0) {
        BayerFrame f;
        auto a = reader->readFrame(0, f);
        if (!a) fail("first frame does not decode: " + a.error().toString());
        auto b = reader->readFrame(info->frameCount - 1, f);
        if (!b) fail("last frame does not decode: " + b.error().toString());
        if (a && b) pass("first and last frame decode");
    }
    if (!r.cancelled && e.audio.present() && !e.audioReference.empty() && info->audio.present())
        compareAudio(*reader, e.audioReference, e, r);
    reader->close();
    return r;
}

} // namespace r3dss::pipeline
