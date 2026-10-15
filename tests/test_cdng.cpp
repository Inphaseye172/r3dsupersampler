#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/synth/synth.hpp"

using namespace r3dss;
using namespace r3dss::codec;
namespace fs = std::filesystem;
using Catch::Approx;

namespace {
fs::path tmpDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("r3dss-test-" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
} // namespace

TEST_CASE("SMPTE timecode tag encodes and decodes") {
    Timecode tc = *Timecode::parse("12:34:56;07", Rational{30000, 1001});
    auto b = encodeSmpteTimecode(tc);
    Timecode back = decodeSmpteTimecode(b.data(), Rational{30000, 1001});
    REQUIRE(back == tc);
    REQUIRE(back.toString() == "12:34:56;07");
}

TEST_CASE("CinemaDNG writer -> reader round trip: samples, metadata, audio, timecode") {
    for (bool lj : {true, false}) {
        const fs::path dir = tmpDir(lj ? "cdng-lj" : "cdng-raw");
        synth::SynthClipParams p;
        p.frame.width = 320;
        p.frame.height = 180;
        p.frame.cfa = CfaPattern::GBRG;
        p.frame.pattern = synth::Pattern::Scene;
        p.frames = 5;
        p.fps = Rational{24000, 1001};
        p.audioChannels = 2;
        p.audioFormat = SampleFormat::Float32;
        p.startTimecode = Timecode::parse("01:02:03:04", p.fps);
        p.losslessJpeg = lj;
        p.clipName = "clip";
        auto infoR = synth::writeSyntheticClip(dir / "clip", p);
        REQUIRE(infoR);
        const ClipInfo& info = *infoR;
        REQUIRE(info.format == Format::CinemaDNG);
        REQUIRE(info.width == 320);
        REQUIRE(info.height == 180);
        REQUIRE(info.bits == 12);
        REQUIRE(info.white == 4095);
        REQUIRE(info.black[0] == 64);
        REQUIRE(info.cfa == CfaPattern::GBRG);
        REQUIRE(info.frameCount == 5);
        REQUIRE(info.fps.toDouble() == Approx(24000.0 / 1001.0));
        REQUIRE(info.audio.channels == 2);
        REQUIRE(info.audio.format == SampleFormat::Float32);
        REQUIRE(info.audio.sampleCount == std::llround(5.0 / p.fps.toDouble() * 48000));
        REQUIRE(info.startTimecode);
        REQUIRE(info.startTimecode->toString() == "01:02:03:04");
        REQUIRE(info.meta.cameraModel == "Synthetic");
        REQUIRE(*info.meta.iso == 800);
        REQUIRE(*info.meta.exposureTimeSec == Approx(0.02));
        REQUIRE(info.meta.whiteBalance->asShotNeutral[0] == Approx(0.6));
        REQUIRE(info.meta.color->colorMatrix1[1] == Approx(-0.2));
        REQUIRE(info.meta.lens->model == "Synthetic 35mm");
        REQUIRE(*info.meta.lens->focalLengthMm == Approx(35.0));
        REQUIRE(info.meta.opaque.size() == 1);
        REQUIRE(info.meta.opaque[0].bytes == std::vector<uint8_t>{1, 2, 3, 4, 5});
        REQUIRE(std::get<std::string>(info.meta.vendor.at("ReelName")) == "REEL01");

        auto factory = makeCinemaDngCodecFactory();
        REQUIRE(factory->probe(dir / "clip"));
        auto reader = factory->makeReader();
        REQUIRE(reader->open(dir / "clip"));
        for (int64_t i = 0; i < 5; ++i) {
            BayerFrame f;
            REQUIRE(reader->readFrame(i, f));
            synth::SynthParams sp = p.frame;
            sp.frameIndex = i;
            BayerFrame expect = synth::makeFrame(sp);
            REQUIRE(f.data == expect.data);
            REQUIRE(f.meta.timecode);
            REQUIRE(f.meta.timecode->toString() == p.startTimecode->plusFrames(i).toString());
        }
        auto audio = reader->readAudio(0, 1 << 20);
        REQUIRE(audio);
        REQUIRE(audio->sampleCount == info.audio.sampleCount);
        REQUIRE(audio->bytes == synth::makeAudio(info.audio, audio->sampleCount, p.frame.seed));
        REQUIRE(listDngFrames(dir / "clip").size() == 5);
    }
}
