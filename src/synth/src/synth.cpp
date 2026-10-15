#include "r3dss/synth/synth.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include "r3dss/codec/memory_codec.hpp"
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/core/paths.hpp"

namespace r3dss::synth {

const char* patternName(Pattern p) {
    switch (p) {
    case Pattern::Constant: return "constant";
    case Pattern::PlaneConstants: return "planes";
    case Pattern::RampX: return "rampx";
    case Pattern::RampY: return "rampy";
    case Pattern::ZonePlate: return "zoneplate";
    case Pattern::Impulse: return "impulse";
    case Pattern::Noise: return "noise";
    case Pattern::Scene: return "scene";
    }
    return "?";
}

std::optional<Pattern> patternFromName(std::string_view s) {
    for (int i = 0; i <= static_cast<int>(Pattern::Scene); ++i)
        if (s == patternName(static_cast<Pattern>(i))) return static_cast<Pattern>(i);
    return std::nullopt;
}

BayerFrame makeFrame(const SynthParams& p) {
    BayerFrame f = BayerFrame::create(p.width, p.height, p.bits, p.cfa);
    f.black = {p.black, p.black, p.black, p.black};
    f.index = p.frameIndex;
    const double white = f.white;
    const double black = p.black;
    std::mt19937 rng(p.seed * 7919u + static_cast<uint32_t>(p.frameIndex));
    std::normal_distribution<double> noise(0.0, 1.0);
    const double cx = p.width * 0.5 + 40.0 * std::sin(0.3 * static_cast<double>(p.frameIndex));
    const double cy = p.height * 0.5 + 30.0 * std::cos(0.2 * static_cast<double>(p.frameIndex));
    for (uint32_t y = 0; y < p.height; ++y) {
        for (uint32_t x = 0; x < p.width; ++x) {
            double v = 0.0;
            const int plane = static_cast<int>((y & 1u) * 2u + (x & 1u));
            switch (p.pattern) {
            case Pattern::Constant: v = black + p.constant * (white - black); break;
            case Pattern::PlaneConstants: v = black + (0.2 + 0.2 * plane) * (white - black); break;
            case Pattern::RampX: v = black + (white - black) * (x / static_cast<double>(p.width - 1)); break;
            case Pattern::RampY: v = black + (white - black) * (y / static_cast<double>(p.height - 1)); break;
            case Pattern::ZonePlate: {
                const double dx = x - p.width * 0.5, dy = y - p.height * 0.5;
                const double r2 = dx * dx + dy * dy;
                const double k = 3.14159265358979323846 / (2.0 * std::max(p.width, p.height));
                v = black + (white - black) * 0.5 * (1.0 + std::cos(k * r2 / 4.0));
                break;
            }
            case Pattern::Impulse:
                v = (x == p.width / 2 && y == p.height / 2) ? white : black;
                break;
            case Pattern::Noise: v = black + (white - black) * 0.5 + 0.1 * (white - black) * noise(rng); break;
            case Pattern::Scene: {
                // smooth gradient + moving clipped highlight disc + fine detail stripe + mild noise
                const double gx = x / static_cast<double>(p.width), gy = y / static_cast<double>(p.height);
                double base = 0.15 + 0.5 * gx * (1.0 - 0.5 * gy);
                const int color = cfaColorAt(p.cfa, x, y);
                base *= color == 0 ? 0.8 : (color == 2 ? 0.6 : 1.0); // camera-neutral-ish colour cast
                const double dx = x - cx, dy = y - cy;
                const double d = std::sqrt(dx * dx + dy * dy);
                if (d < 60.0) base = 1.5;                                  // clipped highlight
                else if (d < 90.0) base += (90.0 - d) / 30.0 * 0.8;       // soft shoulder
                if (y > p.height * 3 / 4) base += 0.15 * std::sin(x * 0.9); // fine detail band
                v = black + (white - black) * base + 0.01 * (white - black) * noise(rng);
                break;
            }
            }
            f.data[static_cast<size_t>(y) * p.width + x] = static_cast<uint16_t>(std::clamp(std::lround(v), 0L, static_cast<long>(f.white)));
        }
    }
    return f;
}

std::vector<uint8_t> makeAudio(const AudioInfo& info, int64_t samples, uint32_t seed) {
    std::vector<uint8_t> out(static_cast<size_t>(samples) * info.frameBytes());
    for (int64_t s = 0; s < samples; ++s) {
        for (uint32_t c = 0; c < info.channels; ++c) {
            const double t = static_cast<double>(s) / info.sampleRate;
            const double v = 0.5 * std::sin(2.0 * 3.14159265358979323846 * (440.0 + 110.0 * c + seed) * t);
            uint8_t* p = out.data() + (static_cast<size_t>(s) * info.channels + c) * sampleFormatBytes(info.format);
            switch (info.format) {
            case SampleFormat::Float32: {
                const float fv = static_cast<float>(v);
                std::memcpy(p, &fv, 4);
                break;
            }
            case SampleFormat::PCM16: {
                const int16_t iv = static_cast<int16_t>(v * 32767);
                p[0] = static_cast<uint8_t>(iv); p[1] = static_cast<uint8_t>(iv >> 8);
                break;
            }
            case SampleFormat::PCM24: {
                const int32_t iv = static_cast<int32_t>(v * 8388607);
                p[0] = static_cast<uint8_t>(iv); p[1] = static_cast<uint8_t>(iv >> 8); p[2] = static_cast<uint8_t>(iv >> 16);
                break;
            }
            case SampleFormat::PCM32: {
                const int32_t iv = static_cast<int32_t>(v * 2147483647.0);
                std::memcpy(p, &iv, 4);
                break;
            }
            }
        }
    }
    return out;
}

namespace {
codec::WriterConfig configFor(const SynthClipParams& p, const std::filesystem::path& dest) {
    codec::WriterConfig wc;
    wc.dest = dest;
    wc.clipName = p.clipName;
    wc.width = p.frame.width;
    wc.height = p.frame.height;
    wc.bits = p.frame.bits;
    wc.cfa = p.frame.cfa;
    wc.black = {p.frame.black, p.frame.black, p.frame.black, p.frame.black};
    wc.white = (1u << p.frame.bits) - 1u;
    wc.fps = p.fps;
    wc.audio.channels = p.audioChannels;
    wc.audio.format = p.audioFormat;
    wc.audio.sampleRate = p.audioRate;
    wc.startTimecode = p.startTimecode;
    wc.meta.cameraMake = "R3DSS";
    wc.meta.cameraModel = "Synthetic";
    wc.meta.iso = 800;
    wc.meta.exposureTimeSec = 1.0 / 50.0;
    WhiteBalance wb;
    wb.asShotNeutral = {0.6, 1.0, 0.75};
    wc.meta.whiteBalance = wb;
    ColorInfo ci;
    ci.colorMatrix1 = {0.9, -0.2, -0.1, -0.3, 1.1, 0.2, 0.05, -0.1, 1.0};
    ci.calibrationIlluminant1 = 21;
    wc.meta.color = ci;
    LensInfo li;
    li.model = "Synthetic 35mm";
    li.focalLengthMm = 35.0;
    li.apertureF = 2.8;
    wc.meta.lens = li;
    wc.meta.opaque.push_back({"DNGPrivateData", {1, 2, 3, 4, 5}});
    wc.meta.vendor["ReelName"] = std::string("REEL01");
    return wc;
}

Expected<codec::ClipInfo> writeWith(codec::ICodecWriter& w, const codec::WriterConfig& wc, const SynthClipParams& p,
                                    codec::ICodecReader& reader, const std::filesystem::path& openPath) {
    R3DSS_TRY(w.begin(wc));
    for (int64_t i = 0; i < p.frames; ++i) {
        SynthParams sp = p.frame;
        sp.frameIndex = i;
        BayerFrame f = makeFrame(sp);
        if (p.startTimecode) f.meta.timecode = p.startTimecode->plusFrames(i);
        R3DSS_TRY(w.writeFrame(f, {}));
    }
    if (p.audioChannels) {
        AudioBlock b;
        b.info = wc.audio;
        const double dur = static_cast<double>(p.frames) / p.fps.toDouble();
        b.sampleCount = static_cast<int64_t>(std::llround(dur * p.audioRate));
        b.bytes = makeAudio(b.info, b.sampleCount, p.frame.seed);
        R3DSS_TRY(w.writeAudio(b));
    }
    R3DSS_TRY(w.finish());
    return reader.open(openPath);
}
} // namespace

Expected<codec::ClipInfo> writeSyntheticClip(const std::filesystem::path& dir, const SynthClipParams& p) {
    codec::CdngWriterOptions o;
    o.losslessJpeg = p.losslessJpeg;
    auto factory = codec::makeCinemaDngCodecFactory(o);
    auto writer = factory->makeWriter();
    auto reader = factory->makeReader();
    return writeWith(*writer, configFor(p, dir), p, *reader, dir);
}

Expected<codec::ClipInfo> makeMemoryClip(const std::string& name, const SynthClipParams& p) {
    auto factory = codec::makeMemoryCodecFactory();
    auto writer = factory->makeWriter();
    auto reader = factory->makeReader();
    const std::filesystem::path path = fromUtf8("mem://" + name);
    return writeWith(*writer, configFor(p, path), p, *reader, path);
}

} // namespace r3dss::synth
