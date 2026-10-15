#include "r3dss/pipeline/verifier.hpp"
#include "r3dss/core/paths.hpp"
#include <cmath>

namespace r3dss::pipeline {

namespace {

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

VerificationResult verifyOutput(const std::filesystem::path& output, const Expectation& e) {
    VerificationResult r;
    auto fail = [&](const std::string& s) { r.ok = false; r.problems.push_back(s); };
    auto pass = [&](const std::string& s) { r.checked.push_back(s); };
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
