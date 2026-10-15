#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "fixtures/red_container_fixture.hpp"
#include "r3dss/codec_r3dsdk/r3dsdk.hpp"
#include "r3dss/codec_red/red_codec.hpp"
#include "r3dss/codec_red/red_container.hpp"
#include "r3dss/codec_red/red_detect.hpp"
#include "r3dss/pipeline/clip_converter.hpp"

using namespace r3dss;
using namespace r3dss::codec;
namespace fs = std::filesystem;

namespace {
fs::path tmpDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("r3dss-test-" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
struct Init {
    Init() { codec::registerBuiltinCodecs(); }
} init;
const std::string kHeaderStrings = std::string("KMDBK008429\0#20260912\0VA060_A027_0912VZ_001.R3D\0", 48) +
                                   std::string("6K 17:9\0KOMODO 6K\0KOMODO S35\0", 29) +
                                   std::string("01:19:31:07\0" "09:02:10:20\0", 24);
} // namespace

TEST_CASE("RED container walker: 3-char tags, version byte, 4 KiB alignment, zero gaps") {
    const fs::path dir = tmpDir("redwalk");
    const fs::path clip = dir / "clip.R3D";
    std::vector<testfx::ChunkSpec> spec = {
        {"RED", 50, 914, kHeaderStrings}, {"RDI", 1, 4505600 % 70000 + 4111, ""}, {"RDI", 1, 9001, ""},
        {"RDI", 1, 4096, ""},             {"RDA", 1, 192128 % 10000 + 8, ""},    {"RDX", 2, 1001, ""},
        {"RII", 1, 740, ""},              {"RIA", 1, 124, ""},                   {"RIX", 1, 68, ""},
        {"RIX", 2, 76, ""},               {"REO", 66, 962, kHeaderStrings}};
    const auto offsets = testfx::writeRedContainer(clip, spec);
    REQUIRE(red::looksLikeRedContainer(clip));
    auto chunks = red::listChunks(clip);
    REQUIRE(chunks);
    REQUIRE(chunks->size() == spec.size());
    for (size_t i = 0; i < spec.size(); ++i) {
        INFO(i);
        REQUIRE((*chunks)[i].offset == offsets[i]);
        REQUIRE((*chunks)[i].offset % 4096 == 0);
        REQUIRE((*chunks)[i].size == spec[i].size);
        REQUIRE((*chunks)[i].tag == spec[i].tag);
        REQUIRE((*chunks)[i].version == spec[i].version);
    }
    auto hist = red::chunkHistogram(*chunks);
    REQUIRE(hist["RDI"].count == 3);
    REQUIRE(hist["RIX"].versions.size() == 2);
    REQUIRE(hist["RED"].minSize == 914);
    auto strings = red::readHeaderStrings(clip);
    REQUIRE(strings);
    REQUIRE(std::find(strings->begin(), strings->end(), "KOMODO 6K") != strings->end());
    REQUIRE(std::find(strings->begin(), strings->end(), "09:02:10:20") != strings->end());
    // Not a RED container: the old "RED2"-style 4-char tag at offset 4..8 with no alignment, or junk
    {
        std::ofstream f(dir / "junk.R3D", std::ios::binary);
        const unsigned char hdr[] = {0, 0, 0, 16, 'X', 'Y', 'Z', 1, 0, 0, 0, 0, 0, 0, 0, 0};
        f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
    }
    REQUIRE_FALSE(red::looksLikeRedContainer(dir / "junk.R3D"));
    REQUIRE_FALSE(red::listChunks(dir / "junk.R3D"));
    // A truncated last chunk stops the walk cleanly.
    {
        std::ofstream f(dir / "trunc.R3D", std::ios::binary);
        std::ifstream in(clip, std::ios::binary);
        std::vector<char> buf(5000);
        in.read(buf.data(), 5000);
        f.write(buf.data(), 5000);
    }
    auto t = red::listChunks(dir / "trunc.R3D");
    REQUIRE(t);
    REQUIRE(t->size() == 1);
}

TEST_CASE("Source detection from SDK metadata and from header strings") {
    using red::detectFromMetadata;
    using red::detectFromHeaderStrings;
    auto k = detectFromMetadata({{"camera_model", "KOMODO 6K"}, {"sensor_name", "KOMODO S35"}, {"redcode", "LQ"}});
    REQUIRE(k.kind == SourceKind::RedcodeRaw);
    REQUIRE(k.label == "REDCODE RAW (KOMODO 6K)");
    auto n = detectFromMetadata({{"camera_model", "NIKON ZR"}, {"sensor_name", "NIKON"}, {"redcode", "R3D NE"}}, ".r3d");
    REQUIRE(n.kind == SourceKind::NikonR3dNe);
    REQUIRE(n.label == "R3D NE (NIKON ZR)");
    auto nev = detectFromMetadata({{"sensor_name", "NIKON"}}, ".nev");
    REQUIRE(nev.kind == SourceKind::NikonNev);
    auto raptor = detectFromMetadata({{"camera_model", "V-RAPTOR 8K VV"}, {"sensor_name", "V-RAPTOR VV"}});
    REQUIRE(raptor.kind == SourceKind::RedcodeRaw);
    REQUIRE(detectFromMetadata({}).kind == SourceKind::Unknown);

    auto h = detectFromHeaderStrings({"RED2", "KMDBK008429", "6K 17:9", "KOMODO 6K", "KOMODO S35", "09:02:10:20"}, ".r3d");
    REQUIRE(h.kind == SourceKind::RedcodeRaw);
    REQUIRE(h.cameraModel == "KOMODO 6K");
    REQUIRE(h.sensorName == "KOMODO S35");
    REQUIRE(h.label == "REDCODE RAW (KOMODO 6K)");
    auto hn = detectFromHeaderStrings({"NIKON ZR", "NIKON", "R3D NE"}, ".r3d");
    REQUIRE(hn.kind == SourceKind::NikonR3dNe);
    REQUIRE(hn.label == "R3D NE (NIKON ZR)");
    REQUIRE(detectFromHeaderStrings({"nothing", "here"}, ".r3d").kind == SourceKind::Unknown);
    REQUIRE(detectFromHeaderStrings({"nothing"}, ".r3d").label == "RED container");
}

TEST_CASE("RED family factory recognises containers and reports the SDK state honestly") {
    const fs::path dir = tmpDir("redfactory");
    const fs::path clip = dir / "clip.R3D";
    testfx::writeRedContainer(clip, {{"RED", 50, 914, kHeaderStrings}, {"RDI", 1, 5000, ""}, {"RDI", 1, 5000, ""}, {"REO", 66, 962, ""}});
    auto& reg = CodecRegistry::instance();
    auto f = reg.probe(clip);
    REQUIRE(f);
    REQUIRE(f->format() == Format::RedFamily);
    REQUIRE_FALSE(f->caps().write);
    const bool usable = f->status().usable();
    REQUIRE(usable == r3dsdk::builtWithSdk());
    REQUIRE(f->caps().read == usable);
    auto reader = f->makeReader();
    auto info = reader->open(clip);
    REQUIRE(info);
    REQUIRE(info->sourceKind == SourceKind::RedcodeRaw);
    REQUIRE(info->sourceLabel == "REDCODE RAW (KOMODO 6K)");
    REQUIRE(info->meta.cameraModel == "KOMODO 6K");
    REQUIRE(std::get<int64_t>(info->meta.vendor.at("container.videoFrames")) == 2);
    REQUIRE(std::get<std::string>(info->meta.vendor.at("header.timecode1")) == "09:02:10:20");
    if (!f->caps().read) {
        BayerFrame fr;
        REQUIRE(reader->readFrame(0, fr).error().code == ErrorCode::CodecUnavailable);
        pipeline::ClipJob job{clip, dir / "out", "", ""};
        pipeline::JobOptions o;
        o.outputFormat = Format::CinemaDNG;
        pipeline::ClipResult r = pipeline::convertClip(job, o, pipeline::ClipRunContext{});
        REQUIRE(r.status == pipeline::ClipStatus::Failed);
        REQUIRE(r.message.find("RED R3D SDK") != std::string::npos);
    }
    // Writing REDCODE is always refused.
    auto w = f->makeWriter();
    REQUIRE(w->begin(WriterConfig{}).error().code == ErrorCode::Unsupported);
    // Extension alone is not enough.
    { std::ofstream(dir / "fake.R3D") << "not a container"; }
    REQUIRE_FALSE(reg.probe(dir / "fake.R3D"));
}
