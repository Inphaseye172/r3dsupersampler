// Milestone 2 acceptance: the RED R3D SDK reader on the samples-v1 KOMODO clip.
// Skips when this build has no RED SDK, when its runtime libraries are not found, or when
// R3DSS_SAMPLES does not contain the clip (scripts/fetch_samples.py).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include "r3dss/codec/codec.hpp"
#include "r3dss/codec_r3dsdk/r3dsdk.hpp"
#include "r3dss/core/paths.hpp"
#include "r3dss/pipeline/clip_converter.hpp"

using namespace r3dss;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {
fs::path komodoClip() {
    const char* dir = std::getenv("R3DSS_SAMPLES");
    if (!dir) return {};
    const fs::path p = fromUtf8(dir) / "A060_A027_0912VZ_001.R3D";
    std::error_code ec;
    return fs::exists(p, ec) ? p : fs::path();
}
std::string vendorText(const codec::ClipInfo& info, const std::string& key) {
    auto it = info.meta.vendor.find(key);
    if (it == info.meta.vendor.end()) return "<absent>";
    if (auto s = std::get_if<std::string>(&it->second)) return *s;
    if (auto i = std::get_if<int64_t>(&it->second)) return std::to_string(*i);
    return std::to_string(std::get<double>(it->second));
}
struct Init {
    Init() { codec::registerBuiltinCodecs(); }
} init;
} // namespace

TEST_CASE("RED SDK reader: KOMODO clip facts, frame decode and audio", "[r3dsdk][samples]") {
    if (!codec::r3dsdk::builtWithSdk()) SKIP("built without the RED R3D SDK (set R3DSDK_ROOT)");
    if (!codec::r3dsdk::initialize()) SKIP("RED SDK runtime not found: " + codec::r3dsdk::statusText());
    const fs::path clip = komodoClip();
    if (clip.empty()) SKIP("R3DSS_SAMPLES unset or KOMODO clip missing");

    auto factory = codec::CodecRegistry::instance().probe(clip);
    REQUIRE(factory);
    REQUIRE(factory->caps().read);
    REQUIRE(factory->caps().readsRgb);
    REQUIRE_FALSE(factory->caps().readsMosaic);
    REQUIRE(factory->status().usable());
    auto reader = factory->makeReader();
    auto info = reader->open(clip);
    REQUIRE(info);
    INFO("sdk " << vendorText(*info, "sdk.version"));

    // Verified facts (docs/CODESTREAM.md §6)
    CHECK(info->sourceLabel == "REDCODE RAW (KOMODO 6K)");
    CHECK(info->sourceKind == codec::SourceKind::RedcodeRaw);
    CHECK(info->width == 6144);
    CHECK(info->height == 3240);
    CHECK(info->frameCount == 84);
    CHECK(info->fps.toDouble() == Approx(24.0));
    REQUIRE(info->startTimecode);
    CHECK(info->startTimecode->toString() == "09:02:10:20");
    CHECK(vendorText(*info, "edge_timecode") == "01:19:31:07");
    REQUIRE(info->meta.iso);
    CHECK(*info->meta.iso == 12800);
    REQUIRE(info->meta.whiteBalance);
    REQUIRE(info->meta.whiteBalance->kelvin);
    CHECK(*info->meta.whiteBalance->kelvin == Approx(4400.0));
    CHECK(info->audio.channels == 2);
    CHECK(info->audio.format == SampleFormat::PCM32);
    CHECK(info->audio.sampleCount > 0);
    CHECK(vendorText(*info, "rmd.redcode") == "LQ");
    CHECK(vendorText(*info, "rmd.sensor_name") == "KOMODO S35");
    CHECK(vendorText(*info, "sdk.colorVersionDefault") == "3");
    CHECK(info->meta.cameraModel == "KOMODO 6K");

    // Mosaic decode is refused with the real reason; RAW-to-RAW conversion is refused up front.
    BayerFrame mosaic;
    auto m = reader->readFrame(0, mosaic);
    REQUIRE_FALSE(m);
    CHECK(m.error().code == ErrorCode::Unsupported);

    // Frame 0 at half resolution (fast), then full resolution, then the last frame.
    codec::ICodecReader::RgbOptions half;
    half.downscale = 2;
    RgbFrame img;
    REQUIRE(reader->readRgbFrame(0, img, half));
    CHECK(img.width == 3072);
    CHECK(img.height == 1620);
    REQUIRE(img.data.size() == size_t(3072) * 1620 * 3);
    double sum = 0;
    uint16_t mn = 65535, mx = 0;
    for (uint16_t v : img.data) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    const double mean = sum / static_cast<double>(img.data.size());
    INFO("frame 0 half-res: min " << mn << " max " << mx << " mean " << mean);
    CHECK(mx > mn);                 // not a flat image
    CHECK(mean > 0.01 * 65535.0);   // not black
    CHECK(mean < 0.95 * 65535.0);   // not white
    REQUIRE(img.meta.timecode);
    CHECK(img.meta.timecode->toString() == "09:02:10:20");

    codec::ICodecReader::RgbOptions full;
    RgbFrame big;
    REQUIRE(reader->readRgbFrame(0, big, full));
    CHECK(big.width == 6144);
    CHECK(big.height == 3240);
    REQUIRE(reader->readRgbFrame(83, img, half));
    REQUIRE(img.meta.timecode);
    CHECK(img.meta.timecode->toString() == info->startTimecode->plusFrames(83).toString());

    // Audio: one second from the start, little-endian 32-bit words.
    auto audio = reader->readAudio(0, 48000);
    REQUIRE(audio);
    CHECK(audio->sampleCount == std::min<int64_t>(48000, info->audio.sampleCount));
    CHECK(audio->bytes.size() == static_cast<size_t>(audio->sampleCount) * 2 * 4);

    // The converter refuses a REDCODE source with the real reason instead of a vague failure.
    pipeline::ClipJob job{clip, fs::temp_directory_path() / "r3dss-test-r3dsdk-out", "", ""};
    pipeline::JobOptions o;
    o.outputFormat = codec::Format::CinemaDNG;
    auto r = pipeline::convertClip(job, o, pipeline::ClipRunContext{});
    CHECK(r.status == pipeline::ClipStatus::Failed);
    CHECK(r.message.find("sensor mosaic") != std::string::npos);
}
