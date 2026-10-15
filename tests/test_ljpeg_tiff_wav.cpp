#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include "r3dss/codec_cdng/ljpeg.hpp"
#include "r3dss/codec_cdng/tiff.hpp"
#include "r3dss/codec_cdng/wav.hpp"
#include "r3dss/synth/synth.hpp"

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
} // namespace

TEST_CASE("Lossless JPEG round trip, 16-bit container, 1 to 4 components, all predictors") {
    std::mt19937 rng(7);
    for (uint16_t comps : {uint16_t(1), uint16_t(2), uint16_t(4)}) {
        for (uint8_t pred = 1; pred <= 7; ++pred) {
            const uint32_t w = 64, h = 17;
            std::vector<uint16_t> s(w * h);
            for (auto& v : s) v = static_cast<uint16_t>(rng() & 0xFFFF);
            ljpeg::EncodeParams p;
            p.components = comps;
            p.predictor = pred;
            auto enc = ljpeg::encode(s.data(), w, h, p);
            REQUIRE(enc);
            auto dec = ljpeg::decode(enc->data(), enc->size());
            REQUIRE(dec);
            REQUIRE(dec->width == w);
            REQUIRE(dec->height == h);
            REQUIRE(dec->components == comps);
            REQUIRE(dec->samples == s);
        }
    }
}

TEST_CASE("Lossless JPEG 12-bit precision and extreme values") {
    const uint32_t w = 48, h = 8;
    std::vector<uint16_t> s(w * h);
    for (size_t i = 0; i < s.size(); ++i) s[i] = (i % 3 == 0) ? 4095 : (i % 3 == 1 ? 0 : 2048);
    ljpeg::EncodeParams p;
    p.precision = 12;
    p.components = 2;
    auto enc = ljpeg::encode(s.data(), w, h, p);
    REQUIRE(enc);
    auto dec = ljpeg::decode(enc->data(), enc->size());
    REQUIRE(dec);
    REQUIRE(dec->samples == s);
    // 16-bit extremes incl. the +32768 special category
    std::vector<uint16_t> e(w * h);
    for (size_t i = 0; i < e.size(); ++i) e[i] = (i & 1) ? 65535 : 0;
    e[5] = 32768;
    ljpeg::EncodeParams p16;
    p16.components = 1;
    auto enc16 = ljpeg::encode(e.data(), w, h, p16);
    REQUIRE(enc16);
    auto dec16 = ljpeg::decode(enc16->data(), enc16->size());
    REQUIRE(dec16);
    REQUIRE(dec16->samples == e);
}

TEST_CASE("Lossless JPEG compresses a smooth synthetic frame") {
    synth::SynthParams sp;
    sp.width = 512; sp.height = 288; sp.pattern = synth::Pattern::Scene;
    BayerFrame f = synth::makeFrame(sp);
    ljpeg::EncodeParams p;
    p.components = 2;
    auto enc = ljpeg::encode(f.data.data(), f.width, f.height, p);
    REQUIRE(enc);
    // 12-bit content with mild noise codes at ~9 bits/sample in the 16-bit container.
    REQUIRE(enc->size() < f.byteSize() * 0.65);
}

TEST_CASE("TIFF writer output is readable by the TIFF reader") {
    const fs::path dir = tmpDir("tiff");
    tiff::Writer w;
    std::vector<uint8_t> payload(1000, 0xAB);
    const uint64_t off = w.appendData(payload);
    tiff::Writer::IfdBuilder exif, main;
    exif.setU16(tiff::tag::ISOSpeedRatings, {800});
    const uint64_t exifOff = w.writeIfd(exif);
    main.setU32(tiff::tag::ImageWidth, {640});
    main.setU32(tiff::tag::ImageLength, {480});
    main.setAscii(tiff::tag::Make, "Test");
    main.setRational(tiff::tag::ExposureTime, {{1, 50}});
    main.setSRational(tiff::tag::ColorMatrix1, {{-5, 10}});
    main.setU32(tiff::tag::StripOffsets, {static_cast<uint32_t>(off)});
    main.setU32(tiff::tag::ExifIFD, {static_cast<uint32_t>(exifOff)});
    w.setFirstIfd(w.writeIfd(main));
    REQUIRE(w.save(dir / "t.tif"));
    tiff::Reader r;
    REQUIRE(r.open(dir / "t.tif"));
    auto ifd = r.readIfd(r.firstIfdOffset());
    REQUIRE(ifd);
    REQUIRE(*ifd->u32(tiff::tag::ImageWidth) == 640);
    REQUIRE(ifd->ascii(tiff::tag::Make) == "Test");
    REQUIRE(*ifd->real(tiff::tag::ExposureTime) == 0.02);
    REQUIRE(*ifd->real(tiff::tag::ColorMatrix1) == -0.5);
    auto exifR = r.readIfd(*ifd->u32(tiff::tag::ExifIFD));
    REQUIRE(exifR);
    REQUIRE(*exifR->u32(tiff::tag::ISOSpeedRatings) == 800);
    REQUIRE(r.bytes()[off] == 0xAB);
}

TEST_CASE("WAV stream writer and reader round trip float32 and pcm24") {
    const fs::path dir = tmpDir("wav");
    for (auto fmt : {SampleFormat::Float32, SampleFormat::PCM24, SampleFormat::PCM16}) {
        AudioInfo info;
        info.channels = fmt == SampleFormat::PCM16 ? 2 : 4;
        info.sampleRate = 48000;
        info.format = fmt;
        AudioBlock b;
        b.info = info;
        b.sampleCount = 4801;
        b.bytes = synth::makeAudio(info, b.sampleCount, 3);
        const fs::path p = dir / (std::string(sampleFormatName(fmt)) + ".wav");
        wav::StreamWriter w;
        REQUIRE(w.begin(p, info));
        REQUIRE(w.write(b));
        REQUIRE(w.finish());
        auto wi = wav::probe(p);
        REQUIRE(wi);
        REQUIRE(wi->audio.channels == info.channels);
        REQUIRE(wi->audio.format == fmt);
        REQUIRE(wi->audio.sampleCount == b.sampleCount);
        auto rd = wav::read(p, *wi, 0, 100000);
        REQUIRE(rd);
        REQUIRE(rd->sampleCount == b.sampleCount);
        REQUIRE(rd->bytes == b.bytes);
        auto part = wav::read(p, *wi, 4800, 10);
        REQUIRE(part->sampleCount == 1);
    }
}
