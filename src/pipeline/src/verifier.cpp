#include "r3dss/pipeline/verifier.hpp"
#include "r3dss/core/paths.hpp"
#include <cmath>

namespace r3dss::pipeline {

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
    if (e.decodeFirstAndLast && info->frameCount > 0) {
        BayerFrame f;
        auto a = reader->readFrame(0, f);
        if (!a) fail("first frame does not decode: " + a.error().toString());
        auto b = reader->readFrame(info->frameCount - 1, f);
        if (!b) fail("last frame does not decode: " + b.error().toString());
        if (a && b) pass("first and last frame decode");
    }
    reader->close();
    return r;
}

} // namespace r3dss::pipeline
